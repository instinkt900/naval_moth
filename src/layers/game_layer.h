#pragma once

#include "game/camera.h"
#include "game/defs.h"

#include <box2d/box2d.h>
#include <entt/entt.hpp>
#include <moth_ui/events/event_mouse.h>
#include <moth_ui/layers/layer.h>

namespace moth_graphics::graphics {
    class IGraphics;
}

namespace naval {
    // The play space: owns the ECS registry and the Box2D world, steps the
    // simulation at the fixed tick, and renders the sea, ship and move target.
    // Click anywhere to command the ship to that point.
    class GameLayer : public moth_ui::Layer {
    public:
        GameLayer(moth_graphics::graphics::IGraphics& graphics, int widthPx, int heightPx);

        bool OnEvent(moth_ui::Event const& event) override;
        void Update(uint32_t ticks) override;
        void Draw() override;

        // Draw and receive input in the fixed design resolution, so clicks and
        // rendering share one coordinate space on both backends.
        bool UseRenderSize() const override { return true; }

    private:
        bool OnMouseDown(moth_ui::EventMouseDown const& event);
        bool OnMouseWheel(moth_ui::EventMouseWheel const& event);

        moth_graphics::graphics::IGraphics& m_graphics;
        Camera m_camera;
        b2World m_world;
        entt::registry m_registry;
        defs::Database m_db;
        entt::entity m_ship;
        entt::entity m_enemy;
    };
}
