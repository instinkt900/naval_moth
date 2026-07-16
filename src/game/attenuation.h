#pragma once

#include <box2d/box2d.h>

namespace naval {
    struct Camera;

    // How much of something happening out on the water reaches the player: 1 is
    // full effect, 0 is nothing at all. Where it happened and how far the camera
    // has pulled back are both in it.
    //
    // One curve, deliberately shared. The audio scales a sound's volume by it and
    // the camera shake scales a jolt by it, so the two can never disagree about
    // how present a thing is: a gun you can barely hear is a gun that barely
    // moves the picture, and it stays that way through any amount of tuning
    // because there is only one number to tune. Two curves that merely happened
    // to match would drift apart the first time either was touched.
    //
    // What it is *not* is a claim that a shake and a sound are the same thing.
    // They share how far a thing reaches, not what it then does: the shake still
    // decides for itself how a jolt accumulates and decays, and the audio still
    // pans and pitches on its own. This is the reach, and only the reach.
    //
    // Volume is two independent things multiplied together: how far away the
    // thing is across the water, and how far the camera has pulled back from it.
    // They are kept separate because they answer different questions and want
    // different curves — folding the zoom into the distance (by giving the
    // listener an altitude, say) ties the zoom response to the silence range and
    // squashes it to nothing over the near half of the zoom range.
    class Attenuation {
    public:
        // Where the camera looks and how far it is zoomed in. Set once per frame,
        // before anything asks for a gain.
        void SetCamera(Camera const& camera);

        // The gain for something at `position` (world metres). Zero until
        // SetCamera has been called, so nothing carries from a camera that isn't
        // there yet.
        float GainAt(b2Vec2 position) const;

    private:
        b2Vec2 m_center{ 0.0f, 0.0f }; // where the camera looks (m)
        float m_zoomGain = 0.0f;       // what this zoom alone lets through
    };
}
