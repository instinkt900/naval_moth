#pragma once

#include <box2d/box2d.h>
#include <moth_ui/utils/vector.h>

namespace naval {
    // Box2D simulates in metres; moth draws in pixels. The camera is the single
    // place the two systems meet. All world state (body sizes, positions, ranges)
    // is authored in metres; the camera maps world<->screen about a fixed view
    // centre, so pixelsPerMeter is a live zoom the player drives with the wheel.
    // Phase 1 keeps `center` put (camera locked); a follow camera can pan it later.
    struct Camera {
        b2Vec2 center{ 0.0f, 0.0f };               // world point held at the view centre (m)
        moth_ui::FloatVec2 viewSize{ 0.0f, 0.0f }; // view extent in pixels
        float pixelsPerMeter = 2.0f;              // world-to-screen scale (zoom)

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
