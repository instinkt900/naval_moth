#include "game/attenuation.h"

#include "game/camera.h"

#include <algorithm>
#include <cmath>

namespace naval {
    namespace {
        // How far a thing carries across the sea: felt and heard out to
        // kSilenceM, nothing beyond. There is no full-effect band around the
        // camera, so the fade starts the moment something is off-centre.
        //
        // A reach in metres of water rather than a fraction of the view, so it is
        // a property of the sea and not of how closely you happen to be looking
        // at it. That does mean a shake can arrive from beyond the edge of the
        // screen when zoomed right in — the guns of a battle you are not looking
        // at still reach you, which is the point of measuring in metres.
        //
        // It has to clear the longest weapon range in the content by a good
        // margin, or the biggest guns in the game are the ones nobody can hear:
        // at 2500m this sat *inside* the mark-7's 3800m reach, so a battleship
        // duel at range was fought in silence, and anything more than a screen or
        // so out at a map-survey zoom fell off a cliff to exactly nothing rather
        // than fading. Raise it if a longer gun is ever authored.
        constexpr float kSilenceM = 6000.0f;

        // What the zoom does. A thing plays as authored only at kMaxZoom — right
        // down on the action — and is scaled to kZoomGainFloor when pulled all the
        // way out to kMinZoom, which is a floor rather than silence so a battle
        // you have zoomed out to watch is still audible. Raise the floor to
        // flatten the effect; lower it to make surveying the map quieter still.
        constexpr float kZoomGainFloor = 0.15f;

        // The gain for something `distance` metres from the camera. The fade is
        // squared rather than linear so it drops away quickly as it leaves the
        // near field and then trails off, which reads as distance far better than
        // an even ramp does.
        float DistanceGain(float distance) {
            if (distance >= kSilenceM) {
                return 0.0f;
            }
            float const t = distance / kSilenceM;
            return (1.0f - t) * (1.0f - t);
        }

        // The gain for the camera being zoomed to `pixelsPerMeter`.
        //
        // Interpolated on a log scale because zoom is multiplicative — the wheel
        // scales it by a constant factor per notch — so a ramp even in the zoom
        // number would spend nearly all its travel in the last few notches of
        // zoom-in and do almost nothing across the rest of the range. Against
        // the log, one notch of the wheel moves the gain by the same amount
        // wherever you are.
        float ZoomGain(float pixelsPerMeter) {
            float const t = std::clamp(std::log(pixelsPerMeter / kMinZoom) / std::log(kMaxZoom / kMinZoom),
                                       0.0f, 1.0f);
            return kZoomGainFloor + ((1.0f - kZoomGainFloor) * t);
        }
    }

    void Attenuation::SetCamera(Camera const& camera) {
        // Read from `center`, which is where the camera is actually looking,
        // rather than from the shaken picture — otherwise a shake would move the
        // point the next thing is measured against and feed itself.
        m_center = camera.center;
        m_zoomGain = ZoomGain(camera.pixelsPerMeter);
    }

    float Attenuation::GainAt(b2Vec2 position) const {
        return m_zoomGain * DistanceGain((position - m_center).Length());
    }
}
