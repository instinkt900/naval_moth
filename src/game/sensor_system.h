#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Live tuning for the sensor picture. A single mutable block behind an accessor
    // (the AggroTuningRef pattern), shared by the sensor system that reads it and
    // the debug panel that edits it, so a change takes effect at runtime.
    struct SensorTuning {
        // Master switch for the positional noise on radar contacts (see
        // Contact::offset). On, a bearing wanders and an active blip drifts,
        // tightening as the contact firms; off, every fix and cut is exact. Flipping
        // it off hard-zeros the offset at once (no ease-out), so it is a clean A/B —
        // and if the noise proves unwanted it can simply be left off. The TMA solver
        // is unaffected either way: it always works the clean bearing.
        bool noiseEnabled = true;
    };

    // The one live sensor tuning block, behind an accessor so the system and the
    // debug checkbox touch the same value.
    SensorTuning& SensorTuningRef();

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
