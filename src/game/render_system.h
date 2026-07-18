#pragma once

#include <entt/entt.hpp>

namespace moth_graphics::graphics {
    class IGraphics;
}

namespace naval {
    struct Camera;
    struct ContactPicture;

    // Rendering. Read-only over the ECS registry; draws the scene through
    // IGraphics, mapping world metres to screen pixels through the camera. The
    // layer's Draw() owns frame ordering and calls these in turn, mirroring how
    // Update() drives the simulation systems.

    // A ship's weapon firing arcs, brightening when a target sits inside one.
    void DrawArcs(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // Debug preview of a ship's weapon spread: for each weapon with showSpread on
    // and a live target, a line to its aim point and the disc its shots may land
    // within — so a weapon's accuracy at range is visible without firing.
    void DrawWeaponSpread(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // The aggro-range ring around an AI ship: how close a foe must come to wake
    // it. Brightens once the ship has actually aggroed, so the trigger crossing
    // is visible. A debug aid, gated by the shared aggro tuning (radius and
    // visibility both come from it).
    void DrawAggroRing(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // The dashed course line and marker for a ship's active move target — where
    // it is steering, as opposed to what it is shooting at (see DrawTargetMarker).
    void DrawTarget(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // A ring around the contact `ship` has designated (its FireOrder target):
    // green while no gun bears on it, red once one does — so whether the target
    // can be engaged from here is readable without the Target window. Draws
    // nothing when no contact is designated.
    void DrawTargetMarker(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // The viewer's own radar picture over the sea, drawn only while its active
    // radar is radiating: a faint ring at the reach it is buying, and a blip at
    // every contact the sweep paints — including one that has closed into visual
    // range, which keeps its mark on top of its hull so the plot stays complete.
    void DrawContacts(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity viewer);

    // Every visible hull's fading wake, drawn on the water beneath the hulls.
    // Fog-gated like the hulls themselves: a wake is drawn only for `viewer`, for
    // a contact in its `picture`, or for a wreck — an undetected enemy leaves no
    // trail on the player's water any more than it leaves a hull.
    void DrawWakes(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera,
                   entt::entity viewer, ContactPicture const& picture);

    // Every splash left by a spent shot, expanding and fading on the water.
    void DrawSplashes(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera);

    // A single hull with its bow marker.
    void DrawShip(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // Every projectile in flight.
    void DrawProjectiles(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera);

    // A ship's point-defence fire: a flickering tracer stream from each CIWS mount
    // that is trained on an inbound missile out to that missile. The gun is hitscan
    // (no shell entities exist to draw), so this stands in for the rounds in the
    // air — drawn only while a mount is actually engaging, not while it is still
    // slewing onto a mark. Nothing for a ship with no point defence, or none firing.
    void DrawPointDefenseFire(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);
}
