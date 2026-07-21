#include "game/tma_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

namespace naval {
    namespace {
        // --- solver policy (its effect is observable live in the TMA debug window) ---
        constexpr float kSampleIntervalS = 2.0f; // spacing between the bearing cuts fed to the fit
        constexpr int kMaxSamples = 60;          // rolling-window cap (~2 min of history at the interval)
        constexpr int kMinSamples = 6;           // fewest cuts before a fit is attempted
        constexpr float kMinSpanS = 20.0f;       // shortest baseline (oldest-to-newest cut) worth solving
        constexpr float kResidRefRad = 0.05f;    // angular misfit (~3 deg) that halves the confidence
        constexpr float kMaxSpeed = 40.0f;       // m/s; a faster fit is rejected as spurious (~78 kn)
        constexpr float kMinRangeM = 50.0f;      // reject a fit that lands on top of own ship
        constexpr float kMaxRangeFactor = 3.0f;  // reject a fit beyond this multiple of passive reach

        // --- track-hold policy: how a solved track is held, confirmed, and lost ---
        // A solved fix is not re-derived from scratch each cut: it is held and each
        // new bearing either confirms it (the estimate, dead-reckoned forward, still
        // points where the contact is heard) or refutes it (the bearing has walked
        // off — the contact manoeuvred). Confidence ratchets up only when a manoeuvre
        // makes the geometry observable, holds on a steady leg (which cannot separate
        // range, so neither earns nor loses), and bleeds when a cut refutes the fix.
        constexpr float kManeuverResidRad = 0.017f; // ~1 deg: a bearing this far off the held track reads as a target manoeuvre
        constexpr float kManeuverDecay = 0.6f;      // confidence retained per refuting cut

        // Solve the 4x4 system A x = b in place by Gaussian elimination with
        // partial pivoting, reporting the smallest pivot magnitude used as a
        // conditioning measure (the fit is near-singular — an unobservable, too-
        // straight leg — when this is tiny). Returns false if the matrix is
        // outright singular. A is destroyed.
        bool Solve4(double A[4][4], double b[4], double x[4], double& minPivot) {
            minPivot = std::numeric_limits<double>::max();
            for (int col = 0; col < 4; ++col) {
                int pivot = col;
                for (int r = col + 1; r < 4; ++r) {
                    if (std::abs(A[r][col]) > std::abs(A[pivot][col])) {
                        pivot = r;
                    }
                }
                if (pivot != col) {
                    std::swap(A[pivot], A[col]);
                    std::swap(b[pivot], b[col]);
                }
                double const p = A[col][col];
                if (std::abs(p) < 1e-12) {
                    return false;
                }
                minPivot = std::min(minPivot, std::abs(p));
                for (int r = col + 1; r < 4; ++r) {
                    double const f = A[r][col] / p;
                    for (int c = col; c < 4; ++c) {
                        A[r][c] -= f * A[col][c];
                    }
                    b[r] -= f * b[col];
                }
            }
            for (int r = 3; r >= 0; --r) {
                double s = b[r];
                for (int c = r + 1; c < 4; ++c) {
                    s -= A[r][c] * x[c];
                }
                x[r] = s / A[r][r];
            }
            return true;
        }

        // The shortest signed difference between two angles, wrapped to [-pi, pi].
        float WrapPi(float a) { return std::atan2(std::sin(a), std::cos(a)); }

        // Map a raw observability onto a [0,1] quality. Observability spans orders
        // of magnitude, so the map is logarithmic between the tuning's minObs and
        // obsFull: ~0 on a straight leg, climbing toward 1 as a manoeuvre conditions
        // the fit.
        float ObsQuality(float observability) {
            TmaTuning const& tuning = TmaTuningRef();
            float const lo = std::log10(std::max(tuning.minObs, 1e-12f));
            float const hi = std::log10(std::max(tuning.obsFull, tuning.minObs * 1.0001f));
            float const logObs = std::log10(std::max(observability, tuning.minObs));
            return std::clamp((logObs - lo) / (hi - lo), 0.0f, 1.0f);
        }

        // How well the cuts fit a straight-line track: near 1 while the bearings are
        // consistent, falling as the misfit grows past the reference.
        float FitTerm(float residualRad) {
            return std::clamp(1.0f - (residualRad / kResidRefRad), 0.0f, 1.0f);
        }

        // One batch fit over the window, a candidate the track may adopt — it holds
        // no state and does not decide confidence. Pseudo-linear least squares: each
        // cut says the contact lies on the measured bearing ray from that own-ship
        // position, i.e. the perpendicular n·(target - own) = 0. Stacked over the
        // history and solved for position and velocity (four unknowns), the fit is
        // rank-deficient on a straight leg and resolves once own ship has manoeuvred.
        //
        // Everything is referenced to the latest cut (own position O0, time t0) so
        // magnitudes stay small, and the velocity columns are scaled by the time
        // span so the position and velocity blocks share a scale — without which
        // the normal matrix is so ill-conditioned the pivot test is meaningless.
        // `observability` is always reported (for the debug readout); `ok` is set
        // only when the fit is conditioned enough and the solution is sane.
        struct Candidate {
            bool ok = false;
            b2Vec2 position{ 0.0f, 0.0f };
            b2Vec2 velocity{ 0.0f, 0.0f };
            float observability = 0.0f;
            float residualRad = 0.0f;
        };

        Candidate SolveCandidate(std::vector<TmaTrack::Sample> const& s, float maxRangeM) {
            Candidate cand;
            int const n = static_cast<int>(s.size());
            if (n < kMinSamples) {
                return cand;
            }
            float const span = s.back().t - s.front().t;
            if (span < kMinSpanS) {
                return cand;
            }

            b2Vec2 const O0 = s.back().ownPos;
            float const t0 = s.back().t;
            float const T = span; // velocity-column scale, so those columns are O(1) like the position ones

            double A[4][4] = {};
            double b[4] = {};
            for (auto const& smp : s) {
                float const w = (smp.t - t0) / T; // scaled time, in [-1, 0]
                float const nx = std::sin(smp.bearing);
                float const ny = -std::cos(smp.bearing);
                b2Vec2 const oc = smp.ownPos - O0;
                double const row[4] = { nx, ny, w * nx, w * ny };
                double const rhs = (static_cast<double>(oc.x) * nx) + (static_cast<double>(oc.y) * ny);
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        A[r][c] += row[r] * row[c];
                    }
                    b[r] += row[r] * rhs;
                }
            }

            double x[4] = {};
            double minPivot = 0.0;
            if (!Solve4(A, b, x, minPivot)) {
                return cand;
            }
            cand.observability = static_cast<float>(minPivot) / static_cast<float>(n);
            if (cand.observability < TmaTuningRef().minObs) {
                return cand; // the fit is singular to numerical precision — a straight leg
            }

            b2Vec2 const position{ O0.x + static_cast<float>(x[0]), O0.y + static_cast<float>(x[1]) };
            b2Vec2 const velocity{ static_cast<float>(x[2]) / T, static_cast<float>(x[3]) / T };

            // Reject fits the geometry cannot mean: behind the latest bearing (the
            // pseudo-linear form admits a mirror solution), on top of own ship, past
            // a sane multiple of passive reach, or moving implausibly fast.
            b2Vec2 const toContact = position - O0;
            float const rangeM = toContact.Length();
            float const dirX = std::cos(s.back().bearing); // latest bearing direction (not the normal)
            float const dirY = std::sin(s.back().bearing);
            if ((toContact.x * dirX) + (toContact.y * dirY) <= 0.0f) {
                return cand;
            }
            if (rangeM < kMinRangeM || rangeM > maxRangeM * kMaxRangeFactor) {
                return cand;
            }
            if (velocity.Length() > kMaxSpeed) {
                return cand;
            }

            // RMS angular misfit of the cuts to the fit: the perpendicular miss of
            // each predicted position from its bearing ray, over the range.
            double sumSq = 0.0;
            double sumRange = 0.0;
            for (auto const& smp : s) {
                float const w = (smp.t - t0) / T;
                b2Vec2 const predicted{ static_cast<float>(x[0]) + (static_cast<float>(x[2]) * w),
                                        static_cast<float>(x[1]) + (static_cast<float>(x[3]) * w) };
                b2Vec2 const oc = smp.ownPos - O0;
                b2Vec2 const rel{ predicted.x - oc.x, predicted.y - oc.y };
                float const nx = std::sin(smp.bearing);
                float const ny = -std::cos(smp.bearing);
                double const perp = (rel.x * nx) + (rel.y * ny);
                sumSq += perp * perp;
                sumRange += rel.Length();
            }
            float const rmsPerp = static_cast<float>(std::sqrt(sumSq / n));
            float const meanRange = std::max(1.0f, static_cast<float>(sumRange / n));
            cand.residualRad = rmsPerp / meanRange;
            cand.position = position;
            cand.velocity = velocity;
            cand.ok = true;
            return cand;
        }

        // Advance a track's held solution by one cut. Before it has solved, it needs
        // an observable candidate to lock on (a manoeuvre). Once solved, each cut
        // confirms or refutes the held estimate: if the estimate — dead-reckoned to
        // now — still points where the contact is heard, the fix holds and its
        // confidence ratchets up only toward what the current geometry can earn (so
        // a steady leg holds it, a manoeuvre lifts it); if the bearing has walked
        // off, the contact has manoeuvred and confidence bleeds until the fit
        // re-locks. Position and velocity are refreshed only from a well-conditioned
        // solve, so an ill-conditioned steady-leg fit cannot corrupt a good track.
        void AdvanceTrack(TmaTrack& track, Candidate const& cand, float measuredBearing, b2Vec2 ownPos) {
            TmaTuning const& tuning = TmaTuningRef();
            track.observability = cand.observability;
            if (cand.ok) {
                track.residualRad = cand.residualRad;
            }
            float const candQuality = cand.ok ? ObsQuality(cand.observability) * FitTerm(cand.residualRad) : 0.0f;

            if (!track.solved) {
                if (cand.ok && candQuality >= tuning.solvedFloor) {
                    track.position = cand.position;
                    track.velocity = cand.velocity;
                    track.confidence = candQuality;
                    track.solved = true;
                }
                return;
            }

            // Refute: if the held estimate, dead-reckoned to now, no longer points
            // where the contact is heard, the target has manoeuvred — bleed
            // confidence, which also lowers the refine bar below so a fresh leg can
            // re-lock.
            b2Vec2 const toEstimate = track.position - ownPos;
            float const predictedBearing = std::atan2(toEstimate.y, toEstimate.x);
            if (std::abs(WrapPi(measuredBearing - predictedBearing)) > kManeuverResidRad) {
                track.confidence *= kManeuverDecay;
            }

            // Refine the fix only from a solve at least as well-conditioned as the
            // one backing it (its confidence). A weaker, more ill-conditioned solve
            // is pulled toward too-close and too-slow by the least-squares min-norm
            // bias, so it must not drag a firmer fix: it is ignored, holding the
            // better estimate. This is why a gentle manoeuvre (a small speed change)
            // leaves a good fix be while a decisive one sharpens it, and a steady leg
            // (quality ~ 0) neither refines nor erodes. Confidence therefore tracks
            // the best conditioning seen, less any manoeuvre bleed.
            if (cand.ok && candQuality >= track.confidence) {
                track.position = cand.position;
                track.velocity = cand.velocity;
                track.confidence = candQuality;
            }

            if (track.confidence < tuning.solvedFloor) {
                track.solved = false;
            }
        }

        // Sweep the track file once the live bearing contacts have been fed. A
        // track fed this tick is kept. One not fed is either superseded — its
        // contact is now held at a better rung (still in the picture) or its hull is
        // gone — and dropped, or genuinely lost — gone from the picture entirely —
        // and, if solved, held on: dead-reckoned forward and bled down until it
        // fades out, so a lost passive fix decays rather than blinking out. An
        // unsolved lost track has nothing to hold and is dropped.
        void DecayLostTracks(entt::registry& registry, TrackFile& trackFile, ContactPicture const& picture,
                             std::unordered_set<entt::entity> const& bearingContacts, float dt) {
            float const lostFloor = TmaTuningRef().solvedFloor;
            for (auto it = trackFile.tracks.begin(); it != trackFile.tracks.end();) {
                entt::entity const entity = it->first;
                if (bearingContacts.count(entity) != 0) {
                    it = std::next(it);
                    continue;
                }
                TmaTrack& track = it->second;
                bool drop = !registry.valid(entity) || picture.contacts.count(entity) != 0 || !track.solved;
                if (!drop) {
                    track.position += dt * track.velocity;
                    track.confidence -= dt / kContactDecayS;
                    drop = track.confidence <= lostFloor;
                }
                it = drop ? trackFile.tracks.erase(it) : std::next(it);
            }
        }
    }

    TmaTuning& TmaTuningRef() {
        static TmaTuning tuning;
        return tuning;
    }

    void UpdateTMA(entt::registry& registry, float dt) {
        auto observers = registry.view<Physics, ContactPicture, TrackFile, Sensors>();
        for (auto self : observers) {
            b2Vec2 const ownPos = observers.get<Physics>(self).body->GetPosition();
            auto const& pictureComp = observers.get<ContactPicture>(self);
            auto const& picture = pictureComp.contacts;
            float const maxRangeM = observers.get<Sensors>(self).passiveRangeM;
            auto& trackFile = observers.get<TrackFile>(self);
            auto& tracks = trackFile.tracks;

            // Every contact held only as a bearing this tick feeds its track; those
            // held better (ranged, seen) or lost carry no passive track. Gather the
            // live bearing contacts so tracks for anything else can be dropped after.
            std::unordered_set<entt::entity> bearingContacts;
            for (auto const& entry : picture) {
                if (entry.second.level != DetectLevel::Bearing) {
                    continue;
                }
                bearingContacts.insert(entry.first);

                TmaTrack& track = tracks[entry.first]; // opens a fresh track on first hearing
                track.age += dt;
                track.sinceSample += dt;

                // Dead-reckon the held estimate forward every tick so it tracks the
                // contact's assumed motion between cuts, and so the confirm test
                // below compares against where the fix says the contact is *now*.
                if (track.solved) {
                    track.position += dt * track.velocity;
                }

                // Take a cut on the first tick a track is heard, then at a fixed
                // spacing — dense enough to trace the bearing's walk, sparse enough
                // that the window spans real manoeuvre rather than one heading. Each
                // cut advances the held solution: confirm, refute, or first lock.
                if (track.samples.empty() || track.sinceSample >= kSampleIntervalS) {
                    track.samples.push_back(TmaTrack::Sample{ track.age, ownPos, entry.second.trueBearing });
                    track.sinceSample = 0.0f;
                    if (static_cast<int>(track.samples.size()) > kMaxSamples) {
                        track.samples.erase(track.samples.begin());
                    }
                    AdvanceTrack(track, SolveCandidate(track.samples, maxRangeM), entry.second.trueBearing, ownPos);
                }
            }

            // Sweep the track file: keep those fed this tick, drop the superseded or
            // dead, and hold genuinely lost solved tracks as decaying estimates.
            DecayLostTracks(registry, trackFile, pictureComp, bearingContacts, dt);
        }
    }
}
