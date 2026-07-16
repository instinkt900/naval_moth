#pragma once

#include "game/attenuation.h"

#include <box2d/box2d.h>

#include <random>

namespace naval {
    struct Camera;

    // The knock the camera takes when something violent happens in front of it.
    //
    // Deliberately shallow, and for the same reason the audio is: this exists to
    // give weight to things that already happen — a gun firing, a shell striking
    // home, a hull going up — not to be a system anyone tunes for its own sake.
    // There is one accumulated amplitude, not a shake per event: a jolt is added
    // to it, it decays away, and every frame the camera is displaced somewhere on
    // a circle of that radius. Two guns firing together shake harder than one
    // because they add to the same number, which is the whole of the mixing
    // model.
    //
    // How far a jolt reaches is not this class's business — it asks the shared
    // Attenuation, the same one that sets a sound's volume, so what you can
    // barely hear is what barely moves the picture and the two stay in step
    // through any amount of tuning. What is left here is only what a shake does
    // once it has arrived: accumulate, decay, and displace.
    //
    // A jolt's strength is decided when it lands, from where the camera was then,
    // exactly as a sound's volume is. Nothing is tracked afterwards — a shake is
    // over in a couple of hundred milliseconds, far too fast for the camera to
    // have moved anywhere that would matter.
    class CameraShake {
    public:
        // Where the camera is looking and how far it is zoomed in. Set once per
        // frame, before the systems that add anything run — a jolt's strength is
        // read off this.
        void SetCamera(Camera const& camera);

        // Adds a jolt of `amplitudeM` metres at `position` (world metres), scaled
        // by how much of it reaches the camera. The amplitude is what the event is
        // worth at full effect, dead centre and fully zoomed in; it is capped
        // once added, so a whole broadside landing at once cannot throw the
        // camera off the sea. An amplitude of 0 is the deliberate no-shake case
        // and costs nothing.
        void Add(float amplitudeM, b2Vec2 position);

        // Decays the shake and rolls the frame's offset. Call once per tick,
        // after everything that might add to it, so a gun fired this tick is felt
        // on this frame rather than the next.
        void Update(float dt);

        // This frame's camera displacement, in world metres.
        b2Vec2 OffsetM() const { return m_offsetM; }

    private:
        Attenuation m_attenuation;

        float m_amplitudeM = 0.0f;      // how hard the camera is shaking right now (m)
        b2Vec2 m_offsetM{ 0.0f, 0.0f }; // this frame's displacement (m)
        std::mt19937 m_rng{ std::random_device{}() };
    };
}
