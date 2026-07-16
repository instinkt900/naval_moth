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

    // A sound effect. `file` is resolved at load time against the assets root
    // (the data directory's parent), so `"audio/gun.wav"` as authored becomes
    // `assets/audio/gun.wav` here.
    //
    // Volume and pitch variance are properties of the recording rather than of
    // whatever plays it — a gun sample is loud, a splash is not — which is why
    // they live in this table and not on each weapon that names it.
    struct Sound {
        std::filesystem::path file;
        float volume = 1.0f;        // playback gain before distance is applied; 1 = as recorded
        float pitchVariance = 0.0f; // pitch is rolled in [1-v, 1+v] per play; 0 = always as recorded
    };

    // A projectile's visuals — how a shot looks in flight — and the sounds it
    // makes when it arrives. Speed and damage are characteristics of the firing
    // weapon now, not the projectile; a projectile only says how the shot is
    // drawn and heard.
    struct Projectile {
        float radiusM = 0.0f; // draw radius, metres
        moth_ui::Color color;
        std::string impactSound; // id into the sound table; empty = silent
        std::string splashSound; // id into the sound table; empty = silent
        // How hard striking a hull knocks the camera, in metres of shake at full
        // effect (see camera_shake.h); 0 = no shake. Only an impact shakes: a
        // shot falling in the sea is a splash, not a blow, however close it lands.
        float impactShakeM = 0.0f;
    };

    // A weapon type. Fires its projectile at any targetable inside its arc and
    // range, respecting cooldown. The firing arc's centre bearing comes from the
    // hull mount; arcHalfAngle is the half-width to either side of it.
    struct Weapon {
        std::string projectile;      // id into the projectile table (its visuals)
        float muzzleVelocity = 0.0f; // m/s the shot leaves the barrel at
        float damage = 0.0f;         // hit points removed from a hull it strikes
        float cooldown = 0.0f;    // seconds between shots
        float range = 0.0f;       // metres
        float arcHalfAngle = 0.0f; // radians (half-width; loaded from arcDegrees)
        float spread = 0.0f;      // radians (loaded from spreadDegrees); half-angle of the spread disc over the target
        std::string fireSound;    // id into the sound table; empty = silent
        float fireShakeM = 0.0f;  // metres of camera shake at full effect as it fires; 0 = none
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
        std::string explosionSound; // id into the sound table, played as it dies; empty = silent
        float explosionShakeM = 0.0f; // metres of camera shake at full effect as it dies; 0 = none
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
        // Loads the JSON tables from `dir` and validates every reference. Throws
        // std::runtime_error on a missing file, parse error, or dangling
        // reference.
        //
        // A sound reference is validated like any other: naming a sound id that
        // isn't in the table is a typo and fails here. Whether the *file* that
        // sound names exists is not this class's business — it isn't opened
        // until Audio loads the bank, and a missing one is only a warning there.
        static Database Load(std::filesystem::path const& dir);

        Hull const& GetHull(std::string const& id) const;
        Weapon const& GetWeapon(std::string const& id) const;
        Projectile const& GetProjectile(std::string const& id) const;
        Enemy const& GetEnemy(std::string const& id) const;
        Player const& GetPlayer() const;

        // The whole sound table. Unlike the others this is exposed wholesale,
        // because Audio loads every sound up front rather than looking one up
        // when it's wanted.
        std::unordered_map<std::string, Sound> const& GetSounds() const { return m_sounds; }

    private:
        std::unordered_map<std::string, Hull> m_hulls;
        std::unordered_map<std::string, Weapon> m_weapons;
        std::unordered_map<std::string, Projectile> m_projectiles;
        std::unordered_map<std::string, Enemy> m_enemies;
        std::unordered_map<std::string, Sound> m_sounds;
        Player m_player;
    };
}
