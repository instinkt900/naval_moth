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
        fixtureDef.density = 1.0f;
        body->CreateFixture(&fixtureDef);

        registry.emplace<Physics>(entity, Physics{ body });
        registry.emplace<Propulsion>(entity, Propulsion{ hull.propulsion.maxThrust,
                                                         hull.propulsion.maxSpeed,
                                                         hull.propulsion.turnRate,
                                                         hull.propulsion.powerDistance,
                                                         hull.propulsion.rudderRate });
        registry.emplace<Renderable>(entity, Renderable{ hull.color, hull.halfLengthM, hull.halfBeamM,
                                                         hull.foreShoulder, hull.foreShoulderBeam,
                                                         hull.aftShoulder, hull.aftShoulderBeam });
        registry.emplace<Identity>(entity, Identity{ hullId });
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
            defs::Weapon const& weaponDef = db.GetWeapon(mount.weapon);
            defs::Projectile const& projectileDef = db.GetProjectile(weaponDef.projectile);
            Weapon weapon;
            weapon.name = mount.weapon;
            weapon.bearing = mount.bearing;
            weapon.mountOffset = b2Vec2{ mount.forwardM, mount.lateralM };
            weapon.arcHalfAngle = weaponDef.arcHalfAngle;
            weapon.range = weaponDef.range;
            weapon.spread = weaponDef.spread;
            weapon.muzzleVelocity = weaponDef.muzzleVelocity;
            weapon.damage = weaponDef.damage;
            weapon.cooldown = weaponDef.cooldown;
            weapon.projectileRadiusM = projectileDef.radiusM;
            weapon.projectileColor = projectileDef.color;
            weapon.projectileImpactSound = audio.Find(projectileDef.impactSound);
            weapon.projectileSplashSound = audio.Find(projectileDef.splashSound);
            weapon.projectileImpactShakeM = projectileDef.impactShakeM;
            weapon.fireSound = audio.Find(weaponDef.fireSound);
            weapon.fireShakeM = weaponDef.fireShakeM;
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
