#pragma once

#include <array>

namespace naval {
    // The hull silhouette as a simple boat: widest amidships, tapering to a
    // single point at bow and stern. Eight vertices — a beam pair amidships, a
    // beam pair at the taper shoulder, and the two end points. The same outline
    // drives the physics fixture (in metres) and the drawn hull (in pixels), so
    // collision matches what you see. Box2D caps a polygon at 8 vertices, which
    // is why the shape has one tunable shoulder per end — fore and aft — rather
    // than an arbitrary list of stations. The constants below are the default
    // shoulder; a hull may override each end in data (see defs).
    inline constexpr int kHullVertexCount = 8;
    inline constexpr float kHullShoulder = 0.8f;      // default shoulder position, fraction of the half-length
    inline constexpr float kHullShoulderBeam = 0.6f;  // default beam at the shoulder, fraction of the half-beam

    // The eight outline vertices for the given half extents, wound down one side
    // bow-to-stern and back up the other. The fore/aft shoulder args are 0-1
    // factors of the half-length / half-beam placing each taper shoulder, so bow
    // and stern can differ. Vec is any {x, y}-constructible 2D vector — b2Vec2
    // for physics, FloatVec2 for drawing.
    template <typename Vec>
    std::array<Vec, kHullVertexCount> HullOutline(float halfLength, float halfBeam,
                                                  float foreShoulder = kHullShoulder,
                                                  float foreShoulderBeam = kHullShoulderBeam,
                                                  float aftShoulder = kHullShoulder,
                                                  float aftShoulderBeam = kHullShoulderBeam) {
        float const foreX = foreShoulder * halfLength;
        float const foreY = foreShoulderBeam * halfBeam;
        float const aftX = aftShoulder * halfLength;
        float const aftY = aftShoulderBeam * halfBeam;
        return {{
            Vec{ halfLength, 0.0f },   // bow tip
            Vec{ foreX, foreY },       // fore shoulder
            Vec{ 0.0f, halfBeam },     // amidships
            Vec{ -aftX, aftY },        // aft shoulder
            Vec{ -halfLength, 0.0f },  // stern tip
            Vec{ -aftX, -aftY },       // aft shoulder
            Vec{ 0.0f, -halfBeam },    // amidships
            Vec{ foreX, -foreY },      // fore shoulder
        }};
    }
}
