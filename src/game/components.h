#pragma once

#include <box2d/box2d.h>
#include <moth_ui/utils/color.h>

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
}
