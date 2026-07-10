#pragma once

#include "game/hull_shape.h"

#include <box2d/box2d.h>
#include <entt/entt.hpp>
#include <moth_ui/utils/color.h>

#include <string>
#include <vector>

namespace naval {
    // The entity's Box2D body. The body's transform is the source of truth for
    // position and heading; other systems read it, never a duplicate.
    struct Physics {
        b2Body* body = nullptr;
    };

    // Engine characteristics — how hard a ship can drive and how it turns. Form
    // drag balances thrust at maxSpeed, so the hull has a definite top speed.
    // Turning is rudder-like: yaw authority follows a hump in forward speed —
    // near nil at a standstill, peaking below top speed, washing out at flank —
    // with turnRate as the peak. The hump's shape is shared handling character
    // living in the propulsion system; per hull only the peak turn rate and the
    // top speed differ, so different hulls still handle differently.
    struct Propulsion {
        float maxThrust = 0.0f;     // full-power forward force (newtons)
        float maxSpeed = 0.0f;      // top speed (m/s); drag balances thrust here
        float turnRate = 0.0f;      // peak yaw rate (radians / second), at the best turning speed
        float powerDistance = 0.0f; // distance (metres) beyond which throttle is full
        float rudderRate = 0.0f;    // how fast the rudder swings toward its command (1/second)
    };

    // The single commanded destination. Each click replaces it — targets are
    // never queued. Cleared on arrival so the ship coasts to a stop. While a
    // target is active the autopilot has the helm; clearing it hands control to
    // the manual Helm inputs below.
    struct MoveTarget {
        b2Vec2 point{ 0.0f, 0.0f }; // world space (metres)
        bool active = false;        // true while a destination is set; false = coast/idle
    };

    // Direct manual control of a ship, an alternative to the waypoint autopilot.
    // Throttle is signed — negative drives astern. The rudder has a commanded
    // angle the helm turns toward and an actual angle that slews to it over time
    // at the hull's rudderRate, so "hard a-starboard" swings the rudder across
    // rather than snapping it. Both are normalised to [-1, 1]; +rudder turns to
    // starboard. Used only while MoveTarget is inactive.
    struct Helm {
        float throttle = 0.0f;  // commanded thrust, [-1, 1]; negative = astern
        float rudderCmd = 0.0f; // commanded rudder, [-1, 1]; + = starboard
        float rudder = 0.0f;    // actual rudder position, slews toward rudderCmd
    };

    // How to draw the hull. The long axis runs bow-to-stern along local +x
    // (the body's forward direction).
    struct Renderable {
        moth_ui::Color color;     // hull fill colour
        float halfLengthM = 0.0f; // bow-stern half extent, metres
        float halfBeamM = 0.0f;   // port-starboard half extent, metres
        float foreShoulder = kHullShoulder;         // fore shoulder position, factor of the half-length
        float foreShoulderBeam = kHullShoulderBeam; // beam at the fore shoulder, factor of the half-beam
        float aftShoulder = kHullShoulder;          // aft shoulder position, factor of the half-length
        float aftShoulderBeam = kHullShoulderBeam;  // beam at the aft shoulder, factor of the half-beam
    };

    // How long a wake mark lingers before it has fully faded. Shared by the wake
    // system (which expires marks past this age) and the renderer (which fades
    // each mark over it), so the two never disagree.
    inline constexpr float kWakeLifetimeS = 40.0f;

    // A fading wake trailing a moving hull. Marks are dropped at the ship's
    // centre as it makes way, spaced by distance so the trail stays even at any
    // speed, and fade out over kWakeLifetimeS. Purely cosmetic: the wake system
    // maintains it, the renderer draws it.
    struct Wake {
        struct Mark {
            b2Vec2 position{ 0.0f, 0.0f }; // world point (m) where the mark was dropped
            float age = 0.0f;              // seconds since it was dropped
        };
        std::vector<Mark> marks;         // oldest first
        b2Vec2 lastDrop{ 0.0f, 0.0f };   // centre position of the last mark, for distance spacing
        bool seeded = false;             // true once lastDrop holds a real position
    };

    // A single weapon mounted on a ship. Static fields are resolved from the
    // database at spawn; the runtime fields update as it engages. bearing/arc
    // are relative to the bow — the arc's world centre is bodyAngle + bearing.
    // mountOffset places the weapon on the hull in local metres (+x toward the
    // bow, +y toward starboard); it is the origin for aiming, firing, and the
    // drawn arc.
    struct Weapon {
        std::string name;                   // weapon def id, for display
        float bearing = 0.0f;               // rad, mount direction relative to bow
        b2Vec2 mountOffset{ 0.0f, 0.0f };   // hull-local mount position (m)
        float arcHalfAngle = 0.0f;          // rad, half-width of the firing arc
        float range = 0.0f;                 // m, engagement range; also how far its shots travel
        float cooldown = 0.0f;              // s between shots

        // Projectile spec, copied from the database so firing needs no lookup.
        float projectileSpeed = 0.0f;   // m/s the shot travels at
        float projectileRadiusM = 0.0f; // shot draw radius, metres
        float projectileDamage = 0.0f;  // hit points removed on impact
        moth_ui::Color projectileColor; // shot draw colour

        float cooldownRemaining = 0.0f; // s until it can fire again
        bool hasTarget = false;         // a target sits in arc+range this tick

        // Player-facing controls, one set per weapon.
        bool showArc = true;        // draw this weapon's firing arc
        bool autoFire = true;       // acquire and fire automatically
        bool fireRequested = false; // a manual fire order, consumed next update

        // The contact this weapon currently has locked, or entt::null. Refreshed
        // every update; read it only after registry.valid, as the target may be
        // destroyed between updates.
        entt::entity target = entt::null;

        // Aim point on the current target, chosen when engagement begins and
        // held while it lasts, so several guns on one target spread their fire.
        // Stored as signed fractions [-1, 1] of the target's half-extents
        // (x fore-aft, y port-starboard) and resolved against the live target.
        b2Vec2 aimOffset{ 0.0f, 0.0f };
    };

    // Every weapon a ship carries. Weapons acquire targets and fire
    // independently. A vector (rather than one component per weapon) because an
    // entity holds at most one component of a given type.
    struct Armament {
        std::vector<Weapon> weapons;
    };

    // Which side a ship fights for. Weapons engage hulls of a different
    // faction; a projectile strikes only hulls of the faction it was fired at.
    enum class Faction {
        Player,
        Enemy,
    };

    // The side an armed entity belongs to. Every ship carries one so weapons
    // can tell friend from foe — any hull may fire on any hull not its own.
    struct Combatant {
        Faction faction = Faction::Player;
    };

    // A human-readable class name (the hull id), for labelling a contact in the
    // controls readout.
    struct Identity {
        std::string name;
    };

    // Hit points. A hull loses `current` when a projectile strikes it and is
    // removed once it reaches zero. Only ships that can be destroyed carry this.
    struct Health {
        float current = 0.0f;
        float max = 0.0f;
    };

    // A destroyed hull's death sequence, shared by the sinking system (which
    // ages the wreck and finally removes it) and the renderer (which greys the
    // hull, then fades it). The wreck burns charred grey for kSinkBurnS, then
    // alpha-fades under the sea over kSinkDurationS; total life is their sum.
    inline constexpr float kSinkBurnS = 5.0f;      // grey/burning before it starts to go down
    inline constexpr float kSinkDurationS = 10.0f; // fade-out once it begins sinking

    // Marks a hull that has been destroyed and is playing out its death
    // sequence. It is no longer a combatant and carries no armament; the sinking
    // system ages it and removes it once it has fully gone under.
    struct Sinking {
        float age = 0.0f; // seconds since it was destroyed
    };

    // A projectile in flight. Straight-shot: constant velocity, expires once it
    // has travelled its range. Kept out of Box2D — it has no collision yet.
    struct Projectile {
        b2Vec2 position{ 0.0f, 0.0f }; // world space (metres)
        b2Vec2 velocity{ 0.0f, 0.0f }; // m/s
        float remaining = 0.0f;        // m of travel left before it expires
        float radiusM = 0.0f;          // draw radius, metres
        float damage = 0.0f;           // hit points removed from the hull it strikes
        moth_ui::Color color;          // draw colour
        Faction target = Faction::Enemy; // the faction this shot may strike
    };

    // How long a splash lingers before it has fully faded. Shared by the splash
    // system (which removes splashes past this age) and the renderer (which
    // expands and fades each over it), so the two never disagree.
    inline constexpr float kSplashLifetimeS = 0.6f;

    // A brief splash where a spent shot fell into the sea — spawned when a
    // projectile travels its full range without striking a hull. It expands and
    // fades over kSplashLifetimeS, then is removed. Its own entity, not attached
    // to anything, since the projectile that spawned it is already gone. Purely
    // cosmetic: the splash system ages it, the renderer draws it.
    struct Splash {
        b2Vec2 position{ 0.0f, 0.0f }; // world point (m) where the shot fell
        float age = 0.0f;              // seconds since it appeared
        float radiusM = 0.0f;          // the shot's radius; the splash grows from it
    };
}
