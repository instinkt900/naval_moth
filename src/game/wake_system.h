#pragma once

#include <entt/entt.hpp>

namespace naval {
    // Maintains each hull's wake: ages its trail marks and expires the old ones,
    // then drops a fresh mark at the ship's centre once it has made way far
    // enough since the last, giving an even, fading trail behind a moving ship.
    // A ship moving below a crawl leaves no new marks but its existing ones still
    // fade. `dt` is the tick length in seconds.
    void UpdateWake(entt::registry& registry, float dt);
}
