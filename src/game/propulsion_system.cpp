#include "game/propulsion_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>

namespace naval {
    namespace {
        constexpr float kLateralGrip = 0.4f;    // 0 = slides like ice, 1 = no sideways slip
        constexpr float kTurnGain = 4.0f;       // how sharply heading error becomes yaw
        constexpr float kArrivalRadiusM = 0.8f; // stop powering once this close
    }

    void UpdatePropulsion(entt::registry& registry) {
        auto view = registry.view<Physics, Propulsion, MoveTarget>();
        for (auto entity : view) {
            b2Body* body = view.get<Physics>(entity).body;
            auto const& propulsion = view.get<Propulsion>(entity);
            auto& target = view.get<MoveTarget>(entity);

            // Naval feel: bleed off sideways velocity each tick so the hull
            // tracks along its keel instead of drifting like a puck.
            b2Vec2 const right = body->GetWorldVector(b2Vec2{ 0.0f, 1.0f });
            float const lateral = b2Dot(body->GetLinearVelocity(), right);
            body->ApplyLinearImpulseToCenter((body->GetMass() * -lateral * kLateralGrip) * right, true);

            if (!target.active) {
                continue;
            }

            b2Vec2 const toTarget = target.point - body->GetPosition();
            float const dist = toTarget.Length();
            if (dist < kArrivalRadiusM) {
                // Arrived: stop steering and powering, let momentum carry it on.
                target.active = false;
                body->SetAngularVelocity(0.0f);
                continue;
            }

            // Rudder-like turning: yaw authority scales with forward speed, so
            // the boat barely comes around at a standstill and turns faster once
            // it has way on. forward is the body's bow direction.
            b2Vec2 const forward = body->GetWorldVector(b2Vec2{ 1.0f, 0.0f });
            float const speed = std::abs(b2Dot(body->GetLinearVelocity(), forward));
            float const rudder = propulsion.rudderSpeed > 0.0f
                                     ? std::clamp(speed / propulsion.rudderSpeed, 0.0f, 1.0f)
                                     : 1.0f;
            float const effectiveTurnRate = propulsion.minTurnRate +
                                            ((propulsion.turnRate - propulsion.minTurnRate) * rudder);

            // Turn toward the target, capped by the (speed-dependent) turn rate.
            float const desiredHeading = std::atan2(toTarget.y, toTarget.x);
            float headingError = desiredHeading - body->GetAngle();
            while (headingError > b2_pi) {
                headingError -= 2.0f * b2_pi;
            }
            while (headingError < -b2_pi) {
                headingError += 2.0f * b2_pi;
            }
            body->SetAngularVelocity(std::clamp(headingError * kTurnGain, -effectiveTurnRate, effectiveTurnRate));

            // Throttle scales with distance: far = full power, near = gentle.
            b2Vec2 direction = toTarget;
            direction.Normalize();
            float const throttle = std::clamp(dist / propulsion.powerDistance, 0.0f, 1.0f);

            // Far from the target it's worth driving hard even when pointing the
            // wrong way: speed buys turn rate (rudder), so accelerating across or
            // away lets the ship come about faster and carve toward the target.
            // Close in (throttle < 1), gate thrust on how well the bow points at
            // the target so the final approach eases in rather than launching.
            float const alignment = std::clamp(b2Dot(forward, direction), 0.0f, 1.0f);
            float const thrustGate = alignment + ((1.0f - alignment) * throttle);
            body->ApplyForceToCenter((propulsion.maxThrust * throttle * thrustGate) * forward, true);
        }
    }
}
