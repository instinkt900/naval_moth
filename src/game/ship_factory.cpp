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
        registry.emplace<Sensors>(entity, Sensors{ hull.visualRangeM });
        // Only the player keeps a contact picture; the enemy is left omniscient
        // (its aggro system scans hulls directly) until the fight is two-sided, so
        // giving it a picture now would be dead weight the sensor system fills and
        // nothing reads. The player's picture is what gates its rendering and
        // target-picking to what it can actually detect.
        if (faction == Faction::Player) {
            registry.emplace<ContactPicture>(entity, ContactPicture{});
        }
        registry.emplace<MoveTarget>(entity, MoveTarget{ b2Vec2{ 0.0f, 0.0f }, false });
        registry.emplace<Helm>(entity, Helm{});
        registry.emplace<Wake>(entity, Wake{});
        registry.emplace<Combatant>(entity, Combatant{ faction });
        // Every combatant carries an order, empty until something issues one —
        // the player's Target window, or the aggro system for an enemy. Without
        // it the ship's guns are invisible to the weapons system.
        registry.emplace<FireOrder>(entity, FireOrder{});
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
                // it starts fully loaded. A salvo defaults to a single tube so an
                // accidental Salvo does not empty the whole bank; the player raises
                // the salvo size deliberately when they want a heavier volley.
                weapon.tubeCount = launcherDef.tubes;
                weapon.readyTubes = launcherDef.tubes;
                weapon.launchInterval = launcherDef.launchInterval;
                weapon.reloadTime = launcherDef.reloadTime;
                weapon.salvoSize = 1;
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
                weapon.fireSound = audio.Find(launcherDef.fireSound);
                weapon.fireSoundLoops = audio.IsLooping(weapon.fireSound);
                weapon.fireShakeM = launcherDef.fireShakeM;
            }
            // The player's offensive mounts start switched out, so opening an
            // engagement is a deliberate act of ticking in the weapons wanted
            // rather than the whole battery cutting loose at once. Point defence
            // is the exception: it is a standing, hands-off shield, so it comes up
            // enabled even on the player's ship — a CIWS the captain has to switch
            // on before the first missile arrives is a trap, not a control. An
            // enemy battery, which nothing toggles, stays enabled and fights in full.
            weapon.enabled = faction != Faction::Player || weapon.pointDefense;
            armament.weapons.push_back(weapon);
        }
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
