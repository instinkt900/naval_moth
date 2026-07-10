#include "game/combat_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <limits>
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

        // True if segments a-b and c-d cross. Parallel/collinear counts as no
        // crossing; the other overlap tests cover those degenerate cases.
        bool SegmentsCross(b2Vec2 a, b2Vec2 b, b2Vec2 c, b2Vec2 d) {
            auto cross = [](b2Vec2 u, b2Vec2 v) { return (u.x * v.y) - (u.y * v.x); };
            b2Vec2 const r = b - a;
            b2Vec2 const s = d - c;
            float const rxs = cross(r, s);
            if (std::abs(rxs) < 1e-8f) {
                return false;
            }
            b2Vec2 const ac = c - a;
            float const t = cross(ac, s) / rxs;
            float const u = cross(ac, r) / rxs;
            return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
        }

        // True if point `p` lies in the sector with apex `origin`, radius `range`,
        // centred on `arcCentre` with half-width `arcHalfAngle`.
        bool PointInSector(b2Vec2 p, b2Vec2 origin, float arcCentre, float range, float arcHalfAngle) {
            b2Vec2 const d = p - origin;
            float const len = d.Length();
            if (len > range) {
                return false;
            }
            if (len < 1e-6f) {
                return true;
            }
            return std::abs(NormalizeAngle(std::atan2(d.y, d.x) - arcCentre)) <= arcHalfAngle;
        }

        // True if the circle (centre `p`, radius `r`) overlaps the oriented hull
        // rectangle (centre `c`, heading `angle`, half-extents).
        bool HullOverlapsCircle(b2Vec2 c, float angle, float halfLength, float halfBeam, b2Vec2 p, float r) {
            float const cosA = std::cos(angle);
            float const sinA = std::sin(angle);
            b2Vec2 const rel = p - c;
            float const localAlong = (rel.x * cosA) + (rel.y * sinA);
            float const localAcross = (-rel.x * sinA) + (rel.y * cosA);
            float const dx = localAlong - std::clamp(localAlong, -halfLength, halfLength);
            float const dy = localAcross - std::clamp(localAcross, -halfBeam, halfBeam);
            return (dx * dx) + (dy * dy) <= r * r;
        }

        // True if the weapon sector overlaps the target's oriented hull rectangle
        // (centre `c`, heading `angle`, half-extents `halfLength`/`halfBeam`) — any
        // overlap, not merely the hull's centre point.
        bool SectorOverlapsHull(b2Vec2 origin, float arcCentre, float range, float arcHalfAngle,
                                b2Vec2 c, float angle, float halfLength, float halfBeam) {
            float const cosA = std::cos(angle);
            float const sinA = std::sin(angle);
            b2Vec2 const along{ cosA, sinA };    // bow-stern axis
            b2Vec2 const across{ -sinA, cosA };  // port-starboard axis
            b2Vec2 const corners[4] = {
                { c.x + (halfLength * along.x) + (halfBeam * across.x), c.y + (halfLength * along.y) + (halfBeam * across.y) },
                { c.x - (halfLength * along.x) + (halfBeam * across.x), c.y - (halfLength * along.y) + (halfBeam * across.y) },
                { c.x - (halfLength * along.x) - (halfBeam * across.x), c.y - (halfLength * along.y) - (halfBeam * across.y) },
                { c.x + (halfLength * along.x) - (halfBeam * across.x), c.y + (halfLength * along.y) - (halfBeam * across.y) },
            };

            // Any hull corner inside the sector.
            for (auto const& corner : corners) {
                if (PointInSector(corner, origin, arcCentre, range, arcHalfAngle)) {
                    return true;
                }
            }

            // The mount lies inside the hull.
            b2Vec2 const rel = origin - c;
            float const localAlong = (rel.x * along.x) + (rel.y * along.y);
            float const localAcross = (rel.x * across.x) + (rel.y * across.y);
            if (std::abs(localAlong) <= halfLength && std::abs(localAcross) <= halfBeam) {
                return true;
            }

            // Either arc edge (a segment from the mount out to `range`) crosses a hull side.
            b2Vec2 const edges[2] = {
                { origin.x + (range * std::cos(arcCentre - arcHalfAngle)), origin.y + (range * std::sin(arcCentre - arcHalfAngle)) },
                { origin.x + (range * std::cos(arcCentre + arcHalfAngle)), origin.y + (range * std::sin(arcCentre + arcHalfAngle)) },
            };
            for (int i = 0; i < 4; ++i) {
                b2Vec2 const p0 = corners[i];
                b2Vec2 const p1 = corners[(i + 1) % 4];
                if (SegmentsCross(origin, edges[0], p0, p1) || SegmentsCross(origin, edges[1], p0, p1)) {
                    return true;
                }
            }

            // The hull point nearest the mount lies in the sector — catches a hull
            // that dips through the far arc between the two edges.
            float const nearAlong = std::clamp(localAlong, -halfLength, halfLength);
            float const nearAcross = std::clamp(localAcross, -halfBeam, halfBeam);
            b2Vec2 const nearest{ c.x + (nearAlong * along.x) + (nearAcross * across.x),
                                  c.y + (nearAlong * along.y) + (nearAcross * across.y) };
            return PointInSector(nearest, origin, arcCentre, range, arcHalfAngle);
        }

        // The faction a ship fires on: anyone not on its own side.
        Faction Opposing(Faction f) {
            return f == Faction::Player ? Faction::Enemy : Faction::Player;
        }

        // The nearest hull of `enemyFaction` whose shape overlaps a weapon's arc
        // and range, measured from `origin`, or entt::null if none qualifies.
        entt::entity AcquireTarget(entt::registry& registry, Faction enemyFaction,
                                   b2Vec2 origin, float arcCentre, float range, float arcHalfAngle) {
            entt::entity best = entt::null;
            float bestDist = std::numeric_limits<float>::max();
            for (auto target : registry.view<Physics, Renderable, Combatant>()) {
                if (registry.get<Combatant>(target).faction != enemyFaction) {
                    continue;
                }
                auto const& renderable = registry.get<Renderable>(target);
                b2Body* body = registry.get<Physics>(target).body;
                b2Vec2 const centre = body->GetPosition();
                if (!SectorOverlapsHull(origin, arcCentre, range, arcHalfAngle,
                                        centre, body->GetAngle(),
                                        renderable.halfLengthM, renderable.halfBeamM)) {
                    continue;
                }
                float const dist = (centre - origin).Length();
                if (dist < bestDist) {
                    best = target;
                    bestDist = dist;
                }
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
            Faction const enemyFaction = Opposing(registry.get<Combatant>(shooter).faction);
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
                    AcquireTarget(registry, enemyFaction, mountPos, arcCentre, weapon.range, weapon.arcHalfAngle);
                weapon.target = target;
                if (target == entt::null) {
                    weapon.fireRequested = false; // nothing to shoot at; drop any pending order
                    continue;
                }
                weapon.hasTarget = true;

                // On the tick engagement begins, pick a fresh aim point within
                // the target's hull; hold it while the engagement lasts.
                if (!wasEngaging) {
                    weapon.aimOffset = b2Vec2{ RandomUnitSpan(), RandomUnitSpan() };
                }

                // Fire when off cooldown and either the weapon engages
                // automatically or the player has clicked Fire. A manual order is
                // one-shot: consumed whether or not it yields a shot this tick.
                bool const wantFire = weapon.autoFire || weapon.fireRequested;
                weapon.fireRequested = false;
                if (!wantFire || weapon.cooldownRemaining > 0.0f) {
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

                // Fire from the mount position toward the aim point, but never
                // past the weapon's arc: clamp the shot's bearing to the arc so a
                // target straddling the edge is still only shot at within it.
                b2Vec2 const toAim = aimPoint - mountPos;
                float const aimDelta = NormalizeAngle(std::atan2(toAim.y, toAim.x) - arcCentre);
                float const shotBearing =
                    arcCentre + std::clamp(aimDelta, -weapon.arcHalfAngle, weapon.arcHalfAngle);
                b2Vec2 const aim{ std::cos(shotBearing), std::sin(shotBearing) };

                Projectile shot;
                shot.position = mountPos;
                shot.velocity = weapon.projectileSpeed * aim;
                shot.remaining = weapon.range;
                shot.radiusM = weapon.projectileRadiusM;
                shot.damage = weapon.projectileDamage;
                shot.color = weapon.projectileColor;
                shot.target = enemyFaction;
                spawned.push_back(shot);

                weapon.cooldownRemaining = weapon.cooldown;
            }
        }

        for (auto const& shot : spawned) {
            registry.emplace<Projectile>(registry.create(), shot);
        }
    }

    void UpdateProjectiles(entt::registry& registry, float dt) {
        std::vector<entt::entity> expired;   // projectiles to remove
        std::vector<entt::entity> destroyed; // hulls whose health reached zero
        // A projectile collides only with hulls of the faction it was fired at,
        // which keeps a shot from striking its own side (the ship that fired it
        // included). A hit removes the projectile and subtracts its damage from
        // the hull's health; a hull with no Health is simply not destructible.
        // Removals are deferred until after iterating so we never touch pools
        // (entt views or the Box2D world) while a view over them is live.
        auto hulls = registry.view<Physics, Renderable, Combatant>();
        auto view = registry.view<Projectile>();
        for (auto entity : view) {
            auto& projectile = view.get<Projectile>(entity);
            projectile.position += dt * projectile.velocity;
            projectile.remaining -= dt * projectile.velocity.Length();
            if (projectile.remaining <= 0.0f) {
                expired.push_back(entity);
                continue;
            }
            for (auto hull : hulls) {
                if (hulls.get<Combatant>(hull).faction != projectile.target) {
                    continue;
                }
                b2Body* body = hulls.get<Physics>(hull).body;
                auto const& renderable = hulls.get<Renderable>(hull);
                if (HullOverlapsCircle(body->GetPosition(), body->GetAngle(),
                                       renderable.halfLengthM, renderable.halfBeamM,
                                       projectile.position, projectile.radiusM)) {
                    expired.push_back(entity);
                    // Apply damage only while the hull is still alive, so a hull
                    // is queued for destruction exactly once — on the hit that
                    // crosses zero — however many shots land the same tick.
                    if (auto* health = registry.try_get<Health>(hull); health != nullptr && health->current > 0.0f) {
                        health->current -= projectile.damage;
                        if (health->current <= 0.0f) {
                            destroyed.push_back(hull);
                        }
                    }
                    break;
                }
            }
        }
        for (auto entity : expired) {
            registry.destroy(entity);
        }
        for (auto entity : destroyed) {
            b2Body* body = registry.get<Physics>(entity).body;
            body->GetWorld()->DestroyBody(body);
            registry.destroy(entity);
        }
    }
}
