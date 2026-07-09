#include "layers/game_layer.h"

#include "game/combat_system.h"
#include "game/components.h"
#include "game/propulsion_system.h"
#include "game/render_system.h"
#include "game/ship_factory.h"

#include <moth_ui/events/event_dispatch.h>

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
        auto const pos = event.GetPosition();
        auto& target = m_registry.get<MoveTarget>(m_ship);
        // Each click moves the single target; nothing is queued.
        target.point = m_camera.ScreenToWorld({ static_cast<float>(pos.x), static_cast<float>(pos.y) });
        target.active = true;
        return true;
    }

    bool GameLayer::OnMouseWheel(moth_ui::EventMouseWheel const& event) {
        // Wheel drives zoom: each notch scales the view about its centre.
        constexpr float kZoomStep = 1.15f; // per notch
        constexpr float kMinZoom = 3.0f;   // px/m
        constexpr float kMaxZoom = 80.0f;  // px/m
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

        UpdatePropulsion(m_registry);
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
    }
}
