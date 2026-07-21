#include "layers/game_layer.h"

#include "game/aggro_system.h"
#include "game/combat_system.h"
#include "game/components.h"
#include "game/emcon_system.h"
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
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>

namespace naval {
    namespace {
        // An angle in degrees wrapped to [0, 360) — a compass reading, as
        // opposed to the signed [-pi, pi] the steering systems work in (see
        // angles.h). Nothing on a bridge is at a heading of minus forty.
        float Norm360(float deg) {
            deg = std::fmod(deg, 360.0f);
            return deg < 0.0f ? deg + 360.0f : deg;
        }

        // A contact's call-sign from its slot index: 0 -> "Blip A", 25 -> "Blip Z",
        // 26 -> "Blip AA", and up (bijective base-26). Slots are reclaimed as
        // contacts drop, so a short watch never climbs past the single letters.
        std::string BlipLabel(int index) {
            std::string letters;
            for (int n = index + 1; n > 0; n = (n - 1) / 26) {
                letters.insert(letters.begin(), static_cast<char>('A' + ((n - 1) % 26)));
            }
            return "Blip " + letters;
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

        // In planning mode a click drops a flight-plan waypoint instead of a helm
        // order. The two never collide: planning is entered deliberately from the
        // Flight Plans window and left with Escape (see DrawFlightPlansPanel / OnKey).
        if (m_planningId >= 0) {
            auto& library = m_registry.get<FlightPlanLibrary>(m_ship);
            for (auto& plan : library.plans) {
                if (plan.id == m_planningId) {
                    plan.waypoints.push_back(world);
                    break;
                }
            }
            return true;
        }

        // A click is a helm order, plain and simple — designating a contact is no
        // longer a click on the water but a pick from a fire unit's target dropdown
        // in the Target window (see DrawTargetPanel), so the two never collide. Each
        // click moves the single move target; nothing is queued.
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
        // Escape ends flight-plan authoring; ignored otherwise.
        case moth_ui::Key::Escape:
            if (down && m_planningId >= 0) {
                m_planningId = -1;
                return true;
            }
            return false;
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

        // Refresh every ship's contact picture before anything reads it — the aggro
        // steering below, both sides' gunnery, the render loop, and target-picking all
        // consult what a ship can detect, so its picture must be current first. Every
        // combatant carries one now: the enemy fights off its own sensors, not truth.
        UpdateSensors(m_registry, dt);

        // Then let each AI ship set its own radar from that picture: it goes active
        // while it holds a contact and dark when it holds none (see UpdateEmcon).
        // After UpdateSensors so it reads a current picture; the activeOn it sets is
        // read by the next tick's sensor pass, closing the two-sided EMCON loop.
        UpdateEmcon(m_registry);

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
        // What the player's ship can see: shot effects (impacts, splashes) beyond
        // this are raised silently, matching sight to what is on the water.
        Vantage const view{ m_registry.get<Physics>(m_ship).body->GetPosition(),
                            m_registry.get<Sensors>(m_ship).visualRangeM };
        UpdateWeapons(m_registry, m_audio, m_shake, view, dt);
        UpdateProjectiles(m_registry, m_audio, m_shake, m_terrain, view, dt);
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
        DrawFlightPlansPanel();
        DrawContactList();
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
            bool const enemy = m_registry.get<Combatant>(ship).faction == Faction::Enemy;
            if (enemy && (!m_showEnemyArcs || !SeesHull(picture, ship))) {
                continue;
            }
            // Enemy arcs are the debug overlay gated only by the toggle above, so
            // draw every mount; the player's own arcs still honour the per-unit
            // Arcs toggles in the Target window.
            DrawArcs(m_graphics, m_registry, m_camera, ship, /*forceAll=*/enemy);
        }
        DrawTarget(m_graphics, m_registry, m_camera, m_ship);
        DrawTargetMarker(m_graphics, m_registry, m_camera, m_ship);

        DrawProjectiles(m_graphics, m_registry, m_camera);
        // Point-defence tracers and muzzle flash show only within the player's visual
        // range, like the shot effects and the CIWS report itself (see UpdateWeapons):
        // a distant duel is fought unseen. The player's own mounts always draw (its
        // hull sits at zero range from itself).
        b2Vec2 const selfPos = m_registry.get<Physics>(m_ship).body->GetPosition();
        float const visualRangeM = m_registry.get<Sensors>(m_ship).visualRangeM;
        for (auto ship : m_registry.view<Physics, Armament>()) {
            if ((m_registry.get<Physics>(ship).body->GetPosition() - selfPos).Length() > visualRangeM) {
                continue;
            }
            DrawPointDefenseFire(m_graphics, m_registry, m_camera, ship);
        }

        // The authored flight plans on the water, the one being edited highlighted.
        DrawFlightPlans(m_graphics, m_registry, m_camera, m_ship, m_planningId);
    }

    void GameLayer::DrawDebugLayer() {
        // On top of everything: the aggro-range rings around the AI ships, the
        // weapon-spread previews — a line to each enabled weapon's target and the
        // disc its shots may land within — and the passive-TMA reveal (its cut
        // geometry and estimate-vs-truth error). Each carries its own toggle, so
        // this layer being up only makes them available.
        for (auto ship : m_registry.view<Physics, Aggro>()) {
            DrawAggroRing(m_graphics, m_registry, m_camera, ship);
        }
        for (auto ship : m_registry.view<Physics, Armament>()) {
            DrawWeaponSpread(m_graphics, m_registry, m_camera, ship);
        }
        DrawTmaOverlay(m_graphics, m_registry, m_camera, m_ship);
    }

    void GameLayer::DrawAggroDebug() {
        AggroTuning& tuning = AggroTuningRef();
        ImGui::Begin("Aggro (debug)");
        ImGui::SliderFloat("Aggro range (m)", &tuning.aggroRangeM, 100.0f, 4000.0f, "%.0f");
        ImGui::SliderFloat("Disengage range (m)", &tuning.disengageRangeM, 100.0f, 5000.0f, "%.0f");
        ImGui::Checkbox("Show aggro rings", &tuning.showRings);
        ImGui::Checkbox("Show enemy arcs", &m_showEnemyArcs);
        ImGui::End();
    }

    void GameLayer::DrawTmaDebug() {
        auto const* trackFile = m_registry.try_get<TrackFile>(m_ship);
        ImGui::Begin("TMA (debug)");
        // Master switch for the contact positional noise, kept above the track list
        // so it is reachable even with nothing held — flip it to A/B the feature, or
        // leave it off to disable the noise entirely (see SensorTuning).
        ImGui::Checkbox("Contact noise", &SensorTuningRef().noiseEnabled);
        ImGui::Separator();
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
        ImGui::Checkbox("Show resolutions (cuts + truth)", &tuning.showResolutions);

        constexpr float kMetresPerSecToKnots = 1.94384f;
        b2Vec2 const ownPos = m_registry.get<Physics>(m_ship).body->GetPosition();

        // One block per open track, led by the contact's call-sign so it can be
        // matched to its row in the Contacts window, then its current bearing. The
        // call-sign is a neutral tag, not the class — the identity stays masked
        // here, which the passive rung has not earned. The numbers below are the
        // solver's own state, so a stalled solution reads as either too straight a
        // leg (low observability) or a poor fit (high misfit), which is exactly what
        // to change own course for.
        int index = 0;
        for (auto const& entry : trackFile->tracks) {
            TmaTrack const& track = entry.second;
            ImGui::PushID(index++);
            float const brgDeg = Norm360(track.samples.back().bearing * moth_ui::kRadToDeg);
            // The Contacts window reconciles the call-sign map before this window
            // draws (see Draw's order), so a live track's contact has a slot; the
            // fallback only covers a track outliving its contact by a frame.
            auto const lit = m_contactLabels.find(entry.first);
            std::string const name = lit != m_contactLabels.end() ? BlipLabel(lit->second) : "Blip ?";
            ImGui::Separator();
            ImGui::TextUnformatted(
                fmt::format("{}   Brg {:03.0f}   cuts {}", name, brgDeg, track.samples.size()).c_str());

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

    void GameLayer::DrawContactList() {
        auto const& contacts = m_registry.get<ContactPicture>(m_ship).contacts;

        // Reconcile the call-sign map with the live picture before drawing. Drop
        // labels whose contact the ship no longer holds first, so a freed slot can
        // be reused; then hand any newly held contact the lowest free slot. Kept in
        // step with the picture here rather than in the sensor system because a
        // name is a presentation concern, not a measurement.
        for (auto it = m_contactLabels.begin(); it != m_contactLabels.end();) {
            it = contacts.count(it->first) != 0 ? std::next(it) : m_contactLabels.erase(it);
        }
        for (auto const& entry : contacts) {
            if (m_contactLabels.count(entry.first) != 0) {
                continue;
            }
            int slot = 0;
            for (bool taken = true; taken;) {
                taken = false;
                for (auto const& kv : m_contactLabels) {
                    if (kv.second == slot) {
                        taken = true;
                        ++slot;
                        break;
                    }
                }
            }
            m_contactLabels.emplace(entry.first, slot);
        }

        ImGui::Begin("Contacts");
        if (contacts.empty()) {
            ImGui::TextUnformatted("No contacts held");
            ImGui::End();
            return;
        }

        // Sort by slot so the rows hold a stable order (Blip A, B, C...) rather
        // than jumping frame to frame with the unordered picture's iteration.
        std::vector<std::pair<int, entt::entity>> rows;
        rows.reserve(contacts.size());
        for (auto const& entry : contacts) {
            rows.emplace_back(m_contactLabels.at(entry.first), entry.first);
        }
        std::sort(rows.begin(), rows.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });

        constexpr float kMetresPerSecToKnots = 1.94384f;
        b2Vec2 const shipPos = m_registry.get<Physics>(m_ship).body->GetPosition();

        if (ImGui::BeginTable("contacts", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("How");
            ImGui::TableSetupColumn("Class");
            ImGui::TableSetupColumn("Brg");
            ImGui::TableSetupColumn("Rng");
            ImGui::TableSetupColumn("Motion");
            ImGui::TableHeadersRow();

            for (auto const& [slot, entity] : rows) {
                Contact const& c = contacts.at(entity);
                AimBelief const belief = KnownAim(m_registry, m_ship, entity);
                bool const fresh = c.staleness == 0.0f;

                // A row dimmed once the contact is no longer detected this tick — a
                // coasting radar ghost or a lapsed bearing — so a live plot reads
                // apart from a decaying one at a glance.
                auto cell = [fresh](std::string const& s) {
                    if (fresh) {
                        ImGui::TextUnformatted(s.c_str());
                    } else {
                        ImGui::TextDisabled("%s", s.c_str());
                    }
                };

                // How it is held, at the rung it has earned: seen outright, a radar
                // fix, or a bare passive bearing. Never more than the ship knows.
                char const* how = c.level == DetectLevel::Visual    ? "Visual"
                                  : c.level == DetectLevel::Bearing ? "ESM"
                                                                    : "Radar";

                // Class resolves only once identified; a positioned but unclassified
                // return stays Unknown, as at the head of the Target window.
                std::string cls = "Unknown";
                if (c.identified) {
                    auto const* id = m_registry.try_get<Identity>(entity);
                    cls = id != nullptr ? id->name : "contact";
                }

                // Bearing is always known — off the fix if there is one, else the
                // raw passive cut. Range shows only when a fix (or a solved TMA
                // estimate, marked "~") places the contact; a bare bearing shows
                // none, never leaking the range ESM did not give. Motion stays
                // masked until the class (or a passive solution) resolves, mirroring
                // the belief KnownAim reports.
                float brgRad = c.bearing;
                std::string rng = "---";
                std::string motion = "---";
                if (belief.ok) {
                    b2Vec2 const to = belief.pos - shipPos;
                    brgRad = std::atan2(to.y, to.x);
                    rng = fmt::format("{}{:.1f} km", belief.estimate ? "~" : "", to.Length() / 1000.0f);
                    if (belief.estimate || c.motionKnown) {
                        // Course and speed off the snapshot's tracked velocity, never
                        // the live hull — a radar fix once its motion has resolved, a
                        // passive TMA fix from its estimate — so a stale contact shows
                        // the course it was last making, not where it has since turned
                        // unobserved. A contact still being tracked toward a solution
                        // shows range and bearing only, the way course lags a fresh
                        // fix in reality.
                        float const spdKn = belief.vel.Length() * kMetresPerSecToKnots;
                        float const crsDeg = Norm360(std::atan2(belief.vel.y, belief.vel.x) * moth_ui::kRadToDeg);
                        motion = fmt::format("crs {:03.0f}  {:.0f} kn", crsDeg, spdKn);
                    }
                }
                float const brgDeg = Norm360(brgRad * moth_ui::kRadToDeg);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                cell(BlipLabel(slot));
                ImGui::TableNextColumn();
                cell(how);
                ImGui::TableNextColumn();
                cell(cls);
                ImGui::TableNextColumn();
                cell(fmt::format("{:03.0f}", brgDeg));
                ImGui::TableNextColumn();
                cell(rng);
                ImGui::TableNextColumn();
                cell(motion);
            }
            ImGui::EndTable();
        }

        ImGui::End();
    }

    void GameLayer::DrawFlightPlansPanel() {
        auto& library = m_registry.get<FlightPlanLibrary>(m_ship);
        ImGui::Begin("Flight Plans");

        // Start a new plan and drop straight into authoring it: the button hands out
        // a fresh id, and from here clicks on the water append waypoints until Escape.
        if (ImGui::Button("New plan")) {
            FlightPlan plan;
            plan.id = library.nextId++;
            plan.name = fmt::format("Plan {}", plan.id + 1);
            library.plans.push_back(std::move(plan));
            m_planningId = library.plans.back().id;
        }
        if (m_planningId >= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("click to add waypoints - Esc to finish");
        }
        ImGui::Separator();

        if (library.plans.empty()) {
            ImGui::TextUnformatted("No plans");
            ImGui::End();
            return;
        }

        // One block per plan: its name, an Add-waypoints/Done toggle for authoring,
        // Delete, and its waypoints each with a remove. Edits to the vectors are
        // deferred to after the loop so nothing is mutated mid-iteration.
        int deletePlan = -1;
        for (auto& plan : library.plans) {
            ImGui::PushID(plan.id);
            ImGui::TextUnformatted(plan.name.c_str());
            ImGui::SameLine();
            bool const planning = plan.id == m_planningId;
            if (ImGui::SmallButton(planning ? "Done" : "Add waypoints")) {
                m_planningId = planning ? -1 : plan.id;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) {
                deletePlan = plan.id;
            }

            if (plan.waypoints.empty()) {
                ImGui::TextDisabled("  (no waypoints)");
            }
            int removeWaypoint = -1;
            for (std::size_t i = 0; i < plan.waypoints.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextUnformatted(fmt::format("  {}: {:.0f}, {:.0f}", i + 1, plan.waypoints[i].x,
                                                   plan.waypoints[i].y)
                                           .c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    removeWaypoint = static_cast<int>(i);
                }
                ImGui::PopID();
            }
            if (removeWaypoint >= 0) {
                plan.waypoints.erase(plan.waypoints.begin() + removeWaypoint);
            }
            ImGui::Separator();
            ImGui::PopID();
        }

        if (deletePlan >= 0) {
            if (m_planningId == deletePlan) {
                m_planningId = -1;
            }
            library.plans.erase(std::remove_if(library.plans.begin(), library.plans.end(),
                                               [&](FlightPlan const& p) { return p.id == deletePlan; }),
                                library.plans.end());
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

    std::vector<std::pair<entt::entity, std::string>> GameLayer::DesignatableContacts() {
        std::vector<std::pair<entt::entity, std::string>> out;
        auto const* picture = m_registry.try_get<ContactPicture>(m_ship);
        if (picture == nullptr) {
            return out;
        }
        // Every held contact that has a firing solution (KnownAim ok): a positioned
        // fix, or a passive bearing the TMA solver has ranged. A bare unsolved
        // bearing is not designatable — there is nothing to lay guns on. Labelled by
        // the call-sign pinned on the layer, with the class once identified.
        for (auto const& entry : picture->contacts) {
            entt::entity const contact = entry.first;
            if (!m_registry.valid(contact) || !KnownAim(m_registry, m_ship, contact).ok) {
                continue;
            }
            auto const labelIt = m_contactLabels.find(contact);
            std::string const callSign =
                labelIt != m_contactLabels.end()
                    ? fmt::format("Blip {}", static_cast<char>('A' + labelIt->second))
                    : "Blip ?";
            char const* cls = "Unknown";
            if (entry.second.identified) {
                if (auto const* id = m_registry.try_get<Identity>(contact); id != nullptr) {
                    cls = id->name.c_str();
                }
            }
            out.emplace_back(contact, fmt::format("{} - {}", callSign, cls));
        }
        // Sorted by call-sign slot so the dropdown order is stable frame to frame.
        std::sort(out.begin(), out.end(), [this](auto const& a, auto const& b) {
            auto const ita = m_contactLabels.find(a.first);
            auto const itb = m_contactLabels.find(b.first);
            int const sa = ita != m_contactLabels.end() ? ita->second : std::numeric_limits<int>::max();
            int const sb = itb != m_contactLabels.end() ? itb->second : std::numeric_limits<int>::max();
            return sa < sb;
        });
        return out;
    }

    void GameLayer::DrawTargetPanel() {
        auto& fireControl = m_registry.get<FireControl>(m_ship);
        auto& library = m_registry.get<FlightPlanLibrary>(m_ship);
        auto* armament = m_registry.try_get<Armament>(m_ship);

        ImGui::Begin("Target");
        if (armament == nullptr) {
            ImGui::TextUnformatted("No armament");
            ImGui::End();
            return;
        }
        auto& weapons = armament->weapons;

        // The contacts any unit may designate this frame, shared across every unit's
        // dropdown; and the groups (channels shared by mounts), labelled A, B... by
        // order, for the group headers and the "Add to group" menus.
        auto const options = DesignatableContacts();
        std::vector<std::pair<int, std::string>> groups; // (channel id, label)
        for (auto const& channel : fireControl.channels) {
            if (channel.group) {
                groups.emplace_back(channel.id,
                                    fmt::format("Group {}", static_cast<char>('A' + groups.size())));
            }
        }

        // A fire unit's target dropdown: pick a held contact by call-sign (or clear).
        // Designation lives entirely here — clicking the water is a helm order — and
        // never opens fire by itself; that is the Fire button's job.
        auto targetDropdown = [&](FireChannel& channel) {
            std::string preview = "(none)";
            if (channel.target != entt::null && m_registry.valid(channel.target)) {
                preview = "(holding)"; // designated, but its fix has lapsed this frame
                for (auto const& option : options) {
                    if (option.first == channel.target) {
                        preview = option.second;
                        break;
                    }
                }
            }
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::BeginCombo("Target", preview.c_str())) {
                if (ImGui::Selectable("(none)", channel.target == entt::null)) {
                    channel.target = entt::null;
                    channel.firing = false;
                }
                for (auto const& option : options) {
                    if (ImGui::Selectable(option.second.c_str(), option.first == channel.target) &&
                        channel.target != option.first) {
                        channel.target = option.first;
                        channel.firing = false;
                    }
                }
                ImGui::EndCombo();
            }
        };

        // A plan-capable unit's flight-plan dropdown: pick an authored plan for its
        // launchers to fly (or clear). A plan makes the unit fire-and-forget — no
        // designated contact needed — so selecting one is an alternative to the
        // target above, not an addition to it (see combat_system). Shown only for a
        // unit whose munition supports plans.
        auto flightPlanDropdown = [&](FireChannel& channel) {
            std::string preview = "(none)";
            for (auto const& plan : library.plans) {
                if (plan.id == channel.flightPlanId) {
                    preview = plan.name;
                    break;
                }
            }
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::BeginCombo("Flight plan", preview.c_str())) {
                if (ImGui::Selectable("(none)", channel.flightPlanId == -1)) {
                    channel.flightPlanId = -1;
                }
                for (auto const& plan : library.plans) {
                    if (ImGui::Selectable(plan.name.c_str(), plan.id == channel.flightPlanId)) {
                        channel.flightPlanId = plan.id;
                    }
                }
                ImGui::EndCombo();
            }
        };

        // A fire unit's order buttons — identical for a lone mount and a group:
        // Fire/Hold (standing), Salvo (one round from whatever is loaded), and free
        // fire (release to the nearest foe the guns can bear on). Fire is never
        // disabled for want of a gun bearing — ordering fire while manoeuvring onto
        // the mark is the point, and the guns join in as their arcs come on.
        auto orderButtons = [&](FireChannel& channel, Weapon* weapon = nullptr) {
            if (ImGui::Button(channel.firing ? "Hold" : "Fire")) {
                channel.firing = !channel.firing;
            }
            ImGui::SameLine();
            if (ImGui::Button("Salvo")) {
                channel.salvo = true;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Free", &channel.freeFire);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Engage the nearest foe the guns can bear on.\n"
                                  "The designated target still comes first.");
            }
            // Show or hide this unit's firing arcs on the water — a display toggle to
            // declutter the view.
            ImGui::SameLine();
            ImGui::Checkbox("Arcs", &channel.showArc);
            // A lone gun's spread-disc preview sits beside its arc toggle, the two
            // display toggles together. (A group passes no weapon — its members carry
            // their own spread toggle in the member list.)
            if (weapon != nullptr && weapon->kind == WeaponKind::Gun) {
                ImGui::SameLine();
                ImGui::Checkbox("Show spread", &weapon->showSpread);
            }
            // How many rounds a Salvo releases from each weapon on the unit — a gun
            // ripples that many shots over its cooldown, a launcher fires that many
            // tubes (capped at those loaded).
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::InputInt("Salvo size", &channel.salvoSize)) {
                channel.salvoSize = std::clamp(channel.salvoSize, 1, 99);
            }
        };

        // A mount's own extras, not shared by a group: the spread preview toggle for
        // a gun, or the loaded munition for a launcher. (Salvo size is a unit control
        // now, on the order row above.)
        auto weaponExtras = [&](Weapon& weapon) {
            if (weapon.kind == WeaponKind::Gun) {
                ImGui::Checkbox("Show spread", &weapon.showSpread);
            } else if (!weapon.munitionName.empty()) {
                ImGui::TextDisabled("(%s)", weapon.munitionName.c_str());
            }
        };

        // A mount's one-line status about its unit's order: slewing onto the mark,
        // firing, or holding acquired, with the reload clock alongside; a launcher
        // shows its ready tubes instead. Read off the mount's own lay, so a slewing
        // gun says so rather than "firing".
        auto weaponStatus = [&](Weapon const& weapon, FireChannel const& channel) {
            bool const laid = weapon.target != entt::null && m_registry.valid(weapon.target);
            bool const shooting = channel.firing || channel.freeFire;
            char const* status = "no bearing";
            if (laid && !weapon.acquired) {
                status = "slewing";
            } else if (laid) {
                status = shooting ? "firing" : "acquired, holding";
            }
            if (weapon.kind != WeaponKind::Gun) {
                std::string line = fmt::format("{}  {}/{} tubes", status, weapon.readyTubes, weapon.tubeCount);
                if (weapon.readyTubes < weapon.tubeCount && weapon.reloadTimer > 0.0f) {
                    line += fmt::format("  reloading {:.1f}s", weapon.reloadTimer);
                }
                ImGui::TextUnformatted(line.c_str());
            } else if (weapon.cooldownRemaining > 0.0f) {
                ImGui::TextUnformatted(
                    fmt::format("{}  reloading {:.1f}s", status, weapon.cooldownRemaining).c_str());
            } else {
                ImGui::TextUnformatted(status);
            }
        };

        // A deferred edit to the unit list — at most one per frame, applied after the
        // draw so the channel vector is not mutated mid-iteration.
        enum class Op { None, NewGroup, JoinGroup, LeaveGroup, Disband };
        Op op = Op::None;
        std::size_t opWeapon = 0; // mount index for NewGroup / JoinGroup / LeaveGroup
        int opGroup = -1;         // group channel id for JoinGroup

        // The battery as a flat list of fire units, a separator between each: a lone
        // mount reads as itself, a group as the same controls plus its member list.
        // Groups are listed first, the lone mounts after, so the split-fire units sit
        // at the top; within each the channel order holds. Each unit is scoped by its
        // channel id so its widgets stay distinct. (The pointers stay valid — the
        // channel vector is only edited after this draw, from the deferred op.)
        std::vector<FireChannel*> order;
        for (auto& channel : fireControl.channels) {
            if (channel.group) {
                order.push_back(&channel);
            }
        }
        for (auto& channel : fireControl.channels) {
            if (!channel.group) {
                order.push_back(&channel);
            }
        }
        std::size_t groupOrdinal = 0;
        for (auto* channelPtr : order) {
            FireChannel& channel = *channelPtr;
            ImGui::PushID(channel.id);
            ImGui::Separator();
            if (channel.group) {
                ImGui::TextUnformatted(groups[groupOrdinal++].second.c_str());
                ImGui::SameLine();
                bool const disband = ImGui::SmallButton("Disband");
                // A group with any plan-capable launcher is flown by its plan and
                // shows the plan dropdown in place of the target one (see the lone
                // mount above); a group of guns keeps its target dropdown.
                bool planCapable = false;
                for (auto const& weapon : weapons) {
                    if (weapon.channel == channel.id && weapon.kind != WeaponKind::Gun &&
                        weapon.munitionFlightPlan) {
                        planCapable = true;
                        break;
                    }
                }
                if (planCapable) {
                    flightPlanDropdown(channel);
                } else {
                    targetDropdown(channel);
                }
                orderButtons(channel);

                int bearing = 0;
                int total = 0;
                for (auto const& weapon : weapons) {
                    if (weapon.channel == channel.id) {
                        ++total;
                        bearing += weapon.hasTarget ? 1 : 0;
                    }
                }
                ImGui::TextUnformatted(fmt::format("{} of {} guns bear", bearing, total).c_str());

                // The mounts the group drives, each with a Remove back to its own
                // lone unit.
                for (std::size_t i = 0; i < weapons.size(); ++i) {
                    if (weapons[i].channel != channel.id) {
                        continue;
                    }
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::Bullet();
                    ImGui::TextUnformatted(weapons[i].name.empty() ? "Weapon" : weapons[i].name.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) {
                        op = Op::LeaveGroup;
                        opWeapon = i;
                    }
                    weaponExtras(weapons[i]);
                    weaponStatus(weapons[i], channel);
                    ImGui::PopID();
                }
                if (total == 0) {
                    ImGui::TextDisabled("(no weapons)");
                }
                if (disband) {
                    op = Op::Disband;
                    opGroup = channel.id;
                }
            } else {
                // A lone mount: the single weapon that owns this channel.
                std::size_t wi = weapons.size();
                for (std::size_t i = 0; i < weapons.size(); ++i) {
                    if (weapons[i].channel == channel.id) {
                        wi = i;
                        break;
                    }
                }
                if (wi == weapons.size()) {
                    ImGui::PopID();
                    continue; // an orphaned channel with no owning mount; cleaned below
                }
                Weapon& weapon = weapons[wi];
                ImGui::TextUnformatted(weapon.name.empty() ? "Weapon" : weapon.name.c_str());
                // A plan-capable launcher is flown by its plan alone — fire-and-
                // forget, its own seeker finds the mark — so it shows the flight-plan
                // dropdown in place of the target one, never both.
                if (weapon.kind != WeaponKind::Gun && weapon.munitionFlightPlan) {
                    flightPlanDropdown(channel);
                } else {
                    targetDropdown(channel);
                }
                orderButtons(channel, &weapon);
                // A launcher shows its loaded munition; a gun's spread toggle now
                // rides the order row above, beside the arc toggle.
                if (weapon.kind != WeaponKind::Gun && !weapon.munitionName.empty()) {
                    ImGui::TextDisabled("(%s)", weapon.munitionName.c_str());
                }
                // Each weapon shows its engagement range. (Skipped for groups, whose
                // members may reach differently.)
                ImGui::Text("Range: %.0f m", weapon.range);
                weaponStatus(weapon, channel);

                // Fold this mount into a group — an existing one, or a fresh group
                // (which carries the mount's current order over so nothing resets).
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("##addto", "Add to group")) {
                    for (auto const& g : groups) {
                        if (ImGui::Selectable(g.second.c_str())) {
                            op = Op::JoinGroup;
                            opWeapon = wi;
                            opGroup = g.first;
                        }
                    }
                    if (!groups.empty()) {
                        ImGui::Separator();
                    }
                    if (ImGui::Selectable("New group")) {
                        op = Op::NewGroup;
                        opWeapon = wi;
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::PopID();
        }

        // Apply the deferred edit. Moving a mount off a channel is all it takes; the
        // cleanup below drops whatever channel that leaves unowned.
        switch (op) {
        case Op::NewGroup: {
            FireChannel g{ fireControl.nextId++, /*group*/ true };
            for (auto const& c : fireControl.channels) {
                if (c.id == weapons[opWeapon].channel) {
                    g.target = c.target;
                    g.firing = c.firing;
                    g.freeFire = c.freeFire;
                    break;
                }
            }
            weapons[opWeapon].channel = g.id;
            fireControl.channels.push_back(g);
            break;
        }
        case Op::JoinGroup:
            weapons[opWeapon].channel = opGroup; // adopts the group's order
            break;
        case Op::LeaveGroup: {
            FireChannel lone{ fireControl.nextId++, /*group*/ false };
            weapons[opWeapon].channel = lone.id;
            fireControl.channels.push_back(lone);
            break;
        }
        case Op::Disband:
            for (auto& weapon : weapons) {
                if (weapon.channel == opGroup) {
                    FireChannel lone{ fireControl.nextId++, /*group*/ false };
                    weapon.channel = lone.id;
                    fireControl.channels.push_back(lone);
                }
            }
            break;
        case Op::None:
            break;
        }
        if (op != Op::None) {
            // Drop every channel no mount owns now — the lone unit a mount just left
            // for a group, and any group emptied of members. A lone channel always
            // has exactly one owner, so only those two are ever removed.
            std::unordered_set<int> owned;
            for (auto const& weapon : weapons) {
                if (weapon.channel >= 0) {
                    owned.insert(weapon.channel);
                }
            }
            fireControl.channels.erase(
                std::remove_if(fireControl.channels.begin(), fireControl.channels.end(),
                               [&](FireChannel const& c) { return owned.count(c.id) == 0; }),
                fireControl.channels.end());
        }

        // Point defence sits apart, outside the fire-unit model — a standing shield
        // with only an enable tick and a read-only status ("off" switched out, then
        // "tracking" while it slews onto a missile and "engaging" once trained).
        bool headed = false;
        for (std::size_t i = 0; i < weapons.size(); ++i) {
            Weapon& weapon = weapons[i];
            if (!weapon.pointDefense) {
                continue;
            }
            if (!headed) {
                ImGui::Separator();
                ImGui::TextUnformatted("Point Defense");
                headed = true;
            }
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
        }

        ImGui::End();
    }
}
