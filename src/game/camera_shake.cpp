#include "game/camera_shake.h"

#include "game/camera.h"

#include <algorithm>
#include <cmath>

namespace naval {
    namespace {
        // The hardest the camera may ever shake (m), however much lands at once.
        // A cap rather than a sum without end: a broadside arriving on one tick
        // should hit harder than a single shell, but not throw the picture
        // somewhere the player has to wait to get back.
        constexpr float kMaxAmplitudeM = 10.0f;

        // How quickly a jolt dies away: the amplitude falls by 1/e every
        // kDecayTauS, so a shake is most of the way gone in about a fifth of a
        // second. Short on purpose — this is a knock, not a rumble, and a gun on
        // a fast cooldown must be able to land its next one into a camera that
        // has nearly settled, or the shake just smears into a permanent wobble.
        constexpr float kDecayTauS = 0.08f;

        // The amplitude below which the shake is simply over. An exponential
        // decay never actually reaches zero, and a fraction of a pixel of
        // displacement is not worth the reroll every tick for the rest of the
        // session.
        constexpr float kRestM = 0.02f;
    }

    void CameraShake::SetCamera(Camera const& camera) {
        m_attenuation.SetCamera(camera);
    }

    void CameraShake::Add(float amplitudeM, b2Vec2 position) {
        if (amplitudeM <= 0.0f) {
            return;
        }
        m_amplitudeM = std::min(kMaxAmplitudeM,
                                m_amplitudeM + (amplitudeM * m_attenuation.GainAt(position)));
    }

    void CameraShake::Update(float dt) {
        // Frame-rate independent exponential decay, the same approach the camera
        // pan eases with.
        m_amplitudeM *= std::exp(-dt / kDecayTauS);
        if (m_amplitudeM < kRestM) {
            m_amplitudeM = 0.0f;
            m_offsetM = { 0.0f, 0.0f };
            return;
        }
        // Somewhere new on a circle of the current radius every tick. White noise
        // rather than anything smoother because a jolt is over in a handful of
        // frames: there is no time for a wave to read as a wave, and the only
        // thing that carries is that the camera is unsettled and settling.
        std::uniform_real_distribution<float> angleDist(0.0f, 2.0f * b2_pi);
        float const angle = angleDist(m_rng);
        m_offsetM = { m_amplitudeM * std::cos(angle), m_amplitudeM * std::sin(angle) };
    }
}
