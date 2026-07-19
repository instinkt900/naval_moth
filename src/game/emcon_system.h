#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Autonomous emissions control for AI ships. A hull that runs its own EMCON —
    // one with an Aggro brain — radiates while it holds any contact at all and goes
    // dark the moment its picture empties. That single rule is what makes the sensor
    // duel two-sided: a dark, silent player draws no radar, so an enemy beyond visual
    // range never fixes it and holds fire; but the instant the player emits (or is
    // seen), the enemies that hear the emission light up to range and shoot it — and
    // by radiating in turn, hand the player a bearing on themselves. A hunter keeps
    // its set on through a contact's decaying ghost, not just the tick it is fresh,
    // so a target that jinks briefly out of a fix does not shake it at once.
    //
    // The player is deliberately excluded (it carries no Aggro): its radar is a
    // manual lever it works itself, the whole point of the EMCON choice. Reads the
    // picture UpdateSensors has just built; the activeOn it sets is read by the next
    // sensor pass, so this must run after UpdateSensors and before it runs again.
    void UpdateEmcon(entt::registry& registry);
}
