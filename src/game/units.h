#pragma once

#include <box2d/box2d.h>
#include <moth_ui/utils/vector.h>

namespace naval {
    // Box2D simulates in metres; moth draws in pixels. This is the single place
    // the two unit systems meet. At 32 px/m a 64px hull is a 2m body, which
    // keeps the solver in its well-behaved range.
    inline constexpr float kPixelsPerMeter = 32.0f;

    inline float PxToM(float px) { return px / kPixelsPerMeter; }
    inline float MToPx(float m) { return m * kPixelsPerMeter; }

    inline b2Vec2 PxToWorld(moth_ui::FloatVec2 px) {
        return b2Vec2{ PxToM(px.x), PxToM(px.y) };
    }

    inline moth_ui::FloatVec2 WorldToPx(b2Vec2 m) {
        return moth_ui::FloatVec2{ MToPx(m.x), MToPx(m.y) };
    }
}
