#pragma once

#include <entt/entt.hpp>

namespace naval {
    class Terrain;

    // Wander system. Keeps each idle enemy on a slow patrol: any ship with a
    // Wander component and no active MoveTarget is handed a long random waypoint
    // on open water — kilometres off, so it holds a straight heading for minutes
    // — which the propulsion autopilot then steers toward. A ship only changes
    // course when it arrives, or when a periodic progress check finds it has run
    // out of way against a shore and re-rolls. Reads the terrain to keep
    // waypoints off the land; mutates only MoveTarget and the Wander progress
    // state. Call once per fixed tick, before UpdatePropulsion; dt is that tick
    // in seconds.
    void UpdateWander(entt::registry& registry, Terrain const& terrain, float dt);
}
