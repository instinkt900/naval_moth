#include "layers/game_layer.h"

#include "game/aggro_system.h"
#include "game/combat_system.h"
#include "game/components.h"
#include "game/propulsion_system.h"
#include "game/render_system.h"
#include "game/sensor_system.h"
#include "game/ship_factory.h"
#include "game/tma_system.h"
#include "game/wake_system.h"
#include "game/wander_system.h"

#include <moth_ui/events/event_dispatch.h>
#include <moth_ui/utils/transform.h>

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <random>

namespace naval {
    namespace {
        // An angle in degrees wrapped to [0, 360) — a compass reading, as
        // opposed to the signed [-pi, pi] the steering systems work in (see
        // angles.h). Nothing on a bridge is at a heading of minus forty.
        float Norm360(float deg) {
            deg = std::fmod(deg, 360.0f);
            return deg < 0.0f ? deg + 360.0f : deg;
        }
    }

    GameLayer::GameLayer(moth_graphics::graphics::IGraphics& graphics, int widthPx, int heightPx)
        : m_graphics(graphics)
        , m_world(b2Vec2{ 0.0f, 0.0f }) // top-down: no gravity
        , m_terrain(m_world, 1337u, false) // open water while developing the sensor work; pass true to restore land
        , m_db(defs::Database::Load("assets/data")) {
        m_camera.viewSize = { static_cast<float>(widthPx), static_cast<float>(heightPx) };
        // Populate the starting neighbourhood so land is present on frame zero.
        m_terrain.Update(m_camera);

        // Before any spawn: a ship resolves the sound ids in its definitions
        // into handles as it is built, which needs the bank already loaded.
        m_audio.Load(m_db);

        // Player at the view centre; enemies scattered across the surrounding
        // water for the player to hunt down.
        m_ship = SpawnHull(m_registry, m_world, m_db, m_audio, m_db.GetPlayer().hull, m_camera.center, Faction::Player);
        SpawnEnemies();
    }

    void GameLayer::SpawnEnemies() {
        // Radar sandbox: a few enemies of mixed class at random bearings and
        // distances, so the passive plot shows several contacts at once to compare
        // — different radar sizes (hence signal strengths) on different bearings.
        // Each placement retries until the point (and a small clearance around it)
        // is open water; a class that never lands in water is skipped rather than
        // looping forever. Restore the full fleet spawn once the sandbox is done.
        constexpr float kMaxDistM = 28000.0f; // no farther than this out
        constexpr float kClearanceM = 60.0f;  // keep the hull clear of any shore
        constexpr int kMaxAttempts = 64;      // give up on a class after this many tries

        // Keep every enemy outside the player's own visual range so it starts as a
        // sensor contact — a blip or a bearing — never an already-seen hull. Keyed
        // off the player's visual reach (plus a margin) so it tracks the data.
        constexpr float kVisualMarginM = 1000.0f;
        float const minDistM = m_registry.get<Sensors>(m_ship).visualRangeM + kVisualMarginM;

        // Mixed classes so their radar power — and so the passive signal strength
        // — differs: a loud escort against quieter raider and picket.
        char const* const enemyIds[] = { "escort", "raider", "picket" };

        std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> bearingDist(0.0f, 2.0f * b2_pi);
        std::uniform_real_distribution<float> distDist(minDistM, kMaxDistM);
        std::uniform_real_distribution<float> headingDist(0.0f, 2.0f * b2_pi);

        for (char const* const enemyId : enemyIds) {
            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                float const bearing = bearingDist(rng);
                float const radius = distDist(rng);
                b2Vec2 const point{ m_camera.center.x + (radius * std::cos(bearing)),
                                    m_camera.center.y + (radius * std::sin(bearing)) };
                if (m_terrain.IsWater(point, kClearanceM)) {
                    entt::entity const enemy = SpawnEnemy(m_registry, m_world, m_db, m_audio, enemyId, point);
                    // Point it in a random direction so it isn't bow-up; the spawn
                    // point is kept, only the heading changes.
                    m_registry.get<Physics>(enemy).body->SetTransform(point, headingDist(rng));
                    break;
                }
            }
        }
    }

    bool GameLayer::OnEvent(moth_ui::Event const& event) {
        moth_ui::EventDispatch dispatch(event);
        dispatch.Dispatch(this, &GameLayer::OnMouseDown);
        dispatch.Dispatch(this, &GameLayer::OnMouseWheel);
        dispatch.Dispatch(this, &GameLayer::OnKey);
        return dispatch.GetHandled();
    }

    bool GameLayer::OnMouseDown(moth_ui::EventMouseDown const& event) {
        // Let ImGui swallow clicks that land on the helm panel, so interacting
        // with the controls never drops a waypoint on the sea behind them.
        if (ImGui::GetIO().WantCaptureMouse) {
            return false;
        }
        auto const pos = event.GetPosition();
        b2Vec2 const world =
            m_camera.ScreenToWorld({ static_cast<float>(pos.x), static_cast<float>(pos.y) });

        // A click on a contact designates it rather than steering the ship into
        // it. The pick tolerance is a comfortable click in pixels, converted to
        // metres against the live zoom here, so a contact is no harder to hit
        // when it has shrunk to a speck at survey zoom than when it fills the
        // view — the sea below is a big target, but a ship on it isn't.
        constexpr float kPickPx = 8.0f;
        entt::entity contact =
            ContactAt(m_registry, world, Faction::Enemy, kPickPx / m_camera.pixelsPerMeter);
        // Designation needs a *fix*, not just an awareness: a contact the player
        // holds only as a passive bearing (or does not hold at all) has no known
        // position to lay guns on, so it cannot be engaged until radar or the eye
        // gives it a range. A ranged or visual contact can.
        if (contact != entt::null) {
            auto const& contacts = m_registry.get<ContactPicture>(m_ship).contacts;
            auto const it = contacts.find(contact);
            if (it == contacts.end() || it->second.level == DetectLevel::Bearing) {
                contact = entt::null;
            }
        }
        if (contact != entt::null) {
            auto& order = m_registry.get<FireOrder>(m_ship);
            // Designating never opens fire by itself — that is the Fire button's
            // job. Guarded on the target actually changing so that clicking the
            // contact you are already engaging doesn't check fire on it.
            if (order.target != contact) {
                order.target = contact;
                order.firing = false;
            }
            return true;
        }

        // Open water: the click is a helm order. Each one moves the single
        // target; nothing is queued.
        auto& target = m_registry.get<MoveTarget>(m_ship);
        target.point = world;
        target.active = true;
        return true;
    }

    bool GameLayer::OnMouseWheel(moth_ui::EventMouseWheel const& event) {
        // Scrolling over an ImGui window belongs to it (e.g. dragging a slider),
        // not to the camera zoom.
        if (ImGui::GetIO().WantCaptureMouse) {
            return false;
        }
        // Wheel drives zoom: each notch scales the view about its centre. The
        // limits live in camera.h, since the audio measures against them too.
        constexpr float kZoomStep = 1.15f; // per notch
        float const notches = static_cast<float>(event.GetDelta().y);
        m_camera.pixelsPerMeter = std::clamp(m_camera.pixelsPerMeter * std::pow(kZoomStep, notches),
                                             kMinZoom, kMaxZoom);
        return true;
    }

    bool GameLayer::OnKey(moth_ui::EventKey const& event) {
        // Track WASD hold state; the pan itself happens per-tick in Update so it
        // stays smooth and framerate-independent regardless of key-repeat rate.
        bool const down = event.GetAction() == moth_ui::KeyAction::Down;
        switch (event.GetKey()) {
        case moth_ui::Key::W: m_panUp = down; return true;
        case moth_ui::Key::S: m_panDown = down; return true;
        case moth_ui::Key::A: m_panLeft = down; return true;
        case moth_ui::Key::D: m_panRight = down; return true;
        default: return false;
        }
    }

    void GameLayer::Update(uint32_t ticks) {
        float const dt = static_cast<float>(ticks) / 1000.0f;

        // Pan the camera from held WASD. Target speed is fixed in screen
        // pixels/second so panning feels the same at any zoom; the velocity is
        // eased toward that target for a soft ramp on press and coast on release.
        constexpr float kPanPxPerSec = 600.0f;
        constexpr float kPanEaseTau = 0.12f; // s; smaller = snappier
        moth_ui::FloatVec2 dir{ 0.0f, 0.0f };
        if (m_panLeft) { dir.x -= 1.0f; }
        if (m_panRight) { dir.x += 1.0f; }
        if (m_panUp) { dir.y -= 1.0f; }
        if (m_panDown) { dir.y += 1.0f; }
        // Frame-rate independent exponential approach toward the target velocity.
        float const ease = 1.0f - std::exp(-dt / kPanEaseTau);
        m_panVel.x += ((dir.x * kPanPxPerSec) - m_panVel.x) * ease;
        m_panVel.y += ((dir.y * kPanPxPerSec) - m_panVel.y) * ease;
        m_camera.center.x += (m_panVel.x / m_camera.pixelsPerMeter) * dt;
        m_camera.center.y += (m_panVel.y / m_camera.pixelsPerMeter) * dt;

        // Stream land in/out around the (possibly moved) camera view.
        m_terrain.Update(m_camera);

        // Sounds are heard from the camera, and shakes are felt from it — where
        // it looks and how far it has zoomed in — so both follow the pan above
        // and must be set before any system below plays or jolts anything.
        m_audio.SetListener(m_camera);
        m_shake.SetCamera(m_camera);

        // Refresh each ship's contact picture before anything reads it — the
        // aggro steering below, the render loop, and target-picking all consult
        // what a ship can detect, so its picture must be current first. (Only the
        // player carries one today; the enemy aggro still scans hulls directly.)
        UpdateSensors(m_registry, dt);

        // Passive ranging: feed each bearing-only contact's track a fresh cut and
        // re-solve it. After UpdateSensors so it reads a current picture, and — like
        // it — before the world step, so a cut and the own-ship position paired with
        // it are from the same instant (see tma_system).
        UpdateTMA(m_registry, dt);

        // Enemies that sense a foe within aggro range break off to manoeuvre and
        // fight; the rest wander. Aggro runs first so it can claim the helm, and
        // wander then handles only the ships still on patrol. It also issues each
        // engaging enemy's fire order, which UpdateWeapons below consumes — so it
        // must stay ahead of that too, or an enemy shoots a tick late.
        UpdateAggro(m_registry, dt);
        UpdateWander(m_registry, m_terrain, dt);
        UpdatePropulsion(m_registry, dt);
        UpdateWeapons(m_registry, m_audio, m_shake, dt);
        UpdateProjectiles(m_registry, m_audio, m_shake, m_terrain, dt);
        UpdateSplashes(m_registry, dt);
        m_world.Step(dt, 8, 3);

        // After the step so marks are dropped at the hull's settled position.
        UpdateWake(m_registry, dt);

        // Age wrecks and remove them once they have fully sunk. After the step
        // so a wreck's Box2D body is destroyed outside the world update.
        UpdateSinking(m_registry, dt);

        // Decay the shake and roll this frame's jolt, then hand it to the camera
        // to draw with. After everything that fires, hits or explodes, so a gun
        // fired this tick is felt on this frame rather than the next.
        m_shake.Update(dt);
        m_camera.shakeOffsetM = m_shake.OffsetM();

        // Reclaim the voices of sounds that have finished. Last, so anything
        // played this tick has had its voice taken before we look.
        m_audio.Update();
    }

    void GameLayer::Draw() {
        // The frame is composited as four layers, each painting over the last:
        // map (the sea and the hulls on it), then the radar picture, then the
        // tactical command picture, then the debug overlays on top. Any of the
        // four can be switched off from the Layers panel. The player's own contact
        // picture gates every enemy visual — hulls, wakes and arcs draw only for a
        // contact it actually holds — so the layers that show enemies take it (see
        // UpdateSensors).
        auto const& picture = m_registry.get<ContactPicture>(m_ship);
        if (m_showMapLayer) {
            DrawMapLayer(picture);
        }
        if (m_showRadarLayer) {
            DrawRadarLayer();
        }
        if (m_showTacticalLayer) {
            DrawTacticalLayer(picture);
        }
        if (m_showDebugLayer) {
            DrawDebugLayer();
        }

        DrawHelmPanel();
        DrawTargetPanel();
        DrawAggroDebug();
        DrawTmaDebug();
        DrawLayersPanel();
    }

    void GameLayer::DrawMapLayer(ContactPicture const& picture) {
        // The sea and terrain, the fading wake marks and shot splashes on the
        // water, then the hulls the player actually sees on top. An enemy hull
        // draws only while seen (a merely ranged contact is a radar blip in the
        // layer above, not a hull); a wreck is exempt (it has shed the Combatant
        // the picture is keyed on, and is the visible aftermath of a fight the
        // player was in). The player's own hull is drawn last so it stays topmost.
        m_terrain.Draw(m_graphics, m_camera);
        DrawWakes(m_graphics, m_registry, m_camera, m_ship, picture);
        DrawSplashes(m_graphics, m_registry, m_camera);
        for (auto ship : m_registry.view<Physics, Renderable>()) {
            if (ship == m_ship) {
                continue;
            }
            if (!SeesHull(picture, ship) && !m_registry.all_of<Sinking>(ship)) {
                continue;
            }
            DrawShip(m_graphics, m_registry, m_camera, ship);
        }
        DrawShip(m_graphics, m_registry, m_camera, m_ship);
    }

    void GameLayer::DrawRadarLayer() {
        // The player's sensor picture over the map: passive ESM bearing lines
        // (always), and the active radar's reach ring and contact blips while
        // radiating.
        DrawContacts(m_graphics, m_registry, m_camera, m_ship);
    }

    void GameLayer::DrawTacticalLayer(ContactPicture const& picture) {
        // The command picture, over the map and radar: each armed ship's firing
        // arcs, the player's waypoint course and the designated contact's ring,
        // then the shots in the air — projectiles and the point-defence tracer
        // streams cutting down inbound missiles. An enemy's arcs are gated like its
        // hull (nothing unless the player sees it — a ranged blip carries no known
        // armament) and can be hidden even when seen via the debug toggle; the
        // player's own always draw.
        for (auto ship : m_registry.view<Physics, Armament>()) {
            if (m_registry.get<Combatant>(ship).faction == Faction::Enemy &&
                (!m_showEnemyArcs || !SeesHull(picture, ship))) {
                continue;
            }
            DrawArcs(m_graphics, m_registry, m_camera, ship);
        }
        DrawTarget(m_graphics, m_registry, m_camera, m_ship);
        DrawTargetMarker(m_graphics, m_registry, m_camera, m_ship);

        DrawProjectiles(m_graphics, m_registry, m_camera);
        for (auto ship : m_registry.view<Physics, Armament>()) {
            DrawPointDefenseFire(m_graphics, m_registry, m_camera, ship);
        }
    }

    void GameLayer::DrawDebugLayer() {
        // On top of everything: the aggro-range rings around the AI ships, and the
        // weapon-spread previews — a line to each enabled weapon's target and the
        // disc its shots may land within.
        for (auto ship : m_registry.view<Physics, Aggro>()) {
            DrawAggroRing(m_graphics, m_registry, m_camera, ship);
        }
        for (auto ship : m_registry.view<Physics, Armament>()) {
            DrawWeaponSpread(m_graphics, m_registry, m_camera, ship);
        }
    }

    void GameLayer::DrawAggroDebug() {
        AggroTuning& tuning = AggroTuningRef();
        ImGui::Begin("Aggro (debug)");
        ImGui::SliderFloat("Aggro range (m)", &tuning.aggroRangeM, 100.0f, 4000.0f, "%.0f");
        ImGui::SliderFloat("Disengage range (m)", &tuning.disengageRangeM, 100.0f, 5000.0f, "%.0f");
        ImGui::SliderFloat("Standoff (frac)", &tuning.standoffFrac, 0.1f, 1.0f, "%.2f");
        ImGui::SliderFloat("Approach band (m)", &tuning.approachBandM, 50.0f, 2000.0f, "%.0f");
        ImGui::SliderFloat("Helm gain", &tuning.helmGain, 0.1f, 8.0f, "%.2f");
        ImGui::SliderFloat("Throttle gain", &tuning.throttleGain, 0.0005f, 0.02f, "%.4f");
        ImGui::SliderFloat("Backoff weight", &tuning.backoffWeight, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Steerage throttle", &tuning.steerageThrottle, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Steerage error (rad)", &tuning.steerageErrorRad, 0.05f, 1.5f, "%.2f");
        ImGui::SliderFloat("Arc switch margin (rad)", &tuning.switchMarginRad, 0.0f, 1.5f, "%.2f");
        ImGui::Checkbox("Show aggro rings", &tuning.showRings);
        ImGui::Checkbox("Show enemy arcs", &m_showEnemyArcs);
        ImGui::End();
    }

    void GameLayer::DrawTmaDebug() {
        auto const* trackFile = m_registry.try_get<TrackFile>(m_ship);
        ImGui::Begin("TMA (debug)");
        if (trackFile == nullptr || trackFile->tracks.empty()) {
            ImGui::TextUnformatted("No passive tracks");
            ImGui::End();
            return;
        }

        // Live confidence gates, dialled against the obs values the tracks below
        // actually reach. Log sliders, since observability spans orders of
        // magnitude: raise obsFull toward the obs a good two-leg fix reaches, and
        // drop minObs below it. See tma_system for what these mean.
        TmaTuning& tuning = TmaTuningRef();
        ImGui::SliderFloat("min obs", &tuning.minObs, 1e-9f, 1e-2f, "%.1e", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("obs full", &tuning.obsFull, 1e-8f, 1.0f, "%.1e", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("solved floor", &tuning.solvedFloor, 0.0f, 1.0f, "%.2f");

        constexpr float kMetresPerSecToKnots = 1.94384f;
        b2Vec2 const ownPos = m_registry.get<Physics>(m_ship).body->GetPosition();

        // One block per open track, labelled by its current bearing rather than the
        // contact's name — the name would leak an identity the passive rung has not
        // earned. The numbers below the label are the solver's own state, so a
        // stalled solution reads as either too straight a leg (low observability) or
        // a poor fit (high misfit), which is exactly what to change own course for.
        int index = 0;
        for (auto const& entry : trackFile->tracks) {
            TmaTrack const& track = entry.second;
            ImGui::PushID(index++);
            float const brgDeg = Norm360(track.samples.back().bearing * moth_ui::kRadToDeg);
            ImGui::Separator();
            ImGui::TextUnformatted(
                fmt::format("Brg {:03.0f}   cuts {}", brgDeg, track.samples.size()).c_str());

            // The bearing rate is what predicts whether the geometry can range at
            // all: TMA lives on the bearing sweeping, so a steady bearing (running
            // toward or away from the contact) carries no range, however many cuts
            // pile up. Shown so a barren leg reads as barren before obs is even
            // computed — cross the bearing, don't close it.
            float bearingRateDegMin = 0.0f;
            if (track.samples.size() >= 2) {
                float const span = track.samples.back().t - track.samples.front().t;
                float const d = track.samples.back().bearing - track.samples.front().bearing;
                float const dWrapped = std::atan2(std::sin(d), std::cos(d)); // shortest arc, rad
                if (span > 0.0f) {
                    bearingRateDegMin = (dWrapped * moth_ui::kRadToDeg / span) * 60.0f;
                }
            }
            ImGui::TextUnformatted(fmt::format("brg rate {:+.1f} deg/min", bearingRateDegMin).c_str());
            if (!track.solved && std::abs(bearingRateDegMin) < 1.0f) {
                ImGui::TextDisabled("steady bearing: steam across it, not toward it");
            }

            ImGui::TextUnformatted(fmt::format("obs {:.2e}   misfit {:.2f} deg",
                                               track.observability,
                                               track.residualRad * moth_ui::kRadToDeg)
                                       .c_str());
            ImGui::ProgressBar(track.confidence, ImVec2(-1.0f, 0.0f),
                               fmt::format("conf {:.0f}%", track.confidence * 100.0f).c_str());
            if (track.solved) {
                float const rangeM = (track.position - ownPos).Length();
                float const crsDeg = Norm360(std::atan2(track.velocity.y, track.velocity.x) * moth_ui::kRadToDeg);
                float const spdKn = track.velocity.Length() * kMetresPerSecToKnots;
                ImGui::TextUnformatted(
                    fmt::format("est rng {:.0f} m   crs {:03.0f}   spd {:.1f} kn", rangeM, crsDeg, spdKn)
                        .c_str());
            } else {
                ImGui::TextUnformatted("no solution");
            }
            ImGui::PopID();
        }

        ImGui::End();
    }

    void GameLayer::DrawLayersPanel() {
        // Master switches over the four render layers Draw() composites. The label
        // on each names what falls into it, so the toggle reads without having to
        // recall the layer's contents. These only hide draws; the finer toggles
        // (enemy arcs, aggro rings, per-weapon spread) still apply within a shown
        // layer.
        ImGui::Begin("Layers");
        ImGui::Checkbox("Map (sea, terrain, hulls)", &m_showMapLayer);
        ImGui::Checkbox("Radar (blips, ring, bearings)", &m_showRadarLayer);
        ImGui::Checkbox("Tactical (arcs, tracks, shots)", &m_showTacticalLayer);
        ImGui::Checkbox("Debug (aggro rings, spread)", &m_showDebugLayer);
        ImGui::End();
    }

    void GameLayer::DrawHelmPanel() {
        auto& helm = m_registry.get<Helm>(m_ship);
        auto& target = m_registry.get<MoveTarget>(m_ship);

        ImGui::Begin("Helm");
        if (target.active) {
            ImGui::TextUnformatted("Autopilot: steering to waypoint");
            if (ImGui::Button("Take the helm")) {
                target.active = false;
            }
        } else {
            ImGui::TextUnformatted("Manual control");
        }
        ImGui::Separator();

        // Speed along the keel, signed so making sternway reads negative, shown
        // in knots for flavour (1 m/s = 1.94384 kn).
        constexpr float kMetresPerSecToKnots = 1.94384f;
        b2Body* body = m_registry.get<Physics>(m_ship).body;
        b2Vec2 const forward = body->GetWorldVector(b2Vec2{ 1.0f, 0.0f });
        float const speedKnots = b2Dot(body->GetLinearVelocity(), forward) * kMetresPerSecToKnots;
        ImGui::TextUnformatted(fmt::format("Speed: {:.1f} kn", speedKnots).c_str());

        // Heading as a three-digit compass reading, and how fast it is changing.
        // The rate is the body's angular velocity read straight off: the
        // propulsion system sets that directly rather than letting it accumulate
        // from torque, so it is the rate of turn outright — nothing to
        // differentiate, and the two readouts can't disagree about the same tick.
        float const headingDeg = Norm360(body->GetAngle() * moth_ui::kRadToDeg);
        float const rateDegPerSec = body->GetAngularVelocity() * moth_ui::kRadToDeg;
        ImGui::TextUnformatted(fmt::format("Heading: {:03.0f}", headingDeg).c_str());

        // Named to the side it is swinging rather than left signed, so the
        // readout answers in the same terms as the rudder order that caused it —
        // positive is starboard for both. A hull is never exactly steady while
        // making way, so anything under the deadband reads as steady rather than
        // flickering through hundredths.
        constexpr float kSteadyDegPerSec = 0.05f;
        if (std::abs(rateDegPerSec) < kSteadyDegPerSec) {
            ImGui::TextUnformatted("Turn: steady");
        } else {
            ImGui::TextUnformatted(fmt::format("Turn: {:.1f} deg/s {}", std::abs(rateDegPerSec),
                                               rateDegPerSec > 0.0f ? "stbd" : "port")
                                       .c_str());
        }
        ImGui::Separator();

        // Engine-order telegraph: quick throttle presets above the slider. Five
        // evenly-spaced bells each way — each step is 20% of full power — with
        // Flank at the rail (±1). Ahead is positive throttle, astern negative;
        // every order takes manual control, like nudging the slider. The bell
        // names are traditional and don't map to literal fractions.
        auto order = [&](char const* label, float throttle) {
            if (ImGui::Button(label)) {
                helm.throttle = throttle;
                target.active = false;
            }
        };
        auto bells = [&](float sign) {
            order("1/3", sign * 0.2f);   ImGui::SameLine();
            order("2/3", sign * 0.4f);   ImGui::SameLine();
            order("Std", sign * 0.6f);   ImGui::SameLine();
            order("Full", sign * 0.8f);  ImGui::SameLine();
            order("Flank", sign * 1.0f);
        };

        ImGui::TextUnformatted("Ahead");
        ImGui::PushID("ahead");
        bells(1.0f);
        ImGui::PopID();

        if (ImGui::Button("Stop")) {
            helm.throttle = 0.0f;
            target.active = false;
        }

        ImGui::TextUnformatted("Astern");
        ImGui::PushID("astern");
        bells(-1.0f);
        ImGui::PopID();

        // Grabbing either control drops the waypoint so manual input takes over;
        // clicking the sea re-engages the autopilot.
        if (ImGui::SliderFloat("Throttle", &helm.throttle, -1.0f, 1.0f, "%.2f")) {
            target.active = false;
        }
        if (ImGui::SliderFloat("Rudder (ordered)", &helm.rudderCmd, -1.0f, 1.0f, "%.2f")) {
            target.active = false;
        }

        // The actual blade position, read-only, so its lag behind the order is
        // visible as the rudder swings across.
        ImGui::BeginDisabled();
        ImGui::SliderFloat("Rudder (actual)", &helm.rudder, -1.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();

        if (ImGui::Button("All stop")) {
            helm.throttle = 0.0f;
            helm.rudderCmd = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Midships")) {
            helm.rudderCmd = 0.0f;
        }

        // Sensors. Active radar lights up contacts out to the set reach as
        // unidentified blips; it starts off (silent — the EMCON posture), and
        // turning it on will, once the enemy can listen, announce own position.
        // Passive ESM needs no toggle: it is always listening, and hears an
        // emitting contact as a bearing (a wedge on the water) whatever the radar
        // is doing.
        ImGui::Separator();
        auto& sensors = m_registry.get<Sensors>(m_ship);
        ImGui::Checkbox("Active radar", &sensors.activeOn);
        ImGui::TextUnformatted(fmt::format("Visual {:.0f}   Radar {:.0f}   ESM {:.0f} km",
                                           sensors.visualRangeM / 1000.0f, sensors.activeRangeM / 1000.0f,
                                           sensors.passiveRangeM / 1000.0f)
                                   .c_str());

        ImGui::End();
    }

    void GameLayer::DrawTargetPanel() {
        auto& order = m_registry.get<FireOrder>(m_ship);

        ImGui::Begin("Target");

        // The designated contact leads the window — its picture and the orders
        // that act on it. The weapons system drops the order the moment its
        // contact dies, so an empty order here is the normal end of an
        // engagement, not an error; free fire and the battery list below stand
        // without a contact, so only this leading block is gated on there being
        // one.
        if (order.target != entt::null && m_registry.valid(order.target)) {
            constexpr float kMetresPerSecToKnots = 1.94384f;
            b2Body* self = m_registry.get<Physics>(m_ship).body;
            b2Vec2 const shipPos = self->GetPosition();
            float const shipAngle = self->GetAngle();

            b2Body* contact = m_registry.get<Physics>(order.target).body;
            b2Vec2 const toContact = contact->GetPosition() - shipPos;
            float const rangeM = toContact.Length();
            float const speedKn = contact->GetLinearVelocity().Length() * kMetresPerSecToKnots;
            // Bearing is relative to our own bow, heading is the contact's own
            // course — the two questions a gunnery picture has to answer.
            float const bearingDeg =
                Norm360((std::atan2(toContact.y, toContact.x) - shipAngle) * moth_ui::kRadToDeg);
            float const headingDeg = Norm360(contact->GetAngle() * moth_ui::kRadToDeg);

            char const* type = "contact";
            if (auto const* id = m_registry.try_get<Identity>(order.target); id != nullptr) {
                type = id->name.c_str();
            }
            ImGui::TextUnformatted(type);
            ImGui::Separator();
            ImGui::TextUnformatted(fmt::format("rng {:.0f} m   spd {:.1f} kn", rangeM, speedKn).c_str());
            ImGui::TextUnformatted(fmt::format("brg {:.0f}   hdg {:.0f}", bearingDeg, headingDeg).c_str());

            if (auto const* health = m_registry.try_get<Health>(order.target);
                health != nullptr && health->max > 0.0f) {
                ImGui::ProgressBar(health->current / health->max, ImVec2(-1.0f, 0.0f),
                                   fmt::format("{:.0f} / {:.0f}", health->current, health->max).c_str());
            }
            ImGui::Separator();

            // How much of the battery can actually reach the contact right now.
            // The count is the honest answer to "why isn't anything happening?"
            // after pressing Fire with the target abaft the beam of every gun.
            // It counts hulls that bear, not guns switched in — a bearing gun
            // held out of the battery still shows here, and its row below says
            // it is disabled.
            int bearing = 0;
            int total = 0;
            if (auto const* armament = m_registry.try_get<Armament>(m_ship); armament != nullptr) {
                total = static_cast<int>(armament->weapons.size());
                for (auto const& weapon : armament->weapons) {
                    bearing += weapon.hasTarget ? 1 : 0;
                }
            }
            ImGui::TextUnformatted(fmt::format("{} of {} guns bear", bearing, total).c_str());

            // One button for the whole ship: every gun that bears and is
            // switched in fires until the contact is dead or this is clicked
            // again. Never disabled for want of a gun bearing — ordering fire
            // while manoeuvring onto the target is the point, and the guns join
            // in as the arcs come onto it.
            if (ImGui::Button(order.firing ? "Hold" : "Fire")) {
                order.firing = !order.firing;
            }

            // A salvo beside it, being the same order with less commitment: one
            // round from whatever is loaded, bearing and switched in, and no
            // standing order left set behind it. Unlike Fire it needs no
            // cancelling, which is why it is a plain button and not a second
            // thing that latches.
            ImGui::SameLine();
            if (ImGui::Button("Salvo")) {
                order.salvo = true;
            }
        } else {
            ImGui::TextUnformatted("No target designated");
            ImGui::TextUnformatted("Click a contact to designate it.");
        }

        // Free fire and the battery both answer to no particular contact, so
        // they sit below the leading block and show whether or not one is set.
        // The per-gun enable ticks in the battery gate all three orders above.
        DrawWeaponsRelease(order);
        DrawWeaponControls();

        ImGui::End();
    }

    void GameLayer::DrawWeaponsRelease(FireOrder& order) {
        // A checkbox rather than a button like the two above it: those are acts,
        // this is a state the ship stays in, and a checkbox shows whether it is
        // set without the player having to work out whether a button's label is
        // naming the mode or the way out of it.
        ImGui::Separator();
        ImGui::Checkbox("Free fire", &order.freeFire);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Every gun engages the nearest foe it can bear on.\n"
                              "The designated target still comes first.");
        }
    }

    void GameLayer::DrawWeaponControls() {
        auto const& order = m_registry.get<FireOrder>(m_ship);
        auto* armament = m_registry.try_get<Armament>(m_ship);
        if (armament == nullptr) {
            ImGui::Separator();
            ImGui::TextUnformatted("No armament");
            return;
        }

        // One weapon's row: the enable tick, the name, its two draw toggles, and
        // its status. Unticked, the weapon is switched out of the fire orders and
        // holds through all of them while still tracking and drawing. This is the
        // one per-weapon fire control — a weapon's only say in the engagement is
        // whether it takes part at all. The tick's label is hidden (the name
        // beside it labels it); PushID keeps it unique across the whole battery.
        auto drawRow = [&](std::size_t i) {
            ImGui::Separator();
            Weapon& weapon = armament->weapons[i];
            ImGui::PushID(static_cast<int>(i));

            ImGui::Checkbox("##enable", &weapon.enabled);
            ImGui::SameLine();
            ImGui::TextUnformatted(weapon.name.empty() ? "Weapon" : weapon.name.c_str());
            if (weapon.kind == WeaponKind::Gun) {
                ImGui::SameLine();
                ImGui::Checkbox("Show spread", &weapon.showSpread);
            } else {
                // A launcher's character is what it is loaded with, so name the
                // munition beside it, greyed to read as a subtitle to the mount.
                if (!weapon.munitionName.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", weapon.munitionName.c_str());
                }

                // How many munitions a Salvo order releases from this launcher: a
                // scroll limited to the tubes ready to fire, since it can send no
                // more than are loaded. Standing Fire ignores it and ripples the
                // whole bank out regardless.
                ImGui::SameLine();
                int const maxLaunch = std::max(1, weapon.readyTubes);
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::InputInt("Salvo size", &weapon.salvoSize)) {
                    weapon.salvoSize = std::clamp(weapon.salvoSize, 1, maxLaunch);
                }
            }

            // What this weapon is doing about the ship's order. Read-only apart
            // from the enable tick above: the order itself is the leading block's
            // to give.
            //
            // Read off the weapon's own mark, not hasTarget, so that under free
            // fire a weapon laid on a contact of its own says so instead of
            // reporting "no bearing" on the strength of the designated one. A
            // switched-out weapon reads "disabled" ahead of any of that, since it
            // is holding whatever it can reach. When it does bear, the barrel is
            // either still slewing onto the mark or acquired and on it — the
            // distinction the turn rate makes visible.
            bool const laid = weapon.target != entt::null && m_registry.valid(weapon.target);
            bool const shooting = order.firing || order.freeFire;

            char const* status = "no bearing";
            if (!weapon.enabled) {
                status = "disabled";
            } else if (laid && !weapon.acquired) {
                status = "slewing";
            } else if (laid) {
                status = shooting ? "firing" : "acquired, holding";
            }

            // The reload clock rides alongside the lay state whenever the weapon
            // is cooling, firing or not — so an auto-firing battery still shows
            // each mount training and reloading rather than a bare "firing". A
            // weapon ordered to fire but still slewing reads "slewing", since with
            // the trigger gated on acquisition it is holding its rounds. A launcher
            // shows its ready tubes instead, with the time to the next reloading in.
            if (weapon.kind != WeaponKind::Gun) {
                std::string line = fmt::format("{}  {}/{} tubes", status, weapon.readyTubes, weapon.tubeCount);
                if (weapon.readyTubes < weapon.tubeCount && weapon.reloadTimer > 0.0f) {
                    line += fmt::format("  reloading {:.1f}s", weapon.reloadTimer);
                }
                ImGui::TextUnformatted(line.c_str());
            } else if (weapon.enabled && weapon.cooldownRemaining > 0.0f) {
                ImGui::TextUnformatted(
                    fmt::format("{}  reloading {:.1f}s", status, weapon.cooldownRemaining).c_str());
            } else {
                ImGui::TextUnformatted(status);
            }

            ImGui::PopID();
        };

        // A point-defence mount's row is stripped down to what it actually offers:
        // the enable tick and its name, then a read-only status. It answers inbound
        // missiles on its own and stands outside the fire order, so there is no
        // salvo, no spread preview, and no fire button to show — the tick is the
        // captain's whole say. Status reads "off" when switched out, then, once it
        // has a missile, "tracking" while the mount is still swinging onto it and
        // "engaging" once trained and firing — the distinction the turn rate makes,
        // since a mount only scores once acquired (see combat_system). "searching"
        // otherwise.
        auto drawPointDefenseRow = [&](std::size_t i) {
            ImGui::Separator();
            Weapon& weapon = armament->weapons[i];
            ImGui::PushID(static_cast<int>(i));

            ImGui::Checkbox("##enable", &weapon.enabled);
            ImGui::SameLine();
            ImGui::TextUnformatted(weapon.name.empty() ? "Weapon" : weapon.name.c_str());

            bool const laid = weapon.target != entt::null && m_registry.valid(weapon.target);
            char const* status = "searching";
            if (!weapon.enabled) {
                status = "off";
            } else if (laid) {
                status = weapon.acquired ? "engaging" : "tracking";
            }
            ImGui::TextUnformatted(status);

            ImGui::PopID();
        };

        // Guns, launchers and point defence are listed apart, each under its own
        // heading, so the battery reads as distinct systems — the gun line, the
        // munition cells, and the close-in shield — rather than one mixed list. A
        // heading shows only if the ship carries that kind. The predicate keeps
        // point-defence guns out of the plain gun list even though they are Gun
        // kind, since they answer to a different control entirely.
        auto drawGroup = [&](char const* heading, auto&& want, auto&& row) {
            bool headed = false;
            for (std::size_t i = 0; i < armament->weapons.size(); ++i) {
                if (!want(armament->weapons[i])) {
                    continue;
                }
                if (!headed) {
                    ImGui::Separator();
                    ImGui::TextUnformatted(heading);
                    headed = true;
                }
                row(i);
            }
        };

        drawGroup(
            "Guns", [](Weapon const& w) { return w.kind == WeaponKind::Gun && !w.pointDefense; }, drawRow);
        drawGroup(
            "Launchers", [](Weapon const& w) { return w.kind != WeaponKind::Gun; }, drawRow);
        drawGroup(
            "Point Defense", [](Weapon const& w) { return w.pointDefense; }, drawPointDefenseRow);
    }
}
