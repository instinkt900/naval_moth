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

        // Parses a [lengthFactor, beamFactor] shoulder pair, validating both stay
        // in the range that keeps the hull polygon convex.
        void ParseShoulder(nlohmann::json const& j, std::string const& id,
                           std::string const& field, float& shoulder, float& beam) {
            Require(j.is_array() && j.size() == 2,
                    "hull '" + id + "' " + field + " must be [lengthFactor, beamFactor]");
            float const l = j.at(0).get<float>();
            float const b = j.at(1).get<float>();
            Require(l > 0.0f && l < 1.0f, "hull '" + id + "' " + field + " length factor must be in (0,1)");
            Require(b > 0.0f && b <= 1.0f, "hull '" + id + "' " + field + " beam factor must be in (0,1]");
            shoulder = l;
            beam = b;
        }
    }

    Database Database::Load(std::filesystem::path const& dir) {
        Database db;

        // Each document is bound to a named local before iterating .items():
        // the iteration proxy holds a pointer into the json, and a temporary
        // returned inline would be destroyed before the loop body runs.

        // Sound file paths are authored relative to the assets root rather than
        // to this data directory, so `"audio/gun.wav"` reads naturally and the
        // audio sits beside the data rather than among it.
        std::filesystem::path const assetsRoot = dir.parent_path();
        nlohmann::json const soundsJson = ReadJson(dir / "sounds.json");
        for (auto const& [id, j] : soundsJson.items()) {
            Sound s;
            s.file = assetsRoot / j.at("file").get<std::string>();
            s.volume = j.value("volume", 1.0f);
            s.pitchVariance = j.value("pitchVariance", 0.0f);
            Require(s.volume >= 0.0f, "sound '" + id + "' volume must not be negative");
            Require(s.pitchVariance >= 0.0f && s.pitchVariance < 1.0f,
                    "sound '" + id + "' pitchVariance must be in [0,1)");
            db.m_sounds.emplace(id, s);
        }

        nlohmann::json const projectilesJson = ReadJson(dir / "projectiles.json");
        for (auto const& [id, j] : projectilesJson.items()) {
            Projectile p;
            p.radiusM = j.at("radiusM").get<float>();
            p.color = ParseColor(j.at("color"));
            // Sounds are optional throughout: an unauthored one is a deliberate
            // silence, not an omission to complain about.
            p.impactSound = j.value("impactSound", std::string{});
            p.splashSound = j.value("splashSound", std::string{});
            p.impactShakeM = j.value("impactShakeM", 0.0f);
            Require(p.impactShakeM >= 0.0f, "projectile '" + id + "' impactShakeM must not be negative");
            db.m_projectiles.emplace(id, p);
        }

        nlohmann::json const weaponsJson = ReadJson(dir / "weapons.json");
        for (auto const& [id, j] : weaponsJson.items()) {
            Weapon w;
            w.name = j.value("name", id);
            w.projectile = j.at("projectile").get<std::string>();
            w.muzzleVelocity = j.at("muzzleVelocity").get<float>();
            w.damage = j.at("damage").get<float>();
            w.cooldown = j.at("cooldown").get<float>();
            w.range = j.at("range").get<float>();
            // Degrees/second as authored; radians/second at runtime. Optional —
            // a weapon that omits it (or gives <= 0) trains instantly, the way
            // guns behaved before barrels had a turn rate.
            w.turnRate = j.value("turnRateDegrees", 0.0f) * moth_ui::kDegToRad;
            // Authored as the arc's full width, halved here because the runtime
            // works in a half-width to either side of the mount bearing.
            w.arcHalfAngle = j.at("arcDegrees").get<float>() * moth_ui::kDegToRad * 0.5f;
            w.spread = j.value("spreadDegrees", 0.0f) * moth_ui::kDegToRad;
            w.fireSound = j.value("fireSound", std::string{});
            w.fireShakeM = j.value("fireShakeM", 0.0f);
            Require(w.fireShakeM >= 0.0f, "weapon '" + id + "' fireShakeM must not be negative");
            db.m_weapons.emplace(id, w);
        }

        nlohmann::json const hullsJson = ReadJson(dir / "hulls.json");
        for (auto const& [id, j] : hullsJson.items()) {
            Hull h;
            h.name = j.value("name", id);
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
            // Optional hull profile. Each shoulder is [lengthFactor, beamFactor]
            // placing a taper shoulder. "shoulder" sets both ends symmetrically;
            // "foreShoulder"/"aftShoulder" then override an individual end for an
            // asymmetric hull. Absent means the default boat shape (hull_shape.h).
            if (j.contains("shoulder")) {
                ParseShoulder(j.at("shoulder"), id, "shoulder", h.foreShoulder, h.foreShoulderBeam);
                h.aftShoulder = h.foreShoulder;
                h.aftShoulderBeam = h.foreShoulderBeam;
            }
            if (j.contains("foreShoulder")) {
                ParseShoulder(j.at("foreShoulder"), id, "foreShoulder", h.foreShoulder, h.foreShoulderBeam);
            }
            if (j.contains("aftShoulder")) {
                ParseShoulder(j.at("aftShoulder"), id, "aftShoulder", h.aftShoulder, h.aftShoulderBeam);
            }
            h.angularDamping = j.at("angularDamping").get<float>();
            h.health = j.value("health", 0.0f);
            h.color = ParseColor(j.at("color"));
            h.explosionSound = j.value("explosionSound", std::string{});
            h.explosionShakeM = j.value("explosionShakeM", 0.0f);
            Require(h.explosionShakeM >= 0.0f, "hull '" + id + "' explosionShakeM must not be negative");
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

        // The player is a single definition, not a table.
        nlohmann::json const playerJson = ReadJson(dir / "player.json");
        db.m_player.hull = playerJson.at("hull").get<std::string>();

        // Validate every cross-reference so spawning can trust its lookups. A
        // sound reference is checked like the rest, except that an empty one is
        // allowed: it means the content deliberately makes no sound there.
        auto requireSound = [&db](std::string const& sound, std::string const& owner) {
            Require(sound.empty() || db.m_sounds.count(sound) != 0,
                    owner + " references unknown sound '" + sound + "'");
        };

        for (auto const& [id, weapon] : db.m_weapons) {
            Require(db.m_projectiles.count(weapon.projectile) != 0,
                    "weapon '" + id + "' references unknown projectile '" + weapon.projectile + "'");
            requireSound(weapon.fireSound, "weapon '" + id + "'");
        }
        for (auto const& [id, projectile] : db.m_projectiles) {
            requireSound(projectile.impactSound, "projectile '" + id + "'");
            requireSound(projectile.splashSound, "projectile '" + id + "'");
        }
        for (auto const& [id, hull] : db.m_hulls) {
            requireSound(hull.explosionSound, "hull '" + id + "'");
            for (auto const& mount : hull.mounts) {
                Require(db.m_weapons.count(mount.weapon) != 0,
                        "hull '" + id + "' mounts unknown weapon '" + mount.weapon + "'");
            }
        }
        for (auto const& [id, enemy] : db.m_enemies) {
            Require(db.m_hulls.count(enemy.hull) != 0,
                    "enemy '" + id + "' references unknown hull '" + enemy.hull + "'");
        }
        Require(db.m_hulls.count(db.m_player.hull) != 0,
                "player references unknown hull '" + db.m_player.hull + "'");

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

    Player const& Database::GetPlayer() const {
        return m_player;
    }
}
