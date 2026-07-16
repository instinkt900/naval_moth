#include "layers/game_layer.h"

#include "game/aggro_system.h"
#include "game/combat_system.h"
#include "game/components.h"
#include "game/propulsion_system.h"
#include "game/render_system.h"
#include "game/ship_factory.h"
#include "game/wake_system.h"
#include "game/wander_system.h"

#include <moth_ui/events/event_dispatch.h>
#include <moth_ui/utils/transform.h>

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace naval {
    GameLayer::GameLayer(moth_graphics::graphics::IGraphics& graphics, int widthPx, int heightPx)
        : m_graphics(graphics)
        , m_world(b2Vec2{ 0.0f, 0.0f }) // top-down: no gravity
        , m_terrain(m_world, 1337u)
        , m_db(defs::Database::Load("assets/data")) {
        m_camera.viewSize = { static_cast<float>(widthPx), static_cast<float>(heightPx) };
        // Populate the starting neighbourhood so land is present on frame zero.
        m_terrain.Update(m_camera);

        // Before any spawn: a ship resolves the sound ids in its definitions
        // into handles as it is built, which needs the bank already loaded.
        m_audio.Load(m_db);

        // Player at the view centre; enemies scattered across the surrounding
        // water for the player to hunt down.
        m_ship = SpawnHull(m_registry, m_world, m_db, m_audio, m_db.GetPlayer().hull, m_camera.center, Faction::Player);
        SpawnEnemies();
    }

    void GameLayer::SpawnEnemies() {
        // Place each enemy at a random bearing and distance in a ring around the
        // player start, retrying until the point (and a small clearance around
        // it) is open water; a slot that never lands in water is skipped rather
        // than looping forever.
        constexpr int kEnemyCount = 6;
        constexpr float kMinDistM = 800.0f;  // no closer than this to the player
        constexpr float kMaxDistM = 3500.0f; // no farther than this out
        constexpr float kClearanceM = 60.0f; // keep the hull clear of any shore
        constexpr int kMaxAttempts = 64;     // give up on a slot after this many tries

        // Give each enemy its own angular slice (with jitter inside it) so they
        // fan out around the player instead of clustering on one bearing, and
        // scatter the range across the whole band.
        float const slice = (2.0f * b2_pi) / static_cast<float>(kEnemyCount);
        std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> jitterDist(-slice * 0.4f, slice * 0.4f);
        std::uniform_real_distribution<float> distDist(kMinDistM, kMaxDistM);
        std::uniform_real_distribution<float> headingDist(0.0f, 2.0f * b2_pi);

        for (int i = 0; i < kEnemyCount; ++i) {
            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                float const angle = (slice * static_cast<float>(i)) + jitterDist(rng);
                float const radius = distDist(rng);
                b2Vec2 const point{ m_camera.center.x + (radius * std::cos(angle)),
                                    m_camera.center.y + (radius * std::sin(angle)) };
                if (m_terrain.IsWater(point, kClearanceM)) {
                    entt::entity const enemy = SpawnEnemy(m_registry, m_world, m_db, m_audio, "raider", point);
                    // Point each enemy in a random direction so they aren't all
                    // bow-up; the spawn point is kept, only the heading changes.
                    m_registry.get<Physics>(enemy).body->SetTransform(point, headingDist(rng));
                    break;
                }
            }
        }
    }

    bool GameLayer::OnEvent(moth_ui::Event const& event) {
        moth_ui::EventDispatch dispatch(event);
        dispatch.Dispatch(this, &GameLayer::OnMouseDown);
        dispatch.Dispatch(this, &GameLayer::OnMouseWheel);
        dispatch.Dispatch(this, &GameLayer::OnKey);
        return dispatch.GetHandled();
    }

    bool GameLayer::OnMouseDown(moth_ui::EventMouseDown const& event) {
        // Let ImGui swallow clicks that land on the helm panel, so interacting
        // with the controls never drops a waypoint on the sea behind them.
        if (ImGui::GetIO().WantCaptureMouse) {
            return false;
        }
        auto const pos = event.GetPosition();
        auto& target = m_registry.get<MoveTarget>(m_ship);
        // Each click moves the single target; nothing is queued.
        target.point = m_camera.ScreenToWorld({ static_cast<float>(pos.x), static_cast<float>(pos.y) });
        target.active = true;
        return true;
    }

    bool GameLayer::OnMouseWheel(moth_ui::EventMouseWheel const& event) {
        // Scrolling over an ImGui window belongs to it (e.g. dragging a slider),
        // not to the camera zoom.
        if (ImGui::GetIO().WantCaptureMouse) {
            return false;
        }
        // Wheel drives zoom: each notch scales the view about its centre. The
        // limits live in camera.h, since the audio measures against them too.
        constexpr float kZoomStep = 1.15f; // per notch
        float const notches = static_cast<float>(event.GetDelta().y);
        m_camera.pixelsPerMeter = std::clamp(m_camera.pixelsPerMeter * std::pow(kZoomStep, notches),
                                             kMinZoom, kMaxZoom);
        return true;
    }

    bool GameLayer::OnKey(moth_ui::EventKey const& event) {
        // Track WASD hold state; the pan itself happens per-tick in Update so it
        // stays smooth and framerate-independent regardless of key-repeat rate.
        bool const down = event.GetAction() == moth_ui::KeyAction::Down;
        switch (event.GetKey()) {
        case moth_ui::Key::W: m_panUp = down; return true;
        case moth_ui::Key::S: m_panDown = down; return true;
        case moth_ui::Key::A: m_panLeft = down; return true;
        case moth_ui::Key::D: m_panRight = down; return true;
        default: return false;
        }
    }

    void GameLayer::Update(uint32_t ticks) {
        float const dt = static_cast<float>(ticks) / 1000.0f;

        // Pan the camera from held WASD. Target speed is fixed in screen
        // pixels/second so panning feels the same at any zoom; the velocity is
        // eased toward that target for a soft ramp on press and coast on release.
        constexpr float kPanPxPerSec = 600.0f;
        constexpr float kPanEaseTau = 0.12f; // s; smaller = snappier
        moth_ui::FloatVec2 dir{ 0.0f, 0.0f };
        if (m_panLeft) { dir.x -= 1.0f; }
        if (m_panRight) { dir.x += 1.0f; }
        if (m_panUp) { dir.y -= 1.0f; }
        if (m_panDown) { dir.y += 1.0f; }
        // Frame-rate independent exponential approach toward the target velocity.
        float const ease = 1.0f - std::exp(-dt / kPanEaseTau);
        m_panVel.x += ((dir.x * kPanPxPerSec) - m_panVel.x) * ease;
        m_panVel.y += ((dir.y * kPanPxPerSec) - m_panVel.y) * ease;
        m_camera.center.x += (m_panVel.x / m_camera.pixelsPerMeter) * dt;
        m_camera.center.y += (m_panVel.y / m_camera.pixelsPerMeter) * dt;

        // Stream land in/out around the (possibly moved) camera view.
        m_terrain.Update(m_camera);

        // Sounds are heard from the camera, and shakes are felt from it — where
        // it looks and how far it has zoomed in — so both follow the pan above
        // and must be set before any system below plays or jolts anything.
        m_audio.SetListener(m_camera);
        m_shake.SetCamera(m_camera);

        // Enemies that sense a foe within aggro range break off to manoeuvre and
        // fight; the rest wander. Aggro runs first so it can claim the helm, and
        // wander then handles only the ships still on patrol.
        UpdateAggro(m_registry, dt);
        UpdateWander(m_registry, m_terrain, dt);
        UpdatePropulsion(m_registry, dt);
        UpdateWeapons(m_registry, m_audio, m_shake, dt);
        UpdateProjectiles(m_registry, m_audio, m_shake, dt);
        UpdateSplashes(m_registry, dt);
        m_world.Step(dt, 8, 3);

        // After the step so marks are dropped at the hull's settled position.
        UpdateWake(m_registry, dt);

        // Age wrecks and remove them once they have fully sunk. After the step
        // so a wreck's Box2D body is destroyed outside the world update.
        UpdateSinking(m_registry, dt);

        // Decay the shake and roll this frame's jolt, then hand it to the camera
        // to draw with. After everything that fires, hits or explodes, so a gun
        // fired this tick is felt on this frame rather than the next.
        m_shake.Update(dt);
        m_camera.shakeOffsetM = m_shake.OffsetM();

        // Reclaim the voices of sounds that have finished. Last, so anything
        // played this tick has had its voice taken before we look.
        m_audio.Update();
    }

    void GameLayer::Draw() {
        m_terrain.Draw(m_graphics, m_camera);

        // Wakes on the water, beneath everything else the ships lay down.
        DrawWakes(m_graphics, m_registry, m_camera);

        // Splashes from spent shots, on the water alongside the wakes.
        DrawSplashes(m_graphics, m_registry, m_camera);

        // Aggro-range rings around the AI ships (debug aid), beneath the arcs.
        for (auto ship : m_registry.view<Physics, Aggro>()) {
            DrawAggroRing(m_graphics, m_registry, m_camera, ship);
        }

        // Firing arcs beneath the hulls, for every armed ship still alive.
        for (auto ship : m_registry.view<Physics, Armament>()) {
            DrawArcs(m_graphics, m_registry, m_camera, ship);
        }
        DrawTarget(m_graphics, m_registry, m_camera, m_ship);

        // Hulls on top; the player's is drawn last so it stays the topmost.
        for (auto ship : m_registry.view<Physics, Renderable>()) {
            if (ship != m_ship) {
                DrawShip(m_graphics, m_registry, m_camera, ship);
            }
        }
        DrawShip(m_graphics, m_registry, m_camera, m_ship);
        DrawProjectiles(m_graphics, m_registry, m_camera);

        // Debug spread previews on top: a line to each enabled weapon's target
        // and the disc its shots may land within.
        for (auto ship : m_registry.view<Physics, Armament>()) {
            DrawWeaponSpread(m_graphics, m_registry, m_camera, ship);
        }

        DrawHelmPanel();
        DrawWeaponControls();
        DrawAggroDebug();
    }

    void GameLayer::DrawAggroDebug() {
        AggroTuning& tuning = AggroTuningRef();
        ImGui::Begin("Aggro (debug)");
        ImGui::SliderFloat("Aggro range (m)", &tuning.aggroRangeM, 100.0f, 4000.0f, "%.0f");
        ImGui::SliderFloat("Disengage range (m)", &tuning.disengageRangeM, 100.0f, 5000.0f, "%.0f");
        ImGui::SliderFloat("Standoff (frac)", &tuning.standoffFrac, 0.1f, 1.0f, "%.2f");
        ImGui::SliderFloat("Approach band (m)", &tuning.approachBandM, 50.0f, 2000.0f, "%.0f");
        ImGui::SliderFloat("Helm gain", &tuning.helmGain, 0.1f, 8.0f, "%.2f");
        ImGui::SliderFloat("Throttle gain", &tuning.throttleGain, 0.0005f, 0.02f, "%.4f");
        ImGui::SliderFloat("Backoff weight", &tuning.backoffWeight, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Steerage throttle", &tuning.steerageThrottle, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Steerage error (rad)", &tuning.steerageErrorRad, 0.05f, 1.5f, "%.2f");
        ImGui::SliderFloat("Arc switch margin (rad)", &tuning.switchMarginRad, 0.0f, 1.5f, "%.2f");
        ImGui::Checkbox("Show aggro rings", &tuning.showRings);
        ImGui::End();
    }

    void GameLayer::DrawHelmPanel() {
        auto& helm = m_registry.get<Helm>(m_ship);
        auto& target = m_registry.get<MoveTarget>(m_ship);

        ImGui::Begin("Helm");
        if (target.active) {
            ImGui::TextUnformatted("Autopilot: steering to waypoint");
            if (ImGui::Button("Take the helm")) {
                target.active = false;
            }
        } else {
            ImGui::TextUnformatted("Manual control");
        }
        ImGui::Separator();

        // Speed along the keel, signed so making sternway reads negative, shown
        // in knots for flavour (1 m/s = 1.94384 kn).
        constexpr float kMetresPerSecToKnots = 1.94384f;
        b2Body* body = m_registry.get<Physics>(m_ship).body;
        b2Vec2 const forward = body->GetWorldVector(b2Vec2{ 1.0f, 0.0f });
        float const speedKnots = b2Dot(body->GetLinearVelocity(), forward) * kMetresPerSecToKnots;
        ImGui::TextUnformatted(fmt::format("Speed: {:.1f} kn", speedKnots).c_str());
        ImGui::Separator();

        // Engine-order telegraph: quick throttle presets above the slider. Five
        // evenly-spaced bells each way — each step is 20% of full power — with
        // Flank at the rail (±1). Ahead is positive throttle, astern negative;
        // every order takes manual control, like nudging the slider. The bell
        // names are traditional and don't map to literal fractions.
        auto order = [&](char const* label, float throttle) {
            if (ImGui::Button(label)) {
                helm.throttle = throttle;
                target.active = false;
            }
        };
        auto bells = [&](float sign) {
            order("1/3", sign * 0.2f);   ImGui::SameLine();
            order("2/3", sign * 0.4f);   ImGui::SameLine();
            order("Std", sign * 0.6f);   ImGui::SameLine();
            order("Full", sign * 0.8f);  ImGui::SameLine();
            order("Flank", sign * 1.0f);
        };

        ImGui::TextUnformatted("Ahead");
        ImGui::PushID("ahead");
        bells(1.0f);
        ImGui::PopID();

        if (ImGui::Button("Stop")) {
            helm.throttle = 0.0f;
            target.active = false;
        }

        ImGui::TextUnformatted("Astern");
        ImGui::PushID("astern");
        bells(-1.0f);
        ImGui::PopID();

        // Grabbing either control drops the waypoint so manual input takes over;
        // clicking the sea re-engages the autopilot.
        if (ImGui::SliderFloat("Throttle", &helm.throttle, -1.0f, 1.0f, "%.2f")) {
            target.active = false;
        }
        if (ImGui::SliderFloat("Rudder (ordered)", &helm.rudderCmd, -1.0f, 1.0f, "%.2f")) {
            target.active = false;
        }

        // The actual blade position, read-only, so its lag behind the order is
        // visible as the rudder swings across.
        ImGui::BeginDisabled();
        ImGui::SliderFloat("Rudder (actual)", &helm.rudder, -1.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();

        if (ImGui::Button("All stop")) {
            helm.throttle = 0.0f;
            helm.rudderCmd = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Midships")) {
            helm.rudderCmd = 0.0f;
        }

        ImGui::End();
    }

    void GameLayer::DrawWeaponControls() {
        ImGui::Begin("Weapons");
        auto* armament = m_registry.try_get<Armament>(m_ship);
        if (armament == nullptr) {
            ImGui::TextUnformatted("No armament");
            ImGui::End();
            return;
        }
        constexpr float kMetresPerSecToKnots = 1.94384f;
        b2Body* body = m_registry.get<Physics>(m_ship).body;
        b2Vec2 const shipPos = body->GetPosition();
        float const shipAngle = body->GetAngle();
        auto norm360 = [](float deg) {
            deg = std::fmod(deg, 360.0f);
            return deg < 0.0f ? deg + 360.0f : deg;
        };

        for (std::size_t i = 0; i < armament->weapons.size(); ++i) {
            ImGui::Separator();
            Weapon& weapon = armament->weapons[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(weapon.name.empty() ? "Weapon" : weapon.name.c_str());

            ImGui::Checkbox("Show arc", &weapon.showArc);
            ImGui::SameLine();
            ImGui::Checkbox("Show spread", &weapon.showSpread);
            ImGui::SameLine();
            ImGui::Checkbox("Auto fire", &weapon.autoFire);

            bool const ready = weapon.hasTarget && weapon.cooldownRemaining <= 0.0f;
            ImGui::BeginDisabled(!ready);
            if (ImGui::Button("Fire")) {
                weapon.fireRequested = true;
            }
            ImGui::EndDisabled();
            if (weapon.cooldownRemaining > 0.0f) {
                ImGui::SameLine();
                ImGui::TextUnformatted(
                    fmt::format("reloading {:.1f}s", weapon.cooldownRemaining).c_str());
            }

            // The target picture, guarded by registry.valid since the locked
            // contact may have been destroyed since the last weapons update.
            if (weapon.target != entt::null && m_registry.valid(weapon.target)) {
                b2Body* contact = m_registry.get<Physics>(weapon.target).body;
                b2Vec2 const toContact = contact->GetPosition() - shipPos;
                float const rangeM = toContact.Length();
                float const speedKn = contact->GetLinearVelocity().Length() * kMetresPerSecToKnots;
                float const bearingDeg =
                    norm360((std::atan2(toContact.y, toContact.x) - shipAngle) * moth_ui::kRadToDeg);
                float const headingDeg = norm360(contact->GetAngle() * moth_ui::kRadToDeg);
                char const* type = "contact";
                if (auto const* id = m_registry.try_get<Identity>(weapon.target); id != nullptr) {
                    type = id->name.c_str();
                }
                ImGui::TextUnformatted(fmt::format("Target: {}", type).c_str());
                ImGui::TextUnformatted(
                    fmt::format("  rng {:.0f} m   spd {:.1f} kn", rangeM, speedKn).c_str());
                ImGui::TextUnformatted(
                    fmt::format("  brg {:.0f}   hdg {:.0f}", bearingDeg, headingDeg).c_str());
            } else {
                ImGui::TextUnformatted("No target");
            }

            ImGui::PopID();
        }
        ImGui::End();
    }
}
