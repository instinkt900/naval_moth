#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Live tuning for the aggro behaviour. A single mutable block, shared by the
    // aggro system that reads it, the debug panel that edits it, and the renderer
    // that draws the aggro-range ring from it — so all three stay in step as the
    // values are dialled in at runtime. Ranges are in metres.
    struct AggroTuning {
        float aggroRangeM = 600.0f;      // a foe closer than this wakes a patrolling ship
        float disengageRangeM = 900.0f;  // ...and it breaks off once the foe is farther than this
        float standoffFrac = 0.7f;       // range to hold, as a fraction of the chosen weapon's range
        float approachBandM = 300.0f;    // over this band beyond standoff the ship swings bow-on -> broadside
        float helmGain = 2.0f;           // heading error (rad) -> rudder command
        float throttleGain = 0.005f;     // range error (m) -> throttle (ahead, when beyond standoff)
        float backoffWeight = 0.0f;      // astern scale when inside standoff (0 = never reverse to open range)
        float steerageThrottle = 0.3f;   // minimum ahead bell held while turning, to keep rudder authority
        float steerageErrorRad = 0.25f;  // heading error at which the steerage bell reaches full
        float switchMarginRad = 0.3f;    // a rival arc must beat the current one by this much to steal the helm
        bool showRings = true;           // draw the aggro-range ring around each AI ship
    };

    // The one live tuning block. A mutable static behind an accessor so the
    // system, the debug sliders, and the ring renderer all touch the same values.
    AggroTuning& AggroTuningRef();

    // Aggro system. Each ship carrying an Aggro component looks for the nearest
    // hull of an opposing faction; inside aggro range it locks on and takes the
    // helm (dropping any autopilot waypoint), then steers to bring a weapon to
    // bear — swinging bow-on to close the distance and turning to present its
    // chosen battery as it reaches standoff range. Firing stays with the weapons
    // system. Breaks off past the (larger) disengage range, handing the ship back
    // to the wander patrol. Reads Physics + Combatant + Armament + Aggro and
    // mutates Helm + MoveTarget + the Aggro state. Call once per fixed tick,
    // before UpdateWander; dt is that tick in seconds.
    void UpdateAggro(entt::registry& registry, float dt);
}
