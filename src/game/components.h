#pragma once

#include <box2d/box2d.h>
#include <moth_ui/utils/color.h>

#include <vector>

namespace naval {
    // The entity's Box2D body. The body's transform is the source of truth for
    // position and heading; other systems read it, never a duplicate.
    struct Physics {
        b2Body* body = nullptr;
    };

    // Engine characteristics — how hard a ship can drive and how it turns.
    // Turning is rudder-like: yaw authority scales with forward speed, from a
    // sluggish minTurnRate at a standstill up to turnRate once the ship is
    // making rudderSpeed through the water. The navigation system reads these
    // instead of assuming fixed constants, so different hulls handle
    // differently.
    struct Propulsion {
        float maxThrust = 0.0f;     // full-power forward force (newtons)
        float minTurnRate = 0.0f;   // yaw rate at a standstill (radians / second)
        float turnRate = 0.0f;      // yaw rate at or above rudderSpeed (radians / second)
        float rudderSpeed = 0.0f;   // forward speed (m/s) at which turning saturates
        float powerDistance = 0.0f; // distance (metres) beyond which throttle is full
    };

    // The single commanded destination. Each click replaces it — targets are
    // never queued. Cleared on arrival so the ship coasts to a stop.
    struct MoveTarget {
        b2Vec2 point{ 0.0f, 0.0f }; // world space (metres)
        bool active = false;
    };

    // How to draw the hull. The long axis runs bow-to-stern along local +x
    // (the body's forward direction).
    struct Renderable {
        moth_ui::Color color;
        float halfLengthPx = 0.0f; // bow-stern half extent
        float halfBeamPx = 0.0f;   // port-starboard half extent
    };

    // A single weapon mounted on a ship. Static fields are resolved from the
    // database at spawn; the runtime fields update as it engages. bearing/arc
    // are relative to the bow — the arc's world centre is bodyAngle + bearing.
    struct Weapon {
        float bearing = 0.0f;      // rad, mount direction relative to bow
        float arcHalfAngle = 0.0f; // rad, half-width of the firing arc
        float range = 0.0f;        // m, target acquisition range
        float cooldown = 0.0f;     // s between shots

        // Projectile spec, copied from the database so firing needs no lookup.
        float projectileSpeed = 0.0f;
        float projectileRange = 0.0f;
        float projectileRadiusPx = 0.0f;
        moth_ui::Color projectileColor;

        float cooldownRemaining = 0.0f; // s until it can fire again
        bool hasTarget = false;         // a target sits in arc+range this tick
    };

    // Every weapon a ship carries. Weapons acquire targets and fire
    // independently. A vector (rather than one component per weapon) because an
    // entity holds at most one component of a given type.
    struct Armament {
        std::vector<Weapon> weapons;
    };

    // Tags an entity that weapons may fire at.
    struct Targetable {};

    // A projectile in flight. Straight-shot: constant velocity, expires once it
    // has travelled its range. Kept out of Box2D — it has no collision yet.
    struct Projectile {
        b2Vec2 position{ 0.0f, 0.0f }; // world space (metres)
        b2Vec2 velocity{ 0.0f, 0.0f }; // m/s
        float remaining = 0.0f;        // m of travel left before it expires
        float radiusPx = 0.0f;
        moth_ui::Color color;
    };
}
