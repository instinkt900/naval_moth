#include "game/render_system.h"

#include "game/aggro_system.h"
#include "game/camera.h"
#include "game/combat_system.h"
#include "game/components.h"
#include "game/hull_shape.h"

#include <moth_graphics/graphics/igraphics.h>
#include <moth_ui/utils/rect.h>
#include <moth_ui/utils/transform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <unordered_map>

namespace naval {
    namespace {
        // --- colours ---
        const moth_ui::Color kBow{ 0.90f, 0.35f, 0.30f, 1.0f };
        const moth_ui::Color kTargetColor{ 0.95f, 0.85f, 0.40f, 1.0f };
        const moth_ui::Color kLineColor{ 0.55f, 0.65f, 0.75f, 1.0f };
        const moth_ui::Color kArcEnabledColor{ 0.70f, 0.25f, 0.25f, 0.45f };  // arc of an armed gun with nothing bearing
        const moth_ui::Color kArcActiveColor{ 0.95f, 0.55f, 0.35f, 0.9f };    // arc with a target in it
        const moth_ui::Color kDeadZoneColor{ 0.85f, 0.15f, 0.15f, 0.12f };    // launcher minimum range: munitions dud inside
        const moth_ui::Color kAggroRingColor{ 0.80f, 0.30f, 0.30f, 0.20f };       // aggro range, ship still patrolling
        const moth_ui::Color kAggroRingActiveColor{ 0.95f, 0.35f, 0.30f, 0.65f }; // aggro range once the ship has locked on
        const moth_ui::Color kSpreadColor{ 0.95f, 0.85f, 0.35f, 0.6f };           // debug spread preview (aim line + disc)
        const moth_ui::Color kTargetRingColor{ 0.35f, 0.90f, 0.40f, 0.85f };      // designated contact, no gun bearing on it
        const moth_ui::Color kTargetRingArmedColor{ 0.95f, 0.25f, 0.25f, 0.90f }; // designated contact, under the guns
        const moth_ui::Color kCiwsTracerColor{ 1.00f, 0.85f, 0.35f, 0.9f };       // point-defence tracer rounds
        const moth_ui::Color kCiwsMuzzleColor{ 1.00f, 0.95f, 0.70f, 1.0f };       // point-defence muzzle flash and target sparkle
        const moth_ui::Color kContactColor{ 0.45f, 0.85f, 0.75f, 0.90f };         // radar contact blip (any contact the sweep paints)
        const moth_ui::Color kStaleContactColor{ 0.55f, 0.60f, 0.62f, 0.75f };    // lost-track ghost, greyed; alpha fades over kContactDecayS
        const moth_ui::Color kRadarRingColor{ 0.45f, 0.85f, 0.75f, 0.14f };       // active radar reach, drawn while radiating
        const moth_ui::Color kBearingColor{ 0.95f, 0.88f, 0.25f, 0.85f };         // passive ESM bearing line (direction, no range; length = strength)
        const moth_ui::Color kTmaFixColor{ 1.00f, 0.72f, 0.30f, 0.90f };          // solved TMA estimate: uncertainty ring + course stalk (an estimate, not a hard fix)
        const moth_ui::Color kOwnBlipColor{ 0.55f, 0.85f, 1.00f, 0.95f };         // own-ship mark on the plot (ring + heading stalk), distinct from contacts

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

        // --- shapes ---
        // Curves are drawn as polylines subdivided by how long they actually are
        // on screen: about one segment per kSweepSegmentPx of arc, which matches
        // the density DrawFillCircleF uses. Measuring in pixels rather than
        // counting segments is what keeps a curve smooth at any zoom without
        // spending vertices on one that has shrunk to a dot — the same circle is
        // a speck at survey zoom and fills the view up close.
        //
        // Clamped at both ends: a sliver still curves, and a battleship's arc at
        // full zoom can't blow up the line count.
        constexpr float kSweepSegmentPx = 4.0f;
        constexpr int kMinSweepSegments = 8;
        constexpr int kMaxSweepSegments = 128;

        // An arc about a screen-space centre, from `startAngle` through `sweep`
        // radians, as a polyline. Colour, blend mode and transform are the
        // caller's — this only puts the line down.
        void DrawSweep(moth_graphics::graphics::IGraphics& graphics, moth_ui::FloatVec2 centrePx,
                       float radiusPx, float startAngle, float sweep) {
            int const segments = std::clamp(
                static_cast<int>(std::ceil((radiusPx * std::abs(sweep)) / kSweepSegmentPx)),
                kMinSweepSegments, kMaxSweepSegments);
            float const step = sweep / static_cast<float>(segments);
            auto point = [&](int i) {
                float const a = startAngle + (step * static_cast<float>(i));
                return moth_ui::FloatVec2{ centrePx.x + (radiusPx * std::cos(a)),
                                           centrePx.y + (radiusPx * std::sin(a)) };
            };
            moth_ui::FloatVec2 prev = point(0);
            for (int i = 1; i <= segments; ++i) {
                moth_ui::FloatVec2 const cur = point(i);
                graphics.DrawLineF(prev, cur);
                prev = cur;
            }
        }

        // A full circle — a sweep all the way round.
        void DrawCircle(moth_graphics::graphics::IGraphics& graphics, moth_ui::FloatVec2 centrePx,
                        float radiusPx) {
            DrawSweep(graphics, centrePx, radiusPx, 0.0f, 2.0f * b2_pi);
        }
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

    namespace {
        // An estimated position: an uncertainty ring that tightens as confidence
        // firms and a course stalk for the estimated heading and speed. A fixed
        // screen size so it reads at any zoom, its alpha riding confidence so a
        // fading track dims. `colour` tells a live fix (heard now) from a lost one
        // being dead-reckoned on.
        void DrawEstimate(moth_graphics::graphics::IGraphics& graphics, Camera const& camera,
                          b2Vec2 pos, b2Vec2 vel, float confidence, moth_ui::Color colour) {
            moth_ui::FloatVec2 const estPx = camera.WorldToScreen(pos);
            constexpr float kUncMaxPx = 46.0f;
            constexpr float kUncMinPx = 10.0f;
            float const uncPx = kUncMaxPx + ((kUncMinPx - kUncMaxPx) * confidence);
            graphics.SetColor(
                moth_ui::Color{ colour.r, colour.g, colour.b, colour.a * (0.35f + (0.65f * confidence)) });
            DrawCircle(graphics, estPx, uncPx);
            constexpr float kCourseLeadS = 8.0f;
            b2Vec2 const ahead{ pos.x + (vel.x * kCourseLeadS), pos.y + (vel.y * kCourseLeadS) };
            graphics.DrawLineF(estPx, camera.WorldToScreen(ahead));
        }

        // The viewer's passive picture. For a live bearing contact heard but not
        // positioned, a bearing struck out from own ship along its direction — a
        // direction with the range deliberately unknown — whose length reads the
        // signal strength (a loud, near or big emitter reaches toward passive range,
        // a faint one is a short stub: "big or near" without telling the two apart,
        // see Contact::strength). Where the TMA solver has ranged one, instead the
        // amber line runs to the estimate with its uncertainty ring and course
        // stalk. A track whose contact has since been lost is dead-reckoned on and
        // fading (see UpdateTMA): it draws greyed and with no bearing line — memory,
        // not a live cut. Everything is built in world space from the stored bearing
        // or estimate, never the hull's true position, so a bare bearing leaks no
        // range.
        void DrawPassiveBearings(moth_graphics::graphics::IGraphics& graphics, Camera const& camera,
                                 b2Vec2 selfPos, moth_ui::FloatVec2 selfPx, float passiveRangeM,
                                 ContactPicture const& picture, TrackFile const* trackFile) {
            constexpr float kMinLenFrac = 0.12f; // shortest stub, as a fraction of the drawn reach
            constexpr float kLenScale = 0.1f;    // stub length as a fraction of passive reach at full strength
            for (auto const& entry : picture.contacts) {
                Contact const& contact = entry.second;
                if (contact.level != DetectLevel::Bearing) {
                    continue;
                }

                TmaTrack const* track = nullptr;
                if (trackFile != nullptr) {
                    auto const it = trackFile->tracks.find(entry.first);
                    if (it != trackFile->tracks.end() && it->second.solved) {
                        track = &it->second;
                    }
                }

                if (track != nullptr) {
                    graphics.SetColor(kBearingColor);
                    graphics.DrawLineF(selfPx, camera.WorldToScreen(track->position));
                    DrawEstimate(graphics, camera, track->position, track->velocity, track->confidence,
                                 kTmaFixColor);
                    continue;
                }

                graphics.SetColor(kBearingColor);
                float const len =
                    passiveRangeM * kLenScale * (kMinLenFrac + ((1.0f - kMinLenFrac) * contact.strength));
                b2Vec2 const end{ selfPos.x + (len * std::cos(contact.bearing)),
                                  selfPos.y + (len * std::sin(contact.bearing)) };
                graphics.DrawLineF(selfPx, camera.WorldToScreen(end));
            }

            // Lost TMA tracks: a solved track whose contact is no longer a live
            // bearing is held on, dead-reckoned and fading, as a decaying estimate.
            // Greyed and with no bearing line, since there is no live cut to draw.
            if (trackFile == nullptr) {
                return;
            }
            for (auto const& entry : trackFile->tracks) {
                TmaTrack const& track = entry.second;
                if (!track.solved) {
                    continue;
                }
                auto const pit = picture.contacts.find(entry.first);
                if (pit != picture.contacts.end() && pit->second.level == DetectLevel::Bearing) {
                    continue; // live, already drawn above
                }
                DrawEstimate(graphics, camera, track.position, track.velocity, track.confidence,
                             kStaleContactColor);
            }
        }
    }

    void DrawContacts(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity viewer) {
        auto const* sensors = registry.try_get<Sensors>(viewer);
        auto const* picture = registry.try_get<ContactPicture>(viewer);
        if (sensors == nullptr || picture == nullptr) {
            return;
        }

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);

        b2Vec2 const selfPos = registry.get<Physics>(viewer).body->GetPosition();
        moth_ui::FloatVec2 const selfPx = camera.WorldToScreen(selfPos);

        // Passive ESM bearings and any TMA fixes, drawn whatever own radar is doing
        // — listening is always on.
        DrawPassiveBearings(graphics, camera, selfPos, selfPx, sensors->passiveRangeM, *picture,
                            registry.try_get<TrackFile>(viewer));

        // Own ship's own mark on the plot, drawn whatever the radar is doing —
        // own position is never in doubt. A ring with a stalk struck out along the
        // heading, distinct from the open-diamond contact blips so the player picks
        // themselves out of the plot at a glance, and the one mark that carries a
        // heading rather than only a position. Screen-space pixels like the blips,
        // so it holds its size at any zoom; the heading is the body's angle used
        // directly, the same convention DrawShip lays the hull down with.
        graphics.SetColor(kOwnBlipColor);
        float const heading = registry.get<Physics>(viewer).body->GetAngle();
        constexpr float kOwnRingPx = 6.0f;
        constexpr float kOwnStalkPx = 15.0f;
        DrawCircle(graphics, selfPx, kOwnRingPx);
        graphics.DrawLineF(selfPx, { selfPx.x + (kOwnStalkPx * std::cos(heading)),
                                     selfPx.y + (kOwnStalkPx * std::sin(heading)) });

        // Every positioned mark is a fixed screen-size diamond at the contact's
        // last-known position — not scaled to the hull (a return is a position, not
        // a size) and kept distinct from the target ring (a circle) and the waypoint
        // marker (a boxed dot). Filled once the contact is identified, an open
        // outline until then, so a classified track reads apart from a bare return.
        constexpr float kBlipPx = 8.0f;
        auto drawDiamond = [&](moth_ui::FloatVec2 p, bool filled) {
            std::array<moth_ui::FloatVec2, 4> const pts{ { { p.x, p.y - kBlipPx },
                                                           { p.x + kBlipPx, p.y },
                                                           { p.x, p.y + kBlipPx },
                                                           { p.x - kBlipPx, p.y } } };
            if (filled) {
                graphics.DrawFillPolygonF(pts.data(), pts.size());
                return;
            }
            graphics.DrawLineF(pts[0], pts[1]);
            graphics.DrawLineF(pts[1], pts[2]);
            graphics.DrawLineF(pts[2], pts[3]);
            graphics.DrawLineF(pts[3], pts[0]);
        };

        // Active radar, only while radiating: its reach ring, and a live blip over
        // every fresh contact the sweep paints. A contact that has closed into
        // visual range keeps its mark atop its hull, so the plot stays complete and
        // a speck at survey zoom stays findable; a bearing-only contact has no
        // position to blip and is skipped. Drawn from lastPos, which for a fresh
        // contact is its true position this tick.
        if (sensors->activeOn) {
            graphics.SetColor(kRadarRingColor);
            DrawCircle(graphics, selfPx, camera.MToPx(sensors->activeRangeM));

            graphics.SetColor(kContactColor);
            for (auto const& entry : picture->contacts) {
                Contact const& contact = entry.second;
                if (!contact.hasPos || contact.fixStaleness != 0.0f) {
                    continue;
                }
                drawDiamond(camera.WorldToScreen(contact.lastPos), contact.identified);
            }
        }

        // Lost tracks: a positioned contact no longer held decays from its
        // last-known position rather than vanishing (see UpdateSensors). A diamond
        // greyed and fading over its remaining life, drawn whatever the radar is
        // doing — it is memory, not a live return — so a stale track reads apart
        // from a fresh one and stays put while the real hull steams on unseen.
        for (auto const& entry : picture->contacts) {
            Contact const& contact = entry.second;
            if (!contact.hasPos || contact.fixStaleness <= 0.0f) {
                continue;
            }
            // A sinking wreck stands in for a contact seen to die, so its track
            // isn't also ghosted over the top.
            if (registry.valid(entry.first) && registry.all_of<Sinking>(entry.first)) {
                continue;
            }
            float const life = std::clamp(1.0f - (contact.fixStaleness / kContactDecayS), 0.0f, 1.0f);
            graphics.SetColor(moth_ui::Color{ kStaleContactColor.r, kStaleContactColor.g,
                                              kStaleContactColor.b, kStaleContactColor.a * life });
            drawDiamond(camera.WorldToScreen(contact.lastPos), contact.identified);
        }
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawWakes(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera,
                   entt::entity viewer, ContactPicture const& picture) {
        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        // The scene otherwise draws opaque (BlendMode::Replace ignores alpha), so
        // switch to alpha blending for the fading marks, then hand it back so the
        // rest of the frame is unaffected.
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);
        for (auto entity : registry.view<Renderable, Wake>()) {
            // Same fog rule as the hull render in the layer: the viewer's own
            // trail, a *seen* contact's, or a wreck's. A merely ranged contact
            // leaves no wake any more than it draws a hull, and an undetected one
            // nothing at all.
            if (entity != viewer && !SeesHull(picture, entity) &&
                !registry.all_of<Sinking>(entity)) {
                continue;
            }
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
        float const shipAngle = body->GetAngle();

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());

        // Alpha blend so the arc colours' alpha reads: the scene otherwise draws
        // opaque (BlendMode::Replace ignores alpha), which would show every arc
        // at full strength and flatten the idle/active distinction.
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);

        // Each shown weapon's arc: two radial edges out to its range plus the outer
        // sweep between them, brightening when a target sits inside. The arc
        // originates from the mount's world position, not the hull centre. A battery
        // mount's arc draws while its fire unit has arcs switched on (the per-unit
        // toggle in the Target window); a point-defence mount's while it is enabled.
        auto const* fireControl = registry.try_get<FireControl>(ship);
        auto unitShowsArc = [&](int channel) {
            if (fireControl == nullptr) {
                return false;
            }
            for (auto const& ch : fireControl->channels) {
                if (ch.id == channel) {
                    return ch.showArc;
                }
            }
            return false;
        };
        for (auto const& weapon : armament->weapons) {
            bool const shown = weapon.pointDefense ? weapon.enabled : unitShowsArc(weapon.channel);
            if (!shown) {
                continue;
            }
            moth_ui::FloatVec2 const originPx = camera.WorldToScreen(body->GetWorldPoint(weapon.mountOffset));

            // A launcher's minimum range: the dead zone inside which its munitions
            // strike before arming and do no damage. Drawn as a faint red disc at
            // the mount so the player can read where a launch would be wasted.
            if (weapon.munitionMinRange > 0.0f) {
                graphics.SetColor(kDeadZoneColor);
                graphics.DrawFillCircleF(originPx, camera.MToPx(weapon.munitionMinRange));
            }

            float const rangePx = camera.MToPx(weapon.range);
            float const arcCentre = shipAngle + weapon.bearing;
            float const start = arcCentre - weapon.arcHalfAngle;

            auto edge = [&](float angle) {
                return moth_ui::FloatVec2{ originPx.x + (rangePx * std::cos(angle)),
                                           originPx.y + (rangePx * std::sin(angle)) };
            };

            // A shown gun draws a faint red arc, brightening to the active colour
            // with a target inside its arc. A gun whose unit has arcs switched off
            // draws none (skipped above).
            moth_ui::Color const arcColor = weapon.hasTarget ? kArcActiveColor : kArcEnabledColor;
            graphics.SetColor(arcColor);

            // A full-circle arc (an omnidirectional mount — a VLS, or a launcher
            // authored at 360) draws as a plain ring. The two radial edges would
            // land on the same bearing and leave a stray spoke from the mount to
            // the rim, and with no arc bound there is nothing for a barrel-lay line
            // to sit within either, so both are dropped as pure noise.
            constexpr float kFullCircle = (2.0f * b2_pi) - 0.01f;
            float const arcAngle = 2.0f * weapon.arcHalfAngle;
            if (arcAngle >= kFullCircle) {
                DrawCircle(graphics, originPx, rangePx);
                continue;
            }

            graphics.DrawLineF(originPx, edge(start));               // near radial edge
            DrawSweep(graphics, originPx, rangePx, start, arcAngle); // the outer sweep between them
            graphics.DrawLineF(originPx, edge(start + arcAngle));    // far radial edge

            // The barrel: a radius out to the arc's edge showing where the gun
            // is currently trained as it slews within the fixed arc toward its
            // mark (see combat_system). The arc's own hue, but far fainter — a
            // thin hint of the lay, not a second bright edge competing with the
            // arc it sits inside.
            graphics.SetColor(moth_ui::Color{ arcColor.r, arcColor.g, arcColor.b, arcColor.a * 0.6f });
            graphics.DrawLineF(originPx, edge(shipAngle + weapon.aimBearing));
        }

        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawWeaponSpread(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const* armament = registry.try_get<Armament>(ship);
        if (armament == nullptr) {
            return;
        }
        b2Body* body = registry.get<Physics>(ship).body;

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);
        graphics.SetColor(kSpreadColor);

        for (auto const& weapon : armament->weapons) {
            if (!weapon.showSpread || weapon.target == entt::null || !registry.valid(weapon.target)) {
                continue;
            }
            // Aim line from the mount's world position out to the aim point.
            moth_ui::FloatVec2 const originPx = camera.WorldToScreen(body->GetWorldPoint(weapon.mountOffset));
            moth_ui::FloatVec2 const aimPx = camera.WorldToScreen(weapon.aimWorld);
            graphics.DrawLineF(originPx, aimPx);

            // The spread disc over the aim point: a shot may land anywhere within
            // it, so its size shows the weapon's accuracy at this range.
            DrawCircle(graphics, aimPx, camera.MToPx(weapon.spreadRadiusM));
        }
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawTargetMarker(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const* fireControl = registry.try_get<FireControl>(ship);
        if (fireControl == nullptr) {
            return;
        }
        auto const* armament = registry.try_get<Armament>(ship);

        // The distinct contacts the ship's fire units are pointed at, and whether
        // any assigned gun bears on each. Distinct, because several fire units (each
        // gun is its own unit) can share a target, and their rings would stack into
        // one over-bright mark; one ring per contact, red if *any* gun laid on it
        // bears. Reads off the weapons' own bearing test rather than re-deriving
        // range, so it cannot disagree with what the guns will do.
        std::unordered_map<entt::entity, bool> armedByTarget;
        for (auto const& channel : fireControl->channels) {
            if (channel.target == entt::null || !registry.valid(channel.target)) {
                continue;
            }
            bool armed = false;
            if (armament != nullptr) {
                for (auto const& weapon : armament->weapons) {
                    if (weapon.channel == channel.id && weapon.hasTarget) {
                        armed = true;
                        break;
                    }
                }
            }
            auto const inserted = armedByTarget.emplace(channel.target, armed);
            if (!inserted.second) {
                inserted.first->second = inserted.first->second || armed;
            }
        }

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);

        for (auto const& [target, armed] : armedByTarget) {
            // Ring the contact where the ship *believes* it is — the true hull for a
            // radar or visual fix, the estimate for a passive TMA fix (see KnownAim)
            // — so the mark never betrays a position the player has not actually
            // fixed. Nothing to ring if the firing solution has lapsed.
            AimBelief const belief = KnownAim(registry, ship, target);
            if (!belief.ok) {
                continue;
            }

            // A known hull takes a circle clearing it whatever way it points: its
            // circumscribed radius (bow corner to centre), padded so the ring doesn't
            // graze it and floored in pixels so a speck at survey zoom still carries a
            // findable mark. A passive estimate has no known size, so it takes a fixed
            // screen ring on the estimate point instead.
            constexpr float kPadFrac = 1.35f;
            constexpr float kMinRadiusPx = 14.0f;
            constexpr float kEstimateRadiusPx = 18.0f;
            float radiusPx = kEstimateRadiusPx;
            if (!belief.estimate) {
                if (auto const* renderable = registry.try_get<Renderable>(target); renderable != nullptr) {
                    float const hullRadiusM = std::sqrt((renderable->halfLengthM * renderable->halfLengthM) +
                                                        (renderable->halfBeamM * renderable->halfBeamM));
                    radiusPx = std::max(camera.MToPx(hullRadiusM * kPadFrac), kMinRadiusPx);
                }
            }
            moth_ui::FloatVec2 const centrePx = camera.WorldToScreen(belief.pos);

            graphics.SetColor(armed ? kTargetRingArmedColor : kTargetRingColor);
            DrawCircle(graphics, centrePx, radiusPx);
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
        DrawCircle(graphics, centrePx, radiusPx);
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }

    void DrawProjectiles(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera) {
        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        for (auto entity : registry.view<Projectile>()) {
            auto const& projectile = registry.get<Projectile>(entity);
            graphics.SetColor(projectile.color);

            // A ballistic shell is a round dot; a guided munition draws as a small
            // rectangle laid along its velocity so its heading reads as it turns
            // onto the target. The same translate-then-rotate transform the hull
            // uses, with local +x forward along the direction of travel. At rest
            // (the tick it launches) velocity has no direction, so it lies flat.
            if (projectile.guidance != Guidance::Guided) {
                graphics.DrawFillCircleF(camera.WorldToScreen(projectile.position),
                                         camera.MToPx(projectile.radiusM));
                continue;
            }

            float const angleDeg = projectile.velocity.LengthSquared() > 1e-6f
                                       ? std::atan2(projectile.velocity.y, projectile.velocity.x) *
                                             moth_ui::kRadToDeg
                                       : 0.0f;
            moth_ui::FloatVec2 const posPx = camera.WorldToScreen(projectile.position);
            graphics.SetTransform(moth_ui::FloatMat4x4::Translation(posPx) *
                                  moth_ui::FloatMat4x4::Rotation(angleDeg, { 0.0f, 0.0f }));

            float const halfLenPx = camera.MToPx(projectile.drawLengthM * 0.5f);
            float const halfWidPx = camera.MToPx(projectile.drawWidthM * 0.5f);
            std::array<moth_ui::FloatVec2, 4> const shape{ {
                { halfLenPx, -halfWidPx },
                { halfLenPx, halfWidPx },
                { -halfLenPx, halfWidPx },
                { -halfLenPx, -halfWidPx },
            } };
            graphics.DrawFillPolygonF(shape.data(), shape.size());
            graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        }
    }

    void DrawPointDefenseFire(moth_graphics::graphics::IGraphics& graphics, entt::registry& registry, Camera const& camera, entt::entity ship) {
        auto const* armament = registry.try_get<Armament>(ship);
        if (armament == nullptr) {
            return;
        }
        b2Body* body = registry.get<Physics>(ship).body;

        // Per-frame jitter is what turns a static line into a chattering stream, so
        // the tracer is rebuilt from fresh random samples every frame rather than
        // carrying any state. One rng for the whole pass.
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        graphics.SetTransform(moth_ui::FloatMat4x4::Identity());
        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Alpha);

        for (auto const& weapon : armament->weapons) {
            // Only a mount that is actually engaging — trained (acquired) on a live
            // inbound munition — throws rounds. One still slewing onto its mark, or
            // switched out, draws nothing; the hitscan gun spawns no shells, so this
            // is the only thing standing in for the stream (see combat_system).
            if (!weapon.pointDefense || !weapon.enabled || !weapon.acquired ||
                weapon.target == entt::null || !registry.valid(weapon.target)) {
                continue;
            }

            moth_ui::FloatVec2 const muzzlePx = camera.WorldToScreen(body->GetWorldPoint(weapon.mountOffset));
            moth_ui::FloatVec2 const targetPx = camera.WorldToScreen(weapon.aimWorld);
            moth_ui::FloatVec2 const delta{ targetPx.x - muzzlePx.x, targetPx.y - muzzlePx.y };
            float const len = std::sqrt((delta.x * delta.x) + (delta.y * delta.y));
            if (len < 1.0f) {
                continue;
            }
            moth_ui::FloatVec2 const dir{ delta.x / len, delta.y / len };
            moth_ui::FloatVec2 const perp{ -dir.y, dir.x };

            // A few tracer rounds strung along the line of fire: each a short bright
            // dash at a random distance out, nudged off-axis so the burst reads as a
            // cone of fire rather than a laser, and at a random brightness so the
            // whole stream flickers. All in screen pixels so it stays legible at any
            // zoom — a stand-in for rounds, not a physical object measured in metres.
            constexpr int kTracerCount = 3;
            constexpr float kDashPx = 9.0f;
            constexpr float kJitterPx = 3.0f;
            for (int t = 0; t < kTracerCount; ++t) {
                float const s = unit(rng) * std::max(0.0f, len - kDashPx);
                float const j = (unit(rng) - 0.5f) * 2.0f * kJitterPx;
                moth_ui::FloatVec2 const a{ muzzlePx.x + (dir.x * s) + (perp.x * j),
                                            muzzlePx.y + (dir.y * s) + (perp.y * j) };
                moth_ui::FloatVec2 const b{ a.x + (dir.x * kDashPx), a.y + (dir.y * kDashPx) };
                graphics.SetColor(moth_ui::Color{ kCiwsTracerColor.r, kCiwsTracerColor.g,
                                                  kCiwsTracerColor.b, kCiwsTracerColor.a * (0.5f + (0.5f * unit(rng))) });
                graphics.DrawLineF(a, b);
            }

            // A muzzle flash at the mount and a spark where the rounds are striking,
            // both pulsing with the same per-frame flicker as the dashes.
            graphics.SetColor(moth_ui::Color{ kCiwsMuzzleColor.r, kCiwsMuzzleColor.g,
                                              kCiwsMuzzleColor.b, 0.6f + (0.4f * unit(rng)) });
            graphics.DrawFillCircleF(muzzlePx, 2.0f + (unit(rng) * 1.5f));
            graphics.SetColor(moth_ui::Color{ kCiwsTracerColor.r, kCiwsTracerColor.g,
                                              kCiwsTracerColor.b, 0.5f + (0.5f * unit(rng)) });
            graphics.DrawFillCircleF(targetPx, 1.5f + (unit(rng) * 1.5f));
        }

        graphics.SetBlendMode(moth_graphics::graphics::BlendMode::Replace);
    }
}
