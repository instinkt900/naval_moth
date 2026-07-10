#include "game/wake_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>

namespace naval {
    namespace {
        constexpr float kSpacingM = 12.0f;      // drop a mark every this far the stern travels
        constexpr float kMinSpeed = 1.0f;      // m/s; below this the ship leaves no new wake
        constexpr std::size_t kMaxMarks = 128; // hard cap so the trail can't grow unbounded
    }

    void UpdateWake(entt::registry& registry, float dt) {
        auto view = registry.view<Physics, Wake>();
        for (auto entity : view) {
            b2Body* body = view.get<Physics>(entity).body;
            auto& wake = view.get<Wake>(entity);

            // Age every mark, then drop those past their lifetime. Marks are held
            // oldest-first so the expired ones are always a prefix at the front.
            for (auto& mark : wake.marks) {
                mark.age += dt;
            }
            auto const firstLive = std::find_if(wake.marks.begin(), wake.marks.end(),
                                                [](Wake::Mark const& m) { return m.age <= kWakeLifetimeS; });
            wake.marks.erase(wake.marks.begin(), firstLive);

            // Below a crawl the ship leaves no new wake, but the marks above keep
            // ageing out so a stopping ship's trail dissolves behind it.
            if (body->GetLinearVelocity().Length() < kMinSpeed) {
                continue;
            }

            // Drop a fresh mark at the ship's centre once it has travelled far
            // enough since the last, so spacing is even regardless of speed or
            // framerate.
            b2Vec2 const centre = body->GetPosition();
            if (!wake.seeded || (centre - wake.lastDrop).Length() >= kSpacingM) {
                wake.marks.push_back(Wake::Mark{ centre, 0.0f });
                wake.lastDrop = centre;
                wake.seeded = true;
                if (wake.marks.size() > kMaxMarks) {
                    wake.marks.erase(wake.marks.begin());
                }
            }
        }
    }
}
