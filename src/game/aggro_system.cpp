#include "game/aggro_system.h"

#include "game/angles.h"
#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace naval {
    namespace {
        // Nearest living hull of a faction opposing `faction`, or entt::null if
        // there is none. `outDist` is set to its range when one is found.
        entt::entity NearestFoe(entt::registry& registry, entt::entity self,
                                b2Vec2 selfPos, Faction faction, float& outDist) {
            entt::entity nearest = entt::null;
            float nearestDist = std::numeric_limits<float>::max();
            for (auto other : registry.view<Physics, Combatant>()) {
                if (other == self || registry.get<Combatant>(other).faction == faction) {
                    continue;
                }
                if (registry.all_of<Sinking>(other)) {
                    continue;
                }
                float const d = (registry.get<Physics>(other).body->GetPosition() - selfPos).Length();
                if (d < nearestDist) {
                    nearestDist = d;
                    nearest = other;
                }
            }
            outDist = nearestDist;
            return nearest;
        }

        // The armament slot whose arc costs the least turn to bring onto a foe at
        // world bearing `beta` from a hull heading `shipAngle`. A weapon's arc is
        // centred (in world) at shipAngle + bearing, so centring the foe wants
        // shipAngle = beta - bearing; the cost is how far the bow must swing to get
        // there. Sticks with `current` unless a rival beats it by `switchMargin`,
        // so the ship commits to one battery instead of flipping port/starboard as
        // the foe drifts across the bow.
        int ChooseWeapon(std::vector<Weapon> const& weapons, float beta, float shipAngle,
                         int current, float switchMargin) {
            auto turnCost = [&](Weapon const& w) {
                return std::abs(WrapPi((beta - w.bearing) - shipAngle));
            };
            int const count = static_cast<int>(weapons.size());
            int cheapest = 0;
            for (int i = 1; i < count; ++i) {
                if (turnCost(weapons[i]) < turnCost(weapons[cheapest])) {
                    cheapest = i;
                }
            }
            if (current < 0 || current >= count) {
                return cheapest;
            }
            if (cheapest != current &&
                turnCost(weapons[cheapest]) + switchMargin < turnCost(weapons[current])) {
                return cheapest;
            }
            return current;
        }
    }

    AggroTuning& AggroTuningRef() {
        static AggroTuning tuning;
        return tuning;
    }

    void UpdateAggro(entt::registry& registry, float /*dt*/) {
        AggroTuning const& tuning = AggroTuningRef();

        auto view = registry.view<Physics, Combatant, Armament, Aggro, FireOrder, Helm, MoveTarget>();
        for (auto entity : view) {
            auto& aggro = view.get<Aggro>(entity);

            // A sinking wreck answers no helm and fights no one: break it off so
            // the death sequence isn't fighting the aggro steering.
            if (registry.all_of<Sinking>(entity)) {
                aggro.target = entt::null;
                aggro.weaponIndex = -1;
                continue;
            }

            b2Body* body = view.get<Physics>(entity).body;
            b2Vec2 const selfPos = body->GetPosition();
            Faction const faction = view.get<Combatant>(entity).faction;

            float nearestDist = std::numeric_limits<float>::max();
            entt::entity const nearest = NearestFoe(registry, entity, selfPos, faction, nearestDist);

            // Enter/exit engagement with hysteresis: wake inside aggro range,
            // break off past the larger disengage range, otherwise hold on.
            if (aggro.target == entt::null) {
                if (nearest != entt::null && nearestDist <= tuning.aggroRangeM) {
                    aggro.target = nearest;
                }
            } else if (nearest == entt::null || nearestDist > tuning.disengageRangeM) {
                aggro.target = entt::null;
                aggro.weaponIndex = -1;
            } else {
                aggro.target = nearest; // re-lock the nearest each tick while engaged
            }

            // The aggro lock is the firing order: an enemy that has decided to
            // fight shoots whenever a gun bears, with no separate decision to
            // open fire the way the player has one. Issued here — where the
            // decision to engage is actually made — so both sides' guns run off
            // the same order, and the weapons system alone works out which
            // batteries bear and when the order is spent.
            auto& order = view.get<FireOrder>(entity);
            order.target = aggro.target;
            order.firing = aggro.target != entt::null;

            auto const& armament = view.get<Armament>(entity);
            if (aggro.target == entt::null || armament.weapons.empty()) {
                continue; // patrolling (or unarmed); wander + autopilot keep the helm
            }

            // Engaged: take the helm from the autopilot for direct steering.
            view.get<MoveTarget>(entity).active = false;
            auto& helm = view.get<Helm>(entity);

            b2Vec2 const toTarget = registry.get<Physics>(aggro.target).body->GetPosition() - selfPos;
            float const range = toTarget.Length();
            float const beta = std::atan2(toTarget.y, toTarget.x); // world bearing to the foe
            float const shipAngle = body->GetAngle();

            aggro.weaponIndex = ChooseWeapon(armament.weapons, beta, shipAngle,
                                             aggro.weaponIndex, tuning.switchMarginRad);
            Weapon const& weapon = armament.weapons[aggro.weaponIndex];

            // Standoff comes from the chosen weapon's reach, so a long-ranged gun
            // fights from farther out than a short one. It is a preferred distance
            // the ship noses toward, never one it backs away to reach.
            float const standoff = tuning.standoffFrac * weapon.range;

            // Blend bow-on (out of range) into full arc presentation (in range):
            // while the foe is beyond gun range the ship swings bow-on to close
            // the distance fast, then presents its chosen battery the moment the
            // foe is in range. Anchored to gun range, not standoff, so being
            // closer than the preferred standoff never pulls the bow off the arc.
            float const t = std::clamp((range - weapon.range) / tuning.approachBandM, 0.0f, 1.0f);
            float const approachFactor = 1.0f - t;
            float const desiredHeading = beta - (weapon.bearing * approachFactor);
            float const headingError = WrapPi(desiredHeading - shipAngle);
            helm.rudderCmd = std::clamp(headingError * tuning.helmGain, -1.0f, 1.0f);

            // Throttle seeks the standoff, but only ever ahead: once inside gun
            // range the ship holds and works the arc rather than backing off to
            // open the range. backoffWeight scales any astern order (0 = never
            // reverse), so range-keeping can't fight the guns when already in range.
            float const rangeError = range - standoff;
            float const gain = rangeError >= 0.0f ? tuning.throttleGain
                                                  : tuning.throttleGain * tuning.backoffWeight;
            helm.throttle = std::clamp(rangeError * gain, -1.0f, 1.0f);

            // Steerage way: at a standstill the rudder loses authority (see the
            // propulsion TurnCoef hump), so a ship still swinging its bow onto the
            // arc holds a minimum ahead bell rather than coasting to a stall where
            // the target slips out. Scaled by how far off the arc it is, so a
            // lined-up ship can still settle to a stop, and a broadside ship keeps
            // way on to circle its mark instead of parking beam-on and stalling.
            float const turnDemand = std::clamp(std::abs(headingError) / tuning.steerageErrorRad, 0.0f, 1.0f);
            helm.throttle = std::max(helm.throttle, tuning.steerageThrottle * turnDemand);
        }
    }
}
