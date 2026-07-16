#pragma once

#include <box2d/box2d.h>

#include <memory>
#include <string>

namespace naval::defs {
    class Database;
}

namespace naval {
    struct Camera;

    // A resolved handle to a loaded sound: an index into the bank, not a name.
    // Definitions name their sounds by id; that id is resolved to a handle once,
    // at spawn, so firing a gun costs an array index rather than a string hash —
    // the same trade the Weapon component already makes for its projectile
    // stats.
    //
    // kNoSound is the silent handle, and both ways a sound can go missing land
    // on it: a definition that never named a sound, and a sound whose file
    // failed to load. Playing it does nothing, so no call site needs to ask
    // whether a sound exists before asking for it.
    inline constexpr int kNoSound = -1;

    // The audio device and the bank of loaded sounds.
    //
    // Deliberately small. Sounds are one-shot, fire-and-forget effects, played
    // at a volume that falls off with distance from the camera, placed left or
    // right by where they sit across the view, and given a little random pitch
    // variation so a repeated gun doesn't sound mechanical. That is the whole
    // of it: the panning is a stereo placement, not spatialisation, and there
    // is no doppler, no filtering and no music — a naval battle heard from a map
    // camera doesn't need them.
    //
    // Sounds mix on miniaudio's own device thread, but every method here is
    // called from the game thread only.
    //
    // miniaudio's header is ~96k lines and drags in the platform audio backends,
    // so it is kept out of this interface behind a pimpl: only audio.cpp pays to
    // include it.
    class Audio {
    public:
        Audio();
        ~Audio();

        // The device, the bank and the voices playing out of it are unique to
        // this object; voices hold pointers into the engine that a copy would
        // duplicate.
        Audio(Audio const&) = delete;
        Audio& operator=(Audio const&) = delete;

        // Loads every sound in `db`'s sound table into the bank. A sound whose
        // file will not load is logged and left silent rather than throwing —
        // an absent asset shouldn't stop the game from running. That is a
        // deliberate softening of the database's own rule: a dangling sound *id*
        // is a typo and still fails the load, but a missing *file* only costs
        // you the noise.
        //
        // Must be called before anything spawns, since spawning resolves handles
        // against this bank.
        void Load(defs::Database const& db);

        // The handle for sound id `id`: kNoSound if `id` is empty (the field was
        // never authored) or names a sound whose file failed to load.
        int Find(std::string const& id) const;

        // Where sounds are heard from: the camera, both where it is pointed and
        // how far it is zoomed out. Set once per frame, before the systems that
        // play anything run.
        void SetListener(Camera const& camera);

        // Plays `sound` once at `position` (world metres): volume scaled by its
        // distance from the listener, panned by where it lies across the view,
        // and at a randomly varied pitch. All three are fixed when it starts and
        // do not follow the camera while it plays — these are short effects, and
        // a shot that lands is over before a pan could track it anywhere.
        void Play(int sound, b2Vec2 position);

        // Returns voices whose sound has finished to the pool. Call once per
        // frame; without it the pool fills and later sounds are dropped.
        void Update();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
