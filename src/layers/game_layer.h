#pragma once

#include "game/camera.h"
#include "game/defs.h"
#include "game/terrain.h"

#include <box2d/box2d.h>
#include <entt/entt.hpp>
#include <moth_ui/events/event_key.h>
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
        bool OnKey(moth_ui::EventKey const& event);

        // Scatter enemies across open water in a ring around the player start.
        void SpawnEnemies();

        // ImGui window giving direct throttle and rudder control of the player
        // ship, as an alternative to clicking waypoints.
        void DrawHelmPanel();

        // Per-weapon rows within the helm window: arc and auto-fire toggles, a
        // manual fire button, and the locked contact's picture.
        void DrawWeaponControls();

        // ImGui window with live sliders over the shared aggro tuning, for
        // dialling in enemy engagement behaviour without a rebuild.
        void DrawAggroDebug();

        moth_graphics::graphics::IGraphics& m_graphics;
        Camera m_camera;
        // Held WASD state; the camera pans continuously while any is down.
        bool m_panUp = false;
        bool m_panDown = false;
        bool m_panLeft = false;
        bool m_panRight = false;
        // Smoothed pan velocity (screen px/sec) eased toward the held direction,
        // giving a soft ramp up on press and coast to rest on release.
        moth_ui::FloatVec2 m_panVel{ 0.0f, 0.0f };
        b2World m_world;
        Terrain m_terrain;
        entt::registry m_registry;
        defs::Database m_db;
        entt::entity m_ship;
    };
}
