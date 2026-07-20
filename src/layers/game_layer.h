#pragma once

#include "game/audio.h"
#include "game/camera.h"
#include "game/camera_shake.h"
#include "game/defs.h"
#include "game/terrain.h"

#include <box2d/box2d.h>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <moth_ui/events/event_key.h>
#include <moth_ui/events/event_mouse.h>
#include <moth_ui/layers/layer.h>

namespace moth_graphics::graphics {
    class IGraphics;
}

namespace naval {
    struct FireChannel;
    struct ContactPicture;

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

        // The frame's four render layers, composited bottom to top by Draw() and
        // each toggleable from the Layers panel. Map is the sea and the hulls on
        // it; radar overlays the sensor picture; tactical draws the command picture
        // (arcs, tracks, shots) over that; debug sits on top of everything. The two
        // that gate enemy draws on what the player can see take the player's
        // contact picture.
        void DrawMapLayer(ContactPicture const& picture);
        void DrawRadarLayer();
        void DrawTacticalLayer(ContactPicture const& picture);
        void DrawDebugLayer();

        // ImGui window giving direct throttle and rudder control of the player
        // ship, as an alternative to clicking waypoints.
        void DrawHelmPanel();

        // The Target window: the ship's gunnery as a flat list of fire units. Each
        // ungrouped mount is a unit (name, target dropdown, salvo size, Fire/Salvo
        // orders, and an "Add to group" control); each group is the same unit with a
        // member list of the mounts it drives; a separator sits between units. Point
        // defence gets a plain enable-and-status section at the foot.
        void DrawTargetPanel();

        // The contacts the player may currently designate — those it holds with a
        // firing solution (KnownAim ok) — each paired with its call-sign label
        // ("Blip A - DD", or "Blip A - Unknown" before it is classified), sorted by
        // call-sign slot. Feeds every fire unit's target dropdown, so a unit is
        // pointed by picking a contact rather than clicking the water.
        std::vector<std::pair<entt::entity, std::string>> DesignatableContacts();

        // ImGui window listing every contact the player holds — the whole picture
        // as a table, complementing the plot: an auto-assigned call-sign (Blip A,
        // B...), how it is held (Visual/Radar/ESM), its class once identified, and
        // the range/bearing/motion known at the rung it has earned. A bare passive
        // bearing shows its cut but no range; a positioned but unclassified return
        // stays Unknown — the list never shows more than the sensors have earned
        // (see KnownAim). Call-signs are pinned here on the layer, not on the
        // Contact, and persist across the picture's per-tick rebuild.
        void DrawContactList();

        // ImGui window with live sliders over the shared aggro tuning, for
        // dialling in enemy engagement behaviour without a rebuild.
        void DrawAggroDebug();

        // ImGui window listing the player's passive TMA tracks and each one's
        // solver state — cut count, observability, misfit and confidence, and the
        // solved range/course/speed once it has one — so the solver is verifiable
        // and its gates tunable without a test suite.
        void DrawTmaDebug();

        // ImGui window of render-layer toggles. The frame is composited as four
        // layers — map (sea, terrain, seen hulls), radar (blips, reach ring,
        // passive bearings), tactical (arcs, waypoints, projectiles) and debug
        // (aggro rings, spread previews) — each of which this panel can switch off
        // to declutter the view while developing.
        void DrawLayersPanel();

        moth_graphics::graphics::IGraphics& m_graphics;
        Camera m_camera;
        // The camera's shake. Owns the jolt; m_camera only carries the offset it
        // is handed each tick, for drawing.
        CameraShake m_shake;
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
        // Declared after m_db: the constructor loads the sound bank from it
        // before anything spawns, since spawning resolves sound handles here.
        Audio m_audio;
        entt::entity m_ship;
        // The player's auto-assigned contact call-signs, keyed by the observed
        // hull's entity and holding each contact's slot index (0 -> "Blip A").
        // Slots are handed out lowest-free and reclaimed when a contact drops from
        // the picture, so labels stay short over a long watch. Lives on the layer
        // rather than the Contact because a call-sign is a label the player pins on
        // a track, not something the sensors measure — and it has to outlast the
        // per-tick rebuild of the contact picture.
        std::unordered_map<entt::entity, int> m_contactLabels;
        // Debug draw toggle: whether enemy ships' firing arcs are drawn. The
        // player's own arcs are always shown; this only hides the AI batteries,
        // which otherwise clutter the view once several ships are in range.
        bool m_showEnemyArcs = true;
        // Master visibility of each render layer, toggled from the Layers panel.
        // These gate whole categories of the frame at once; the finer per-feature
        // toggles (m_showEnemyArcs, aggro showRings, per-weapon showSpread) still
        // apply within a layer that is shown.
        bool m_showMapLayer = true;
        bool m_showRadarLayer = true;
        bool m_showTacticalLayer = true;
        bool m_showDebugLayer = true;
    };
}
