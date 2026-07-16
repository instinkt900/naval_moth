#pragma once

#include <box2d/box2d.h>
#include <entt/entt.hpp>

#include <string>

#include "game/components.h"

namespace naval::defs {
    class Database;
}

namespace naval {
    class Audio;

    // Spawns entities from database definitions. A ship is built entirely from
    // its hull id — physics body, propulsion, render spec and armament — with no
    // inline constants, so ship classes differ purely by data.
    //
    // `audio` is here to turn the sound ids named in the definitions into
    // handles, once, rather than leaving names to be looked up every time a gun
    // goes off. Its bank must already be loaded.

    // Spawns the hull `hullId` at `position` (world metres), fighting for
    // `faction`, and returns it.
    entt::entity SpawnHull(entt::registry& registry, b2World& world,
                           defs::Database const& db, Audio const& audio,
                           std::string const& hullId, b2Vec2 position, Faction faction);

    // Spawns the enemy `enemyId` (its hull) at `position` on the Enemy faction.
    entt::entity SpawnEnemy(entt::registry& registry, b2World& world,
                            defs::Database const& db, Audio const& audio,
                            std::string const& enemyId, b2Vec2 position);
}
