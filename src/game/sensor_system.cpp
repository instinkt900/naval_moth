#include "game/sensor_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <random>

namespace naval {
    SensorTuning& SensorTuningRef() {
        static SensorTuning tuning;
        return tuning;
    }

    namespace {
        // The dwell a positional fix must accumulate to resolve a contact. Motion
        // (course and speed) falls out of tracking first — a run of fixes
        // differenced over kMotionResolveS betrays where the contact is heading —
        // while the class settles far more slowly, over the minutes of unbroken hold
        // an identification asks. That dwell is not one shared constant: each contact
        // draws its own threshold uniformly in [kIdentifyMinS, kIdentifyMaxS] when the
        // track is acquired (see Contact::identifyThresholdS), so a screen full of
        // blips resolves raggedly rather than all snapping to identified on the same
        // tick — one hull's cut reads clean early, another's takes the full eight
        // minutes. Seeing the hull (inside visual range) shortcuts both clocks at
        // once; a radar contact earns them only by being held, with no close-range
        // shortcut, so a distant blip is a position long before it is a known ship.
        constexpr float kMotionResolveS = 3.0f;    // s of hold before the velocity estimate is trusted (course shown)
        constexpr float kIdentifyMinS = 180.0f;    // s; fastest a radar class can resolve (3 min)
        constexpr float kIdentifyMaxS = 480.0f;    // s; slowest a radar class can resolve (8 min)
        constexpr float kVelTauS = 8.0f;           // s; velocity-tracker time constant (alpha-beta, beta = dt/tau)

        // Positional-noise offset tuning (see Contact::offset). Game-feel constants,
        // meant to be dialled by hand against the sensor ranges in hulls.json (tens
        // of km), not derived: a wide passive cut, a tight active one that only
        // exists until the class resolves, and a slow drift so the mark wanders
        // rather than jitters.
        constexpr float kBearingMaxOffsetM = 4000.0f; // widest passive wander (m of lateral offset at zero confidence)
        constexpr float kRangedMaxOffsetM = 300.0f;   // widest active-fix wander (m), before the class resolves
        constexpr float kDriftAngRate = 0.02f;        // rad·s^-1/2; angular random-walk rate — ~6° of drift over ~30 s
        constexpr float kMagEaseRate = 0.1f;          // 1/s; offset length chases its cap on a ~10 s time constant
        constexpr float kPi = 3.14159265f;

        // One generator for the whole run, seeded once, shared by every observer's
        // identification draw — the same pattern the wander system uses. The draw
        // happens once per contact at acquisition, not per tick.
        std::mt19937& IdentifyRng() {
            static std::mt19937 rng{ std::random_device{}() };
            return rng;
        }

        // A second run-wide generator for the per-tick noise drift, kept apart from
        // IdentifyRng so the one-shot identification draw stays reproducible
        // independent of how many drift steps happen to have run.
        std::mt19937& NoiseRng() {
            static std::mt19937 rng{ std::random_device{}() };
            return rng;
        }

        // Evolve a contact's positional-noise offset one tick: ease its length
        // toward the confidence-set cap `capM` while its direction wanders on a slow
        // random walk, so the mark drifts smoothly and tightens as the contact
        // firms. Seeds a random direction when the offset is ~zero (a fresh track,
        // or one whose cap collapsed to nothing and is opening back up).
        b2Vec2 DriftOffset(b2Vec2 offset, float capM, float dt) {
            std::mt19937& rng = NoiseRng();
            float const mag = offset.Length();
            float ang = 0.0f;
            if (mag > 1e-3f) {
                ang = std::atan2(offset.y, offset.x);
            } else {
                std::uniform_real_distribution<float> seed(-kPi, kPi);
                ang = seed(rng);
            }
            std::normal_distribution<float> step(0.0f, 1.0f);
            ang += step(rng) * kDriftAngRate * std::sqrt(dt);
            float const newMag = mag + ((capM - mag) * std::clamp(kMagEaseRate * dt, 0.0f, 1.0f));
            return { newMag * std::cos(ang), newMag * std::sin(ang) };
        }

        // The observer's last-tick TMA confidence on `other`, or 0 if it runs no
        // track file or has no solved solution for it yet. Used only to tighten a
        // passive cut's noise as its range solves.
        float SolvedConfidence(TrackFile const* trackFile, entt::entity other) {
            if (trackFile == nullptr) {
                return 0.0f;
            }
            auto const it = trackFile->tracks.find(other);
            return (it != trackFile->tracks.end() && it->second.solved) ? it->second.confidence : 0.0f;
        }

        // Classify one opposing hull `other` against the observer's senses and
        // refresh its contact at the best rung it reaches this tick, resetting its
        // staleness to zero. Leaves the picture untouched when the hull is beyond
        // all three rungs — the caller's aging then carries or expires whatever was
        // held. `otherSensors` is the hull's own Sensors (or null), needed only for
        // the passive rung, which depends on the *other* ship radiating.
        void RefreshContact(ContactPicture& picture, entt::entity other, b2Vec2 otherPos,
                            b2Vec2 otherVel, b2Vec2 delta, float d, float dt,
                            Sensors const& sensors, Sensors const* otherSensors,
                            float tmaConfidence, bool noiseEnabled) {
            // Seen outright inside visual range: the real hull, full truth at once —
            // a fixed position, its true motion (no tracking lag when you can watch
            // it), and an immediate identification. A positional fix, so both clocks
            // reset.
            if (d <= sensors.visualRangeM) {
                Contact& c = picture.contacts[other];
                c.level = DetectLevel::Visual;
                c.offset = { 0.0f, 0.0f }; // you can see it: no uncertainty to bake in
                c.lastPos = otherPos;
                c.hasPos = true;
                c.velocity = otherVel;
                c.motionKnown = true;
                c.staleness = 0.0f;
                c.fixStaleness = 0.0f;
                c.dwell += dt;
                c.identified = true;
                return;
            }
            // A ranged blip if the active radar is up and reaches it. A fix gives a
            // position each tick but not motion or identity outright: the velocity is
            // tracked out of the run of fixes, and dwell accrues toward first the
            // course (kMotionResolveS) and then, far later, the class (kIdentifyDwellS)
            // — both sticky once earned. Until then it is an unidentified Ranged blip.
            if (sensors.activeOn && d <= sensors.activeRangeM) {
                Contact& c = picture.contacts[other];
                // Track the fix. The first one only seeds a position — there is no
                // previous fix to difference. After that the position snaps to the
                // exact return while the velocity is nudged toward the one-tick
                // prediction error: an alpha-beta tracker (alpha = 1, beta = dt/kVelTauS)
                // whose estimate converges over ~kVelTauS. The prediction it corrects
                // against is lastPos as the ageing pass dead-reckoned it this tick.
                if (!c.hasPos) {
                    c.hasPos = true;
                    // First fix on this track: draw the class-resolve threshold it
                    // will have to dwell past. Drawn once here, so the contact keeps
                    // the same target for the life of the track rather than rerolling
                    // it each tick and never converging.
                    std::uniform_real_distribution<float> dist(kIdentifyMinS, kIdentifyMaxS);
                    c.identifyThresholdS = dist(IdentifyRng());
                    // No previous fix to difference; velocity stays as it was (zero
                    // on a fresh track).
                } else {
                    // Difference the true fix against the previous *true* position —
                    // the surfaced lastPos with its baked offset stripped off — so
                    // the positional noise rides along and never registers as motion.
                    c.velocity += (1.0f / kVelTauS) * (otherPos - (c.lastPos - c.offset));
                }
                c.staleness = 0.0f;
                c.fixStaleness = 0.0f;
                c.dwell += dt;
                if (c.dwell >= kMotionResolveS) {
                    c.motionKnown = true;
                }
                if (c.dwell >= c.identifyThresholdS) {
                    c.identified = true;
                }
                // A radar fix is accurate from the first return and only tightens as
                // the class is nailed: shrink the wander over the same dwell that
                // resolves identity, to nothing once identified. Then bake it into
                // the believed position everything downstream reads.
                float const resolve =
                    c.identified ? 1.0f : std::clamp(c.dwell / c.identifyThresholdS, 0.0f, 1.0f);
                c.offset = noiseEnabled ? DriftOffset(c.offset, kRangedMaxOffsetM * (1.0f - resolve), dt)
                                        : b2Vec2{ 0.0f, 0.0f };
                c.lastPos = otherPos + c.offset;
                c.level = c.identified ? DetectLevel::Identified : DetectLevel::Ranged;
                return;
            }
            // Else a passive bearing, if the contact is itself radiating and within
            // ESM reach. Depends on the *other* ship emitting, not on own radar —
            // this is how a dark ship still gets a cut on a careless one. Only a
            // bearing and a strength are kept; the range is thrown away deliberately
            // (see Contact::bearing), so a bearing yields no fix and cannot start a
            // decaying track.
            if (otherSensors == nullptr || !otherSensors->activeOn) {
                return;
            }
            // Received signal ~ emitter power / distance^2, with power ~ the
            // emitter's radar reach squared. The hearing floor is where an emitter
            // as loud as own radar sits at own passive range, so passiveRangeM stays
            // the anchor: the detection range for this emitter scales with how much
            // louder (bigger radar) it is than own set. A dead-in-the-water listener
            // with no radar of its own would divide by zero, but every hull's
            // activeRangeM is required positive.
            float const detectRangeM = sensors.passiveRangeM * (otherSensors->activeRangeM / sensors.activeRangeM);
            if (d > detectRangeM) {
                return;
            }
            // q = detectRange / d, the received signal relative to the floor: 1 at
            // the edge of hearing, larger as it strengthens. Normalised to [0,1]
            // against a "full strength" multiple. It reads off the received signal
            // alone, so a big emitter far off and a small one near resolve to the
            // same strength — big or near, not which.
            constexpr float kFullStrengthQ = 5.0f;
            float const q = detectRangeM / d;
            // A bearing is a detection but not a fix: reset staleness (the contact
            // is held) but leave fixStaleness climbing, so a contact downgraded from
            // a fix to a bearing keeps its live bearing while its blip decays.
            Contact& c = picture.contacts[other];
            c.level = DetectLevel::Bearing;
            c.strength = std::clamp((q - 1.0f) / (kFullStrengthQ - 1.0f), 0.0f, 1.0f);
            // Positional noise as a vector offset on the true contact, read out as
            // an angle. The wander is widest for a faint cut and tightens as the
            // received strength grows or the TMA solution firms (tmaConfidence is
            // last tick's — this runs before UpdateTMA). Strength alone bootstraps
            // it, so a closing contact tightens even before TMA solves and the
            // conf→noise loop can't deadlock. Only the direction is kept; no
            // displaced position is stored, so a bearing stays unplaceable.
            float const conf = std::max(c.strength, tmaConfidence);
            c.offset = noiseEnabled ? DriftOffset(c.offset, kBearingMaxOffsetM * (1.0f - conf), dt)
                                    : b2Vec2{ 0.0f, 0.0f };
            b2Vec2 const noisyDelta = delta + c.offset;
            c.bearing = std::atan2(noisyDelta.y, noisyDelta.x);
            c.trueBearing = std::atan2(delta.y, delta.x); // clean cut for the TMA solver
            c.staleness = 0.0f;
        }
    }

    void UpdateSensors(entt::registry& registry, float dt) {
        bool const noiseEnabled = SensorTuningRef().noiseEnabled;
        auto observers = registry.view<Physics, Combatant, Sensors, ContactPicture>();
        for (auto self : observers) {
            b2Vec2 const selfPos = observers.get<Physics>(self).body->GetPosition();
            Faction const faction = observers.get<Combatant>(self).faction;
            Sensors const& sensors = observers.get<Sensors>(self);
            ContactPicture& picture = observers.get<ContactPicture>(self);
            // Last tick's TMA solution per contact, if this observer runs one — read
            // here (UpdateSensors precedes UpdateTMA) to tighten a passive cut as its
            // range solves. Absent for an observer with no TrackFile.
            TrackFile const* trackFile = registry.try_get<TrackFile>(self);

            // Updated in place, not rebuilt: age both clocks on every held contact
            // first, then a detection below resets what it refreshes — any detection
            // clears staleness, only a positional fix clears fixStaleness. What is
            // left aged is a contact that has dropped a rung or dropped out, carried
            // on from its last-known position rather than erased outright.
            for (auto& entry : picture.contacts) {
                Contact& c = entry.second;
                c.staleness += dt;
                c.fixStaleness += dt;
                // Dead-reckon a positioned ghost forward on its last-known velocity,
                // so a lost fix coasts on its course rather than freezing on the
                // water. A contact re-fixed this tick has its lastPos overwritten by
                // the fix below, so only genuinely stale tracks actually drift.
                if (c.hasPos) {
                    c.lastPos += dt * c.velocity;
                }
            }

            for (auto other : registry.view<Physics, Combatant>()) {
                if (other == self || registry.get<Combatant>(other).faction == faction) {
                    continue;
                }
                b2Body* const otherBody = registry.get<Physics>(other).body;
                b2Vec2 const otherPos = otherBody->GetPosition();
                b2Vec2 const delta = otherPos - selfPos;
                RefreshContact(picture, other, otherPos, otherBody->GetLinearVelocity(),
                               delta, delta.Length(), dt, sensors,
                               registry.try_get<Sensors>(other),
                               SolvedConfidence(trackFile, other), noiseEnabled);
            }

            // Keep a track while it is either still detected (staleness zero) or
            // still carrying a visible position ghost (a fix within kContactDecayS);
            // forget it once neither holds. So a lost bearing with no fix falls off
            // at once, a lost positioned contact lingers as a decaying ghost, and a
            // contact still heard on a bearing is kept even after its blip has fully
            // faded. A destroyed hull's entity is purged outright — a track is memory
            // of a contact that left, not of one seen to die (its wreck stands in).
            for (auto it = picture.contacts.begin(); it != picture.contacts.end();) {
                if (!registry.valid(it->first)) {
                    it = picture.contacts.erase(it);
                    continue;
                }
                Contact const& c = it->second;
                bool const detected = c.staleness == 0.0f;
                bool const ghostVisible = c.hasPos && c.fixStaleness <= kContactDecayS;
                it = (!detected && !ghostVisible) ? picture.contacts.erase(it) : std::next(it);
            }
        }
    }
}
