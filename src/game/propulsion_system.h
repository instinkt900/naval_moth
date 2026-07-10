#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Propulsion system. Bleeds off sideways slip so hulls track along their
    // keel, then steers each ship one of two ways: with an active move target
    // the autopilot powers toward it and clears it on arrival; otherwise the
    // ship answers its manual Helm — signed throttle and a rudder that slews
    // toward its command at the hull's rudderRate. Reads Physics + Propulsion +
    // MoveTarget + Helm and mutates the Box2D bodies. Call once per fixed tick,
    // before stepping the world; dt is that tick in seconds.
    void UpdatePropulsion(entt::registry& registry, float dt);
}
