#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Targeting and firing. For every armed ship, each weapon looks for a hull
    // of an opposing faction inside its arc and range; if one is there and the
    // weapon is off cooldown, it spawns a projectile toward it. Also advances
    // each weapon's cooldown and records whether it currently has a target (for
    // rendering).
    // `dt` is the tick length in seconds.
    void UpdateWeapons(entt::registry& registry, float dt);

    // Advances projectiles in a straight line and destroys those that have
    // travelled their full range.
    void UpdateProjectiles(entt::registry& registry, float dt);
}
