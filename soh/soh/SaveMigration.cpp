#include "SaveMigration.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include <spdlog/spdlog.h>

#include "soh/util.h"
#include "soh/Enhancements/randomizer/randomizerTypes.h"

extern "C" {
#include "z64.h"
#include <variables.h>
}

namespace SaveMigration {

namespace {

nlohmann::json* GetSectionData(nlohmann::json& saveBlock, const char* section) {
    if (!saveBlock.contains("sections")) {
        return nullptr;
    }
    nlohmann::json& sections = saveBlock["sections"];
    if (!sections.contains(section) || !sections[section].contains("data")) {
        return nullptr;
    }
    return &sections[section]["data"];
}

// ---------------------------------------------------------------------------------------------------------------
// Generic helper for enums that gained entries in the middle between releases.
// ---------------------------------------------------------------------------------------------------------------

// Describes which indices of an enum were inserted, expressed in terms of the *current* enum values.
struct IndexRemap {
    std::vector<size_t> inserted; // sorted ascending
    size_t newSize;

    size_t OldSize() const {
        return newSize - inserted.size();
    }

    bool IsInserted(size_t newIndex) const {
        return std::binary_search(inserted.begin(), inserted.end(), newIndex);
    }

    // Maps an index from the old layout to the new one.
    size_t ToNew(size_t oldIndex) const {
        size_t result = oldIndex;
        for (size_t ins : inserted) {
            if (ins <= result) {
                result++;
            } else {
                break;
            }
        }
        return result;
    }

    // Rebuilds a JSON array in the new layout, filling inserted slots with `fill`.
    nlohmann::json RemapArray(const nlohmann::json& oldArray, const nlohmann::json& fill) const {
        nlohmann::json out = nlohmann::json::array();
        size_t oldIndex = 0;
        for (size_t i = 0; i < newSize; i++) {
            if (IsInserted(i)) {
                out.push_back(fill);
            } else if (oldArray.is_array() && oldIndex < oldArray.size()) {
                out.push_back(oldArray[oldIndex++]);
            } else {
                out.push_back(fill);
            }
        }
        return out;
    }

    // Rebuilds a packed u16 bit array (as used by randomizerInf) in the new layout.
    nlohmann::json RemapBits(const nlohmann::json& oldWords) const {
        const size_t oldBits = OldSize();
        std::vector<bool> oldFlags(oldBits, false);
        if (oldWords.is_array()) {
            for (size_t bit = 0; bit < oldBits; bit++) {
                size_t word = bit >> 4;
                if (word < oldWords.size()) {
                    oldFlags[bit] = (oldWords[word].get<uint32_t>() & (1u << (bit & 0xF))) != 0;
                }
            }
        }

        std::vector<uint16_t> newWords((newSize + 15) / 16, 0);
        size_t oldBit = 0;
        for (size_t bit = 0; bit < newSize; bit++) {
            bool set = false;
            if (!IsInserted(bit)) {
                set = oldBit < oldBits && oldFlags[oldBit];
                oldBit++;
            }
            if (set) {
                newWords[bit >> 4] |= static_cast<uint16_t>(1u << (bit & 0xF));
            }
        }

        nlohmann::json out = nlohmann::json::array();
        for (uint16_t w : newWords) {
            out.push_back(w);
        }
        return out;
    }
};

std::vector<size_t> Range(size_t first, size_t last) {
    std::vector<size_t> v;
    for (size_t i = first; i <= last; i++) {
        v.push_back(i);
    }
    return v;
}

std::vector<size_t> Concat(std::vector<size_t> a, const std::vector<size_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    std::sort(a.begin(), a.end());
    return a;
}

// ---------------------------------------------------------------------------------------------------------------
// Step: DeweyBravoEnumLayout
//
// Ship of Harkinian 9.3.1 (Dewey Bravo) -> KShip 10.0.0. Upstream inserted new entries in the middle of several
// enums after 9.3.1 (Fisherman's Hat, Fire Temple Gorons, Shadow bunny statue trick, Nayru's Love uses), which
// shifts every later value in the index-based arrays.
// ---------------------------------------------------------------------------------------------------------------

// Enum sizes this step was written against. If any of these change, the inserted-index lists below need to be
// revisited and a follow-up step is required for saves written by the current build.
static_assert(RSK_MAX == 311, "RandomizerSettingKey changed, add a new save migration step");
static_assert(RC_MAX == 3484, "RandomizerCheck changed, add a new save migration step");
static_assert(RAND_INF_MAX == 3021, "RandomizerInf changed, add a new save migration step");
static_assert(RT_MAX == 275, "RandomizerTrick changed, add a new save migration step");
static_assert(RHT_MAX == 1672, "RandomizerHintTextKey changed, add a new save migration step");

const IndexRemap kDeweySettingsRemap = {
    { RSK_SHUFFLE_FISHERMANS_HAT, RSK_SHUFFLE_FIRE_GORONS, RSK_NAYRUS_LOVE_USES },
    RSK_MAX,
};

const IndexRemap kDeweyCheckRemap = {
    Concat({ RC_FISHERMANS_HAT },
           Range(RC_FIRE_TEMPLE_NEAR_BOSS_GORON, RC_FIRE_TEMPLE_MQ_LIZALFOS_MAZE_SIDE_ROOM_GORON)),
    RC_MAX,
};

const IndexRemap kDeweyInfRemap = {
    Concat({ RAND_INF_FISHERMANS_HAT },
           Range(RAND_INF_FIRE_TEMPLE_NEAR_BOSS_GORON, RAND_INF_FIRE_TEMPLE_MQ_LIZALFOS_MAZE_SIDE_ROOM_GORON)),
    RAND_INF_MAX,
};

const IndexRemap kDeweyTrickRemap = {
    { RT_SHADOW_BUNNY_STATUE },
    RT_MAX,
};

const IndexRemap kDeweyHintTextRemap = {
    Concat({ RHT_FISHERMANS_HAT },
           Range(RHT_FIRE_TEMPLE_NEAR_BOSS_GORON, RHT_FIRE_TEMPLE_MQ_LIZALFOS_MAZE_SIDE_ROOM_GORON)),
    RHT_MAX,
};

bool DeweyApplies(nlohmann::json& saveBlock) {
    nlohmann::json* rando = GetSectionData(saveBlock, "randomizer");
    nlohmann::json* base = GetSectionData(saveBlock, "base");
    if (rando == nullptr || base == nullptr) {
        return false;
    }
    const nlohmann::json& settings = (*rando)["randoSettings"];
    const nlohmann::json& locations = (*rando)["itemLocations"];
    const nlohmann::json& tricks = (*rando)["trickOptions"];
    return settings.is_array() && settings.size() == kDeweySettingsRemap.OldSize() && locations.is_array() &&
           locations.size() == kDeweyCheckRemap.OldSize() && tricks.is_array() &&
           tricks.size() == kDeweyTrickRemap.OldSize();
}

void DeweyApply(nlohmann::json& saveBlock) {
    nlohmann::json& rando = *GetSectionData(saveBlock, "randomizer");
    nlohmann::json& base = *GetSectionData(saveBlock, "base");

    rando["randoSettings"] = kDeweySettingsRemap.RemapArray(rando["randoSettings"], 0);
    rando["itemLocations"] = kDeweyCheckRemap.RemapArray(rando["itemLocations"], nlohmann::json{ { "rgID", 0 } });
    rando["trickOptions"] = kDeweyTrickRemap.RemapArray(rando["trickOptions"], 0);
    base["randomizerInf"] = kDeweyInfRemap.RemapBits(base["randomizerInf"]);

    if (rando.contains("hintLocations") && rando["hintLocations"].is_array()) {
        for (nlohmann::json& hint : rando["hintLocations"]) {
            if (!hint.is_object()) {
                continue;
            }
            if (hint.contains("hintKeys") && hint["hintKeys"].is_array()) {
                for (nlohmann::json& key : hint["hintKeys"]) {
                    key = kDeweyHintTextRemap.ToNew(key.get<size_t>());
                }
            } else if (hint.contains("hintKey") && hint["hintKey"].is_number()) {
                hint["hintKey"] = kDeweyHintTextRemap.ToNew(hint["hintKey"].get<size_t>());
            }
        }
    }

    nlohmann::json* tracker = GetSectionData(saveBlock, "trackerData");
    if (tracker != nullptr && tracker->contains("checkStatus") && (*tracker)["checkStatus"].is_array()) {
        for (nlohmann::json& entry : (*tracker)["checkStatus"]) {
            if (entry.is_object() && entry.contains("randomizerCheck") && entry["randomizerCheck"].is_number()) {
                entry["randomizerCheck"] = kDeweyCheckRemap.ToNew(entry["randomizerCheck"].get<size_t>());
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Step registry
// ---------------------------------------------------------------------------------------------------------------

struct Step {
    const char* name;
    std::function<bool(nlohmann::json&)> applies;
    std::function<void(nlohmann::json&)> apply;
};

const std::vector<Step> kSteps = {
    { "DeweyBravoEnumLayout", DeweyApplies, DeweyApply },
};

// Source versions this build knows how to migrate from. Only major/minor are compared.
bool IsMigratableSource(const Version& from) {
    return from.major == 9 && from.minor == 3;
}

Version ReadVersion(const nlohmann::json& stats) {
    return Version{ stats.value("buildVersionMajor", 0), stats.value("buildVersionMinor", 0),
                    stats.value("buildVersionPatch", 0) };
}

} // namespace

std::string Version::ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

Version CurrentVersion() {
    return Version{ gBuildVersionMajor, gBuildVersionMinor, gBuildVersionPatch };
}

Result Migrate(nlohmann::json& saveBlock) {
    Result result;

    if (saveBlock.value("fileType", (int)FILE_TYPE_SAVE_VANILLA) != FILE_TYPE_SAVE_RANDO) {
        return result;
    }
    nlohmann::json* stats = GetSectionData(saveBlock, "sohStats");
    if (stats == nullptr) {
        return result;
    }

    result.from = ReadVersion(*stats);
    result.fromName = stats->value("buildVersion", result.from.ToString());

    if (result.from == CurrentVersion()) {
        return result;
    }
    if (!IsMigratableSource(result.from)) {
        result.outcome = Outcome::Unsupported;
        return result;
    }

    for (const Step& step : kSteps) {
        if (step.applies(saveBlock)) {
            SPDLOG_INFO("Save migration: applying step {} to save from {}", step.name, result.fromName);
            step.apply(saveBlock);
            result.steps.push_back(step.name);
        }
    }

    (*stats)["buildVersion"] = std::string((const char*)gBuildVersion);
    (*stats)["buildVersionMajor"] = gBuildVersionMajor;
    (*stats)["buildVersionMinor"] = gBuildVersionMinor;
    (*stats)["buildVersionPatch"] = gBuildVersionPatch;

    result.outcome = Outcome::Migrated;
    SPDLOG_INFO("Save migration: {} -> {} ({} step(s))", result.fromName, (const char*)gBuildVersion,
                result.steps.size());
    return result;
}

} // namespace SaveMigration
