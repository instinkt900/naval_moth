#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Propulsion system. For every ship with an engine and an active move
    // target, steers and powers it toward that target (momentum-driven), and
    // bleeds off sideways slip so hulls track along their keel. Reads
    // Physics + Propulsion + MoveTarget and mutates the Box2D bodies; clears a
    // target once the ship arrives. Call once per fixed tick, before stepping
    // the world.
    void UpdatePropulsion(entt::registry& registry);
}
