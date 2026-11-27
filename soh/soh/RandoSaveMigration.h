#pragma once

#include <nlohmann/json.hpp>

namespace RandoSaveMigration {

// Randomizer saves store settings, item locations, flags, tricks and hint keys as arrays indexed by enum value.
// Builds after 9.3.1 (Dewey Bravo) inserted new entries in the middle of several of those enums, which shifts
// every later value when such a save is loaded by a newer build.
//
// Detects a save written by 9.3.1 and rewrites the affected arrays in place to the current enum layout.
// Returns true if the save block was modified and should be written back to disk.
bool MigrateDeweyBravoLayout(nlohmann::json& saveBlock);

} // namespace RandoSaveMigration
