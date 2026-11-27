#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Migrates randomizer save files written by older builds to the layout expected by the running build.
//
// Randomizer saves store settings, item locations, flags, tricks and hint keys as arrays indexed by enum value and
// carry the build version that wrote them in `sections.sohStats.data.buildVersion*`. Whenever the save layout
// changes between releases, add a migration step in SaveMigration.cpp:
//
//   1. Write an `Applies(saveBlock)` predicate that recognises the old layout (check the stored version and/or the
//      shape of the affected data so the step is idempotent).
//   2. Write an `Apply(saveBlock)` function that rewrites the JSON in place.
//   3. Append the step to `kSteps`. Steps run in order; the framework stamps the current build version afterwards.
//
// Steps must only ever transform *from* an older layout *to* the current one. Saves from unknown or newer versions
// are reported as Unsupported and left untouched.
namespace SaveMigration {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    bool operator==(const Version&) const = default;
    std::string ToString() const;
};

// Version of the running build.
Version CurrentVersion();

enum class Outcome {
    UpToDate,    // Save already matches the running build, nothing changed.
    Migrated,    // Save was rewritten and should be written back to disk.
    Unsupported, // Save comes from a version this build cannot migrate; save left untouched.
};

struct Result {
    Outcome outcome = Outcome::UpToDate;
    Version from;                   // Version recorded in the save before migration.
    std::string fromName;           // Human readable build name recorded in the save, e.g. "Dewey Bravo (9.3.1)".
    std::vector<std::string> steps; // Names of the migration steps that were applied.
};

// Migrates a randomizer save block in place. Non-randomizer saves are reported as UpToDate.
Result Migrate(nlohmann::json& saveBlock);

} // namespace SaveMigration
