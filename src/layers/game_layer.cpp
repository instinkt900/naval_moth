#include "layers/game_layer.h"

#include "game/components.h"
#include "game/propulsion_system.h"
#include "game/units.h"

#include <moth_graphics/graphics/igraphics.h>
#include <moth_ui/events/event_dispatch.h>
#include <moth_ui/utils/rect.h>
#include <moth_ui/utils/transform.h>

#include <algorithm>
#include <cmath>

namespace naval {
    namespace {
        // --- physics feel (tune these to taste) ---
        constexpr float kLinearDamping = 0.5f;  // water drag; lower = more coasting/drift
        constexpr float kAngularDamping = 3.0f; // settles residual spin once uncommanded

        // --- ship spec ---
        constexpr float kHullHalfLenPx = 35.0f;
        constexpr float kHullHalfBeamPx = 13.0f;
        constexpr float kMaxThrust = 12.0f;      // full-power forward force
        constexpr float kMinTurnRate = 0.5f;     // yaw rate dead in the water (rad/s)
        constexpr float kTurnRate = 2.5f;        // yaw rate at cruise (rad/s)
        constexpr float kRudderSpeed = 10.0f;    // forward speed (m/s) at which turning saturates
        constexpr float kPowerDistanceM = 8.0f;  // clicks beyond this drive at full power

        // --- colours ---
        const moth_ui::Color kSea{ 0.10f, 0.20f, 0.32f, 1.0f };
        const moth_ui::Color kHull{ 0.85f, 0.82f, 0.70f, 1.0f };
        const moth_ui::Color kBow{ 0.90f, 0.35f, 0.30f, 1.0f };
        const moth_ui::Color kTargetColor{ 0.95f, 0.85f, 0.40f, 1.0f };
        const moth_ui::Color kLineColor{ 0.55f, 0.65f, 0.75f, 1.0f };
    }

    GameLayer::GameLayer(moth_graphics::graphics::IGraphics& graphics, int widthPx, int heightPx)
        : m_graphics(graphics)
        , m_world(b2Vec2{ 0.0f, 0.0f }) { // top-down: no gravity
        m_ship = m_registry.create();

        b2BodyDef bodyDef;
        bodyDef.type = b2_dynamicBody;
        bodyDef.position = PxToWorld({ static_cast<float>(widthPx) * 0.5f, static_cast<float>(heightPx) * 0.5f });
        bodyDef.linearDamping = kLinearDamping;
        bodyDef.angularDamping = kAngularDamping;
        b2Body* body = m_world.CreateBody(&bodyDef);

        b2PolygonShape hull;
        hull.SetAsBox(PxToM(kHullHalfLenPx), PxToM(kHullHalfBeamPx));
        b2FixtureDef fixtureDef;
        fixtureDef.shape = &hull;
        fixtureDef.density = 1.0f;
        body->CreateFixture(&fixtureDef);

        m_registry.emplace<Physics>(m_ship, Physics{ body });
        m_registry.emplace<Propulsion>(m_ship, Propulsion{ kMaxThrust, kMinTurnRate, kTurnRate, kRudderSpeed, kPowerDistanceM });
        m_registry.emplace<Renderable>(m_ship, Renderable{ kHull, kHullHalfLenPx, kHullHalfBeamPx });
        m_registry.emplace<MoveTarget>(m_ship, MoveTarget{ b2Vec2{ 0.0f, 0.0f }, false });
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
        UpdatePropulsion(m_registry);
        m_world.Step(static_cast<float>(ticks) / 1000.0f, 8, 3);
    }

    void GameLayer::Draw() {
        m_graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        m_graphics.SetColor(kSea);
        m_graphics.Clear();

        DrawTarget(m_ship);
        DrawShip(m_ship);
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
}
