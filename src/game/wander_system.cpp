#include "game/wander_system.h"

#include "game/components.h"
#include "game/terrain.h"

#include <box2d/box2d.h>

#include <cmath>
#include <random>

namespace naval {
    namespace {
        // Legs are kilometres so a ship runs a heading for minutes at a time —
        // this is a slow patrol, not twitchy wandering. A ship only changes
        // course on arrival or when the progress check below finds it stuck.
        constexpr float kMinLegM = 2000.0f;  // nearest a new waypoint may be picked
        constexpr float kMaxLegM = 6000.0f;  // farthest a new waypoint may be picked
        constexpr float kClearanceM = 80.0f; // keep the waypoint clear of any shore
        constexpr int kMaxAttempts = 12;     // give up finding water this tick after this many tries

        // Stall watch: every kCheckIntervalS, a ship that has travelled less than
        // kMinTravelM since the last check has run out of way (pinned on a shore,
        // since a healthy hull covers far more) and re-rolls its waypoint. Reads
        // distance travelled, not distance closed to the target, so a ship merely
        // coming about between legs isn't mistaken for a stalled one.
        constexpr float kCheckIntervalS = 8.0f; // how often to test for progress
        constexpr float kMinTravelM = 30.0f;    // way a check must see, or the ship is stuck

        constexpr float kCruiseThrottle = 0.6f; // patrol at a standard bell, not flank
    }

    void UpdateWander(entt::registry& registry, Terrain const& terrain, float dt) {
        // One generator for the whole run, seeded once. The distributions are
        // cheap to rebuild each call and keep the state local.
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> angleDist(0.0f, 2.0f * b2_pi);
        std::uniform_real_distribution<float> legDist(kMinLegM, kMaxLegM);

        for (auto entity : registry.view<Physics, MoveTarget, Wander>()) {
            // A sinking wreck answers no helm; leave its waypoint be so the
            // wander AI isn't fighting the death sequence.
            if (registry.all_of<Sinking>(entity)) {
                continue;
            }

            auto& target = registry.get<MoveTarget>(entity);
            auto& wander = registry.get<Wander>(entity);
            b2Vec2 const from = registry.get<Physics>(entity).body->GetPosition();

            // Keep running the current leg until the autopilot clears it on
            // arrival, or the periodic progress check finds the ship has stopped
            // making way and so must be stuck against a shore.
            if (target.active) {
                wander.sinceCheck += dt;
                if (wander.sinceCheck < kCheckIntervalS) {
                    continue;
                }
                float const travelled = (from - wander.lastPos).Length();
                wander.lastPos = from;
                wander.sinceCheck = 0.0f;
                if (travelled >= kMinTravelM) {
                    continue; // still making way; hold the heading
                }
            }

            // Pick a random water point kilometres off in any direction; retry a
            // few times, and if nothing lands on open water this tick, leave the
            // target as-is and try again next tick.
            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                float const angle = angleDist(rng);
                float const leg = legDist(rng);
                b2Vec2 const point{ from.x + (leg * std::cos(angle)),
                                    from.y + (leg * std::sin(angle)) };
                if (terrain.IsWater(point, kClearanceM)) {
                    target.point = point;
                    target.active = true;
                    target.maxThrottle = kCruiseThrottle;
                    wander.lastPos = from;
                    wander.sinceCheck = 0.0f;
                    break;
                }
            }
        }
    }
}
