#include "game/render_system.h"

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

        // The boat outline: a beam bulge amidships tapering to a point at each
        // end (see hull_shape.h). The same eight vertices are the collision
        // fixture, so the silhouette matches what the ship bumps into.
        auto const outline = HullOutline<moth_ui::FloatVec2>(halfLengthPx, halfBeamPx,
                                                             renderable.foreShoulder, renderable.foreShoulderBeam,
                                                             renderable.aftShoulder, renderable.aftShoulderBeam);
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

            constexpr int kSegments = 16;
            float const step = (2.0f * weapon.arcHalfAngle) / kSegments;
            moth_ui::FloatVec2 prev = edge(start);
            graphics.DrawLineF(originPx, prev); // near radial edge
            for (int i = 1; i <= kSegments; ++i) {
                moth_ui::FloatVec2 const point = edge(start + (step * static_cast<float>(i)));
                graphics.DrawLineF(prev, point);
                prev = point;
            }
            graphics.DrawLineF(originPx, prev); // far radial edge
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
