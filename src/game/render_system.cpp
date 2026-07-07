#include "game/render_system.h"

#include "game/camera.h"
#include "game/components.h"

#include <moth_graphics/graphics/igraphics.h>
#include <moth_ui/utils/rect.h>
#include <moth_ui/utils/transform.h>

#include <algorithm>
#include <cmath>

namespace naval {
    namespace {
        // --- colours ---
        const moth_ui::Color kSea{ 0.10f, 0.20f, 0.32f, 1.0f };
        const moth_ui::Color kBow{ 0.90f, 0.35f, 0.30f, 1.0f };
        const moth_ui::Color kTargetColor{ 0.95f, 0.85f, 0.40f, 1.0f };
        const moth_ui::Color kLineColor{ 0.55f, 0.65f, 0.75f, 1.0f };
        const moth_ui::Color kArcColor{ 0.35f, 0.45f, 0.55f, 0.6f };       // firing arc at rest
        const moth_ui::Color kArcActiveColor{ 0.95f, 0.55f, 0.35f, 0.9f }; // arc with a target in it
    }

    void DrawSea(moth_graphics::graphics::IGraphics& graphics) {
        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        graphics.SetColor(kSea);
        graphics.Clear();
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

        // Hull profile: half-beam at a point x along the keel (local +x is the
        // bow). A rounded stern cap, parallel midships, and a sharp raked bow
        // that tapers to a point — a sharp capsule. There is no filled-polygon
        // primitive, so the silhouette is laid down as contiguous strips whose
        // height follows this profile; the whole set rotates with the transform.
        float const sternCapEnd = -halfLengthPx + halfBeamPx; // aft semicircle
        float const bowStart = halfLengthPx * 0.60f;          // taper begins just fwd of amidships
        auto halfBeamAt = [&](float x) -> float {
            if (x <= sternCapEnd) {
                float const d = x - sternCapEnd;
                float const r2 = (halfBeamPx * halfBeamPx) - (d * d);
                return r2 > 0.0f ? std::sqrt(r2) : 0.0f;
            }
            if (x >= bowStart) {
                float const t = (halfLengthPx - x) / (halfLengthPx - bowStart);
                return halfBeamPx * std::clamp(t, 0.0f, 1.0f);
            }
            return halfBeamPx;
        };

        // Forward strips take the bow colour so heading stays readable.
        constexpr int kHullStrips = 48;
        float const dx = (2.0f * halfLengthPx) / kHullStrips;
        float const bowMarkStart = halfLengthPx * 0.55f;
        for (int i = 0; i < kHullStrips; ++i) {
            float const x0 = -halfLengthPx + (dx * static_cast<float>(i));
            float const x1 = x0 + dx;
            float const mid = (x0 + x1) * 0.5f;
            float const h = halfBeamAt(mid);
            graphics.SetColor(mid >= bowMarkStart ? kBow : renderable.color);
            graphics.DrawFillRectF(moth_ui::FloatRect{ { x0, -h }, { x1, h } });
        }

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
    }

    void DrawArcs(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const* armament = registry.try_get<Armament>(ship);
        if (armament == nullptr) {
            return;
        }
        b2Body* body = registry.get<Physics>(ship).body;
        moth_ui::FloatVec2 const centerPx = camera.WorldToScreen(body->GetPosition());
        float const shipAngle = body->GetAngle();

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());

        // Each weapon's arc: two radial edges out to its range plus the outer
        // sweep between them, brightening when a target sits inside.
        for (auto const& weapon : armament->weapons) {
            float const rangePx = camera.MToPx(weapon.range);
            float const arcCentre = shipAngle + weapon.bearing;
            float const start = arcCentre - weapon.arcHalfAngle;

            auto edge = [&](float angle) {
                return moth_ui::FloatVec2{ centerPx.x + (rangePx * std::cos(angle)),
                                           centerPx.y + (rangePx * std::sin(angle)) };
            };

            graphics.SetColor(weapon.hasTarget ? kArcActiveColor : kArcColor);

            constexpr int kSegments = 16;
            float const step = (2.0f * weapon.arcHalfAngle) / kSegments;
            moth_ui::FloatVec2 prev = edge(start);
            graphics.DrawLineF(centerPx, prev); // near radial edge
            for (int i = 1; i <= kSegments; ++i) {
                moth_ui::FloatVec2 const point = edge(start + (step * static_cast<float>(i)));
                graphics.DrawLineF(prev, point);
                prev = point;
            }
            graphics.DrawLineF(centerPx, prev); // far radial edge
        }
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
