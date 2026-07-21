#pragma once

#include <box2d/box2d.h>
#include <moth_ui/utils/vector.h>

namespace naval {
    // The limits of the wheel zoom, in pixels per metre. Shared rather than
    // private to the wheel handler because the attenuation measures against them
    // too — a sound plays as authored at kMaxZoom and is at its quietest at
    // kMinZoom, and a camera shake fades on the same curve — so "how zoomed out
    // are we" is only meaningful relative to these.
    inline constexpr float kMinZoom = 0.01f;
    inline constexpr float kMaxZoom = 2.0f;

    // Box2D simulates in metres; moth draws in pixels. The camera is the single
    // place the two systems meet. All world state (body sizes, positions, ranges)
    // is authored in metres; the camera maps world<->screen about a fixed view
    // centre, so pixelsPerMeter is a live zoom the player drives with the wheel.
    // Phase 1 keeps `center` put (camera locked); a follow camera can pan it later.
    struct Camera {
        b2Vec2 center{ 0.0f, 0.0f };               // world point held at the view centre (m)
        moth_ui::FloatVec2 viewSize{ 0.0f, 0.0f }; // view extent in pixels
        float pixelsPerMeter = 0.5f;              // world-to-screen scale (zoom)

        // A few frames' worth of camera shake, displacing the whole view (m).
        // Kept beside `center` rather than added into it because the two are
        // different things: `center` is where the camera is looking — what the
        // pan drives, what the terrain streams around, and what the shake and the
        // audio measure their distances from — while this is a transient knock to
        // the picture that CameraShake owns and rolls every tick. Both transforms
        // below carry it, so a shaken frame and a click into a shaken frame agree
        // on where the world is.
        b2Vec2 shakeOffsetM{ 0.0f, 0.0f };

        moth_ui::FloatVec2 WorldToScreen(b2Vec2 world) const {
            return { ((world.x - center.x - shakeOffsetM.x) * pixelsPerMeter) + (viewSize.x * 0.5f),
                     ((world.y - center.y - shakeOffsetM.y) * pixelsPerMeter) + (viewSize.y * 0.5f) };
        }

        b2Vec2 ScreenToWorld(moth_ui::FloatVec2 screen) const {
            return { ((screen.x - (viewSize.x * 0.5f)) / pixelsPerMeter) + center.x + shakeOffsetM.x,
                     ((screen.y - (viewSize.y * 0.5f)) / pixelsPerMeter) + center.y + shakeOffsetM.y };
        }

        float MToPx(float metres) const { return metres * pixelsPerMeter; }
    };
}
