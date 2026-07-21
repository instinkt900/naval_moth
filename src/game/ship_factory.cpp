#include "game/ship_factory.h"

#include "game/audio.h"
#include "game/components.h"
#include "game/defs.h"
#include "game/hull_shape.h"

namespace naval {
    entt::entity SpawnHull(entt::registry& registry, b2World& world,
                           defs::Database const& db, Audio const& audio,
                           std::string const& hullId, b2Vec2 position, Faction faction) {
        defs::Hull const& hull = db.GetHull(hullId);
        entt::entity entity = registry.create();

        b2BodyDef bodyDef;
        bodyDef.type = b2_dynamicBody;
        bodyDef.position = position;
        bodyDef.angularDamping = hull.angularDamping;
        b2Body* body = world.CreateBody(&bodyDef);

        b2PolygonShape shape;
        auto const outline = HullOutline<b2Vec2>(hull.halfLengthM, hull.halfBeamM,
                                                 hull.foreShoulder, hull.foreShoulderBeam,
                                                 hull.aftShoulder, hull.aftShoulderBeam);
        shape.Set(outline.data(), kHullVertexCount);
        b2FixtureDef fixtureDef;
        fixtureDef.shape = &shape;
        // Box2D is 2D, so a fixture's mass is area * density. Create it at unit
        // density (mass then equals the hull's area in m^2), then scale density
        // so the body weighs the hull's authored real displacement. This is what
        // makes accel and coast ordered by real tonnage — a battleship is heavy
        // in the sim because it is heavy in the data, not because a number was
        // hand-tuned.
        fixtureDef.density = 1.0f;
        b2Fixture* fixture = body->CreateFixture(&fixtureDef);
        float const areaM2 = body->GetMass();
        fixture->SetDensity(hull.massKg / areaM2);
        body->ResetMassData();

        registry.emplace<Physics>(entity, Physics{ body });
        registry.emplace<Propulsion>(entity, Propulsion{ hull.propulsion.maxThrust,
                                                         hull.propulsion.maxSpeed,
                                                         hull.propulsion.turnRate,
                                                         hull.propulsion.powerDistance,
                                                         hull.propulsion.rudderRate });
        registry.emplace<Renderable>(entity, Renderable{ hull.color, hull.halfLengthM, hull.halfBeamM,
                                                         hull.foreShoulder, hull.foreShoulderBeam,
                                                         hull.aftShoulder, hull.aftShoulderBeam });
        registry.emplace<Identity>(entity, Identity{ hull.name });
        // Every ship starts dark: radiating is a choice, not a default. The player
        // toggles its radar by hand; an AI ship runs its own emissions control (see
        // UpdateEmcon), going active only once it holds a contact worth ranging. So
        // the sea begins silent and the first ship to emit is the first one heard.
        registry.emplace<Sensors>(entity, Sensors{ hull.visualRangeM, hull.activeRangeM,
                                                   hull.passiveRangeM, false });
        // Every combatant now keeps its own contact picture: the fight is two-sided,
        // so an enemy holds only what its sensors detect — its aggro steering and its
        // gunnery (via KnownAim) both read this picture, not omniscient truth — just
        // as the player does. The passive track file, though, rides only with the
        // player: target motion analysis is a player affordance, since an AI ship
        // simply goes active to range a bearing rather than solving one by manoeuvre.
        registry.emplace<ContactPicture>(entity, ContactPicture{});
        if (faction == Faction::Player) {
            registry.emplace<TrackFile>(entity, TrackFile{});
            registry.emplace<FlightPlanLibrary>(entity, FlightPlanLibrary{});
        }
        registry.emplace<MoveTarget>(entity, MoveTarget{ b2Vec2{ 0.0f, 0.0f }, false });
        registry.emplace<Helm>(entity, Helm{});
        registry.emplace<Wake>(entity, Wake{});
        registry.emplace<Combatant>(entity, Combatant{ faction });
        registry.emplace<Sounds>(entity, Sounds{ audio.Find(hull.explosionSound) });
        registry.emplace<Shake>(entity, Shake{ hull.explosionShakeM });

        Armament armament;
        for (auto const& mount : hull.mounts) {
            Weapon weapon;
            weapon.bearing = mount.bearing;
            // The barrel/rail starts centred in its arc, i.e. laid along the mount.
            weapon.aimBearing = mount.bearing;
            weapon.mountOffset = b2Vec2{ mount.forwardM, mount.lateralM };

            if (mount.type == defs::MountType::Gun) {
                defs::Gun const& gunDef = db.GetGun(mount.gun);
                defs::Projectile const& projectileDef = db.GetProjectile(gunDef.projectile);
                weapon.kind = WeaponKind::Gun;
                weapon.pointDefense = gunDef.pointDefense;
                weapon.name = gunDef.name;
                weapon.arcHalfAngle = gunDef.arcHalfAngle;
                weapon.turnRate = gunDef.turnRate;
                weapon.range = gunDef.range;
                weapon.spread = gunDef.spread;
                weapon.barrelCount = gunDef.barrelCount;
                weapon.barrelSeparationM = gunDef.barrelSeparationM;
                weapon.cooldown = gunDef.cooldown;
                weapon.muzzleVelocity = gunDef.muzzleVelocity;
                weapon.damage = gunDef.damage;
                weapon.projectileRadiusM = projectileDef.radiusM;
                weapon.projectileColor = projectileDef.color;
                weapon.projectileImpactSound = audio.Find(projectileDef.impactSound);
                weapon.projectileSplashSound = audio.Find(projectileDef.splashSound);
                weapon.projectileImpactShakeM = projectileDef.impactShakeM;
                weapon.fireSound = audio.Find(gunDef.fireSound);
                weapon.fireSoundLoops = audio.IsLooping(weapon.fireSound);
                weapon.fireShakeM = gunDef.fireShakeM;
            } else {
                // A launcher's reach, damage and the shot's look/sound all come
                // from the munition loaded at the mount; the launcher itself gives
                // only how it aims (arc/train) and the launch report. A VLS is
                // omnidirectional — a full-circle arc; other launchers carry the
                // arc they were authored with, and a zero turn rate means a fixed
                // tube the munition manoeuvres out of (see combat_system).
                defs::Launcher const& launcherDef = db.GetLauncher(mount.launcher);
                defs::Munition const& munitionDef = db.GetMunition(mount.munition);
                weapon.kind = launcherDef.type == defs::LaunchType::VLS ? WeaponKind::VLS
                                                                        : WeaponKind::Launcher;
                weapon.name = launcherDef.name;
                weapon.munitionName = munitionDef.name;
                weapon.arcHalfAngle =
                    launcherDef.type == defs::LaunchType::VLS ? b2_pi : launcherDef.arcHalfAngle;
                weapon.turnRate = launcherDef.turnRate;
                weapon.range = munitionDef.range;
                // A launcher fires from a bank of tubes rather than on a cooldown:
                // it starts fully loaded. The salvo size lives on the fire unit
                // (FireChannel), defaulting to one so an accidental Salvo does not
                // empty the whole bank; the player raises it deliberately for a
                // heavier volley.
                weapon.tubeCount = launcherDef.tubes;
                weapon.readyTubes = launcherDef.tubes;
                weapon.launchInterval = launcherDef.launchInterval;
                weapon.reloadTime = launcherDef.reloadTime;
                weapon.damage = munitionDef.damage;
                weapon.projectileRadiusM = munitionDef.radiusM;
                weapon.munitionDrawLengthM = munitionDef.drawLengthM;
                weapon.munitionDrawWidthM = munitionDef.drawWidthM;
                weapon.projectileColor = munitionDef.color;
                weapon.projectileImpactSound = audio.Find(munitionDef.impactSound);
                weapon.projectileSplashSound = audio.Find(munitionDef.splashSound);
                weapon.projectileImpactShakeM = munitionDef.impactShakeM;
                weapon.munitionMaxSpeed = munitionDef.maxSpeed;
                weapon.munitionAcceleration = munitionDef.acceleration;
                weapon.munitionTurnRate = munitionDef.turnRate;
                weapon.munitionMinRange = munitionDef.minRange;
                weapon.munitionInitialSpeed = munitionDef.initialSpeed;
                weapon.munitionWaterborne = munitionDef.medium == defs::Medium::Water;
                weapon.munitionHealth = munitionDef.health;
                weapon.munitionFlightPlan = munitionDef.flightPlan;
                weapon.munitionSeekerRangeM = munitionDef.seekerRangeM;
                weapon.fireSound = audio.Find(launcherDef.fireSound);
                weapon.fireSoundLoops = audio.IsLooping(weapon.fireSound);
                weapon.fireShakeM = launcherDef.fireShakeM;
            }
            // Point defence stands outside the fire-unit model — a standing,
            // hands-off shield answering inbound missiles on its own — so it comes up
            // enabled even on the player's ship (a CIWS the captain has to switch on
            // before the first missile arrives is a trap, not a control). A battery
            // mount's `enabled` is unused; its participation is its channel, assigned
            // below.
            weapon.enabled = faction != Faction::Player || weapon.pointDefense;
            armament.weapons.push_back(weapon);
        }

        // Seed fire control from the battery just built. The player gets one lone
        // channel per mount — each gun its own fire unit, silent until committed —
        // and forms groups from the Target window. The enemy gets a single shared
        // channel its whole battery rides, which the aggro system drives; it never
        // splits its fire. Point-defence mounts stay off every channel (-1).
        FireControl fireControl;
        if (faction == Faction::Player) {
            for (auto& weapon : armament.weapons) {
                if (weapon.pointDefense) {
                    continue;
                }
                int const id = fireControl.nextId++;
                fireControl.channels.push_back(FireChannel{ id, /*group*/ false });
                weapon.channel = id;
            }
        } else {
            int const id = fireControl.nextId++;
            fireControl.channels.push_back(FireChannel{ id, /*group*/ true });
            for (auto& weapon : armament.weapons) {
                if (!weapon.pointDefense) {
                    weapon.channel = id;
                }
            }
        }
        registry.emplace<FireControl>(entity, std::move(fireControl));
        registry.emplace<Armament>(entity, std::move(armament));

        return entity;
    }

    entt::entity SpawnEnemy(entt::registry& registry, b2World& world,
                            defs::Database const& db, Audio const& audio,
                            std::string const& enemyId, b2Vec2 position) {
        defs::Enemy const& enemy = db.GetEnemy(enemyId);
        entt::entity entity = SpawnHull(registry, world, db, audio, enemy.hull, position, Faction::Enemy);
        float const hp = db.GetHull(enemy.hull).health;
        registry.emplace<Health>(entity, Health{ hp, hp });
        // Enemies patrol on their own until a foe comes within aggro range, at
        // which point the aggro system breaks them off to manoeuvre and fight.
        registry.emplace<Wander>(entity, Wander{});
        registry.emplace<Aggro>(entity, Aggro{});
        return entity;
    }
}
