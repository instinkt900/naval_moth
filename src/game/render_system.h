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
    // forceAll draws every battery mount's arc regardless of its unit's per-unit
    // Arcs toggle — used for the enemy-arc overlay, which the player controls only
    // through the global "Show enemy arcs" debug toggle, not per unit.
    void DrawArcs(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship, bool forceAll = false);

    // Debug preview of a ship's weapon spread: for each weapon with showSpread on
    // and a live target, a line to its aim point and the disc its shots may land
    // within — so a weapon's accuracy at range is visible without firing.
    void DrawWeaponSpread(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // The aggro-range ring around an AI ship: how close a foe must come to wake
    // it. Brightens once the ship has actually aggroed, so the trigger crossing
    // is visible. A debug aid, gated by the shared aggro tuning (radius and
    // visibility both come from it).
    void DrawAggroRing(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // Debug overlay of `viewer`'s passive TMA state: each track's fan of bearing
    // cuts and the own-ship baseline they span, its current estimate, and a line
    // from that estimate to the contact's real hull so the solution error reads at
    // a glance. Reveals ground truth, so it is a debug aid only — gated by the
    // showResolutions toggle in the shared TMA tuning.
    void DrawTmaOverlay(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity viewer);

    // The authored missile/torpedo flight plans in `viewer`'s library, drawn on the
    // water as their legs and waypoint marks. `activePlanId` is the plan currently
    // being authored (or -1), drawn highlighted so it stands out while the player
    // clicks waypoints into it.
    void DrawFlightPlans(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity viewer, int activePlanId);

    // The dashed course line and marker for a ship's active move target — where
    // it is steering, as opposed to what it is shooting at (see DrawTargetMarker).
    void DrawTarget(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // A ring around each distinct contact `ship`'s fire units have designated:
    // green while no gun bears on it, red once one does — so whether a designated
    // contact can be engaged from here is readable without the Target window. One
    // ring per contact however many units point at it. Draws nothing for a ship
    // with no unit designating anything.
    void DrawTargetMarker(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship);

    // The viewer's own sensor picture over the sea. Passive ESM bearings — a line
    // struck out toward an emitting contact, its length the signal strength, no
    // range — draw always, since listening is free, as does the viewer's own mark
    // (a ring with a heading stalk, distinct from the contact blips). A bearing the
    // TMA solver has ranged (see tma_system) instead runs out to the estimated
    // position and carries an uncertainty ring that tightens with confidence and a
    // course stalk. The active radar's reach ring and a live blip over every fresh
    // contact it paints (including one closed into visual range, which keeps its
    // mark atop its hull) draw only while radiating. A contact that has dropped out
    // decays as a greyed, fading diamond at its last-known position — drawn
    // whatever the radar is doing, since it is memory, not a live return.
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
