#include "layers/game_layer.h"

#include "game/combat_system.h"
#include "game/components.h"
#include "game/propulsion_system.h"
#include "game/render_system.h"
#include "game/ship_factory.h"

#include <moth_ui/events/event_dispatch.h>

#include <algorithm>
#include <cmath>

namespace naval {
    GameLayer::GameLayer(moth_graphics::graphics::IGraphics& graphics, int widthPx, int heightPx)
        : m_graphics(graphics)
        , m_world(b2Vec2{ 0.0f, 0.0f }) // top-down: no gravity
        , m_db(defs::Database::Load("assets/data")) {
        m_camera.viewSize = { static_cast<float>(widthPx), static_cast<float>(heightPx) };

        // Player at the view centre; a stationary target off the bow-quarter,
        // within broadside reach once the player brings a beam to bear.
        m_ship = SpawnHull(m_registry, m_world, m_db, "cutter", m_camera.center);
        m_enemy = SpawnEnemy(m_registry, m_world, m_db, "target_dummy",
                             m_camera.ScreenToWorld({ (m_camera.viewSize.x * 0.5f) + 160.0f,
                                                      (m_camera.viewSize.y * 0.5f) - 160.0f }));
    }

    bool GameLayer::OnEvent(moth_ui::Event const& event) {
        moth_ui::EventDispatch dispatch(event);
        dispatch.Dispatch(this, &GameLayer::OnMouseDown);
        dispatch.Dispatch(this, &GameLayer::OnMouseWheel);
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

    void GameLayer::Update(uint32_t ticks) {
        float const dt = static_cast<float>(ticks) / 1000.0f;
        UpdatePropulsion(m_registry);
        UpdateWeapons(m_registry, dt);
        UpdateProjectiles(m_registry, dt);
        m_world.Step(dt, 8, 3);
    }

    void GameLayer::Draw() {
        DrawSea(m_graphics);
        DrawArcs(m_graphics, m_registry, m_camera, m_ship);
        DrawTarget(m_graphics, m_registry, m_camera, m_ship);
        DrawShip(m_graphics, m_registry, m_camera, m_enemy);
        DrawShip(m_graphics, m_registry, m_camera, m_ship);
        DrawProjectiles(m_graphics, m_registry, m_camera);
    }
}
