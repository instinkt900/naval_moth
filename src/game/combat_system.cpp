#include "game/combat_system.h"

#include "game/angles.h"
#include "game/audio.h"
#include "game/camera_shake.h"
#include "game/components.h"
#include "game/terrain.h"

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

        // A uniform draw in [0, 1), for probabilistic hit rolls (point defence).
        float RandomUnit() {
            static std::mt19937 rng{ std::random_device{}() };
            static std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            return unit(rng);
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

        // Slew a mount's barrel one tick toward the world bearing `aimWorldBearing`,
        // returning true once it has settled on that mark. The lay lives in
        // weapon.aimBearing as a bow-relative angle; the arc spans arcHalfAngle
        // either side of the mount bearing, and `arcCentre` is that mount bearing in
        // world space (shipAngle + bearing).
        //
        // The training runs in the offset from the mount bearing and steps by a
        // plain difference, not the shortest wrapped path. That is what keeps a wide
        // arc's barrel sweeping across its own field, through the centre, rather than
        // taking the short way round through the blind sector behind the mount: a
        // target jumping from one arc edge to the far one on an arc wider than 180
        // degrees would otherwise slew the barrel straight out of the arc. The offset
        // is clamped to the arc every tick, so the lay can never leave it. A turn
        // rate of zero or less trains in a single step.
        bool TrainBarrel(Weapon& weapon, float aimWorldBearing, float arcCentre, float dt) {
            float const desiredOffset =
                std::clamp(WrapPi(aimWorldBearing - arcCentre), -weapon.arcHalfAngle, weapon.arcHalfAngle);
            float const currentOffset = weapon.aimBearing - weapon.bearing;
            float const delta = desiredOffset - currentOffset;
            float const step = weapon.turnRate * dt;
            bool const snap = weapon.turnRate <= 0.0f || std::abs(delta) <= step;
            float offset = currentOffset + (snap ? delta : std::copysign(step, delta));
            offset = std::clamp(offset, -weapon.arcHalfAngle, weapon.arcHalfAngle);
            weapon.aimBearing = weapon.bearing + offset;
            constexpr float kAcquiredToleranceRad = 0.017f; // ~1 degree
            return std::abs(desiredOffset - offset) <= kAcquiredToleranceRad;
        }

        // True if `munition` is still a live inbound air threat a point-defence
        // mount defending `defend` may engage: a valid, guided, airborne projectile
        // aimed at that faction with warhead health left. The shared predicate
        // behind both acquiring a fresh target and holding the current one.
        bool IsInboundMunition(entt::registry& registry, entt::entity munition, Faction defend) {
            if (munition == entt::null || !registry.valid(munition)) {
                return false;
            }
            auto const* p = registry.try_get<Projectile>(munition);
            return p != nullptr && p->guidance == Guidance::Guided && !p->waterborne &&
                   p->target == defend && p->health > 0.0f;
        }

        // The nearest inbound guided *air* munition that a point-defence mount at
        // `origin` can bear on and that no other mount has already taken this tick,
        // or entt::null if none qualifies — a live inbound threat (see
        // IsInboundMunition), absent from `claimed`, whose position falls inside the
        // mount's arc and range. `claimed` is how the battery spreads its fire: each
        // mount adds the missile it locks, so the next mount looks past it to the
        // next threat rather than dogpiling one and leaving others to leak. Only
        // point defence reaches this — the anti-ship path never looks at projectiles
        // as targets.
        entt::entity AcquireMunition(entt::registry& registry, Faction defend, b2Vec2 origin,
                                     float arcCentre, float range, float arcHalfAngle,
                                     std::vector<entt::entity> const& claimed) {
            entt::entity best = entt::null;
            float bestDist = std::numeric_limits<float>::max();
            for (auto entity : registry.view<Projectile>()) {
                if (!IsInboundMunition(registry, entity, defend)) {
                    continue;
                }
                if (std::find(claimed.begin(), claimed.end(), entity) != claimed.end()) {
                    continue;
                }
                auto const& munition = registry.get<Projectile>(entity);
                if (!PointInSector(munition.position, origin, arcCentre, range, arcHalfAngle)) {
                    continue;
                }
                float const dist = (munition.position - origin).Length();
                if (dist < bestDist) {
                    best = entity;
                    bestDist = dist;
                }
            }
            return best;
        }

        // The munition a point-defence mount engages this tick: the one it was
        // already tracking (`held`) if that is still a live threat inside its arc
        // that no earlier mount has taken, otherwise the nearest unclaimed threat it
        // can bear on. Holding the current mark is what commits a mount from the
        // moment it starts slewing — it keeps and re-claims its target across ticks
        // rather than re-picking the nearest each tick and swapping onto another
        // mount's mark as the missiles close.
        entt::entity SelectMunition(entt::registry& registry, entt::entity held, Faction defend,
                                    b2Vec2 origin, float arcCentre, float range, float arcHalfAngle,
                                    std::vector<entt::entity> const& claimed) {
            if (IsInboundMunition(registry, held, defend) &&
                std::find(claimed.begin(), claimed.end(), held) == claimed.end() &&
                PointInSector(registry.get<Projectile>(held).position, origin, arcCentre, range, arcHalfAngle)) {
                return held;
            }
            return AcquireMunition(registry, defend, origin, arcCentre, range, arcHalfAngle, claimed);
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
        // pools while a view over them is live. Point defence adds two more
        // deferred effects: munitions it shot down (destroyed after the loop, as
        // they are unrelated entities the shooter view does not cover) and the
        // splash each throws up as its warhead cooks off.
        std::vector<Projectile> spawned;
        std::vector<entt::entity> downedMunitions;
        std::vector<Splash> splashes;

        // Every inbound munition a point-defence mount has locked this tick, so the
        // batteries spread their fire: each mount looks past what an earlier one
        // already took (see AcquireMunition). One list for the whole pass — a
        // munition is aimed at a single faction, so only that faction's guns ever
        // consider it, and the shared list never crosses the sides.
        std::vector<entt::entity> claimedMunitions;

        auto shooters = registry.view<Physics, Armament, FireOrder>();

        for (auto shooter : shooters) {
            Faction const ownFaction = registry.get<Combatant>(shooter).faction;
            Faction const enemyFaction = Opposing(ownFaction);
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
                // Point defence is a world apart from anti-ship gunnery: it ignores
                // the fire order entirely (no designated contact, no salvo, no free
                // fire), answering only to its own enable tick, and it hunts inbound
                // missiles rather than hulls. So it runs on its own branch here and
                // never falls through to any of the targeting below.
                if (weapon.pointDefense) {
                    weapon.cooldownRemaining = std::max(0.0f, weapon.cooldownRemaining - dt);
                    // hasTarget reports on the *designated ship* contact, which a
                    // CIWS never bears on; keep it false so it neither inflates the
                    // "guns bear" count nor lights the target ring.
                    weapon.hasTarget = false;

                    float const arcCentre = shipAngle + weapon.bearing;
                    b2Vec2 const mountPos = body->GetWorldPoint(weapon.mountOffset);

                    // Switched out, it holds fire and tracks nothing — the enable
                    // tick is its whole say, exactly as it is for a battery gun.
                    // Otherwise it keeps the mark it was already tracking, or takes a
                    // fresh one (see SelectMunition), passing its current target so a
                    // slew in progress is not abandoned.
                    entt::entity const munition =
                        weapon.enabled ? SelectMunition(registry, weapon.target, ownFaction, mountPos,
                                                        arcCentre, weapon.range, weapon.arcHalfAngle,
                                                        claimedMunitions)
                                       : entt::null;
                    weapon.target = munition;
                    if (munition == entt::null) {
                        weapon.acquired = false;
                        continue;
                    }
                    // Take this missile off the table for the rest of the battery,
                    // so the next mount engages the next threat rather than piling
                    // onto this one. Claimed the moment it is locked — while the mount
                    // is still slewing on, not only once it opens fire — since it is
                    // committed to this mark either way.
                    claimedMunitions.push_back(munition);

                    // Train the barrel onto the missile so the drawn lay tracks it.
                    // The hit itself does not depend on the lay (see below), so this
                    // is purely what the arc draw shows; it reuses the gun's own
                    // slew-toward-and-clamp-to-the-arc rule.
                    auto& warhead = registry.get<Projectile>(munition);
                    weapon.aimWorld = warhead.position;
                    weapon.spreadRadiusM = 0.0f;
                    float const aimWorldBearing = std::atan2(warhead.position.y - mountPos.y,
                                                             warhead.position.x - mountPos.x);
                    weapon.acquired = TrainBarrel(weapon, aimWorldBearing, arcCentre, dt);

                    // Fire only once the barrel has trained onto the missile, so
                    // the mount has to slew onto a new mark before it can engage it
                    // — the turn rate is a real acquisition delay, and a crosser fast
                    // enough to out-run the traverse is never settled on and leaks
                    // through. The burst itself is resolved hitscan (a CIWS throws
                    // far too dense a stream to model as separate shells), chipping
                    // the warhead's health directly rather than flying a shot down
                    // the bore; gating it on the lay is what makes the traverse
                    // matter despite the hit not travelling.
                    if (!weapon.acquired || weapon.cooldownRemaining > 0.0f) {
                        continue;
                    }
                    weapon.cooldownRemaining = weapon.cooldown;
                    audio.Play(weapon.fireSound, mountPos);
                    shake.Add(weapon.fireShakeM, mountPos);

                    // Accuracy. Rather than sample a point in the spread disc and
                    // test overlap the way a gun's shell does, the hitscan burst
                    // takes the analytic odds of that same scatter landing on the
                    // missile: at the target's range the spread opens a disc of
                    // radius dist*tan(spread), and the chance a round falls within
                    // the missile's own radius of the aim point is the ratio of the
                    // two — a certainty point-blank (the disc tighter than the
                    // missile) and thinning with range, so a CIWS only truly answers
                    // a missile as it closes. spread 0 is a perfect gun. A miss still
                    // spends the burst — the gun fired — it simply does no damage.
                    //
                    // The health > 0 test both spares a munition authored with none
                    // (impervious by design) and destroys one exactly once, however
                    // many mounts fire on it this tick — once past zero it no longer
                    // qualifies. A downed warhead cooks off where it was hit: its own
                    // impact report and shake, plus a splash, all carried on the
                    // munition so they play whether or not the gun that killed it
                    // survives — the same trade every shot already makes.
                    if (warhead.health > 0.0f) {
                        float const dist = (warhead.position - mountPos).Length();
                        float const discRadius = dist * std::tan(weapon.spread);
                        float const hitChance =
                            discRadius <= warhead.radiusM ? 1.0f : warhead.radiusM / discRadius;
                        if (RandomUnit() < hitChance) {
                            warhead.health -= weapon.damage;
                            if (warhead.health <= 0.0f) {
                                audio.Play(warhead.impactSound, warhead.position);
                                shake.Add(warhead.impactShakeM, warhead.position);
                                splashes.push_back(Splash{ warhead.position, 0.0f, warhead.radiusM });
                                downedMunitions.push_back(munition);
                            }
                        }
                    }
                    continue;
                }

                // Advance the mount's fire timers. A gun counts down one barrel's
                // cooldown; a launcher counts down the interval between tube
                // launches and reloads spent tubes one at a time (reloadTimer is
                // the tube in progress). Both run whether or not the mount has a
                // target, so a battery reloads while idle.
                if (weapon.kind == WeaponKind::Gun) {
                    weapon.cooldownRemaining = std::max(0.0f, weapon.cooldownRemaining - dt);
                } else {
                    weapon.launchTimer = std::max(0.0f, weapon.launchTimer - dt);
                    if (weapon.readyTubes < weapon.tubeCount && weapon.reloadTimer > 0.0f) {
                        weapon.reloadTimer -= dt;
                        if (weapon.reloadTimer <= 0.0f) {
                            ++weapon.readyTubes;
                            // Chain straight into the next tube so a spent battery
                            // refills one by one rather than all at once.
                            weapon.reloadTimer =
                                weapon.readyTubes < weapon.tubeCount ? weapon.reloadTime : 0.0f;
                        }
                    }
                }

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
                    // Nothing to acquire; the barrel holds its lay (see below).
                    weapon.acquired = false;
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

                // Where the mount would point to lay onto the target, in
                // bow-relative bearings so the lay rides the hull's own swing and
                // never leaves the arc.
                float const aimWorldBearing = std::atan2(weapon.aimWorld.y - mountPos.y,
                                                         weapon.aimWorld.x - mountPos.x);

                // A launcher with no turn rate is a fixed tube — a canister bolted
                // at its mount bearing that never slews, leaving the munition to
                // manoeuvre onto the target once it is away (this is how a real
                // canister launcher works). It holds its lay at the mount bearing
                // and counts as acquired at once. A launcher with a turn rate
                // trains its rail like a gun; a gun always trains, and there a zero
                // rate still means an instant traverse rather than a fixed barrel.
                //
                // Training runs before the fire gates below, so a mount merely
                // holding — or switched out — still slews onto its mark; with no
                // target the loop already continued, so it simply holds its last lay
                // rather than recentring. A barrel that cannot keep pace with a fast
                // crosser reads as slewing, which is the truth of it.
                if (weapon.kind != WeaponKind::Gun && weapon.turnRate <= 0.0f) {
                    weapon.aimBearing = weapon.bearing;
                    weapon.acquired = true;
                } else {
                    weapon.acquired = TrainBarrel(weapon, aimWorldBearing, arcCentre, dt);
                }

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
                if (weapon.kind == WeaponKind::Gun) {
                    // The trigger. Standing fire — the fire order or weapons-free —
                    // holds until the barrel has trained onto the mark, so a slewing
                    // gun keeps its rounds rather than throwing them wide of a lead
                    // it has not reached. A salvo is exempt: it is the deliberate
                    // "fire now", and spends its one round whether or not the gun has
                    // settled. What the gun is laid on, and how far it has trained,
                    // were both settled above; this only decides whether to shoot.
                    bool const standingFire = (order.firing || order.freeFire) && weapon.acquired;
                    if (!salvo && !standingFire) {
                        continue;
                    }
                    if (weapon.cooldownRemaining > 0.0f) {
                        continue;
                    }
                } else {
                    // A launcher triggers in whole tubes. A ship-wide salvo queues
                    // this launcher's selected count (never more than are ready);
                    // standing fire ripples tubes out as fast as the interval lets
                    // it. Either way a launch needs a ready tube and the interval
                    // since the last one elapsed — that spacing is what makes a
                    // six-tube salvo six shots in quick succession rather than one
                    // impossible instant. A launched munition homes, so unlike a gun
                    // the launcher does not wait on the mount being 'acquired'.
                    if (salvo) {
                        weapon.pending = std::min(weapon.salvoSize, weapon.readyTubes);
                    }
                    bool const standingFire = order.firing || order.freeFire;
                    if (weapon.pending <= 0 && !standingFire) {
                        continue;
                    }
                    if (weapon.readyTubes <= 0 || weapon.launchTimer > 0.0f) {
                        continue;
                    }
                }

                Projectile shot;
                shot.position = mountPos;
                shot.radiusM = weapon.projectileRadiusM;
                shot.damage = weapon.damage;
                shot.color = weapon.projectileColor;
                shot.target = enemyFaction;
                // The shot carries its arrival sounds with it: by the time it
                // lands the gun that fired it may itself be on the bottom.
                shot.impactSound = weapon.projectileImpactSound;
                shot.splashSound = weapon.projectileSplashSound;
                shot.impactShakeM = weapon.projectileImpactShakeM;

                if (weapon.kind == WeaponKind::Gun) {
                    // The shot leaves along the barrel, not straight at the aim
                    // point: while the gun is still training the two diverge, and
                    // that lag is the whole point of the turn rate. Take where the
                    // barrel points at the aim's range as the shot's centre, then
                    // scatter the spread disc around it — so a laid gun matches the
                    // old behaviour and a slewing one throws wide of the lead in the
                    // direction it has yet to cover.
                    float const barrelBearing = shipAngle + weapon.aimBearing;
                    float const aimRange = (weapon.aimWorld - mountPos).Length();
                    b2Vec2 const barrelDir{ std::cos(barrelBearing), std::sin(barrelBearing) };

                    // A gun may fire several barrels abreast on one trigger (a
                    // battleship turret), each a projectile of its own. They lie in a
                    // line perpendicular to the bore, centred on the mount: the row
                    // spans (count - 1) separations, and starting half that width to
                    // one side puts an even count either side of the mount centreline
                    // and lands an odd count's middle barrel on it. A single-barrel
                    // gun runs this once at zero offset — exactly the old behaviour.
                    b2Vec2 const abreast{ -std::sin(barrelBearing), std::cos(barrelBearing) };
                    float const firstOffset = -0.5f * static_cast<float>(weapon.barrelCount - 1)
                                              * weapon.barrelSeparationM;
                    for (int barrel = 0; barrel < weapon.barrelCount; ++barrel) {
                        float const offset = firstOffset + (static_cast<float>(barrel) * weapon.barrelSeparationM);
                        b2Vec2 const muzzle = mountPos + (offset * abreast);
                        b2Vec2 const barrelPoint = muzzle + (aimRange * barrelDir);
                        b2Vec2 const firePoint = barrelPoint + RandomInDisc(weapon.spreadRadiusM);

                        // Fire from the barrel toward the scattered point, but never
                        // past the weapon's arc: clamp the shot's bearing to the arc so
                        // a target straddling the edge is still only shot at within it.
                        b2Vec2 const toFire = firePoint - muzzle;
                        float const aimDelta = WrapPi(std::atan2(toFire.y, toFire.x) - arcCentre);
                        float const shotBearing =
                            arcCentre + std::clamp(aimDelta, -weapon.arcHalfAngle, weapon.arcHalfAngle);
                        b2Vec2 const aim{ std::cos(shotBearing), std::sin(shotBearing) };

                        // Fuze the shot to the scattered point's range so it detonates
                        // there — near the target — rather than flying on to max range.
                        // Capped at the weapon's reach. Each barrel is its own shot,
                        // spawned from its own muzzle with its own spread sample.
                        Projectile barrelShot = shot;
                        barrelShot.position = muzzle;
                        barrelShot.velocity = weapon.muzzleVelocity * aim;
                        barrelShot.remaining = std::clamp(toFire.Length(), 0.0f, weapon.range);
                        spawned.push_back(barrelShot);
                    }
                } else {
                    // A launcher throws a guided munition that flies itself,
                    // turning onto the target and building speed under its own
                    // power. Its reach is its self-contained run distance — it
                    // homes until it strikes or runs that out. How it leaves the
                    // mount is the one thing the launcher type decides: a VLS pops
                    // it up at rest (the vertical climb abstracted as acceleration
                    // from zero), while a rail/canister launcher sends it out along
                    // the launch bearing at the munition's initial speed, before its
                    // own acceleration builds. Either way guidance owns it the
                    // moment it is away.
                    if (weapon.kind == WeaponKind::VLS) {
                        shot.velocity = b2Vec2{ 0.0f, 0.0f };
                    } else {
                        float const railBearing = shipAngle + weapon.aimBearing;
                        shot.velocity = weapon.munitionInitialSpeed *
                                        b2Vec2{ std::cos(railBearing), std::sin(railBearing) };
                    }
                    shot.remaining = weapon.range;
                    shot.guidance = Guidance::Guided;
                    shot.health = weapon.munitionHealth; // what point defence must chew through
                    shot.homingTarget = target;
                    shot.maxSpeed = weapon.munitionMaxSpeed;
                    shot.acceleration = weapon.munitionAcceleration;
                    shot.turnRate = weapon.munitionTurnRate;
                    shot.armDistance = weapon.munitionMinRange;
                    shot.waterborne = weapon.munitionWaterborne;
                    spawned.push_back(shot);
                }

                // Heard and felt at the mount rather than the ship's centre, so
                // the guns of one hull are each placed where they actually sit.
                audio.Play(weapon.fireSound, mountPos);
                shake.Add(weapon.fireShakeM, mountPos);

                if (weapon.kind == WeaponKind::Gun) {
                    weapon.cooldownRemaining = weapon.cooldown;
                } else {
                    // Spend a tube: hold the next launch off by the interval, and
                    // start it reloading behind any reload already in progress so
                    // the pool refills one tube at a time. One drained from the
                    // salvo queue, if this launch came from one.
                    --weapon.readyTubes;
                    weapon.launchTimer = weapon.launchInterval;
                    if (weapon.reloadTimer <= 0.0f) {
                        weapon.reloadTimer = weapon.reloadTime;
                    }
                    if (weapon.pending > 0) {
                        --weapon.pending;
                    }
                }
            }
        }

        for (auto const& shot : spawned) {
            registry.emplace<Projectile>(registry.create(), shot);
        }
        // Point defence's leavings: remove the munitions it shot down and drop the
        // splash each left. After the shooter loop for the same reason as the
        // shells above — nothing touches a pool while a view over it is live.
        for (auto munition : downedMunitions) {
            registry.destroy(munition);
        }
        for (auto const& splash : splashes) {
            registry.emplace<Splash>(registry.create(), splash);
        }
    }

    void UpdateProjectiles(entt::registry& registry, Audio& audio, CameraShake& shake,
                           Terrain const& terrain, float dt) {
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

            // A guided munition steers and accelerates before it is integrated. It
            // turns its heading toward the homing target at its turn rate and ramps
            // speed toward maxSpeed, so it leaves the cell at rest and drives in. A
            // target that began sinking, left the registry, or otherwise stopped
            // being a live combatant drops the lock: the munition goes dumb, holds
            // its heading and keeps accelerating to the end of its run rather than
            // circling the wreck. (A sinking hull keeps its Physics body but loses
            // its Combatant, so testing Combatant is what releases the lock the tick
            // it dies.) Ballistic shots fall straight through with constant velocity.
            if (projectile.guidance == Guidance::Guided) {
                float speed = projectile.velocity.Length();
                bool const locked = projectile.homingTarget != entt::null &&
                                    registry.valid(projectile.homingTarget) &&
                                    registry.all_of<Physics, Combatant>(projectile.homingTarget);
                // At (near) rest the heading is undefined, so seed it toward the
                // target when locked, or hold due east when it has no mark to steer
                // by; otherwise carry the current heading.
                float heading = speed > 1e-3f
                                    ? std::atan2(projectile.velocity.y, projectile.velocity.x)
                                    : 0.0f;
                if (locked) {
                    b2Vec2 const targetPos = registry.get<Physics>(projectile.homingTarget).body->GetPosition();
                    float const desired = std::atan2(targetPos.y - projectile.position.y,
                                                     targetPos.x - projectile.position.x);
                    if (speed <= 1e-3f) {
                        heading = desired;
                    }
                    float const delta = WrapPi(desired - heading);
                    float const step = projectile.turnRate * dt;
                    heading += std::abs(delta) <= step ? delta : std::copysign(step, delta);
                } else {
                    projectile.homingTarget = entt::null;
                }
                speed = std::min(projectile.maxSpeed, speed + (projectile.acceleration * dt));
                projectile.velocity = speed * b2Vec2{ std::cos(heading), std::sin(heading) };
            }

            float const stepDist = dt * projectile.velocity.Length();
            projectile.position += dt * projectile.velocity;
            projectile.remaining -= stepDist;
            projectile.armDistance = std::max(0.0f, projectile.armDistance - stepDist);

            // Strike test first: if the shot overlaps a hull this tick it hits,
            // even if its fuze also came due — so a shot fuzed to the target's
            // range damages the target rather than splashing on top of it.
            entt::entity const hull = StruckHull(registry, projectile);
            if (hull != entt::null) {
                expired.push_back(entity);
                // A warhead still short of its arming distance is a dud: the shot
                // is spent, but it detonates nothing — no damage, no sound, no
                // shake — which is what a launcher's minimum range comes to.
                if (projectile.armDistance > 0.0f) {
                    continue;
                }
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

            // A torpedo that swims onto land beaches and is lost — quietly, since
            // it is in the sea and not the air, so there is no surface splash. The
            // captain is free to fire one into a shoreline; that it is wasted is
            // their error, not something the launcher guards against.
            if (projectile.waterborne && !terrain.IsWater(projectile.position, 0.0f)) {
                expired.push_back(entity);
                continue;
            }

            // Fuze: the shot has run to its set range without striking a hull, so
            // it detonates — leave a splash where it went off, near the target it
            // was aimed at. A waterborne munition (a torpedo) is already in the sea,
            // so it simply stops and vanishes with no surface splash or sound.
            if (projectile.remaining <= 0.0f) {
                expired.push_back(entity);
                if (!projectile.waterborne) {
                    splashes.push_back(Splash{ projectile.position, 0.0f, projectile.radiusM });
                    audio.Play(projectile.splashSound, projectile.position);
                }
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
