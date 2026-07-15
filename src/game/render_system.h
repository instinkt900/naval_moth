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

    // A ship's weapon firing arcs, brightening when a target sits inside one.
    void DrawArcs(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // The aggro-range ring around an AI ship: how close a foe must come to wake
    // it. Brightens once the ship has actually aggroed, so the trigger crossing
    // is visible. A debug aid, gated by the shared aggro tuning (radius and
    // visibility both come from it).
    void DrawAggroRing(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // The dashed course line and marker for a ship's active move target.
    void DrawTarget(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // Every hull's fading wake, drawn on the water beneath the hulls.
    void DrawWakes(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera);

    // Every splash left by a spent shot, expanding and fading on the water.
    void DrawSplashes(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera);

    // A single hull with its bow marker.
    void DrawShip(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // Every projectile in flight.
    void DrawProjectiles(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera);
}
