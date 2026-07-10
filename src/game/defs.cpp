#include "game/defs.h"

#include <moth_ui/utils/transform.h> // kDegToRad

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace naval::defs {
    namespace {
        constexpr float kKnotsToMetersPerSecond = 0.514444f;

        nlohmann::json ReadJson(std::filesystem::path const& path) {
            std::ifstream in(path);
            if (!in) {
                throw std::runtime_error("naval data: cannot open " + path.string());
            }
            try {
                return nlohmann::json::parse(in);
            } catch (nlohmann::json::exception const& e) {
                throw std::runtime_error("naval data: parse error in " + path.string() + ": " + e.what());
            }
        }

        moth_ui::Color ParseColor(nlohmann::json const& j) {
            return moth_ui::Color{ j.at(0).get<float>(), j.at(1).get<float>(),
                                   j.at(2).get<float>(), j.at(3).get<float>() };
        }

        void Require(bool condition, std::string const& message) {
            if (!condition) {
                throw std::runtime_error("naval data: " + message);
            }
        }
    }

    Database Database::Load(std::filesystem::path const& dir) {
        Database db;

        // Each document is bound to a named local before iterating .items():
        // the iteration proxy holds a pointer into the json, and a temporary
        // returned inline would be destroyed before the loop body runs.
        nlohmann::json const projectilesJson = ReadJson(dir / "projectiles.json");
        for (auto const& [id, j] : projectilesJson.items()) {
            Projectile p;
            p.speed = j.at("speed").get<float>();
            p.radiusM = j.at("radiusM").get<float>();
            p.damage = j.at("damage").get<float>();
            p.color = ParseColor(j.at("color"));
            db.m_projectiles.emplace(id, p);
        }

        nlohmann::json const weaponsJson = ReadJson(dir / "weapons.json");
        for (auto const& [id, j] : weaponsJson.items()) {
            Weapon w;
            w.projectile = j.at("projectile").get<std::string>();
            w.cooldown = j.at("cooldown").get<float>();
            w.range = j.at("range").get<float>();
            w.arcHalfAngle = j.at("arcDegrees").get<float>() * moth_ui::kDegToRad;
            db.m_weapons.emplace(id, w);
        }

        nlohmann::json const hullsJson = ReadJson(dir / "hulls.json");
        for (auto const& [id, j] : hullsJson.items()) {
            Hull h;
            auto const& jp = j.at("propulsion");
            h.propulsion.maxThrust = jp.at("maxThrust").get<float>();
            h.propulsion.maxSpeed = jp.at("maxSpeedKnots").get<float>() * kKnotsToMetersPerSecond;
            h.propulsion.turnRate = jp.at("turnRate").get<float>();
            h.propulsion.powerDistance = jp.at("powerDistance").get<float>();
            h.propulsion.rudderRate = jp.at("rudderRate").get<float>();
            Require(h.propulsion.maxThrust <= 0.0f || h.propulsion.maxSpeed > 0.0f,
                    "hull '" + id + "' has thrust but no maxSpeed");
            h.halfLengthM = j.at("halfLengthM").get<float>();
            h.halfBeamM = j.at("halfBeamM").get<float>();
            h.angularDamping = j.at("angularDamping").get<float>();
            h.health = j.value("health", 0.0f);
            h.color = ParseColor(j.at("color"));
            for (auto const& jm : j.at("mounts")) {
                Mount m;
                m.weapon = jm.at("weapon").get<std::string>();
                m.bearing = jm.at("bearingDegrees").get<float>() * moth_ui::kDegToRad;
                m.forwardM = jm.value("forwardM", 0.0f);
                m.lateralM = jm.value("lateralM", 0.0f);
                h.mounts.push_back(m);
            }
            db.m_hulls.emplace(id, h);
        }

        nlohmann::json const enemiesJson = ReadJson(dir / "enemies.json");
        for (auto const& [id, j] : enemiesJson.items()) {
            Enemy e;
            e.hull = j.at("hull").get<std::string>();
            db.m_enemies.emplace(id, e);
        }

        // Validate every cross-reference so spawning can trust its lookups.
        for (auto const& [id, weapon] : db.m_weapons) {
            Require(db.m_projectiles.count(weapon.projectile) != 0,
                    "weapon '" + id + "' references unknown projectile '" + weapon.projectile + "'");
        }
        for (auto const& [id, hull] : db.m_hulls) {
            for (auto const& mount : hull.mounts) {
                Require(db.m_weapons.count(mount.weapon) != 0,
                        "hull '" + id + "' mounts unknown weapon '" + mount.weapon + "'");
            }
        }
        for (auto const& [id, enemy] : db.m_enemies) {
            Require(db.m_hulls.count(enemy.hull) != 0,
                    "enemy '" + id + "' references unknown hull '" + enemy.hull + "'");
        }

        return db;
    }

    Hull const& Database::GetHull(std::string const& id) const {
        auto it = m_hulls.find(id);
        Require(it != m_hulls.end(), "no hull '" + id + "'");
        return it->second;
    }

    Weapon const& Database::GetWeapon(std::string const& id) const {
        auto it = m_weapons.find(id);
        Require(it != m_weapons.end(), "no weapon '" + id + "'");
        return it->second;
    }

    Projectile const& Database::GetProjectile(std::string const& id) const {
        auto it = m_projectiles.find(id);
        Require(it != m_projectiles.end(), "no projectile '" + id + "'");
        return it->second;
    }

    Enemy const& Database::GetEnemy(std::string const& id) const {
        auto it = m_enemies.find(id);
        Require(it != m_enemies.end(), "no enemy '" + id + "'");
        return it->second;
    }
}
