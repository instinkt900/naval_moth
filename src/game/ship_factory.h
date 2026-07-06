#pragma once

#include <box2d/box2d.h>
#include <entt/entt.hpp>

#include <string>

namespace naval::defs {
    class Database;
}

namespace naval {
    // Spawns entities from database definitions. A ship is built entirely from
    // its hull id — physics body, propulsion, render spec and armament — with no
    // inline constants, so ship classes differ purely by data.

    // Spawns the hull `hullId` at `position` (world metres) and returns it.
    entt::entity SpawnHull(entt::registry& registry, b2World& world,
                           defs::Database const& db, std::string const& hullId,
                           b2Vec2 position);

    // Spawns the enemy `enemyId` (its hull) at `position` and tags it Targetable.
    entt::entity SpawnEnemy(entt::registry& registry, b2World& world,
                            defs::Database const& db, std::string const& enemyId,
                            b2Vec2 position);
}
