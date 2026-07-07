#pragma once

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

    // A straight-shot projectile. Angles are irrelevant; it just travels.
    struct Projectile {
        float speed = 0.0f;    // m/s
        float maxRange = 0.0f; // metres before it expires
        float radiusM = 0.0f; // draw radius, metres
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
    };

    // A weapon fixed to a hull at a bearing relative to the bow (radians;
    // -pi/2 = port beam, +pi/2 = starboard beam).
    struct Mount {
        std::string weapon;  // id into the weapon table
        float bearing = 0.0f; // radians (loaded from bearingDegrees)
    };

    struct Propulsion {
        float maxThrust = 0.0f;
        float minTurnRate = 0.0f;
        float turnRate = 0.0f;
        float rudderSpeed = 0.0f;
        float powerDistance = 0.0f;
    };

    // A ship class: how it moves, how it draws, and what it carries.
    struct Hull {
        Propulsion propulsion;
        float halfLengthM = 0.0f;
        float halfBeamM = 0.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.0f;
        moth_ui::Color color;
        std::vector<Mount> mounts;
    };

    // An opponent: a hull plus (later) a loadout and AI profile.
    struct Enemy {
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

    private:
        std::unordered_map<std::string, Hull> m_hulls;
        std::unordered_map<std::string, Weapon> m_weapons;
        std::unordered_map<std::string, Projectile> m_projectiles;
        std::unordered_map<std::string, Enemy> m_enemies;
    };
}
