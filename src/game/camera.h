#pragma once

#include <box2d/box2d.h>
#include <moth_ui/utils/vector.h>

namespace naval {
    // The limits of the wheel zoom, in pixels per metre. Shared rather than
    // private to the wheel handler because the audio measures against them too:
    // sounds play as authored at kMaxZoom and are at their quietest at kMinZoom,
    // so "how zoomed out are we" is only meaningful relative to these.
    inline constexpr float kMinZoom = 0.06f;
    inline constexpr float kMaxZoom = 8.0f;

    // Box2D simulates in metres; moth draws in pixels. The camera is the single
    // place the two systems meet. All world state (body sizes, positions, ranges)
    // is authored in metres; the camera maps world<->screen about a fixed view
    // centre, so pixelsPerMeter is a live zoom the player drives with the wheel.
    // Phase 1 keeps `center` put (camera locked); a follow camera can pan it later.
    struct Camera {
        b2Vec2 center{ 0.0f, 0.0f };               // world point held at the view centre (m)
        moth_ui::FloatVec2 viewSize{ 0.0f, 0.0f }; // view extent in pixels
        float pixelsPerMeter = 0.5f;              // world-to-screen scale (zoom)

        moth_ui::FloatVec2 WorldToScreen(b2Vec2 world) const {
            return { ((world.x - center.x) * pixelsPerMeter) + (viewSize.x * 0.5f),
                     ((world.y - center.y) * pixelsPerMeter) + (viewSize.y * 0.5f) };
        }

        b2Vec2 ScreenToWorld(moth_ui::FloatVec2 screen) const {
            return { ((screen.x - (viewSize.x * 0.5f)) / pixelsPerMeter) + center.x,
                     ((screen.y - (viewSize.y * 0.5f)) / pixelsPerMeter) + center.y };
        }

        float MToPx(float metres) const { return metres * pixelsPerMeter; }
    };
}
