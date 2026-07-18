#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Sensor system. Each ship carrying a ContactPicture rebuilds it here: it
    // scans every hull of an opposing faction and records the ones its own senses
    // reach, at the rung the detection ladder has earned on each. Today only the
    // Visual rung exists — a contact within the ship's visualRangeM is held; one
    // beyond it is not held at all — so this is the whole of a ship's knowledge
    // until the radar steps add the active and passive rungs.
    //
    // Only ships with both Sensors and ContactPicture are processed, so while the
    // player alone carries a picture the enemy stays omniscient (its aggro system
    // scans hulls directly, bypassing any picture). Reads Physics + Combatant +
    // Sensors and the world's hulls; mutates each observer's ContactPicture.
    //
    // Call once per fixed tick, before anything that reads the picture — the
    // renderer and the target-picking both do, so the picture must be current
    // before either runs.
    void UpdateSensors(entt::registry& registry, float dt);
}
