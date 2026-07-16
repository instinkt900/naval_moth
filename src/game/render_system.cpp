#include "game/render_system.h"

#include "game/aggro_system.h"
#include "game/camera.h"
#include "game/components.h"
#include "game/hull_shape.h"

#include <moth_graphics/graphics/igraphics.h>
#include <moth_ui/utils/rect.h>
#include <moth_ui/utils/transform.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace naval {
    namespace {
        // --- colours ---
        const moth_ui::Color kBow{ 0.90f, 0.35f, 0.30f, 1.0f };
        const moth_ui::Color kTargetColor{ 0.95f, 0.85f, 0.40f, 1.0f };
        const moth_ui::Color kLineColor{ 0.55f, 0.65f, 0.75f, 1.0f };
        const moth_ui::Color kArcColor{ 0.35f, 0.45f, 0.55f, 0.6f };       // firing arc at rest
        const moth_ui::Color kArcActiveColor{ 0.95f, 0.55f, 0.35f, 0.9f }; // arc with a target in it
        const moth_ui::Color kAggroRingColor{ 0.80f, 0.30f, 0.30f, 0.20f };       // aggro range, ship still patrolling
        const moth_ui::Color kAggroRingActiveColor{ 0.95f, 0.35f, 0.30f, 0.65f }; // aggro range once the ship has locked on
        const moth_ui::Color kSpreadColor{ 0.95f, 0.85f, 0.35f, 0.6f };           // debug spread preview (aim line + disc)
        const moth_ui::Color kTargetRingColor{ 0.35f, 0.90f, 0.40f, 0.85f };      // designated contact, no gun bearing on it
        const moth_ui::Color kTargetRingArmedColor{ 0.95f, 0.25f, 0.25f, 0.90f }; // designated contact, under the guns

        // --- wake ---
        const moth_ui::Color kWakeColor{ 0.85f, 0.90f, 0.95f, 1.0f }; // pale foam; alpha set per mark
        constexpr float kWakeAlpha = 0.22f;         // opacity of a fresh mark — low so it stays subtle
        constexpr float kWakeStartBeamFrac = 0.5f; // fresh mark radius, fraction of the half-beam
        constexpr float kWakeEndBeamFrac = 12.0f;    // faded mark radius (the wake widens as it dissipates)

        // --- splash ---
        const moth_ui::Color kSplashColor{ 0.85f, 0.90f, 0.95f, 1.0f }; // pale foam; alpha set per splash
        constexpr float kSplashAlpha = 0.5f;        // opacity of a fresh splash
        constexpr float kSplashStartRadiusFrac = 1.5f; // fresh radius, factor of the shot radius
        constexpr float kSplashEndRadiusFrac = 5.0f;   // faded radius (the ring spreads as it dies)

        // --- wreck ---
        const moth_ui::Color kWreckColor{ 0.16f, 0.16f, 0.18f, 1.0f }; // charred hull; alpha set per sink stage
    }

    void DrawTarget(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const& target = registry.get<MoveTarget>(ship);
        if (!target.active) {
            return;
        }
        moth_ui::FloatVec2 const shipPx = camera.WorldToScreen(registry.get<Physics>(ship).body->GetPosition());
        moth_ui::FloatVec2 const targetPx = camera.WorldToScreen(target.point);

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());

        // Dashed line from ship to target, drawn as short segments with gaps.
        graphics.SetColor(kLineColor);
        moth_ui::FloatVec2 const delta{ targetPx.x - shipPx.x, targetPx.y - shipPx.y };
        float const length = std::sqrt((delta.x * delta.x) + (delta.y * delta.y));
        if (length > 1.0f) {
            moth_ui::FloatVec2 const dir{ delta.x / length, delta.y / length };
            constexpr float dash = 10.0f;
            constexpr float gap = 8.0f;
            for (float s = 0.0f; s < length; s += dash + gap) {
                float const e = std::min(s + dash, length);
                graphics.DrawLineF({ shipPx.x + (dir.x * s), shipPx.y + (dir.y * s) },
                                   { shipPx.x + (dir.x * e), shipPx.y + (dir.y * e) });
            }
        }

        // Target marker: a small dot inside a ring.
        graphics.SetColor(kTargetColor);
        graphics.DrawFillCircleF(targetPx, 3.0f);
        graphics.DrawRectF(moth_ui::FloatRect{ { targetPx.x - 8.0f, targetPx.y - 8.0f },
                                               { targetPx.x + 8.0f, targetPx.y + 8.0f } });
    }

    void DrawWakes(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera) {
        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        // The scene otherwise draws opaque (BlendMode::Replace ignores alpha), so
        // switch to alpha blending for the fading marks, then hand it back so the
        // rest of the frame is unaffected.
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);
        for (auto entity : registry.view<Renderable, Wake>()) {
            auto const& renderable = registry.get<Renderable>(entity);
            auto const& wake = registry.get<Wake>(entity);
            float const startR = renderable.halfBeamM * kWakeStartBeamFrac;
            float const endR = renderable.halfBeamM * kWakeEndBeamFrac;
            // Each mark fades and widens as it ages: a fresh drop is a tight,
            // brighter spot, an old one a broad faint patch of dissipating foam.
            // The fade is a smoothstep on remaining life so alpha eases into zero
            // with no slope at the end — the mark dissolves rather than winking
            // out when it is finally culled.
            for (auto const& mark : wake.marks) {
                float const t = std::clamp(mark.age / kWakeLifetimeS, 0.0f, 1.0f);
                float const life = 1.0f - t;
                float const fade = life * life * (3.0f - (2.0f * life)); // smoothstep, soft landing at 0
                graphics.SetColor(moth_ui::Color{ kWakeColor.r, kWakeColor.g, kWakeColor.b,
                                                  kWakeAlpha * fade });
                float const radiusM = startR + ((endR - startR) * t);
                graphics.DrawFillCircleF(camera.WorldToScreen(mark.position), camera.MToPx(radiusM));
            }
        }
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawSplashes(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera) {
        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        // Alpha blend for the fading splashes, then hand it back so the rest of
        // the frame is unaffected — matching how the wakes are drawn.
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);
        for (auto entity : registry.view<Splash>()) {
            auto const& splash = registry.get<Splash>(entity);
            // Each splash expands from the shot's size and fades as it ages,
            // easing into zero with a smoothstep so it dissolves rather than
            // winking out — the same soft landing the wake marks use.
            float const t = std::clamp(splash.age / kSplashLifetimeS, 0.0f, 1.0f);
            float const life = 1.0f - t;
            float const fade = life * life * (3.0f - (2.0f * life)); // smoothstep, soft landing at 0
            graphics.SetColor(moth_ui::Color{ kSplashColor.r, kSplashColor.g, kSplashColor.b,
                                              kSplashAlpha * fade });
            float const startR = splash.radiusM * kSplashStartRadiusFrac;
            float const endR = splash.radiusM * kSplashEndRadiusFrac;
            float const radiusM = startR + ((endR - startR) * t);
            graphics.DrawFillCircleF(camera.WorldToScreen(splash.position), camera.MToPx(radiusM));
        }
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawShip(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        b2Body* body = registry.get<Physics>(ship).body;
        auto const& renderable = registry.get<Renderable>(ship);
        moth_ui::FloatVec2 const posPx = camera.WorldToScreen(body->GetPosition());
        float const degrees = body->GetAngle() * moth_ui::kRadToDeg;

        auto const transform = moth_ui::FloatMat4x4::Translation(posPx) *
                               moth_ui::FloatMat4x4::Rotation(degrees, { 0.0f, 0.0f });
        graphics.SetTransform(transform);

        float const halfLengthPx = camera.MToPx(renderable.halfLengthM);
        float const halfBeamPx = camera.MToPx(renderable.halfBeamM);

        // The boat outline: a beam bulge amidships tapering to a point at each
        // end (see hull_shape.h). The same eight vertices are the collision
        // fixture, so the silhouette matches what the ship bumps into.
        auto const outline = HullOutline<moth_ui::FloatVec2>(halfLengthPx, halfBeamPx,
                                                             renderable.foreShoulder, renderable.foreShoulderBeam,
                                                             renderable.aftShoulder, renderable.aftShoulderBeam);

        // A destroyed hull draws as a uniform charred wreck — no heading bow —
        // solid grey through the burn phase, then alpha-fading to nothing as it
        // slips under over the sink phase (see kSinkBurnS / kSinkDurationS).
        if (auto const* sinking = registry.try_get<Sinking>(ship); sinking != nullptr) {
            float const sinkT = std::clamp((sinking->age - kSinkBurnS) / kSinkDurationS, 0.0f, 1.0f);
            graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);
            graphics.SetColor(moth_ui::Color{ kWreckColor.r, kWreckColor.g, kWreckColor.b, 1.0f - sinkT });
            graphics.DrawFillPolygonF(outline.data(), outline.size());
            graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
            graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
            return;
        }

        graphics.SetColor(renderable.color);
        graphics.DrawFillPolygonF(outline.data(), outline.size());

        // Overlay the bow in the heading colour — the forward triangle from the
        // two taper shoulders to the tip — so the ship's facing stays readable.
        std::array<moth_ui::FloatVec2, 3> const bow{ { outline[0], outline[1], outline[7] } };
        graphics.SetColor(kBow);
        graphics.DrawFillPolygonF(bow.data(), bow.size());

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
    }

    void DrawArcs(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const* armament = registry.try_get<Armament>(ship);
        if (armament == nullptr) {
            return;
        }
        b2Body* body = registry.get<Physics>(ship).body;
        b2Vec2 const shipPos = body->GetPosition();
        float const shipAngle = body->GetAngle();
        float const cosA = std::cos(shipAngle);
        float const sinA = std::sin(shipAngle);

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());

        // Each weapon's arc: two radial edges out to its range plus the outer
        // sweep between them, brightening when a target sits inside. The arc
        // originates from the mount's world position, not the hull centre.
        for (auto const& weapon : armament->weapons) {
            if (!weapon.showArc) {
                continue;
            }
            b2Vec2 const off = weapon.mountOffset;
            b2Vec2 const mountWorld{ shipPos.x + (cosA * off.x) - (sinA * off.y),
                                     shipPos.y + (sinA * off.x) + (cosA * off.y) };
            moth_ui::FloatVec2 const originPx = camera.WorldToScreen(mountWorld);

            float const rangePx = camera.MToPx(weapon.range);
            float const arcCentre = shipAngle + weapon.bearing;
            float const start = arcCentre - weapon.arcHalfAngle;

            auto edge = [&](float angle) {
                return moth_ui::FloatVec2{ originPx.x + (rangePx * std::cos(angle)),
                                           originPx.y + (rangePx * std::sin(angle)) };
            };

            graphics.SetColor(weapon.hasTarget ? kArcActiveColor : kArcColor);

            // Subdivide the outer sweep by its pixel length so wide or far-
            // reaching arcs stay smooth — a fixed count left a full-circle arc
            // looking angular. ~4px per segment matches the density
            // DrawFillCircleF uses; clamped so a sliver still curves and a huge
            // arc doesn't blow up the line count.
            float const arcAngle = 2.0f * weapon.arcHalfAngle;
            int const segments = std::clamp(static_cast<int>(std::ceil((rangePx * arcAngle) / 4.0f)), 8, 64);
            float const step = arcAngle / static_cast<float>(segments);
            moth_ui::FloatVec2 prev = edge(start);
            graphics.DrawLineF(originPx, prev); // near radial edge
            for (int i = 1; i <= segments; ++i) {
                moth_ui::FloatVec2 const point = edge(start + (step * static_cast<float>(i)));
                graphics.DrawLineF(prev, point);
                prev = point;
            }
            graphics.DrawLineF(originPx, prev); // far radial edge
        }
    }

    void DrawWeaponSpread(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const* armament = registry.try_get<Armament>(ship);
        if (armament == nullptr) {
            return;
        }
        b2Body* body = registry.get<Physics>(ship).body;
        b2Vec2 const shipPos = body->GetPosition();
        float const shipAngle = body->GetAngle();
        float const cosA = std::cos(shipAngle);
        float const sinA = std::sin(shipAngle);

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);
        graphics.SetColor(kSpreadColor);

        for (auto const& weapon : armament->weapons) {
            if (!weapon.showSpread || weapon.target == entt::null || !registry.valid(weapon.target)) {
                continue;
            }
            // Aim line from the mount's world position out to the aim point.
            b2Vec2 const off = weapon.mountOffset;
            b2Vec2 const mountWorld{ shipPos.x + (cosA * off.x) - (sinA * off.y),
                                     shipPos.y + (sinA * off.x) + (cosA * off.y) };
            moth_ui::FloatVec2 const originPx = camera.WorldToScreen(mountWorld);
            moth_ui::FloatVec2 const aimPx = camera.WorldToScreen(weapon.aimWorld);
            graphics.DrawLineF(originPx, aimPx);

            // The spread disc over the aim point: a shot may land anywhere within
            // it, so its size shows the weapon's accuracy at this range. Full
            // circle as a polyline, subdivided by pixel circumference like the arcs.
            float const radiusPx = camera.MToPx(weapon.spreadRadiusM);
            int const segments = std::clamp(static_cast<int>(std::ceil((2.0f * b2_pi * radiusPx) / 4.0f)), 16, 128);
            float const step = (2.0f * b2_pi) / static_cast<float>(segments);
            auto point = [&](int i) {
                float const a = step * static_cast<float>(i);
                return moth_ui::FloatVec2{ aimPx.x + (radiusPx * std::cos(a)),
                                           aimPx.y + (radiusPx * std::sin(a)) };
            };
            moth_ui::FloatVec2 prev = point(0);
            for (int i = 1; i <= segments; ++i) {
                moth_ui::FloatVec2 const cur = point(i);
                graphics.DrawLineF(prev, cur);
                prev = cur;
            }
        }
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawTargetMarker(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const* order = registry.try_get<FireOrder>(ship);
        if (order == nullptr || order->target == entt::null || !registry.valid(order->target)) {
            return;
        }
        auto const* renderable = registry.try_get<Renderable>(order->target);
        auto const* physics = registry.try_get<Physics>(order->target);
        if (renderable == nullptr || physics == nullptr) {
            return;
        }

        // Red once any gun bears, green while the contact is designated but out
        // of reach of all of them. The ring answers "can I hit that from here?"
        // at a glance — the same question the Target window's gun count answers
        // in words — so it reads off the weapons' own bearing test rather than
        // re-deriving range, and cannot disagree with what the guns will do.
        bool armed = false;
        if (auto const* armament = registry.try_get<Armament>(ship); armament != nullptr) {
            for (auto const& weapon : armament->weapons) {
                armed = armed || weapon.hasTarget;
            }
        }

        // A circle that clears the hull whatever way it is pointing: the hull's
        // circumscribed radius (bow corner to centre), padded so the ring doesn't
        // graze it. Floored in pixels so a contact zoomed down to a speck still
        // carries a mark big enough to find — at survey zoom the mark is the only
        // way to see which speck you are fighting.
        constexpr float kPadFrac = 1.35f;
        constexpr float kMinRadiusPx = 14.0f;
        float const hullRadiusM = std::sqrt((renderable->halfLengthM * renderable->halfLengthM) +
                                            (renderable->halfBeamM * renderable->halfBeamM));
        float const radiusPx = std::max(camera.MToPx(hullRadiusM * kPadFrac), kMinRadiusPx);
        moth_ui::FloatVec2 const centrePx = camera.WorldToScreen(physics->body->GetPosition());

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);
        graphics.SetColor(armed ? kTargetRingArmedColor : kTargetRingColor);

        // Full circle as a polyline, subdivided by pixel circumference so it stays
        // smooth at any zoom — the same ~4px-per-segment density the arcs use.
        int const segments = std::clamp(static_cast<int>(std::ceil((2.0f * b2_pi * radiusPx) / 4.0f)), 24, 128);
        float const step = (2.0f * b2_pi) / static_cast<float>(segments);
        auto point = [&](int i) {
            float const a = step * static_cast<float>(i);
            return moth_ui::FloatVec2{ centrePx.x + (radiusPx * std::cos(a)),
                                       centrePx.y + (radiusPx * std::sin(a)) };
        };
        moth_ui::FloatVec2 prev = point(0);
        for (int i = 1; i <= segments; ++i) {
            moth_ui::FloatVec2 const cur = point(i);
            graphics.DrawLineF(prev, cur);
            prev = cur;
        }
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawAggroRing(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const* aggro = registry.try_get<Aggro>(ship);
        if (aggro == nullptr) {
            return;
        }
        AggroTuning const& tuning = AggroTuningRef();
        if (!tuning.showRings) {
            return;
        }

        moth_ui::FloatVec2 const centrePx = camera.WorldToScreen(registry.get<Physics>(ship).body->GetPosition());
        float const radiusPx = camera.MToPx(tuning.aggroRangeM);

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        // Faint while patrolling, bright once locked on, so the ring reads as the
        // exact threshold the player has to cross. Alpha-blended like the wakes.
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);
        graphics.SetColor(aggro->target != entt::null ? kAggroRingActiveColor : kAggroRingColor);

        // Full circle as a polyline, subdivided by pixel circumference so it stays
        // smooth at any zoom — the same ~4px-per-segment density the arcs use.
        int const segments = std::clamp(static_cast<int>(std::ceil((2.0f * b2_pi * radiusPx) / 4.0f)), 24, 128);
        float const step = (2.0f * b2_pi) / static_cast<float>(segments);
        auto point = [&](int i) {
            float const a = step * static_cast<float>(i);
            return moth_ui::FloatVec2{ centrePx.x + (radiusPx * std::cos(a)),
                                       centrePx.y + (radiusPx * std::sin(a)) };
        };
        moth_ui::FloatVec2 prev = point(0);
        for (int i = 1; i <= segments; ++i) {
            moth_ui::FloatVec2 const cur = point(i);
            graphics.DrawLineF(prev, cur);
            prev = cur;
        }
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawProjectiles(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera) {
        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        for (auto entity : registry.view<Projectile>()) {
            auto const& projectile = registry.get<Projectile>(entity);
            graphics.SetColor(projectile.color);
            graphics.DrawFillCircleF(camera.WorldToScreen(projectile.position), camera.MToPx(projectile.radiusM));
        }
    }
}
