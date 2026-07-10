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

        // Shared handling character for every hull. Yaw authority follows a hump
        // in forward speed: near nil dead in the water, peaking at kBestTurnFraction
        // of the hull's top speed, then washing out toward flank — but flat-out
        // still turns better than a standstill. A hull's turnRate is the peak;
        // these scale it. Break them out to data only if a hull needs to differ.
        constexpr float kMinTurnCoef = 0.03f;      // authority dead in the water
        constexpr float kMaxTurnCoef = 0.4f;       // authority at top speed
        constexpr float kBestTurnFraction = 0.5f;  // speed of peak turning, as a fraction of maxSpeed

        // Yaw-authority multiplier for a hull making `speed` through the water,
        // given its top speed. Rises kMinTurnCoef -> 1 up to the best-turn speed,
        // then falls 1 -> kMaxTurnCoef out to maxSpeed. Zero if the hull cannot move.
        float TurnCoef(float speed, float maxSpeed) {
            if (maxSpeed <= 0.0f) {
                return 0.0f;
            }
            float const bestSpeed = kBestTurnFraction * maxSpeed;
            if (speed <= bestSpeed) {
                return kMinTurnCoef + ((1.0f - kMinTurnCoef) * (speed / bestSpeed));
            }
            float const t = std::clamp((speed - bestSpeed) / (maxSpeed - bestSpeed), 0.0f, 1.0f);
            return 1.0f + ((kMaxTurnCoef - 1.0f) * t);
        }
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

            // Form drag grows with the square of speed, so thrust and drag balance
            // at exactly maxSpeed: the hull accelerates and asymptotes to its top
            // speed rather than creeping toward it. dragCoef is picked so full
            // thrust equals full drag right at maxSpeed.
            if (propulsion.maxSpeed > 0.0f) {
                b2Vec2 const velocity = body->GetLinearVelocity();
                float const dragCoef = propulsion.maxThrust / (propulsion.maxSpeed * propulsion.maxSpeed);
                body->ApplyForceToCenter((-dragCoef * velocity.Length()) * velocity, true);
            }

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
                float const maxYaw = propulsion.turnRate * TurnCoef(speed, propulsion.maxSpeed);
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
            float const effectiveTurnRate = propulsion.turnRate * TurnCoef(speed, propulsion.maxSpeed);

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

            // Gate thrust so a big engine can't out-run the rudder. Below the
            // best-turn speed the ship drives freely to gain steerage way; once it
            // has that way on, thrust is gated on how well the bow points at the
            // target — so it coasts at its best-turning speed while it swings the
            // bow around, then opens up to full power as it lines up. steerage is
            // that same best-turn speed the yaw hump peaks at.
            float const bestSpeed = kBestTurnFraction * propulsion.maxSpeed;
            float const steerage = bestSpeed > 0.0f ? std::clamp(speed / bestSpeed, 0.0f, 1.0f) : 1.0f;
            float const alignment = std::clamp(b2Dot(forward, direction), 0.0f, 1.0f);
            float const thrustGate = alignment + ((1.0f - alignment) * (1.0f - steerage));
            body->ApplyForceToCenter((propulsion.maxThrust * throttle * thrustGate) * forward, true);
        }
    }
}
