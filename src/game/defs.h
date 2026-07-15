#pragma once

#include "game/hull_shape.h"

#include <moth_ui/utils/color.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace naval::defs {
    // Read-only content, authored in JSON and loaded once at startup. Entities
    // are spawned from these definitions; their mutable runtime state lives in
    // components, never back here. References between tables are by id and are
    // validated when the database loads, so a bad id fails at startup rather
    // than at spawn time.

    // A straight-shot projectile. Angles are irrelevant; it just travels. How
    // far it flies is governed by the firing weapon's range, not the projectile.
    struct Projectile {
        float speed = 0.0f;   // m/s
        float radiusM = 0.0f; // draw radius, metres
        float damage = 0.0f;  // hit points removed from a hull it strikes
        moth_ui::Color color;
    };

    // A weapon type. Fires its projectile at any targetable inside its arc and
    // range, respecting cooldown. The firing arc's centre bearing comes from the
    // hull mount; arcHalfAngle is the half-width to either side of it.
    struct Weapon {
        std::string projectile;   // id into the projectile table
        float cooldown = 0.0f;    // seconds between shots
        float range = 0.0f;       // metres
        float arcHalfAngle = 0.0f; // radians (half-width; loaded from arcDegrees)
        float spread = 0.0f;      // radians (loaded from spreadDegrees); half-angle of the spread disc over the target
    };

    // A weapon fixed to a hull at a bearing relative to the bow (radians;
    // -pi/2 = port beam, +pi/2 = starboard beam) and a position on the hull
    // (local metres; +forward toward the bow, +lateral toward starboard).
    struct Mount {
        std::string weapon;   // id into the weapon table
        float bearing = 0.0f; // radians (loaded from bearingDegrees)
        float forwardM = 0.0f; // hull-local offset toward the bow
        float lateralM = 0.0f; // hull-local offset toward starboard
    };

    struct Propulsion {
        float maxThrust = 0.0f;
        float maxSpeed = 0.0f;
        float turnRate = 0.0f;
        float powerDistance = 0.0f;
        float rudderRate = 0.0f;
    };

    // A ship class: how it moves, how it draws, and what it carries.
    struct Hull {
        Propulsion propulsion;
        float halfLengthM = 0.0f;
        float halfBeamM = 0.0f;
        float foreShoulder = kHullShoulder;         // fore taper shoulder position, 0-1 factor of the half-length
        float foreShoulderBeam = kHullShoulderBeam; // beam at the fore shoulder, 0-1 factor of the half-beam
        float aftShoulder = kHullShoulder;          // aft taper shoulder position, 0-1 factor of the half-length
        float aftShoulderBeam = kHullShoulderBeam;  // beam at the aft shoulder, 0-1 factor of the half-beam
        float angularDamping = 0.0f;
        float health = 0.0f; // hit points; 0 means the hull is not destructible
        moth_ui::Color color;
        std::vector<Mount> mounts;
    };

    // An opponent: a hull plus (later) a loadout and AI profile.
    struct Enemy {
        std::string hull; // id into the hull table
    };

    // The player's ship: a hull plus (later) a chosen loadout. A single
    // definition rather than a table — there is only ever one player.
    struct Player {
        std::string hull; // id into the hull table
    };

    // Loaded content, keyed by id. Accessors throw if an id is missing; all
    // cross-references are checked at load, so lookups made by the spawn factory
    // are safe.
    class Database {
    public:
        // Loads the four JSON tables from `dir` and validates every reference.
        // Throws std::runtime_error on a missing file, parse error, or dangling
        // reference.
        static Database Load(std::filesystem::path const& dir);

        Hull const& GetHull(std::string const& id) const;
        Weapon const& GetWeapon(std::string const& id) const;
        Projectile const& GetProjectile(std::string const& id) const;
        Enemy const& GetEnemy(std::string const& id) const;
        Player const& GetPlayer() const;

    private:
        std::unordered_map<std::string, Hull> m_hulls;
        std::unordered_map<std::string, Weapon> m_weapons;
        std::unordered_map<std::string, Projectile> m_projectiles;
        std::unordered_map<std::string, Enemy> m_enemies;
        Player m_player;
    };
}
