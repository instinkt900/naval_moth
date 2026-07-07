#include "game/combat_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace naval {
    namespace {
        // Wrap an angle into [-pi, pi].
        float NormalizeAngle(float a) {
            while (a > b2_pi) {
                a -= 2.0f * b2_pi;
            }
            while (a < -b2_pi) {
                a += 2.0f * b2_pi;
            }
            return a;
        }

        // A signed fraction in [-1, 1], used to pick an aim point within the
        // span of a target's hull half-extents.
        float RandomUnitSpan() {
            static std::mt19937 rng{ std::random_device{}() };
            static std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
            return dist(rng);
        }

        // The nearest targetable that sits inside a weapon's arc and range,
        // measured from `origin`, or entt::null if none qualifies.
        entt::entity AcquireTarget(entt::registry& registry, entt::entity shooter,
                                   b2Vec2 origin, float arcCentre, float range, float arcHalfAngle) {
            entt::entity best = entt::null;
            float bestDist = range;
            for (auto target : registry.view<Physics, Renderable, Targetable>()) {
                if (target == shooter) {
                    continue;
                }
                b2Vec2 const toTarget = registry.get<Physics>(target).body->GetPosition() - origin;
                float const dist = toTarget.Length();
                if (dist > bestDist) {
                    continue;
                }
                float const bearing = std::atan2(toTarget.y, toTarget.x);
                if (std::abs(NormalizeAngle(bearing - arcCentre)) > arcHalfAngle) {
                    continue;
                }
                best = target;
                bestDist = dist;
            }
            return best;
        }
    }

    void UpdateWeapons(entt::registry& registry, float dt) {
        // Buffer projectiles and create them after iterating, so we never touch
        // pools while a view over them is live.
        std::vector<Projectile> spawned;

        auto shooters = registry.view<Physics, Armament>();

        for (auto shooter : shooters) {
            b2Body* body = shooters.get<Physics>(shooter).body;
            b2Vec2 const shipPos = body->GetPosition();
            float const shipAngle = body->GetAngle();
            float const cosA = std::cos(shipAngle);
            float const sinA = std::sin(shipAngle);

            for (auto& weapon : shooters.get<Armament>(shooter).weapons) {
                weapon.cooldownRemaining = std::max(0.0f, weapon.cooldownRemaining - dt);
                bool const wasEngaging = weapon.hasTarget;
                weapon.hasTarget = false;

                float const arcCentre = shipAngle + weapon.bearing;

                // The mount's world position: its hull-local offset rotated into
                // the ship's frame. Aiming and firing both originate here.
                b2Vec2 const off = weapon.mountOffset;
                b2Vec2 const mountPos{ shipPos.x + (cosA * off.x) - (sinA * off.y),
                                       shipPos.y + (sinA * off.x) + (cosA * off.y) };

                entt::entity const target =
                    AcquireTarget(registry, shooter, mountPos, arcCentre, weapon.range, weapon.arcHalfAngle);
                if (target == entt::null) {
                    continue;
                }
                weapon.hasTarget = true;

                // On the tick engagement begins, pick a fresh aim point within
                // the target's hull; hold it while the engagement lasts.
                if (!wasEngaging) {
                    weapon.aimOffset = b2Vec2{ RandomUnitSpan(), RandomUnitSpan() };
                }

                if (weapon.cooldownRemaining > 0.0f) {
                    continue;
                }

                // Resolve the aim offset against the target's live transform so
                // the shot tracks a fixed spot on the enemy hull as it moves.
                b2Body* targetBody = registry.get<Physics>(target).body;
                auto const& targetRenderable = registry.get<Renderable>(target);
                b2Vec2 const targetPos = targetBody->GetPosition();
                float const targetAngle = targetBody->GetAngle();
                float const targetCos = std::cos(targetAngle);
                float const targetSin = std::sin(targetAngle);
                b2Vec2 const localAim{ weapon.aimOffset.x * targetRenderable.halfLengthM,
                                       weapon.aimOffset.y * targetRenderable.halfBeamM };
                b2Vec2 const aimPoint{ targetPos.x + (targetCos * localAim.x) - (targetSin * localAim.y),
                                       targetPos.y + (targetSin * localAim.x) + (targetCos * localAim.y) };

                // Fire from the mount position, straight at the aim point.
                b2Vec2 aim = aimPoint - mountPos;
                aim.Normalize();

                Projectile shot;
                shot.position = mountPos;
                shot.velocity = weapon.projectileSpeed * aim;
                shot.remaining = weapon.projectileRange;
                shot.radiusM = weapon.projectileRadiusM;
                shot.color = weapon.projectileColor;
                spawned.push_back(shot);

                weapon.cooldownRemaining = weapon.cooldown;
            }
        }

        for (auto const& shot : spawned) {
            registry.emplace<Projectile>(registry.create(), shot);
        }
    }

    void UpdateProjectiles(entt::registry& registry, float dt) {
        std::vector<entt::entity> expired;
        auto view = registry.view<Projectile>();
        for (auto entity : view) {
            auto& projectile = view.get<Projectile>(entity);
            projectile.position += dt * projectile.velocity;
            projectile.remaining -= dt * projectile.velocity.Length();
            if (projectile.remaining <= 0.0f) {
                expired.push_back(entity);
            }
        }
        for (auto entity : expired) {
            registry.destroy(entity);
        }
    }
}
