#pragma once

#include <entt/entt.hpp>

namespace moth_graphics::graphics {
    class IGraphics;
}

namespace naval {
    struct Camera;

    // Rendering. Read-only over the ECS registry; draws the scene through
    // IGraphics, mapping world metres to screen pixels through the camera. The
    // layer's Draw() owns frame ordering and calls these in turn, mirroring how
    // Update() drives the simulation systems.

    // Clears to the sea colour. Call first each frame.
    void DrawSea(moth_graphics::graphics::IGraphics& graphics);

    // A ship's weapon firing arcs, brightening when a target sits inside one.
    void DrawArcs(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // The dashed course line and marker for a ship's active move target.
    void DrawTarget(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // A single hull with its bow marker.
    void DrawShip(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // Every projectile in flight.
    void DrawProjectiles(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera);
}
