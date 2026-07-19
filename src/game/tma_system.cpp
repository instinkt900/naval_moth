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

        // Fit a constant-velocity solution to a track's bearing cuts and write it
        // back onto the track. Pseudo-linear least squares: each cut says the
        // contact lies on the measured bearing ray from that own-ship position, i.e.
        // the perpendicular n·(target - own) = 0. Stacked over the history and
        // solved for the target's position and velocity (four unknowns), the fit is
        // rank-deficient on a straight leg and resolves once own ship has manoeuvred.
        //
        // Everything is referenced to the latest cut (own position O0, time t0) so
        // magnitudes stay small, and the velocity columns are scaled by the time
        // span so the position and velocity blocks share a scale — without which
        // the normal matrix is so ill-conditioned the pivot test is meaningless.
        void Solve(TmaTrack& track, float maxRangeM) {
            track.solved = false;
            track.confidence = 0.0f;
            track.observability = 0.0f;
            track.residualRad = 0.0f;

            auto const& s = track.samples;
            int const n = static_cast<int>(s.size());
            if (n < kMinSamples) {
                return;
            }
            float const span = s.back().t - s.front().t;
            if (span < kMinSpanS) {
                return;
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
                return;
            }
            TmaTuning const& tuning = TmaTuningRef();
            float const observability = static_cast<float>(minPivot) / static_cast<float>(n);
            track.observability = observability;
            if (observability < tuning.minObs) {
                return; // the fit is singular to numerical precision — a straight leg
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
                return;
            }
            if (rangeM < kMinRangeM || rangeM > maxRangeM * kMaxRangeFactor) {
                return;
            }
            if (velocity.Length() > kMaxSpeed) {
                return;
            }

            // RMS angular misfit of the cuts to the fit, as a fit-quality term: the
            // perpendicular miss of each predicted position from its bearing ray,
            // over the range, root-mean-squared.
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
            float const residualRad = rmsPerp / meanRange;
            track.residualRad = residualRad;

            // Confidence: how observable the geometry is (the manoeuvre payoff),
            // discounted by how well the bearings actually fit a straight-line
            // track. Observability spans orders of magnitude, so it is mapped on a
            // log scale between the tuning's minObs and obsFull; the fit term stays
            // near 1 while bearings are exact and only bites when a contact has
            // manoeuvred inside the window and broken the constant-velocity model.
            float const lo = std::log10(std::max(tuning.minObs, 1e-12f));
            float const hi = std::log10(std::max(tuning.obsFull, tuning.minObs * 1.0001f));
            float const logObs = std::log10(std::max(observability, tuning.minObs));
            float const obsTerm = std::clamp((logObs - lo) / (hi - lo), 0.0f, 1.0f);
            float const fitTerm = std::clamp(1.0f - (residualRad / kResidRefRad), 0.0f, 1.0f);
            float const confidence = obsTerm * fitTerm;
            if (confidence < tuning.solvedFloor) {
                return;
            }

            track.position = position;
            track.velocity = velocity;
            track.confidence = confidence;
            track.solved = true;
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
            auto const& picture = observers.get<ContactPicture>(self).contacts;
            float const maxRangeM = observers.get<Sensors>(self).passiveRangeM;
            auto& tracks = observers.get<TrackFile>(self).tracks;

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

                // Take a cut on the first tick a track is heard, then at a fixed
                // spacing — dense enough to trace the bearing's walk, sparse enough
                // that the window spans real manoeuvre rather than one heading.
                if (track.samples.empty() || track.sinceSample >= kSampleIntervalS) {
                    track.samples.push_back(TmaTrack::Sample{ track.age, ownPos, entry.second.bearing });
                    track.sinceSample = 0.0f;
                    if (static_cast<int>(track.samples.size()) > kMaxSamples) {
                        track.samples.erase(track.samples.begin());
                    }
                    Solve(track, maxRangeM);
                }
            }

            // Drop tracks whose contact is no longer a bearing hold — ranged, seen,
            // or gone. Contact decay (keeping a lost track on from its last fix)
            // lands in the next step; for now a lost contact simply forgets.
            for (auto it = tracks.begin(); it != tracks.end();) {
                it = bearingContacts.count(it->first) == 0 ? tracks.erase(it) : std::next(it);
            }
        }
    }
}
