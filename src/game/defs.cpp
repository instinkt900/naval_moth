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

        nlohmann::json const gunsJson = ReadJson(dir / "guns.json");
        for (auto const& [id, j] : gunsJson.items()) {
            Gun g;
            g.name = j.value("name", id);
            g.projectile = j.at("projectile").get<std::string>();
            g.muzzleVelocity = j.at("muzzleVelocity").get<float>();
            g.damage = j.at("damage").get<float>();
            g.cooldown = j.at("cooldown").get<float>();
            g.range = j.at("range").get<float>();
            // Degrees/second as authored; radians/second at runtime. Optional —
            // a gun that omits it (or gives <= 0) trains instantly, the way
            // guns behaved before barrels had a turn rate.
            g.turnRate = j.value("turnRateDegrees", 0.0f) * moth_ui::kDegToRad;
            // Authored as the arc's full width, halved here because the runtime
            // works in a half-width to either side of the mount bearing.
            g.arcHalfAngle = j.at("arcDegrees").get<float>() * moth_ui::kDegToRad * 0.5f;
            g.spread = j.value("spreadDegrees", 0.0f) * moth_ui::kDegToRad;
            g.fireSound = j.value("fireSound", std::string{});
            g.fireShakeM = j.value("fireShakeM", 0.0f);
            Require(g.fireShakeM >= 0.0f, "gun '" + id + "' fireShakeM must not be negative");
            db.m_guns.emplace(id, g);
        }

        nlohmann::json const launchersJson = ReadJson(dir / "launchers.json");
        for (auto const& [id, j] : launchersJson.items()) {
            Launcher l;
            l.name = j.value("name", id);
            std::string const type = j.value("type", std::string{ "vls" });
            Require(type == "vls" || type == "launcher",
                    "launcher '" + id + "' type must be \"vls\" or \"launcher\"");
            l.type = type == "vls" ? LaunchType::VLS : LaunchType::Launcher;
            l.tubes = j.at("tubes").get<int>();
            l.launchInterval = j.at("launchInterval").get<float>();
            l.reloadTime = j.at("reloadTime").get<float>();
            Require(l.tubes >= 1, "launcher '" + id + "' tubes must be at least 1");
            Require(l.launchInterval >= 0.0f, "launcher '" + id + "' launchInterval must not be negative");
            Require(l.reloadTime > 0.0f, "launcher '" + id + "' reloadTime must be positive");
            // Arc and rail turn rate matter only to a trainable launcher; a VLS
            // is omnidirectional and never trains, so both are left at zero and
            // the runtime treats it as a full-circle arc.
            l.arcHalfAngle = j.value("arcDegrees", 0.0f) * moth_ui::kDegToRad * 0.5f;
            l.turnRate = j.value("turnRateDegrees", 0.0f) * moth_ui::kDegToRad;
            Require(l.type != LaunchType::Launcher || j.contains("arcDegrees"),
                    "launcher '" + id + "' of type \"launcher\" needs arcDegrees");
            l.fireSound = j.value("fireSound", std::string{});
            l.fireShakeM = j.value("fireShakeM", 0.0f);
            Require(l.fireShakeM >= 0.0f, "launcher '" + id + "' fireShakeM must not be negative");
            db.m_launchers.emplace(id, l);
        }

        nlohmann::json const missilesJson = ReadJson(dir / "missiles.json");
        for (auto const& [id, j] : missilesJson.items()) {
            Missile m;
            m.name = j.value("name", id);
            m.range = j.at("range").get<float>();
            m.minRange = j.value("minRange", 0.0f);
            m.acceleration = j.at("acceleration").get<float>();
            m.maxSpeed = j.at("topSpeed").get<float>();
            m.initialSpeed = j.value("initialSpeed", 0.0f);
            m.damage = j.at("damage").get<float>();
            m.turnRate = j.at("turnRateDegrees").get<float>() * moth_ui::kDegToRad;
            m.radiusM = j.at("radiusM").get<float>();
            m.color = ParseColor(j.at("color"));
            m.impactSound = j.value("impactSound", std::string{});
            m.splashSound = j.value("splashSound", std::string{});
            m.impactShakeM = j.value("impactShakeM", 0.0f);
            Require(m.range > 0.0f, "missile '" + id + "' range must be positive");
            Require(m.minRange >= 0.0f && m.minRange < m.range,
                    "missile '" + id + "' minRange must be in [0, range)");
            Require(m.acceleration > 0.0f, "missile '" + id + "' acceleration must be positive");
            Require(m.maxSpeed > 0.0f, "missile '" + id + "' topSpeed must be positive");
            Require(m.initialSpeed >= 0.0f, "missile '" + id + "' initialSpeed must not be negative");
            Require(m.impactShakeM >= 0.0f, "missile '" + id + "' impactShakeM must not be negative");
            db.m_missiles.emplace(id, m);
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
                std::string const type = jm.value("type", std::string{ "gun" });
                Require(type == "gun" || type == "launcher",
                        "hull '" + id + "' mount type must be \"gun\" or \"launcher\"");
                if (type == "gun") {
                    m.type = MountType::Gun;
                    m.gun = jm.at("gun").get<std::string>();
                } else {
                    m.type = MountType::Launcher;
                    m.launcher = jm.at("launcher").get<std::string>();
                    m.missile = jm.at("missile").get<std::string>();
                }
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

        for (auto const& [id, gun] : db.m_guns) {
            Require(db.m_projectiles.count(gun.projectile) != 0,
                    "gun '" + id + "' references unknown projectile '" + gun.projectile + "'");
            requireSound(gun.fireSound, "gun '" + id + "'");
        }
        for (auto const& [id, launcher] : db.m_launchers) {
            requireSound(launcher.fireSound, "launcher '" + id + "'");
        }
        for (auto const& [id, missile] : db.m_missiles) {
            requireSound(missile.impactSound, "missile '" + id + "'");
            requireSound(missile.splashSound, "missile '" + id + "'");
        }
        for (auto const& [id, projectile] : db.m_projectiles) {
            requireSound(projectile.impactSound, "projectile '" + id + "'");
            requireSound(projectile.splashSound, "projectile '" + id + "'");
        }
        for (auto const& [id, hull] : db.m_hulls) {
            requireSound(hull.explosionSound, "hull '" + id + "'");
            for (auto const& mount : hull.mounts) {
                if (mount.type == MountType::Gun) {
                    Require(db.m_guns.count(mount.gun) != 0,
                            "hull '" + id + "' mounts unknown gun '" + mount.gun + "'");
                } else {
                    Require(db.m_launchers.count(mount.launcher) != 0,
                            "hull '" + id + "' mounts unknown launcher '" + mount.launcher + "'");
                    Require(db.m_missiles.count(mount.missile) != 0,
                            "hull '" + id + "' loads unknown missile '" + mount.missile + "'");
                }
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

    Gun const& Database::GetGun(std::string const& id) const {
        auto it = m_guns.find(id);
        Require(it != m_guns.end(), "no gun '" + id + "'");
        return it->second;
    }

    Launcher const& Database::GetLauncher(std::string const& id) const {
        auto it = m_launchers.find(id);
        Require(it != m_launchers.end(), "no launcher '" + id + "'");
        return it->second;
    }

    Missile const& Database::GetMissile(std::string const& id) const {
        auto it = m_missiles.find(id);
        Require(it != m_missiles.end(), "no missile '" + id + "'");
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
