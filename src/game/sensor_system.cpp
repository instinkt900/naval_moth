#include "game/sensor_system.h"

#include "game/components.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace naval {
    namespace {
        // The dwell a positional fix must accumulate to resolve a contact. Motion
        // (course and speed) falls out of tracking first — a run of fixes
        // differenced over kMotionResolveS betrays where the contact is heading —
        // while the class settles far more slowly, over the minutes of unbroken hold
        // kIdentifyDwellS asks. Seeing the hull (inside visual range) shortcuts both
        // at once; a radar contact earns them only by being held, with no close-range
        // shortcut, so a distant blip is a position long before it is a known ship.
        constexpr float kMotionResolveS = 3.0f;   // s of hold before the velocity estimate is trusted (course shown)
        constexpr float kIdentifyDwellS = 180.0f;  // s of hold before the class resolves (~3 min)
        constexpr float kVelTauS = 8.0f;           // s; velocity-tracker time constant (alpha-beta, beta = dt/tau)

        // Classify one opposing hull `other` against the observer's senses and
        // refresh its contact at the best rung it reaches this tick, resetting its
        // staleness to zero. Leaves the picture untouched when the hull is beyond
        // all three rungs — the caller's aging then carries or expires whatever was
        // held. `otherSensors` is the hull's own Sensors (or null), needed only for
        // the passive rung, which depends on the *other* ship radiating.
        void RefreshContact(ContactPicture& picture, entt::entity other, b2Vec2 otherPos,
                            b2Vec2 otherVel, b2Vec2 delta, float d, float dt,
                            Sensors const& sensors, Sensors const* otherSensors) {
            // Seen outright inside visual range: the real hull, full truth at once —
            // a fixed position, its true motion (no tracking lag when you can watch
            // it), and an immediate identification. A positional fix, so both clocks
            // reset.
            if (d <= sensors.visualRangeM) {
                Contact& c = picture.contacts[other];
                c.level = DetectLevel::Visual;
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
                    c.lastPos = otherPos;
                    c.hasPos = true;
                } else {
                    c.velocity += (1.0f / kVelTauS) * (otherPos - c.lastPos);
                    c.lastPos = otherPos;
                }
                c.staleness = 0.0f;
                c.fixStaleness = 0.0f;
                c.dwell += dt;
                if (c.dwell >= kMotionResolveS) {
                    c.motionKnown = true;
                }
                if (c.dwell >= kIdentifyDwellS) {
                    c.identified = true;
                }
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
            c.bearing = std::atan2(delta.y, delta.x);
            c.strength = std::clamp((q - 1.0f) / (kFullStrengthQ - 1.0f), 0.0f, 1.0f);
            c.staleness = 0.0f;
        }
    }

    void UpdateSensors(entt::registry& registry, float dt) {
        auto observers = registry.view<Physics, Combatant, Sensors, ContactPicture>();
        for (auto self : observers) {
            b2Vec2 const selfPos = observers.get<Physics>(self).body->GetPosition();
            Faction const faction = observers.get<Combatant>(self).faction;
            Sensors const& sensors = observers.get<Sensors>(self);
            ContactPicture& picture = observers.get<ContactPicture>(self);

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
                               registry.try_get<Sensors>(other));
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
