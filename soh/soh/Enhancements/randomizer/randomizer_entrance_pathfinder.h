#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "randomizerEnums.h"

// Route finding for the entrance tracker ("navigation").
//
// The search runs on the randomizer's logic graph (areaTable), so a route only uses transitions that are possible with
// the current inventory, age and time of day. It never spoils: a shuffled entrance is only used once the player has
// discovered it, no matter whether logic is ignored or not.
namespace EntrancePathfinder {

typedef enum {
    ROUTE_STEP_ENTRANCE,       // Loading zone that has an entrance index
    ROUTE_STEP_PASSAGE,        // Fixed scene change that the randomizer never shuffles
    ROUTE_STEP_WARP_SONG,      // Play a warp song
    ROUTE_STEP_SAVEWARP,       // Save, quit and reload the file
    ROUTE_STEP_WAIT_FOR_DAY,   // Let time pass until day
    ROUTE_STEP_WAIT_FOR_NIGHT, // Let time pass until night
    ROUTE_STEP_BECOME_ADULT,   // Travel through time at the Master Sword pedestal
    ROUTE_STEP_BECOME_CHILD,
} RouteStepType;

typedef struct {
    RouteStepType type;
    int16_t entranceIndex; // Entrance index of the transition, -1 if the step has none
    std::string text;      // Description to show to the player
} RouteStep;

typedef enum {
    ROUTE_NONE,           // Nothing was calculated (no destination, or the logic graph is not available)
    ROUTE_FOUND,          // steps holds the way to the destination
    ROUTE_AT_DESTINATION, // Link already is at the destination
    ROUTE_NO_KNOWN_ROUTE, // No way over discovered entrances. Says nothing about whether one exists at all.
    ROUTE_UNKNOWN_START,  // Link's current location could not be matched to a region
} RouteStatus;

typedef struct {
    RouteStatus status = ROUTE_NONE;
    std::vector<RouteStep> steps;
} Route;

// A destination the player can pick. This is not strictly a scene: every grotto uses SCENE_GROTTOS, both bazaars use
// SCENE_BAZAAR and so on, so a place is a group of connected regions that share a scene.
typedef struct {
    std::string name;
    std::string sceneName; // Name of the scene the place belongs to, used as an additional search term
    bool visited;          // Only visited places may be offered as a destination
} Place;

// Finds the region Link spawns in when a scene is entered through the given (real, already overridden) entrance index.
// Entrance indices that are not mapped themselves fall back to the previous mapped index of the same scene,
// e.g. ENTR_DEKU_TREE_0_1 is not mapped, but ENTR_DEKU_TREE_ENTRANCE is. Returns RR_NONE if there is no such entrance.
RandomizerRegion GetRegionFromEntranceIndex(int16_t entranceIndex);

// The region Link is in, as far as that is known: the region he last spawned in, or the grotto he is inside of.
RandomizerRegion GetCurrentRegion();
RandoAgeTime GetCurrentAgeTime();

// All places of the loaded seed, the index of a place is its id. Empty while the logic graph is not available.
const std::vector<Place>& GetPlaces();
// Refreshes Place::visited from the discovered entrances and Link's current location
void UpdateVisitedPlaces();
// Id of the place Link is in, -1 if unknown
int32_t GetCurrentPlace();

Route FindRoute(int32_t targetPlace, bool ignoreLogic);

// Drops everything that was derived from the loaded seed
void Reset();

} // namespace EntrancePathfinder
