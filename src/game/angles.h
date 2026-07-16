#pragma once

#include <box2d/b2_math.h>

namespace naval {
    // Wrap an angle into [-pi, pi]: the shortest way round to it, signed.
    //
    // Shared because every system that steers or aims is asking the same
    // question — "how far off are we, and which way?" — and they have to agree
    // on the answer. The propulsion system's heading error, the aggro system's
    // turn cost and the weapons system's aim clamp all measure the same circle,
    // and each used to carry a private copy of this, under two different names
    // plus one unnamed loop. The comments then had to promise the three matched;
    // now they do by construction.
    //
    // Laps are subtracted rather than fmod'd because every input here is the
    // difference of two headings — at most a lap or so out — so the loops run
    // once if at all, and the sign stays exact at the boundary.
    inline float WrapPi(float a) {
        while (a > b2_pi) {
            a -= 2.0f * b2_pi;
        }
        while (a < -b2_pi) {
            a += 2.0f * b2_pi;
        }
        return a;
    }
}
