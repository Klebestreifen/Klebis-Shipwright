#include "RandoSaveMigration.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <spdlog/spdlog.h>

#include "soh/util.h"
#include "soh/Enhancements/randomizer/randomizerTypes.h"

namespace RandoSaveMigration {

namespace {

// Enum sizes this migration was written against. If an upstream merge changes any of these, the inserted-index
// lists below need to be revisited (and a follow-up migration may be required for saves from this build).
static_assert(RSK_MAX == 311, "RandomizerSettingKey changed, revisit Dewey Bravo save migration");
static_assert(RC_MAX == 3484, "RandomizerCheck changed, revisit Dewey Bravo save migration");
static_assert(RAND_INF_MAX == 3021, "RandomizerInf changed, revisit Dewey Bravo save migration");
static_assert(RT_MAX == 275, "RandomizerTrick changed, revisit Dewey Bravo save migration");
static_assert(RHT_MAX == 1672, "RandomizerHintTextKey changed, revisit Dewey Bravo save migration");

// Describes which indices of an enum were inserted after 9.3.1, expressed in terms of the *current* enum values.
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

const IndexRemap kSettingsRemap = {
    { RSK_SHUFFLE_FISHERMANS_HAT, RSK_SHUFFLE_FIRE_GORONS, RSK_NAYRUS_LOVE_USES },
    RSK_MAX,
};

const IndexRemap kCheckRemap = {
    Concat({ RC_FISHERMANS_HAT },
           Range(RC_FIRE_TEMPLE_NEAR_BOSS_GORON, RC_FIRE_TEMPLE_MQ_LIZALFOS_MAZE_SIDE_ROOM_GORON)),
    RC_MAX,
};

const IndexRemap kInfRemap = {
    Concat({ RAND_INF_FISHERMANS_HAT },
           Range(RAND_INF_FIRE_TEMPLE_NEAR_BOSS_GORON, RAND_INF_FIRE_TEMPLE_MQ_LIZALFOS_MAZE_SIDE_ROOM_GORON)),
    RAND_INF_MAX,
};

const IndexRemap kTrickRemap = {
    { RT_SHADOW_BUNNY_STATUE },
    RT_MAX,
};

const IndexRemap kHintTextRemap = {
    Concat({ RHT_FISHERMANS_HAT },
           Range(RHT_FIRE_TEMPLE_NEAR_BOSS_GORON, RHT_FIRE_TEMPLE_MQ_LIZALFOS_MAZE_SIDE_ROOM_GORON)),
    RHT_MAX,
};

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

bool IsDeweyBravoLayout(nlohmann::json& saveBlock) {
    if (saveBlock.value("fileType", (int)FILE_TYPE_SAVE_VANILLA) != FILE_TYPE_SAVE_RANDO) {
        return false;
    }
    nlohmann::json* rando = GetSectionData(saveBlock, "randomizer");
    nlohmann::json* stats = GetSectionData(saveBlock, "sohStats");
    nlohmann::json* base = GetSectionData(saveBlock, "base");
    if (rando == nullptr || stats == nullptr || base == nullptr) {
        return false;
    }
    if (stats->value("buildVersionMajor", 0) != 9 || stats->value("buildVersionMinor", 0) != 3) {
        return false;
    }
    const nlohmann::json& settings = (*rando)["randoSettings"];
    const nlohmann::json& locations = (*rando)["itemLocations"];
    const nlohmann::json& tricks = (*rando)["trickOptions"];
    return settings.is_array() && settings.size() == kSettingsRemap.OldSize() && locations.is_array() &&
           locations.size() == kCheckRemap.OldSize() && tricks.is_array() && tricks.size() == kTrickRemap.OldSize();
}

void RemapRandomizerInf(nlohmann::json& base) {
    nlohmann::json& words = base["randomizerInf"];
    if (!words.is_array()) {
        return;
    }

    const size_t oldBits = kInfRemap.OldSize();
    std::vector<bool> oldFlags(oldBits, false);
    for (size_t bit = 0; bit < oldBits; bit++) {
        size_t word = bit >> 4;
        if (word < words.size()) {
            oldFlags[bit] = (words[word].get<uint32_t>() & (1u << (bit & 0xF))) != 0;
        }
    }

    const size_t newWordCount = (kInfRemap.newSize + 15) / 16;
    std::vector<uint16_t> newWords(newWordCount, 0);
    size_t oldBit = 0;
    for (size_t bit = 0; bit < kInfRemap.newSize; bit++) {
        bool set = false;
        if (!kInfRemap.IsInserted(bit)) {
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
    words = out;
}

void RemapHintKeys(nlohmann::json& rando) {
    if (!rando.contains("hintLocations") || !rando["hintLocations"].is_array()) {
        return;
    }
    for (nlohmann::json& hint : rando["hintLocations"]) {
        if (!hint.is_object()) {
            continue;
        }
        if (hint.contains("hintKeys") && hint["hintKeys"].is_array()) {
            for (nlohmann::json& key : hint["hintKeys"]) {
                key = kHintTextRemap.ToNew(key.get<size_t>());
            }
        } else if (hint.contains("hintKey") && hint["hintKey"].is_number()) {
            hint["hintKey"] = kHintTextRemap.ToNew(hint["hintKey"].get<size_t>());
        }
    }
}

void RemapTrackerData(nlohmann::json& saveBlock) {
    nlohmann::json* tracker = GetSectionData(saveBlock, "trackerData");
    if (tracker == nullptr || !tracker->contains("checkStatus") || !(*tracker)["checkStatus"].is_array()) {
        return;
    }
    for (nlohmann::json& entry : (*tracker)["checkStatus"]) {
        if (entry.is_object() && entry.contains("randomizerCheck") && entry["randomizerCheck"].is_number()) {
            entry["randomizerCheck"] = kCheckRemap.ToNew(entry["randomizerCheck"].get<size_t>());
        }
    }
}

} // namespace

bool MigrateDeweyBravoLayout(nlohmann::json& saveBlock) {
    if (!IsDeweyBravoLayout(saveBlock)) {
        return false;
    }

    SPDLOG_INFO("Migrating randomizer save from 9.3.1 (Dewey Bravo) enum layout");

    nlohmann::json& rando = *GetSectionData(saveBlock, "randomizer");
    nlohmann::json& base = *GetSectionData(saveBlock, "base");

    rando["randoSettings"] = kSettingsRemap.RemapArray(rando["randoSettings"], 0);
    rando["itemLocations"] = kCheckRemap.RemapArray(rando["itemLocations"], nlohmann::json{ { "rgID", 0 } });
    rando["trickOptions"] = kTrickRemap.RemapArray(rando["trickOptions"], 0);
    RemapHintKeys(rando);
    RemapRandomizerInf(base);
    RemapTrackerData(saveBlock);

    return true;
}

} // namespace RandoSaveMigration
