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

    // A gun type. Fires its projectile at any targetable inside its arc and
    // range, respecting cooldown. The firing arc's centre bearing comes from the
    // hull mount; arcHalfAngle is the half-width to either side of it.
    //
    // The JSON authors the arc's *full* width, since that is the thing a reader
    // pictures — a 90 degree gun sweeps 90 degrees of sea, not 180. It is halved
    // on load rather than in the systems because every consumer of it (targeting,
    // aim clamping, aggro's turn cost, the drawn arc) wants the half-width.
    //
    // Speed and damage are the gun's; its projectile only says how the shot looks
    // and sounds. This is the opposite split from a Launcher/Munition, where the
    // munition carries reach and damage — a gun's shell is dumb and interchangeable,
    // a launcher's munition is the whole weapon and the launcher merely throws it.
    struct Gun {
        std::string name;            // display name shown in game; defaults to the id if unspecified
        std::string projectile;      // id into the projectile table (its visuals)
        float muzzleVelocity = 0.0f; // m/s the shot leaves the barrel at
        float damage = 0.0f;         // hit points removed from a hull it strikes
        float cooldown = 0.0f;    // seconds between shots
        float range = 0.0f;       // metres
        float turnRate = 0.0f;    // radians/second the barrel trains at within its arc (loaded from turnRateDegrees); <= 0 trains instantly
        float arcHalfAngle = 0.0f; // radians (half-width; loaded from arcDegrees)
        float spread = 0.0f;      // radians (loaded from spreadDegrees); half-angle of the spread disc over the target
        int barrelCount = 1;      // barrels fired per trigger, each spawning its own projectile; 1 is an ordinary single-barrel gun
        float barrelSeparationM = 0.0f; // metres between adjacent barrels, set abreast and centred on the mount; only meaningful with barrelCount > 1
        // A point-defence gun (a CIWS) answers inbound guided air munitions rather
        // than ships: it ignores the ship's fire order and, while enabled, lays on
        // the nearest incoming missile in its arc and shoots it down. Optional; a
        // gun that omits it is an ordinary anti-ship battery.
        bool pointDefense = false;
        std::string fireSound;    // id into the sound table; empty = silent
        float fireShakeM = 0.0f;  // metres of camera shake at full effect as it fires; 0 = none
    };

    // How a launcher throws its munition, which is the one behaviour that a
    // launcher's own hardware decides — the munition's flight (reach, speed,
    // damage, guidance) all belongs to the Munition it is loaded with.
    enum class LaunchType {
        VLS,      // vertical launch: 360 degrees, no training; the munition leaves at rest and flies itself
        Launcher, // trainable rail: trains within an arc like a gun, then launches the munition along it
    };

    // A launcher type: the hardware that fires a munition, deliberately dumb about
    // what it fires. It carries no reach or damage of its own — those come from
    // the Munition named at the mount — so one launcher can be loaded with any
    // munition without a new definition. arcHalfAngle and turnRate matter only to
    // the trainable Launcher type; a VLS ignores both (it is omnidirectional and
    // never trains).
    struct Launcher {
        std::string name;         // display name shown in game; defaults to the id if unspecified
        LaunchType type = LaunchType::VLS;
        // A launcher fires from a bank of tubes, not on a single cooldown: `tubes`
        // ready rounds launch in quick succession spaced by `launchInterval`, then
        // each spent tube reloads on its own over `reloadTime`, one at a time.
        int tubes = 1;               // number of tubes, each a ready-to-fire munition
        float launchInterval = 0.0f; // seconds enforced between successive launches
        float reloadTime = 0.0f;     // seconds to reload one spent tube
        float arcHalfAngle = 0.0f; // radians (half-width; loaded from arcDegrees); trainable type only
        float turnRate = 0.0f;    // radians/second the rail trains at (loaded from turnRateDegrees); trainable type only
        std::string fireSound;    // id into the sound table; empty = silent
        float fireShakeM = 0.0f;  // metres of camera shake at full effect as it launches; 0 = none
    };

    // Which medium a munition travels through, and the one axis that a missile and
    // a torpedo actually differ on — everything else (homing, acceleration, arming)
    // is shared, so both are one Munition type distinguished only by this. Today it
    // decides whether a spent round splashes the surface (air does, water is already
    // in the sea and just stops); later it will drive what a munition can engage and
    // be engaged by — a torpedo against submarines, a CIWS against inbound missiles.
    enum class Medium {
        Air,   // a missile: flies over anything
        Water, // a torpedo: swims; no surface splash when it expires
    };

    // A munition type: the guided round a launcher fires — a missile or a torpedo,
    // which differ only by medium (above). The opposite of a Projectile in where
    // the weight sits: a munition owns its whole engagement — its reach (both how
    // far the launcher may fire and how far it runs before it self-destructs), its
    // propulsion (it leaves slow and accelerates toward maxSpeed), its damage, and
    // how hard it can manoeuvre (turnRate). Visuals and arrival sounds it carries
    // too, as a projectile does.
    struct Munition {
        std::string name;          // display name; defaults to the id if unspecified
        Medium medium = Medium::Air; // air (missile) or water (torpedo)
        float range = 0.0f;        // metres: launch range, and the run distance before self-destruct
        float minRange = 0.0f;     // metres the warhead must travel to arm; a hit inside this does no damage
        float acceleration = 0.0f; // m/s^2 gained in flight, from rest toward maxSpeed
        float maxSpeed = 0.0f;     // m/s the munition accelerates up to (loaded from topSpeed)
        float initialSpeed = 0.0f; // m/s it leaves a rail/canister at, along the launch bearing before acceleration builds; a VLS ignores it (leaves at rest)
        float damage = 0.0f;       // hit points removed from a hull it strikes
        float turnRate = 0.0f;     // radians/second its heading can steer toward the target (loaded from turnRateDegrees)
        // Warhead health a point-defence gun must whittle down to knock this
        // munition out of the sky. Air munitions only — a torpedo runs below the
        // reach of these guns. 0 (the default) leaves the munition impervious to
        // point defence: a deliberate lever for a round too fast or small to stop.
        float health = 0.0f;
        float radiusM = 0.0f;      // draw radius, metres
        moth_ui::Color color;
        std::string impactSound; // id into the sound table; empty = silent
        std::string splashSound; // id into the sound table; empty = silent
        float impactShakeM = 0.0f; // metres of camera shake at full effect where it strikes a hull; 0 = none
    };

    // Whether a mount holds a gun or a launcher — which weapon table its ids
    // resolve against, and which runtime weapon it spawns.
    enum class MountType {
        Gun,
        Launcher,
    };

    // A weapon fixed to a hull at a bearing relative to the bow (radians;
    // -pi/2 = port beam, +pi/2 = starboard beam) and a position on the hull
    // (local metres; +forward toward the bow, +lateral toward starboard).
    //
    // A gun mount names a gun. A launcher mount names a launcher *and* the munition
    // loaded in it, so the loadout lives here rather than on the launcher — the
    // same launcher hardware carries different munitions on different ships without
    // duplicating its definition.
    struct Mount {
        MountType type = MountType::Gun;
        std::string gun;      // id into the gun table (gun mounts)
        std::string launcher; // id into the launcher table (launcher mounts)
        std::string munition;  // id into the munition table (launcher mounts): the loaded round
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
        std::string name; // display name shown in game; defaults to the id if unspecified
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
        Gun const& GetGun(std::string const& id) const;
        Launcher const& GetLauncher(std::string const& id) const;
        Munition const& GetMunition(std::string const& id) const;
        Projectile const& GetProjectile(std::string const& id) const;
        Enemy const& GetEnemy(std::string const& id) const;
        Player const& GetPlayer() const;

        // The whole sound table. Unlike the others this is exposed wholesale,
        // because Audio loads every sound up front rather than looking one up
        // when it's wanted.
        std::unordered_map<std::string, Sound> const& GetSounds() const { return m_sounds; }

    private:
        std::unordered_map<std::string, Hull> m_hulls;
        std::unordered_map<std::string, Gun> m_guns;
        std::unordered_map<std::string, Launcher> m_launchers;
        std::unordered_map<std::string, Munition> m_munitions;
        std::unordered_map<std::string, Projectile> m_projectiles;
        std::unordered_map<std::string, Enemy> m_enemies;
        std::unordered_map<std::string, Sound> m_sounds;
        Player m_player;
    };
}
