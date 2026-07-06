#include "layers/game_layer.h"

#include "game/combat_system.h"
#include "game/components.h"
#include "game/propulsion_system.h"
#include "game/ship_factory.h"
#include "game/units.h"

#include <moth_graphics/graphics/igraphics.h>
#include <moth_ui/events/event_dispatch.h>
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

    GameLayer::GameLayer(moth_graphics::graphics::IGraphics& graphics, int widthPx, int heightPx)
        : m_graphics(graphics)
        , m_world(b2Vec2{ 0.0f, 0.0f }) // top-down: no gravity
        , m_db(defs::Database::Load("assets/data")) {
        float const cx = static_cast<float>(widthPx) * 0.5f;
        float const cy = static_cast<float>(heightPx) * 0.5f;

        m_ship = SpawnHull(m_registry, m_world, m_db, "cutter", PxToWorld({ cx, cy }));
        // A stationary target off the bow-quarter, within broadside reach once
        // the player brings a beam to bear.
        m_enemy = SpawnEnemy(m_registry, m_world, m_db, "target_dummy", PxToWorld({ cx + 160.0f, cy - 160.0f }));
    }

    bool GameLayer::OnEvent(moth_ui::Event const& event) {
        moth_ui::EventDispatch dispatch(event);
        dispatch.Dispatch(this, &GameLayer::OnMouseDown);
        return dispatch.GetHandled();
    }

    bool GameLayer::OnMouseDown(moth_ui::EventMouseDown const& event) {
        auto const pos = event.GetPosition();
        auto& target = m_registry.get<MoveTarget>(m_ship);
        // Each click moves the single target; nothing is queued.
        target.point = PxToWorld({ static_cast<float>(pos.x), static_cast<float>(pos.y) });
        target.active = true;
        return true;
    }

    void GameLayer::Update(uint32_t ticks) {
        float const dt = static_cast<float>(ticks) / 1000.0f;
        UpdatePropulsion(m_registry);
        UpdateWeapons(m_registry, dt);
        UpdateProjectiles(m_registry, dt);
        m_world.Step(dt, 8, 3);
    }

    void GameLayer::Draw() {
        m_graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        m_graphics.SetColor(kSea);
        m_graphics.Clear();

        DrawArcs(m_ship);
        DrawTarget(m_ship);
        DrawShip(m_enemy);
        DrawShip(m_ship);
        DrawProjectiles();
    }

    void GameLayer::DrawTarget(entt::entity ship) {
        auto const& target = m_registry.get<MoveTarget>(ship);
        if (!target.active) {
            return;
        }
        moth_ui::FloatVec2 const shipPx = WorldToPx(m_registry.get<Physics>(ship).body->GetPosition());
        moth_ui::FloatVec2 const targetPx = WorldToPx(target.point);

        m_graphics.SetTransform(moth_ui::FloatMat4x4::Identity());

        // Dashed line from ship to target, drawn as short segments with gaps.
        m_graphics.SetColor(kLineColor);
        moth_ui::FloatVec2 const delta{ targetPx.x - shipPx.x, targetPx.y - shipPx.y };
        float const length = std::sqrt((delta.x * delta.x) + (delta.y * delta.y));
        if (length > 1.0f) {
            moth_ui::FloatVec2 const dir{ delta.x / length, delta.y / length };
            constexpr float dash = 10.0f;
            constexpr float gap = 8.0f;
            for (float s = 0.0f; s < length; s += dash + gap) {
                float const e = std::min(s + dash, length);
                m_graphics.DrawLineF({ shipPx.x + (dir.x * s), shipPx.y + (dir.y * s) },
                                     { shipPx.x + (dir.x * e), shipPx.y + (dir.y * e) });
            }
        }

        // Target marker: a small dot inside a ring.
        m_graphics.SetColor(kTargetColor);
        m_graphics.DrawFillCircleF(targetPx, 3.0f);
        m_graphics.DrawRectF(moth_ui::FloatRect{ { targetPx.x - 8.0f, targetPx.y - 8.0f },
                                                 { targetPx.x + 8.0f, targetPx.y + 8.0f } });
    }

    void GameLayer::DrawShip(entt::entity ship) {
        b2Body* body = m_registry.get<Physics>(ship).body;
        auto const& renderable = m_registry.get<Renderable>(ship);
        moth_ui::FloatVec2 const posPx = WorldToPx(body->GetPosition());
        float const degrees = body->GetAngle() * moth_ui::kRadToDeg;

        auto const transform = moth_ui::FloatMat4x4::Translation(posPx) *
                               moth_ui::FloatMat4x4::Rotation(degrees, { 0.0f, 0.0f });
        m_graphics.SetTransform(transform);

        // Hull, long axis along local +x (forward).
        m_graphics.SetColor(renderable.color);
        m_graphics.DrawFillRectF(moth_ui::FloatRect{ { -renderable.halfLengthPx, -renderable.halfBeamPx },
                                                     { renderable.halfLengthPx, renderable.halfBeamPx } });

        // Bow marker at the forward end so heading is readable.
        m_graphics.SetColor(kBow);
        m_graphics.DrawFillRectF(moth_ui::FloatRect{ { renderable.halfLengthPx * 0.55f, -renderable.halfBeamPx },
                                                     { renderable.halfLengthPx, renderable.halfBeamPx } });

        m_graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
    }

    void GameLayer::DrawArcs(entt::entity ship) {
        auto const* armament = m_registry.try_get<Armament>(ship);
        if (armament == nullptr) {
            return;
        }
        b2Body* body = m_registry.get<Physics>(ship).body;
        moth_ui::FloatVec2 const centerPx = WorldToPx(body->GetPosition());
        float const shipAngle = body->GetAngle();

        m_graphics.SetTransform(moth_ui::FloatMat4x4::Identity());

        // Each weapon's arc: two radial edges out to its range plus the outer
        // sweep between them, brightening when a target sits inside.
        for (auto const& weapon : armament->weapons) {
            float const rangePx = MToPx(weapon.range);
            float const arcCentre = shipAngle + weapon.bearing;
            float const start = arcCentre - weapon.arcHalfAngle;

            auto edge = [&](float angle) {
                return moth_ui::FloatVec2{ centerPx.x + (rangePx * std::cos(angle)),
                                           centerPx.y + (rangePx * std::sin(angle)) };
            };

            m_graphics.SetColor(weapon.hasTarget ? kArcActiveColor : kArcColor);

            constexpr int kSegments = 16;
            float const step = (2.0f * weapon.arcHalfAngle) / kSegments;
            moth_ui::FloatVec2 prev = edge(start);
            m_graphics.DrawLineF(centerPx, prev); // near radial edge
            for (int i = 1; i <= kSegments; ++i) {
                moth_ui::FloatVec2 const point = edge(start + (step * static_cast<float>(i)));
                m_graphics.DrawLineF(prev, point);
                prev = point;
            }
            m_graphics.DrawLineF(centerPx, prev); // far radial edge
        }
    }

    void GameLayer::DrawProjectiles() {
        m_graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        for (auto entity : m_registry.view<Projectile>()) {
            auto const& projectile = m_registry.get<Projectile>(entity);
            m_graphics.SetColor(projectile.color);
            m_graphics.DrawFillCircleF(WorldToPx(projectile.position), projectile.radiusPx);
        }
    }
}
