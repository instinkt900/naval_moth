#include "game/sensor_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

namespace naval {
    void UpdateSensors(entt::registry& registry, float /*dt*/) {
        auto observers = registry.view<Physics, Combatant, Sensors, ContactPicture>();
        for (auto self : observers) {
            b2Vec2 const selfPos = observers.get<Physics>(self).body->GetPosition();
            Faction const faction = observers.get<Combatant>(self).faction;
            Sensors const& sensors = observers.get<Sensors>(self);

            // Rebuilt from scratch each tick: a contact is held at whatever rung it
            // is in reach of *now*, with no history to keep. Contact decay —
            // carrying a lost track on from its last-known position — is what turns
            // this into an update-in-place rather than a rebuild, and arrives with
            // the step that adds it.
            auto& picture = observers.get<ContactPicture>(self).contacts;
            picture.clear();

            for (auto other : registry.view<Physics, Combatant>()) {
                if (other == self || registry.get<Combatant>(other).faction == faction) {
                    continue;
                }
                float const d = (registry.get<Physics>(other).body->GetPosition() - selfPos).Length();
                // Take the best rung the range earns: seen outright inside visual
                // range, else a ranged blip if the active radar is up and reaches
                // it. Beyond both, the contact is not held at all.
                if (d <= sensors.visualRangeM) {
                    picture.emplace(other, Contact{ DetectLevel::Visual });
                } else if (sensors.activeOn && d <= sensors.activeRangeM) {
                    picture.emplace(other, Contact{ DetectLevel::Ranged });
                }
            }
        }
    }
}
