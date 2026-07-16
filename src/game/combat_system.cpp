#include "game/combat_system.h"

#include "game/angles.h"
#include "game/audio.h"
#include "game/camera_shake.h"
#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace naval {
    namespace {
        // A uniformly random point within a disc of the given radius centred on
        // the origin. The sqrt on the radius keeps the distribution even across
        // the area rather than clustering toward the centre.
        b2Vec2 RandomInDisc(float radius) {
            static std::mt19937 rng{ std::random_device{}() };
            static std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            float const angle = unit(rng) * 2.0f * b2_pi;
            float const r = radius * std::sqrt(unit(rng));
            return b2Vec2{ r * std::cos(angle), r * std::sin(angle) };
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
            return std::abs(WrapPi(std::atan2(d.y, d.x) - arcCentre)) <= arcHalfAngle;
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

        // The first hull of the projectile's target faction that its circle
        // overlaps this tick, or entt::null if it strikes nothing — a shot only
        // collides with the side it was fired at, never its own.
        entt::entity StruckHull(entt::registry& registry, Projectile const& projectile) {
            for (auto hull : registry.view<Physics, Renderable, Combatant>()) {
                if (registry.get<Combatant>(hull).faction != projectile.target) {
                    continue;
                }
                b2Body* body = registry.get<Physics>(hull).body;
                auto const& renderable = registry.get<Renderable>(hull);
                if (HullOverlapsCircle(body->GetPosition(), body->GetAngle(),
                                       renderable.halfLengthM, renderable.halfBeamM,
                                       projectile.position, projectile.radiusM)) {
                    return hull;
                }
            }
            return entt::null;
        }

        // The nearest hull of `enemyFaction` whose shape overlaps a weapon's arc
        // and range, measured from `origin`, or entt::null if none qualifies.
        // Only free fire reaches this: with the guns tight a weapon is told what
        // to shoot and chooses nothing for itself.
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

        // True if `target` is something `enemyFaction`'s guns may still shoot at:
        // a live hull of that faction that is still in the registry. A destroyed
        // hull loses its Combatant as it begins sinking, so this goes false the
        // instant it dies — which is how a fire order ends itself.
        bool IsEngageable(entt::registry& registry, entt::entity target, Faction enemyFaction) {
            if (target == entt::null || !registry.valid(target)) {
                return false;
            }
            if (!registry.all_of<Physics, Renderable, Combatant>(target)) {
                return false;
            }
            return registry.get<Combatant>(target).faction == enemyFaction;
        }

        // Transition a destroyed hull into its sinking death sequence: retire it
        // from combat so nothing targets it or fires from it, cut its helm so the
        // wreck coasts to a stop under drag, and tag it Sinking for the sinking
        // system and renderer to carry through.
        void BeginSinking(entt::registry& registry, Audio& audio, CameraShake& shake, entt::entity hull) {
            // The hull goes up as it dies. Heard and felt here, at the moment it
            // is destroyed, rather than in the sinking system, which would replay
            // both every tick of the wreck's long slide under.
            b2Vec2 const position = registry.get<Physics>(hull).body->GetPosition();
            if (auto const* sounds = registry.try_get<Sounds>(hull); sounds != nullptr) {
                audio.Play(sounds->explosion, position);
            }
            if (auto const* hullShake = registry.try_get<Shake>(hull); hullShake != nullptr) {
                shake.Add(hullShake->explosionM, position);
            }
            registry.remove<Armament>(hull);
            registry.remove<Combatant>(hull);
            if (auto* helm = registry.try_get<Helm>(hull); helm != nullptr) {
                helm->throttle = 0.0f;
                helm->rudderCmd = 0.0f;
            }
            if (auto* target = registry.try_get<MoveTarget>(hull); target != nullptr) {
                target->active = false;
            }
            registry.emplace<Sinking>(hull);
        }
    }

    entt::entity ContactAt(entt::registry& registry, b2Vec2 point, Faction faction, float pickRadiusM) {
        // Nearest by hull centre among those the pick disc touches, so two
        // overlapping contacts resolve to the one actually clicked on rather
        // than to whichever the view happens to visit first.
        entt::entity best = entt::null;
        float bestDist = std::numeric_limits<float>::max();
        for (auto hull : registry.view<Physics, Renderable, Combatant>()) {
            if (registry.get<Combatant>(hull).faction != faction) {
                continue;
            }
            auto const& renderable = registry.get<Renderable>(hull);
            b2Body* body = registry.get<Physics>(hull).body;
            b2Vec2 const centre = body->GetPosition();
            if (!HullOverlapsCircle(centre, body->GetAngle(),
                                    renderable.halfLengthM, renderable.halfBeamM,
                                    point, pickRadiusM)) {
                continue;
            }
            float const dist = (centre - point).Length();
            if (dist < bestDist) {
                best = hull;
                bestDist = dist;
            }
        }
        return best;
    }

    void UpdateWeapons(entt::registry& registry, Audio& audio, CameraShake& shake, float dt) {
        // Buffer projectiles and create them after iterating, so we never touch
        // pools while a view over them is live.
        std::vector<Projectile> spawned;

        auto shooters = registry.view<Physics, Armament, FireOrder>();

        for (auto shooter : shooters) {
            Faction const enemyFaction = Opposing(registry.get<Combatant>(shooter).faction);
            auto& order = shooters.get<FireOrder>(shooter);

            // The order outlives nothing: once the contact is dead or gone the
            // whole order goes with it, guns included. Done here rather than
            // where a hull dies so there is one place that decides an order is
            // over, however the target left — sunk, or removed outright.
            if (!IsEngageable(registry, order.target, enemyFaction)) {
                order.target = entt::null;
                order.firing = false;
            }

            // Take the salvo latch for the whole ship before laying a gun, so
            // one press is one round from each — clearing it per weapon would
            // let the first gun swallow the order for the rest of the battery.
            bool const salvo = order.salvo;
            order.salvo = false;

            b2Body* body = shooters.get<Physics>(shooter).body;
            float const shipAngle = body->GetAngle();

            for (auto& weapon : shooters.get<Armament>(shooter).weapons) {
                weapon.cooldownRemaining = std::max(0.0f, weapon.cooldownRemaining - dt);

                float const arcCentre = shipAngle + weapon.bearing;

                // The mount's world position: its hull-local offset carried into
                // the ship's frame by the body's own transform. Aiming and firing
                // both originate here.
                b2Vec2 const mountPos = body->GetWorldPoint(weapon.mountOffset);

                // Whether the designated contact bears gets asked whatever the
                // orders are: the target ring and the "guns bear" count both
                // report exactly this, and it is what gives that contact its
                // priority when laying the gun below.
                entt::entity const designated = order.target;
                bool bears = false;
                if (designated != entt::null) {
                    auto const& designatedHull = registry.get<Renderable>(designated);
                    b2Body* designatedBody = registry.get<Physics>(designated).body;
                    bears = SectorOverlapsHull(mountPos, arcCentre, weapon.range, weapon.arcHalfAngle,
                                               designatedBody->GetPosition(), designatedBody->GetAngle(),
                                               designatedHull.halfLengthM, designatedHull.halfBeamM);
                }
                weapon.hasTarget = bears;

                // Lay the gun. With the guns tight the weapon makes no choice of
                // its own: it only asks whether the ship's designated contact is
                // inside its arc and range. So a gun that cannot bear holds, and
                // one the target drifts into picks it up on that tick without
                // being told again. Free fire is the one exception — and even
                // then the designated contact is taken first, so releasing the
                // guns never pulls one off the mark the ship chose.
                entt::entity target = designated;
                if (!bears) {
                    target = order.freeFire ? AcquireTarget(registry, enemyFaction, mountPos, arcCentre,
                                                            weapon.range, weapon.arcHalfAngle)
                                            : entt::null;
                }
                weapon.target = target;
                if (target == entt::null) {
                    continue;
                }
                b2Body* targetBody = registry.get<Physics>(target).body;

                // Aim at the target's centre, led for its motion: aim where the
                // hull will be when the shot lands (first-order intercept — time
                // to the target at the projectile's speed, advanced by the
                // target's velocity), so a hull can't just outrun a shot fired at
                // its present position. Where the shot actually falls is the
                // spread's business alone.
                b2Vec2 const targetPos = targetBody->GetPosition();
                b2Vec2 const targetVel = targetBody->GetLinearVelocity();
                float const flightTime = weapon.muzzleVelocity > 0.0f
                                             ? (targetPos - mountPos).Length() / weapon.muzzleVelocity
                                             : 0.0f;

                // The aim point and its spread disc, refreshed every tick a target
                // is held so the debug draw can preview them even between shots.
                // The spread angle is the opposite corner of a right triangle whose
                // adjacent side is the distance to the aim point, so the disc's
                // radius (= dist * tan(spread)) grows with range.
                weapon.aimWorld = targetPos + (flightTime * targetVel);
                weapon.spreadRadiusM = (weapon.aimWorld - mountPos).Length() * std::tan(weapon.spread);

                // Everything above happens whether or not the ship is shooting,
                // so a held gun still tracks its mark and shows a live aim
                // solution. Only the shot itself waits on the order.
                //
                // A gun switched out of the battery holds regardless of the
                // order — the enable tick is each gun's own veto, checked ahead
                // of the ship's orders below, and it tracks either way.
                if (!weapon.enabled) {
                    continue;
                }
                // Three ways to be allowed it, any one of which is enough: the
                // standing order, this tick's salvo, or weapons free. This is
                // only the trigger — what the gun is laid on was settled above,
                // and free fire is what widened that, not this.
                if (!order.firing && !salvo && !order.freeFire) {
                    continue;
                }
                if (weapon.cooldownRemaining > 0.0f) {
                    continue;
                }

                // Scatter: send the shot to a uniformly random point within the
                // spread disc over the aim point, so each shot varies in both
                // bearing and depth together and the scatter widens with range.
                b2Vec2 const firePoint = weapon.aimWorld + RandomInDisc(weapon.spreadRadiusM);

                // Fire from the mount toward the scattered point, but never past
                // the weapon's arc: clamp the shot's bearing to the arc so a target
                // straddling the edge is still only shot at within it.
                b2Vec2 const toFire = firePoint - mountPos;
                float const aimDelta = WrapPi(std::atan2(toFire.y, toFire.x) - arcCentre);
                float const shotBearing =
                    arcCentre + std::clamp(aimDelta, -weapon.arcHalfAngle, weapon.arcHalfAngle);
                b2Vec2 const aim{ std::cos(shotBearing), std::sin(shotBearing) };

                // Fuze the shot to the scattered point's range so it detonates
                // there — near the target — rather than flying on to max range.
                // Capped at the weapon's reach.
                float const fuzeRange = std::clamp(toFire.Length(), 0.0f, weapon.range);

                Projectile shot;
                shot.position = mountPos;
                shot.velocity = weapon.muzzleVelocity * aim;
                shot.remaining = fuzeRange;
                shot.radiusM = weapon.projectileRadiusM;
                shot.damage = weapon.damage;
                shot.color = weapon.projectileColor;
                shot.target = enemyFaction;
                // The shot carries its arrival sounds with it: by the time it
                // lands the gun that fired it may itself be on the bottom.
                shot.impactSound = weapon.projectileImpactSound;
                shot.splashSound = weapon.projectileSplashSound;
                shot.impactShakeM = weapon.projectileImpactShakeM;
                spawned.push_back(shot);

                // Heard and felt at the mount rather than the ship's centre, so
                // the guns of one hull are each placed where they actually sit.
                audio.Play(weapon.fireSound, mountPos);
                shake.Add(weapon.fireShakeM, mountPos);

                weapon.cooldownRemaining = weapon.cooldown;
            }
        }

        for (auto const& shot : spawned) {
            registry.emplace<Projectile>(registry.create(), shot);
        }
    }

    void UpdateProjectiles(entt::registry& registry, Audio& audio, CameraShake& shake, float dt) {
        std::vector<entt::entity> expired;   // projectiles to remove
        std::vector<entt::entity> destroyed; // hulls whose health reached zero
        std::vector<Splash> splashes;        // splashes for shots that fell short of any hull
        // A hit removes the projectile and subtracts its damage from the hull's
        // health; a hull with no Health is simply not destructible. Removals are
        // deferred until after iterating so we never touch pools (entt views or
        // the Box2D world) while a view over them is live.
        auto view = registry.view<Projectile>();
        for (auto entity : view) {
            auto& projectile = view.get<Projectile>(entity);
            projectile.position += dt * projectile.velocity;
            projectile.remaining -= dt * projectile.velocity.Length();

            // Strike test first: if the shot overlaps a hull this tick it hits,
            // even if its fuze also came due — so a shot fuzed to the target's
            // range damages the target rather than splashing on top of it.
            entt::entity const hull = StruckHull(registry, projectile);
            if (hull != entt::null) {
                expired.push_back(entity);
                audio.Play(projectile.impactSound, projectile.position);
                shake.Add(projectile.impactShakeM, projectile.position);
                // Apply damage only while the hull is still alive, so a hull is
                // queued for destruction exactly once — on the hit that crosses
                // zero — however many shots land the same tick.
                if (auto* health = registry.try_get<Health>(hull); health != nullptr && health->current > 0.0f) {
                    health->current -= projectile.damage;
                    if (health->current <= 0.0f) {
                        destroyed.push_back(hull);
                    }
                }
                continue;
            }

            // Fuze: the shot has run to its set range without striking a hull, so
            // it detonates — leave a splash where it went off, near the target it
            // was aimed at.
            if (projectile.remaining <= 0.0f) {
                expired.push_back(entity);
                splashes.push_back(Splash{ projectile.position, 0.0f, projectile.radiusM });
                audio.Play(projectile.splashSound, projectile.position);
            }
        }
        for (auto entity : expired) {
            registry.destroy(entity);
        }
        for (auto entity : destroyed) {
            BeginSinking(registry, audio, shake, entity);
        }
        for (auto const& splash : splashes) {
            registry.emplace<Splash>(registry.create(), splash);
        }
    }

    void UpdateSinking(entt::registry& registry, float dt) {
        std::vector<entt::entity> gone; // wrecks fully under, to remove
        for (auto entity : registry.view<Sinking>()) {
            auto& sinking = registry.get<Sinking>(entity);
            sinking.age += dt;
            if (sinking.age >= kSinkBurnS + kSinkDurationS) {
                gone.push_back(entity);
            }
        }
        for (auto entity : gone) {
            b2Body* body = registry.get<Physics>(entity).body;
            body->GetWorld()->DestroyBody(body);
            registry.destroy(entity);
        }
    }

    void UpdateSplashes(entt::registry& registry, float dt) {
        std::vector<entt::entity> expired;
        for (auto entity : registry.view<Splash>()) {
            auto& splash = registry.get<Splash>(entity);
            splash.age += dt;
            if (splash.age >= kSplashLifetimeS) {
                expired.push_back(entity);
            }
        }
        for (auto entity : expired) {
            registry.destroy(entity);
        }
    }
}
