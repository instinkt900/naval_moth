#include "game/propulsion_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>

namespace naval {
    namespace {
        constexpr float kLateralGrip = 0.4f;    // 0 = slides like ice, 1 = no sideways slip
        constexpr float kTurnGain = 4.0f;       // how sharply heading error becomes yaw
        constexpr float kArrivalRadiusM = 20.0f; // stop powering once this close
    }

    void UpdatePropulsion(entt::registry& registry, float dt) {
        auto view = registry.view<Physics, Propulsion, MoveTarget, Helm>();
        for (auto entity : view) {
            b2Body* body = view.get<Physics>(entity).body;
            auto const& propulsion = view.get<Propulsion>(entity);
            auto& target = view.get<MoveTarget>(entity);
            auto& helm = view.get<Helm>(entity);

            // Naval feel: bleed off sideways velocity each tick so the hull
            // tracks along its keel instead of drifting like a puck.
            b2Vec2 const right = body->GetWorldVector(b2Vec2{ 0.0f, 1.0f });
            float const lateral = b2Dot(body->GetLinearVelocity(), right);
            body->ApplyLinearImpulseToCenter((body->GetMass() * -lateral * kLateralGrip) * right, true);

            // The rudder always eases toward its commanded angle at the hull's
            // slew rate — however the command was set — so ordering it hard over
            // swings the blade across rather than snapping it.
            float const rudderStep = propulsion.rudderRate * dt;
            helm.rudder += std::clamp(helm.rudderCmd - helm.rudder, -rudderStep, rudderStep);

            // Without a waypoint the ship answers the manual helm: signed
            // throttle drives fore and aft, and the rudder yaws the hull with
            // authority that grows with steerage way — the same speed-scaled
            // model the autopilot turn below uses.
            if (!target.active) {
                b2Vec2 const forward = body->GetWorldVector(b2Vec2{ 1.0f, 0.0f });
                float const speed = std::abs(b2Dot(body->GetLinearVelocity(), forward));
                float const authority = propulsion.rudderSpeed > 0.0f
                                            ? std::clamp(speed / propulsion.rudderSpeed, 0.0f, 1.0f)
                                            : 1.0f;
                float const maxYaw = propulsion.minTurnRate +
                                     ((propulsion.turnRate - propulsion.minTurnRate) * authority);
                body->SetAngularVelocity(helm.rudder * maxYaw);
                body->ApplyForceToCenter((propulsion.maxThrust * helm.throttle) * forward, true);
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

            // Speed buys turn rate, but only up to rudderSpeed — past that the
            // rudder is already saturated (see `rudder` above) and extra way just
            // adds momentum to fight while coming about. So below rudderSpeed the
            // ship drives freely to gain steerage way; once it has way on, thrust
            // is gated on how well the bow points at the target. A big engine can
            // no longer out-run the rudder and blast off before it has turned: it
            // reaches rudderSpeed, coasts there while it swings the bow around,
            // then opens up to full power as it lines up.
            float const alignment = std::clamp(b2Dot(forward, direction), 0.0f, 1.0f);
            float const thrustGate = alignment + ((1.0f - alignment) * (1.0f - rudder));
            body->ApplyForceToCenter((propulsion.maxThrust * throttle * thrustGate) * forward, true);
        }
    }
}
