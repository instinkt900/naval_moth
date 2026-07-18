#include "game/sensor_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>

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
                b2Vec2 const delta = registry.get<Physics>(other).body->GetPosition() - selfPos;
                float const d = delta.Length();
                // Take the best rung the range earns: seen outright inside visual
                // range; else a ranged blip if the active radar is up and reaches
                // it; else a passive bearing if the contact is itself radiating and
                // within ESM reach. Beyond all three, it is not held at all.
                if (d <= sensors.visualRangeM) {
                    picture.emplace(other, Contact{ DetectLevel::Visual });
                } else if (sensors.activeOn && d <= sensors.activeRangeM) {
                    picture.emplace(other, Contact{ DetectLevel::Ranged });
                } else {
                    // Passive depends on the *other* ship emitting, not on own
                    // radar — this is how a dark ship still gets a cut on a
                    // careless one. Only a bearing (and a signal strength) is kept;
                    // the range is thrown away deliberately (see Contact::bearing).
                    auto const* otherSensors = registry.try_get<Sensors>(other);
                    if (otherSensors != nullptr && otherSensors->activeOn) {
                        // Received signal ~ emitter power / distance^2, with power ~
                        // the emitter's radar reach squared. The hearing floor is
                        // where an emitter as loud as own radar sits at own passive
                        // range, so passiveRangeM stays the anchor: the detection
                        // range for this emitter scales with how much louder (bigger
                        // radar) it is than own set. A dead-in-the-water listener
                        // with no radar of its own would divide by zero, but every
                        // hull's activeRangeM is required positive.
                        float const detectRangeM =
                            sensors.passiveRangeM * (otherSensors->activeRangeM / sensors.activeRangeM);
                        if (d <= detectRangeM) {
                            float const bearing = std::atan2(delta.y, delta.x);
                            // q = detectRange / d, the received signal relative to
                            // the floor: 1 at the edge of hearing, larger as it
                            // strengthens. Normalised to [0,1] against a "full
                            // strength" multiple. It reads off the received signal
                            // alone, so a big emitter far off and a small one near
                            // resolve to the same strength — big or near, not which.
                            constexpr float kFullStrengthQ = 5.0f;
                            float const q = detectRangeM / d;
                            float const strength =
                                std::clamp((q - 1.0f) / (kFullStrengthQ - 1.0f), 0.0f, 1.0f);
                            picture.emplace(other, Contact{ DetectLevel::Bearing, bearing, strength });
                        }
                    }
                }
            }
        }
    }
}
