#include "layers/game_layer.h"

#include "game/combat_system.h"
#include "game/components.h"
#include "game/propulsion_system.h"
#include "game/render_system.h"
#include "game/ship_factory.h"

#include <moth_ui/events/event_dispatch.h>

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

        // Player at the view centre; enemies scattered across the surrounding
        // water for the player to hunt down.
        m_ship = SpawnHull(m_registry, m_world, m_db, "cutter", m_camera.center, Faction::Player);
        SpawnEnemies();
    }

    void GameLayer::SpawnEnemies() {
        // Place each enemy at a random bearing and distance in a ring around the
        // player start, retrying until the point (and a small clearance around
        // it) is open water; a slot that never lands in water is skipped rather
        // than looping forever.
        constexpr int kEnemyCount = 6;
        constexpr float kMinDistM = 70.0f;   // no closer than this to the player
        constexpr float kMaxDistM = 320.0f;  // no farther than this out
        constexpr float kClearanceM = 6.0f;  // keep the hull clear of any shore
        constexpr int kMaxAttempts = 64;     // give up on a slot after this many tries

        std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> angleDist(0.0f, 2.0f * b2_pi);
        std::uniform_real_distribution<float> distDist(kMinDistM, kMaxDistM);

        for (int i = 0; i < kEnemyCount; ++i) {
            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                float const angle = angleDist(rng);
                float const radius = distDist(rng);
                b2Vec2 const point{ m_camera.center.x + (radius * std::cos(angle)),
                                    m_camera.center.y + (radius * std::sin(angle)) };
                if (m_terrain.IsWater(point, kClearanceM)) {
                    SpawnEnemy(m_registry, m_world, m_db, "target_dummy", point);
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
        // Wheel drives zoom: each notch scales the view about its centre.
        constexpr float kZoomStep = 1.15f; // per notch
        constexpr float kMinZoom = 0.3f;   // px/m
        constexpr float kMaxZoom = 8.0f;  // px/m
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

        UpdatePropulsion(m_registry, dt);
        UpdateWeapons(m_registry, dt);
        UpdateProjectiles(m_registry, dt);
        m_world.Step(dt, 8, 3);
    }

    void GameLayer::Draw() {
        m_terrain.Draw(m_graphics, m_camera);

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

        DrawHelmPanel();
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
}
