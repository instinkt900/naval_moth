#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Sensor system. Each ship carrying a ContactPicture updates it here: it scans
    // every hull of an opposing faction and records the ones its own senses reach,
    // at the rung the detection ladder has earned on each — seen (Visual), fixed by
    // active radar (Ranged), or heard on a bearing (Bearing).
    //
    // The picture is updated in place, not rebuilt: a positioned contact (Ranged or
    // Visual) that drops out of reach is not erased but carried on from its
    // last-known position, its staleness climbing until it fully decays at
    // kContactDecayS and is forgotten (PLAN's contact decay). A bearing carries no
    // position, so a lost one falls off at once.
    //
    // Only ships with both Sensors and ContactPicture are processed, so while the
    // player alone carries a picture the enemy stays omniscient (its aggro system
    // scans hulls directly, bypassing any picture). Reads Physics + Combatant +
    // Sensors and the world's hulls; mutates each observer's ContactPicture, ageing
    // its tracks by dt.
    //
    // Call once per fixed tick, before anything that reads the picture — the
    // renderer and the target-picking both do, so the picture must be current
    // before either runs.
    void UpdateSensors(entt::registry& registry, float dt);
}
