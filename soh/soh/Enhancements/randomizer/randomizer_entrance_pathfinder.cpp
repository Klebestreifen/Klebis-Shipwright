#include "randomizer_entrance_pathfinder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <libultraship/bridge/consolevariablebridge.h>

#include "randomizer_entrance_tracker.h"
#include "location_access.h"
#include "entrance.h"
#include "3drando/fill.hpp"
#include "soh/OTRGlobals.h"
#include "soh/cvar_prefixes.h"
#include "soh/util.h"

extern "C" {
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
}

namespace EntrancePathfinder {
namespace {

constexpr int32_t NO_PLACE = -1;

// Costs of the route steps. Loading zones dominate, walking between the regions of a place only breaks ties.
constexpr uint32_t COST_WALK = 1;
constexpr uint32_t COST_LOADING_ZONE = 1000;
constexpr uint32_t COST_WAIT = 1000;
constexpr uint32_t COST_WARP_SONG = 1200;
constexpr uint32_t COST_AGE_CHANGE = 1500;
// Saving and reloading is meant to be the last resort
constexpr uint32_t COST_SAVEWARP = 3000;

// Age and time of day of a search state. Bit 0 is the time of day and bit 1 the age, so one XOR flips either.
constexpr uint32_t AGE_TIME_NIGHT = 1 << 0;
constexpr uint32_t AGE_TIME_ADULT = 1 << 1;
constexpr uint32_t AGE_TIME_COUNT = 4;

// The logic models the castle grounds with these regions for both ages. They only forward to Hyrule Castle (child) or
// to the outside of Ganon's Castle (adult) and borrow the scene of the latter, so they are not a place of their own.
constexpr RandomizerRegion transitRegions[] = {
    RR_CASTLE_GROUNDS,
    RR_CASTLE_GROUNDS_FROM_GREAT_FAIRY,
    RR_CASTLE_GROUNDS_FROM_GROTTO,
    RR_CASTLE_GROUNDS_FROM_GANONS_CASTLE,
};

struct WarpSong {
    RandomizerGet song;
    RandomizerRegion region;
    const char* name;
};

// Each of these regions has a single exit, which is the (possibly shuffled) destination of the song
constexpr WarpSong warpSongs[] = {
    { RG_MINUET_OF_FOREST, RR_MINUET_OF_FOREST_WARP, "Minuet of Forest" },
    { RG_BOLERO_OF_FIRE, RR_BOLERO_OF_FIRE_WARP, "Bolero of Fire" },
    { RG_SERENADE_OF_WATER, RR_SERENADE_OF_WATER_WARP, "Serenade of Water" },
    { RG_REQUIEM_OF_SPIRIT, RR_REQUIEM_OF_SPIRIT_WARP, "Requiem of Spirit" },
    { RG_NOCTURNE_OF_SHADOW, RR_NOCTURNE_OF_SHADOW_WARP, "Nocturne of Shadow" },
    { RG_PRELUDE_OF_LIGHT, RR_PRELUDE_OF_LIGHT_WARP, "Prelude of Light" },
};

std::vector<Place> places;
std::array<int32_t, RR_MAX> regionPlace;
bool placesBuilt = false;

bool IsValidRegion(int32_t region) {
    return region > RR_NONE && region < RR_MAX;
}

bool IsTransitRegion(RandomizerRegion region) {
    return std::find(std::begin(transitRegions), std::end(transitRegions), region) != std::end(transitRegions);
}

bool RegionHasPlace(RandomizerRegion region) {
    // Regions without a scene are the root, the spawns and the warp songs
    return areaTable[region].scene != SCENE_ID_MAX && !IsTransitRegion(region);
}

// The logic only knows the day variant of the scenes that differ by time of day
std::string GetPlaceSceneName(SceneID scene) {
    std::string name = SohUtils::GetSceneName(scene);
    for (const char* suffix : { " (Child - Day)", " (Day)" }) {
        if (name.ends_with(suffix)) {
            name.erase(name.size() - strlen(suffix));
            break;
        }
    }
    return name;
}

// A shuffled entrance may only be used, shown or reasoned about once the player has seen where it leads.
// Entrances that are not shuffled are vanilla knowledge.
bool IsExitKnown(const Rando::Entrance& exit) {
    return exit.GetIndex() == -1 || !exit.IsShuffled() || EntranceTracker::IsEntranceDiscovered(exit.GetIndex());
}

// The words that all the names start with, e.g. "TH Kitchen" for "TH Kitchen Corridor Lower" and
// "TH Kitchen By Corridor"
std::string GetCommonWordPrefix(const std::vector<std::string>& names) {
    std::vector<std::string> prefix;
    for (size_t i = 0; i < names.size(); i++) {
        std::istringstream stream(names[i]);
        std::vector<std::string> words;
        for (std::string word; stream >> word;) {
            words.push_back(word);
        }

        if (i == 0) {
            prefix = words;
            continue;
        }

        size_t common = 0;
        while (common < prefix.size() && common < words.size() && prefix[common] == words[common]) {
            common++;
        }
        prefix.resize(common);
    }

    std::string result;
    for (const std::string& word : prefix) {
        result += result.empty() ? word : " " + word;
    }
    return result;
}

// Groups the regions into places: regions of the same scene that are connected without a loading zone in between
bool EnsurePlaces() {
    if (placesBuilt) {
        return true;
    }

    // The region table is built by the check tracker when a file is loaded
    if (areaTable[RR_ROOT].exits.empty()) {
        return false;
    }

    std::array<int32_t, RR_MAX> group;
    std::iota(group.begin(), group.end(), 0);
    auto findGroup = [&group](int32_t region) {
        while (group[region] != region) {
            group[region] = group[group[region]];
            region = group[region];
        }
        return region;
    };

    for (int32_t region = RR_NONE + 1; region < RR_MAX; region++) {
        if (!RegionHasPlace((RandomizerRegion)region)) {
            continue;
        }
        for (const Rando::Entrance& exit : areaTable[region].exits) {
            const RandomizerRegion connected = exit.GetConnectedRegionKey();
            if (exit.GetIndex() != -1 || !IsValidRegion(connected) || !RegionHasPlace(connected) ||
                areaTable[connected].scene != areaTable[region].scene) {
                continue;
            }
            group[findGroup(region)] = findGroup(connected);
        }
    }

    places.clear();
    regionPlace.fill(NO_PLACE);
    std::vector<RandomizerRegion> firstRegionOfPlace;
    std::unordered_map<int32_t, int32_t> placeOfGroup;
    std::unordered_map<int32_t, int32_t> placesPerScene;
    for (int32_t region = RR_NONE + 1; region < RR_MAX; region++) {
        if (!RegionHasPlace((RandomizerRegion)region)) {
            continue;
        }
        auto [iter, isNew] = placeOfGroup.try_emplace(findGroup(region), (int32_t)places.size());
        if (isNew) {
            places.push_back({ "", GetPlaceSceneName(areaTable[region].scene), false });
            firstRegionOfPlace.push_back((RandomizerRegion)region);
            placesPerScene[areaTable[region].scene]++;
        }
        regionPlace[region] = iter->second;
    }

    // Places that share their scene with others are named after the entrances that lead into them in vanilla
    std::vector<std::vector<std::string>> entranceNames(places.size());
    for (int32_t region = RR_NONE + 1; region < RR_MAX; region++) {
        for (const Rando::Entrance& exit : areaTable[region].exits) {
            const RandomizerRegion original = exit.GetOriginalConnectedRegionKey();
            if (exit.GetIndex() == -1 || !IsValidRegion(original) || regionPlace[original] == NO_PLACE) {
                continue;
            }
            const EntranceData* data = EntranceTracker::GetEntranceData(exit.GetIndex());
            if (data != nullptr && !data->destination.empty()) {
                entranceNames[regionPlace[original]].push_back(data->destination);
            }
        }
    }

    for (size_t i = 0; i < places.size(); i++) {
        const Region& region = areaTable[firstRegionOfPlace[i]];
        if (placesPerScene[region.scene] == 1) {
            places[i].name = places[i].sceneName;
            continue;
        }

        const std::vector<std::string>& names = entranceNames[i];
        std::string name = names.size() == 1 ? names[0] : GetCommonWordPrefix(names);
        // A single common word is only the abbreviation of the area, e.g. "TH"
        if (name.find(' ') == std::string::npos) {
            name = region.regionName;
        }
        places[i].name = name;
    }

    placesBuilt = true;
    return true;
}

void MarkRegionVisited(RandomizerRegion region) {
    if (!IsValidRegion(region)) {
        return;
    }

    if (regionPlace[region] != NO_PLACE) {
        places[regionPlace[region]].visited = true;
    } else if (IsTransitRegion(region)) {
        // Which side of the castle grounds was seen depends on the age at that time, which is not recorded
        for (const Rando::Entrance& exit : areaTable[region].exits) {
            const RandomizerRegion connected = exit.GetConnectedRegionKey();
            if (exit.GetIndex() == -1 && IsValidRegion(connected) && regionPlace[connected] != NO_PLACE) {
                places[regionPlace[connected]].visited = true;
            }
        }
    }
}

int32_t GetPlaceOfLink(RandomizerRegion region) {
    if (!IsValidRegion(region)) {
        return NO_PLACE;
    }

    if (regionPlace[region] != NO_PLACE || !IsTransitRegion(region) || gPlayState == nullptr) {
        return regionPlace[region];
    }

    // In a transit region the loaded scene tells which side Link is on
    for (const Rando::Entrance& exit : areaTable[region].exits) {
        const RandomizerRegion connected = exit.GetConnectedRegionKey();
        if (exit.GetIndex() == -1 && IsValidRegion(connected) && regionPlace[connected] != NO_PLACE &&
            areaTable[connected].scene == gPlayState->sceneNum) {
            return regionPlace[connected];
        }
    }
    return NO_PLACE;
}

// Mirrors Entrance_SetSavewarpEntrance: only where it leads to the regular spawn is a savewarp worth a step
bool SavewarpLeadsToSpawn(SceneID scene) {
    switch (scene) {
        case SCENE_DEKU_TREE:
        case SCENE_DEKU_TREE_BOSS:
        case SCENE_DODONGOS_CAVERN:
        case SCENE_DODONGOS_CAVERN_BOSS:
        case SCENE_JABU_JABU:
        case SCENE_JABU_JABU_BOSS:
        case SCENE_FOREST_TEMPLE:
        case SCENE_FOREST_TEMPLE_BOSS:
        case SCENE_FIRE_TEMPLE:
        case SCENE_FIRE_TEMPLE_BOSS:
        case SCENE_WATER_TEMPLE:
        case SCENE_WATER_TEMPLE_BOSS:
        case SCENE_SPIRIT_TEMPLE:
        case SCENE_SPIRIT_TEMPLE_BOSS:
        case SCENE_SHADOW_TEMPLE:
        case SCENE_SHADOW_TEMPLE_BOSS:
        case SCENE_BOTTOM_OF_THE_WELL:
        case SCENE_GERUDO_TRAINING_GROUND:
        case SCENE_ICE_CAVERN:
        case SCENE_INSIDE_GANONS_CASTLE:
        case SCENE_GANONS_TOWER:
        case SCENE_GANONDORF_BOSS:
        case SCENE_INSIDE_GANONS_CASTLE_COLLAPSE:
        case SCENE_GANONS_TOWER_COLLAPSE_INTERIOR:
        case SCENE_GANON_BOSS:
        case SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR:
            return false;
        case SCENE_THIEVES_HIDEOUT:
            if (!RAND_GET_OPTION(RSK_SHUFFLE_THIEVES_HIDEOUT_ENTRANCES).Get()) {
                return false;
            }
            break;
        case SCENE_LINKS_HOUSE:
            if (RAND_GET_OPTION(RSK_SHUFFLE_INTERIOR_ENTRANCES).IsNot(RO_INTERIOR_ENTRANCE_SHUFFLE_ALL)) {
                return false;
            }
            break;
        default:
            break;
    }

    // With this enhancement the file is loaded where it was saved, except for grottos and fairy fountains
    if (CVarGetInteger(CVAR_ENHANCEMENT("RememberSaveLocation"), 0)) {
        return scene == SCENE_GROTTOS || scene == SCENE_FAIRYS_FOUNTAIN;
    }
    return true;
}

std::string GetRegionDisplayName(RandomizerRegion region) {
    return regionPlace[region] != NO_PLACE ? places[regionPlace[region]].name : areaTable[region].regionName;
}

// Names an entrance like the tracker's list does: where it is, and where it leads to in this seed
std::string GetEntranceText(const Rando::Entrance& exit, bool withSource = true) {
    const EntranceData* source = EntranceTracker::GetEntranceData(exit.GetIndex());
    const EntranceData* destination =
        EntranceTracker::GetEntranceData(EntranceTracker::GetEntranceOverrideIndex(exit.GetIndex()));
    const std::string destinationName =
        destination != nullptr ? destination->destination : GetRegionDisplayName(exit.GetConnectedRegionKey());
    if (!withSource) {
        return destinationName;
    }
    return (source != nullptr ? source->source : GetRegionDisplayName(exit.GetParentRegionKey())) + " -> " +
           destinationName;
}

// The search evaluates the logic at ages and times that differ from the real ones and it needs the logic to be in
// "available checks" mode, where it reads the inventory from the save. This puts everything back afterwards.
struct LogicStateGuard {
    bool isChild = logic->IsChild;
    bool isAdult = logic->IsAdult;
    bool atDay = logic->AtDay;
    bool atNight = logic->AtNight;
    bool processUndiscoveredExits = logic->ACProcessUndiscoveredExits;
    bool calculatingAvailableChecks = logic->CalculatingAvailableChecks;
    RandomizerRegion currentRegionKey = logic->CurrentRegionKey;

    ~LogicStateGuard() {
        logic->IsChild = isChild;
        logic->IsAdult = isAdult;
        logic->AtDay = atDay;
        logic->AtNight = atNight;
        logic->ACProcessUndiscoveredExits = processUndiscoveredExits;
        logic->CalculatingAvailableChecks = calculatingAvailableChecks;
        logic->CurrentRegionKey = currentRegionKey;
    }
};

void SetLogicAgeTime(uint32_t ageTime) {
    logic->IsChild = !(ageTime & AGE_TIME_ADULT);
    logic->IsAdult = ageTime & AGE_TIME_ADULT;
    logic->AtDay = !(ageTime & AGE_TIME_NIGHT);
    logic->AtNight = ageTime & AGE_TIME_NIGHT;
}

bool CheckExitCondition(const Rando::Entrance& exit, uint32_t ageTime) {
    bool& age = (ageTime & AGE_TIME_ADULT) ? logic->IsAdult : logic->IsChild;
    bool& time = (ageTime & AGE_TIME_NIGHT) ? logic->AtNight : logic->AtDay;
    return exit.CheckConditionAtAgeTime(age, time);
}

// How a search state was reached
struct Arrival {
    int32_t fromState = -1;
    bool isStep = false;
    RouteStepType type = ROUTE_STEP_ENTRANCE;
    const Rando::Entrance* exit = nullptr;
    const WarpSong* warpSong = nullptr;
};

RouteStep MakeRouteStep(const Arrival& arrival) {
    RouteStep step = { arrival.type, -1, "" };
    const RandomizerRegion region = (RandomizerRegion)(arrival.fromState / AGE_TIME_COUNT);

    switch (arrival.type) {
        case ROUTE_STEP_ENTRANCE:
            step.entranceIndex = arrival.exit->GetIndex();
            step.text = GetEntranceText(*arrival.exit);
            break;
        case ROUTE_STEP_PASSAGE:
            step.text = GetRegionDisplayName(arrival.exit->GetParentRegionKey()) + " -> " +
                        GetRegionDisplayName(arrival.exit->GetConnectedRegionKey());
            break;
        case ROUTE_STEP_WARP_SONG:
            step.entranceIndex = arrival.exit->GetIndex();
            step.text = std::string("Play ") + arrival.warpSong->name + " -> " + GetEntranceText(*arrival.exit, false);
            break;
        case ROUTE_STEP_SAVEWARP:
            step.entranceIndex = arrival.exit->GetIndex();
            step.text = "Save, quit and reload -> " + GetEntranceText(*arrival.exit, false);
            break;
        case ROUTE_STEP_WAIT_FOR_DAY:
            step.text = "Wait for day (" + GetRegionDisplayName(region) + ")";
            break;
        case ROUTE_STEP_WAIT_FOR_NIGHT:
            step.text = "Wait for night (" + GetRegionDisplayName(region) + ")";
            break;
        case ROUTE_STEP_BECOME_ADULT:
            step.text = "Become adult at the Master Sword pedestal";
            break;
        case ROUTE_STEP_BECOME_CHILD:
            step.text = "Become child at the Master Sword pedestal";
            break;
    }
    return step;
}

} // namespace

RandomizerRegion GetRegionFromEntranceIndex(int16_t entranceIndex) {
    if (entranceIndex < 0 || entranceIndex >= ENTR_MAX) {
        return RR_NONE;
    }

    const int8_t scene = gEntranceTable[entranceIndex].scene;
    for (; entranceIndex >= 0 && gEntranceTable[entranceIndex].scene == scene; entranceIndex--) {
        const auto entrance = Rando::EntranceShuffler::GetEntranceByIndex(entranceIndex);
        if (entrance != nullptr) {
            return entrance->GetOriginalConnectedRegionKey();
        }
    }
    return RR_NONE;
}

RandomizerRegion GetCurrentRegion() {
    // Grottos and fairy fountains share one scene each, and their entrance index says nothing about which one it is
    if (gPlayState != nullptr &&
        (gPlayState->sceneNum == SCENE_GROTTOS || gPlayState->sceneNum == SCENE_FAIRYS_FOUNTAIN)) {
        const int16_t grottoId = EntranceTracker::GetCurrentGrottoId();
        if (grottoId >= 0) {
            const auto grottoExit = Rando::EntranceShuffler::GetEntranceByIndex(ENTRANCE_GROTTO_EXIT_START + grottoId);
            if (grottoExit != nullptr) {
                return grottoExit->GetParentRegionKey();
            }
        }

        // The grotto id is only tracked if grottos can be shuffled. Otherwise start from where Link will come out,
        // which is the entrance he used to get to the grotto's surroundings.
        const RandomizerRegion returnRegion =
            GetRegionFromEntranceIndex(gSaveContext.respawn[RESPAWN_MODE_RETURN].entranceIndex);
        if (returnRegion != RR_NONE) {
            return returnRegion;
        }
    }

    return GetRegionFromEntranceIndex(gSaveContext.entranceIndex);
}

RandoAgeTime GetCurrentAgeTime() {
    if (LINK_IS_CHILD && IS_DAY) {
        return RAT_CHILD_DAY;
    } else if (LINK_IS_CHILD && IS_NIGHT) {
        return RAT_CHILD_NIGHT;
    } else if (LINK_IS_ADULT && IS_DAY) {
        return RAT_ADULT_DAY;
    } else if (LINK_IS_ADULT && IS_NIGHT) {
        return RAT_ADULT_NIGHT;
    }
    return RAT_NONE;
}

const std::vector<Place>& GetPlaces() {
    EnsurePlaces();
    return places;
}

void UpdateVisitedPlaces() {
    if (!EnsurePlaces()) {
        return;
    }

    for (Place& place : places) {
        place.visited = false;
    }

    // A place counts as visited if the player has used an entrance that starts there or leads there
    for (int32_t region = RR_NONE + 1; region < RR_MAX; region++) {
        for (const Rando::Entrance& exit : areaTable[region].exits) {
            if (exit.GetIndex() != -1 && EntranceTracker::IsEntranceDiscovered(exit.GetIndex())) {
                MarkRegionVisited((RandomizerRegion)region);
                MarkRegionVisited(exit.GetConnectedRegionKey());
            }
        }
    }

    const int32_t currentPlace = GetPlaceOfLink(GetCurrentRegion());
    if (currentPlace != NO_PLACE) {
        places[currentPlace].visited = true;
    }
}

int32_t GetCurrentPlace() {
    if (!EnsurePlaces()) {
        return NO_PLACE;
    }
    return GetPlaceOfLink(GetCurrentRegion());
}

Route FindRoute(int32_t targetPlace, bool ignoreLogic) {
    Route route;
    if (!EnsurePlaces() || targetPlace < 0 || targetPlace >= (int32_t)places.size()) {
        return route;
    }

    const RandomizerRegion startRegion = GetCurrentRegion();
    const RandoAgeTime startAgeTime = GetCurrentAgeTime();
    if (!IsValidRegion(startRegion) || startAgeTime == RAT_NONE) {
        route.status = ROUTE_UNKNOWN_START;
        return route;
    }

    if (GetPlaceOfLink(startRegion) == targetPlace) {
        route.status = ROUTE_AT_DESTINATION;
        return route;
    }

    logic = Rando::Context::GetInstance()->GetLogic();
    const LogicStateGuard logicStateGuard;

    if (!ignoreLogic) {
        // Many conditions depend on events and on which regions each age can reach (e.g. AnyAgeTime). The same search
        // that "available checks" uses fills both in. It must not look past undiscovered entrances, even if the debug
        // console told it to.
        logic->ACProcessUndiscoveredExits = false;
        ReachabilitySearch({}, RG_NONE, true, startRegion, startAgeTime);
    }

    // Dijkstra over (region, age and time of day)
    const size_t stateCount = (size_t)RR_MAX * AGE_TIME_COUNT;
    std::vector<uint32_t> costs(stateCount, std::numeric_limits<uint32_t>::max());
    std::vector<Arrival> arrivals(stateCount);
    typedef std::pair<uint32_t, int32_t> QueueEntry;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;

    const int32_t startState = startRegion * AGE_TIME_COUNT + (startAgeTime - RAT_CHILD_DAY);
    costs[startState] = 0;
    queue.push({ 0, startState });

    int32_t goalState = -1;
    while (!queue.empty()) {
        const auto [cost, state] = queue.top();
        queue.pop();
        if (cost != costs[state]) {
            continue;
        }

        const RandomizerRegion region = (RandomizerRegion)(state / AGE_TIME_COUNT);
        const uint32_t ageTime = state % AGE_TIME_COUNT;
        if (regionPlace[region] == targetPlace) {
            goalState = state;
            break;
        }

        auto arriveAt = [&](RandomizerRegion toRegion, uint32_t toAgeTime, uint32_t stepCost, Arrival arrival) {
            const int32_t toState = toRegion * AGE_TIME_COUNT + toAgeTime;
            if (cost + stepCost < costs[toState]) {
                costs[toState] = cost + stepCost;
                arrival.fromState = state;
                arrivals[toState] = arrival;
                queue.push({ costs[toState], toState });
            }
        };

        const Region& area = areaTable[region];
        const bool isAdult = ageTime & AGE_TIME_ADULT;
        logic->CurrentRegionKey = region;

        for (const Rando::Entrance& exit : area.exits) {
            const RandomizerRegion connected = exit.GetConnectedRegionKey();
            if (!IsValidRegion(connected) || !IsExitKnown(exit)) {
                continue;
            }

            if (exit.GetIndex() == -1 && IsTransitRegion(region)) {
                // Which side of the castle grounds exists is a matter of age, not of logic
                if ((areaTable[connected].scene == SCENE_HYRULE_CASTLE) == isAdult) {
                    continue;
                }
            } else if (!ignoreLogic && !CheckExitCondition(exit, ageTime)) {
                continue;
            }

            if (exit.GetIndex() != -1) {
                arriveAt(connected, ageTime, COST_LOADING_ZONE, { -1, true, ROUTE_STEP_ENTRANCE, &exit });
            } else if (regionPlace[region] != NO_PLACE && regionPlace[connected] != NO_PLACE &&
                       regionPlace[region] != regionPlace[connected]) {
                arriveAt(connected, ageTime, COST_LOADING_ZONE, { -1, true, ROUTE_STEP_PASSAGE, &exit });
            } else {
                arriveAt(connected, ageTime, COST_WALK, {});
            }
        }

        if (area.timePass) {
            arriveAt(region, ageTime ^ AGE_TIME_NIGHT, COST_WAIT,
                     { -1, true, (ageTime & AGE_TIME_NIGHT) ? ROUTE_STEP_WAIT_FOR_DAY : ROUTE_STEP_WAIT_FOR_NIGHT });
        }

        if (region == RR_TOT_BEYOND_DOOR_OF_TIME) {
            arriveAt(region, ageTime ^ AGE_TIME_ADULT, COST_AGE_CHANGE,
                     { -1, true, isAdult ? ROUTE_STEP_BECOME_CHILD : ROUTE_STEP_BECOME_ADULT });
        }

        // Warp songs stay bound to owning them and to being able to play them here, even if logic is ignored
        SetLogicAgeTime(ageTime);
        for (const WarpSong& warpSong : warpSongs) {
            if (areaTable[warpSong.region].exits.empty()) {
                continue;
            }
            const Rando::Entrance& exit = areaTable[warpSong.region].exits.front();
            if (IsValidRegion(exit.GetConnectedRegionKey()) && IsExitKnown(exit) && logic->CanUse(warpSong.song)) {
                arriveAt(exit.GetConnectedRegionKey(), ageTime, COST_WARP_SONG,
                         { -1, true, ROUTE_STEP_WARP_SONG, &exit, &warpSong });
            }
        }

        const Region& spawn = areaTable[isAdult ? RR_ADULT_SPAWN : RR_CHILD_SPAWN];
        if (!spawn.exits.empty() && SavewarpLeadsToSpawn(area.scene)) {
            const Rando::Entrance& exit = spawn.exits.front();
            if (IsValidRegion(exit.GetConnectedRegionKey()) && IsExitKnown(exit)) {
                arriveAt(exit.GetConnectedRegionKey(), ageTime, COST_SAVEWARP,
                         { -1, true, ROUTE_STEP_SAVEWARP, &exit });
            }
        }
    }

    if (goalState == -1) {
        route.status = ROUTE_NO_KNOWN_ROUTE;
        return route;
    }

    for (int32_t state = goalState; state != startState; state = arrivals[state].fromState) {
        if (arrivals[state].isStep) {
            route.steps.push_back(MakeRouteStep(arrivals[state]));
        }
    }
    std::reverse(route.steps.begin(), route.steps.end());
    // Without a single step in between, the destination is part of where Link already is
    route.status = route.steps.empty() ? ROUTE_AT_DESTINATION : ROUTE_FOUND;
    return route;
}

void Reset() {
    places.clear();
    regionPlace.fill(NO_PLACE);
    placesBuilt = false;
}

} // namespace EntrancePathfinder
