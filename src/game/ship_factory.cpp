#include "game/ship_factory.h"

#include "game/components.h"
#include "game/defs.h"
#include "game/units.h"

namespace naval {
    entt::entity SpawnHull(entt::registry& registry, b2World& world,
                           defs::Database const& db, std::string const& hullId,
                           b2Vec2 position) {
        defs::Hull const& hull = db.GetHull(hullId);
        entt::entity entity = registry.create();

        b2BodyDef bodyDef;
        bodyDef.type = b2_dynamicBody;
        bodyDef.position = position;
        bodyDef.linearDamping = hull.linearDamping;
        bodyDef.angularDamping = hull.angularDamping;
        b2Body* body = world.CreateBody(&bodyDef);

        b2PolygonShape shape;
        shape.SetAsBox(PxToM(hull.halfLengthPx), PxToM(hull.halfBeamPx));
        b2FixtureDef fixtureDef;
        fixtureDef.shape = &shape;
        fixtureDef.density = 1.0f;
        body->CreateFixture(&fixtureDef);

        registry.emplace<Physics>(entity, Physics{ body });
        registry.emplace<Propulsion>(entity, Propulsion{ hull.propulsion.maxThrust,
                                                         hull.propulsion.minTurnRate,
                                                         hull.propulsion.turnRate,
                                                         hull.propulsion.rudderSpeed,
                                                         hull.propulsion.powerDistance });
        registry.emplace<Renderable>(entity, Renderable{ hull.color, hull.halfLengthPx, hull.halfBeamPx });
        registry.emplace<MoveTarget>(entity, MoveTarget{ b2Vec2{ 0.0f, 0.0f }, false });

        Armament armament;
        for (auto const& mount : hull.mounts) {
            defs::Weapon const& weaponDef = db.GetWeapon(mount.weapon);
            defs::Projectile const& projectileDef = db.GetProjectile(weaponDef.projectile);
            Weapon weapon;
            weapon.bearing = mount.bearing;
            weapon.arcHalfAngle = weaponDef.arcHalfAngle;
            weapon.range = weaponDef.range;
            weapon.cooldown = weaponDef.cooldown;
            weapon.projectileSpeed = projectileDef.speed;
            weapon.projectileRange = projectileDef.maxRange;
            weapon.projectileRadiusPx = projectileDef.radiusPx;
            weapon.projectileColor = projectileDef.color;
            armament.weapons.push_back(weapon);
        }
        registry.emplace<Armament>(entity, std::move(armament));

        return entity;
    }

    entt::entity SpawnEnemy(entt::registry& registry, b2World& world,
                            defs::Database const& db, std::string const& enemyId,
                            b2Vec2 position) {
        defs::Enemy const& enemy = db.GetEnemy(enemyId);
        entt::entity entity = SpawnHull(registry, world, db, enemy.hull, position);
        registry.emplace<Targetable>(entity);
        return entity;
    }
}
