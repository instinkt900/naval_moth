#include "game/sensor_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

namespace naval {
    void UpdateSensors(entt::registry& registry, float /*dt*/) {
        auto observers = registry.view<Physics, Combatant, Sensors, ContactPicture>();
        for (auto self : observers) {
            b2Vec2 const selfPos = observers.get<Physics>(self).body->GetPosition();
            Faction const faction = observers.get<Combatant>(self).faction;
            float const visualM = observers.get<Sensors>(self).visualRangeM;

            // Rebuilt from scratch each tick: with only the Visual rung, a contact
            // is either in reach now or it is not, and there is no history to keep.
            // Contact decay — carrying a lost track on from its last-known position
            // — is what turns this into an update-in-place rather than a rebuild,
            // and arrives with the step that adds it.
            auto& picture = observers.get<ContactPicture>(self).contacts;
            picture.clear();

            for (auto other : registry.view<Physics, Combatant>()) {
                if (other == self || registry.get<Combatant>(other).faction == faction) {
                    continue;
                }
                float const d = (registry.get<Physics>(other).body->GetPosition() - selfPos).Length();
                if (d <= visualM) {
                    picture.emplace(other, Contact{ DetectLevel::Visual });
                }
            }
        }
    }
}
