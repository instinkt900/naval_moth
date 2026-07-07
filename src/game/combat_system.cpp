#include "game/combat_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
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
    }

    void UpdateWeapons(entt::registry& registry, float dt) {
        // Buffer projectiles and create them after iterating, so we never touch
        // pools while a view over them is live.
        std::vector<Projectile> spawned;

        auto shooters = registry.view<Physics, Armament, Renderable>();
        auto targets = registry.view<Physics, Targetable>();

        for (auto shooter : shooters) {
            b2Body* body = shooters.get<Physics>(shooter).body;
            b2Vec2 const shipPos = body->GetPosition();
            float const shipAngle = body->GetAngle();
            float const halfBeamM = shooters.get<Renderable>(shooter).halfBeamM;

            for (auto& weapon : shooters.get<Armament>(shooter).weapons) {
                weapon.cooldownRemaining = std::max(0.0f, weapon.cooldownRemaining - dt);
                weapon.hasTarget = false;

                float const arcCentre = shipAngle + weapon.bearing;

                // Nearest target inside this weapon's arc and range.
                bool found = false;
                b2Vec2 bestPos{ 0.0f, 0.0f };
                float bestDist = weapon.range;
                for (auto target : targets) {
                    if (target == shooter) {
                        continue;
                    }
                    b2Vec2 const targetPos = targets.get<Physics>(target).body->GetPosition();
                    b2Vec2 const toTarget = targetPos - shipPos;
                    float const dist = toTarget.Length();
                    if (dist > bestDist) {
                        continue;
                    }
                    float const bearing = std::atan2(toTarget.y, toTarget.x);
                    if (std::abs(NormalizeAngle(bearing - arcCentre)) > weapon.arcHalfAngle) {
                        continue;
                    }
                    found = true;
                    bestDist = dist;
                    bestPos = targetPos;
                }

                if (!found) {
                    continue;
                }
                weapon.hasTarget = true;
                if (weapon.cooldownRemaining > 0.0f) {
                    continue;
                }

                // Fire from the mount side of the hull, straight at the target.
                b2Vec2 const beamDir{ std::cos(arcCentre), std::sin(arcCentre) };
                b2Vec2 const muzzle = shipPos + (halfBeamM * beamDir);
                b2Vec2 aim = bestPos - muzzle;
                aim.Normalize();

                Projectile shot;
                shot.position = muzzle;
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
