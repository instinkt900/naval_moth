#include "game/audio.h"

#include "game/attenuation.h"
#include "game/camera.h"
#include "game/defs.h"

#include <miniaudio.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace naval {
    namespace {
        // How many sounds may play at once. A busy engagement is a handful of
        // guns plus their splashes, so this is generous; past it the quietest
        // thing to do is drop the new sound rather than cut off one already
        // playing, since a clipped explosion is more noticeable than an absent
        // one.
        constexpr int kVoiceCount = 32;

        // How hard a sound at the edge of the view is pushed to one side. Short
        // of 1 deliberately: a full balance would silence the opposite channel
        // outright, which is a strange thing to hear from a map camera looking
        // down on the whole engagement.
        constexpr float kMaxPan = 0.8f;
    }

    struct Audio::Impl {
        // A sound loaded into the bank. `prototype` is decoded once and never
        // played: it is only ever cloned, which is what lets many shots share
        // one decoded copy of the audio. Only sounds that actually loaded get in
        // here, so a file that wouldn't open simply has no handle to find.
        struct Sound {
            ma_sound prototype;
            float volume = 1.0f;
            float pitchVariance = 0.0f;
            bool looping = false;
        };

        // One playing sound. `active` says whether `sound` is initialised —
        // an inactive voice's ma_sound is untouched memory, so nothing may read
        // it.
        //
        // A looping voice is held alive by HoldLoop rather than by playing to its
        // end: `looping` marks it as such, `key` is the emitter it belongs to so
        // HoldLoop can find it again, and `refreshed` records that HoldLoop
        // touched it this frame. Update reclaims a looping voice that went a frame
        // unrefreshed, and a one-shot voice the ordinary way, when it reaches its
        // end. All three are meaningless on an inactive voice.
        struct Voice {
            ma_sound sound;
            bool active = false;
            bool looping = false;
            bool refreshed = false;
            uint64_t key = 0;
        };

        ma_engine engine;
        bool engineReady = false;

        // Neither of these vectors may ever move an element: an initialised
        // ma_sound is a node in miniaudio's graph and cannot survive being
        // relocated (see Load). `sounds` is reserved to its final size before
        // anything is initialised into it, and `voices` is sized once here and
        // never resized — so don't grow, shrink, sort or erase either of them.
        std::vector<Sound> sounds;
        std::unordered_map<std::string, int> byId;
        std::vector<Voice> voices{ kVoiceCount }; // fixed size; not an initializer_list of one

        // How much of a sound reaches the player — its distance across the water
        // and the camera's zoom, on the curve the camera shake fades on too.
        Attenuation attenuation;
        b2Vec2 listener{ 0.0f, 0.0f };   // where the camera looks (m); the pan's reference
        float listenerHalfWidthM = 1.0f; // half the sea the camera can see across (m); the pan scale
        std::mt19937 rng{ std::random_device{}() };

        // A voice not currently playing, or nullptr if all are busy.
        Voice* FreeVoice() {
            for (auto& voice : voices) {
                if (!voice.active) {
                    return &voice;
                }
            }
            return nullptr;
        }

        // The held loop belonging to emitter `key`, or nullptr if it has none
        // playing — so HoldLoop can tell a fresh start from a refresh.
        Voice* LoopVoice(uint64_t key) {
            for (auto& voice : voices) {
                if (voice.active && voice.looping && voice.key == key) {
                    return &voice;
                }
            }
            return nullptr;
        }
    };

    Audio::Audio()
        : m_impl(std::make_unique<Impl>()) {
        ma_engine_config config = ma_engine_config_init();
        if (ma_engine_init(&config, &m_impl->engine) != MA_SUCCESS) {
            // No audio device — a machine without a sound card, or one whose
            // audio server isn't running. The game is perfectly playable
            // silently, so this is a warning and every call below no-ops.
            spdlog::warn("naval audio: no audio device; running silent");
            return;
        }
        m_impl->engineReady = true;
    }

    Audio::~Audio() {
        if (!m_impl->engineReady) {
            return;
        }
        // Voices hold clones of the prototypes' data sources, so they must go
        // first, and the engine mixes all of them, so it goes last.
        for (auto& voice : m_impl->voices) {
            if (voice.active) {
                ma_sound_uninit(&voice.sound);
            }
        }
        for (auto& sound : m_impl->sounds) {
            ma_sound_uninit(&sound.prototype);
        }
        ma_engine_uninit(&m_impl->engine);
    }

    void Audio::Load(defs::Database const& db) {
        if (!m_impl->engineReady) {
            return;
        }
        // An initialised ma_sound must never be moved: it is a node in
        // miniaudio's graph, holding pointers into itself and into the nodes
        // either side of it, none of which a copy would fix up. So reserve the
        // whole bank up front — with no reallocation, and each prototype
        // initialised where it will live rather than copied in, every one of
        // them stays put for as long as the engine knows about it.
        m_impl->sounds.reserve(db.GetSounds().size());

        for (auto const& [id, def] : db.GetSounds()) {
            m_impl->sounds.emplace_back();
            Impl::Sound& sound = m_impl->sounds.back();
            sound.volume = def.volume;
            sound.pitchVariance = def.pitchVariance;
            sound.looping = def.looping;

            // MA_SOUND_FLAG_DECODE decodes the whole file up front into a
            // resource-manager buffer. That is what makes the prototype
            // cloneable — miniaudio can only clone data buffers, not streams —
            // and it keeps firing off the disk entirely. Sound effects are small
            // enough that holding them decoded costs nothing worth counting.
            // Spatialisation is off because the gain is worked out here against
            // the camera rather than by miniaudio's 3D listener.
            std::string const path = def.file.string();
            ma_uint32 const flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
            if (ma_sound_init_from_file(&m_impl->engine, path.c_str(), flags,
                                        nullptr, nullptr, &sound.prototype) != MA_SUCCESS) {
                // The id is good — the database checked that — but the file
                // behind it isn't there or won't decode. Name both so it's
                // obvious which entry to fix, then drop it from the bank: with
                // no handle to find, it resolves to kNoSound at spawn and plays
                // nothing.
                spdlog::warn("naval audio: sound '{}' cannot load '{}'; it will be silent", id, path);
                m_impl->sounds.pop_back();
                continue;
            }
            m_impl->byId.emplace(id, static_cast<int>(m_impl->sounds.size()) - 1);
        }
    }

    int Audio::Find(std::string const& id) const {
        auto it = m_impl->byId.find(id);
        return it == m_impl->byId.end() ? kNoSound : it->second;
    }

    void Audio::SetListener(Camera const& camera) {
        m_impl->attenuation.SetCamera(camera);
        m_impl->listener = camera.center;
        // Half the sea the camera can see across: a metre of world is
        // pixelsPerMeter pixels, so half a viewport of pixels is this many
        // metres of water. It sets how far out a sound must be to reach the edge
        // of the stereo image, which is what keeps the pan tracking the picture
        // at any zoom.
        m_impl->listenerHalfWidthM = (camera.viewSize.x * 0.5f) / camera.pixelsPerMeter;
    }

    void Audio::Play(int sound, b2Vec2 position) {
        if (!m_impl->engineReady || sound == kNoSound) {
            return;
        }
        auto& def = m_impl->sounds[static_cast<size_t>(sound)];

        // Worked out before taking a voice, so a shot fired far offscreen is
        // silent for free instead of occupying a voice an audible sound wants.
        float const gain = m_impl->attenuation.GainAt(position);
        if (gain <= 0.0f) {
            return;
        }
        Impl::Voice* voice = m_impl->FreeVoice();
        if (voice == nullptr) {
            return;
        }
        if (ma_sound_init_copy(&m_impl->engine, &def.prototype,
                               MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &voice->sound) != MA_SUCCESS) {
            return;
        }
        voice->active = true;

        // Place the sound left or right by where it sits across the view, so a
        // gun off the port bow is heard to port. Scaled to the visible width, so
        // the stereo image tracks what you can see rather than a fixed distance:
        // a sound at the edge of the screen is at the edge of the image at any
        // zoom, and anything further out is simply pinned there.
        //
        // This is a stereo placement and nothing more. miniaudio applies the
        // panner after its spatialiser, so it costs none of the 3D machinery
        // NO_SPATIALIZATION turns off.
        float const across = position.x - m_impl->listener.x;
        float const pan = kMaxPan * std::clamp(across / m_impl->listenerHalfWidthM, -1.0f, 1.0f);

        // Pitch is a playback-rate multiplier around 1, so a variance of 0.1
        // spans 0.9x to 1.1x. This is the whole of the variety machinery: the
        // same gun sample fired twice never sounds quite identical.
        std::uniform_real_distribution<float> pitchDist(1.0f - def.pitchVariance,
                                                        1.0f + def.pitchVariance);
        ma_sound_set_volume(&voice->sound, def.volume * gain);
        ma_sound_set_pan(&voice->sound, pan);
        ma_sound_set_pitch(&voice->sound, pitchDist(m_impl->rng));
        ma_sound_start(&voice->sound);
    }

    bool Audio::IsLooping(int sound) const {
        return sound != kNoSound && m_impl->sounds[static_cast<size_t>(sound)].looping;
    }

    void Audio::HoldLoop(int sound, uint64_t key, b2Vec2 position) {
        if (!m_impl->engineReady || sound == kNoSound) {
            return;
        }
        auto& def = m_impl->sounds[static_cast<size_t>(sound)];

        // A loop out of earshot takes no voice: an already-playing one is left
        // unrefreshed for the next Update to reclaim, and simply restarts — loops
        // begin cleanly — once it is back in range. Same early-out as Play, for
        // the same reason: don't spend a voice on silence.
        float const gain = m_impl->attenuation.GainAt(position);
        if (gain <= 0.0f) {
            return;
        }

        Impl::Voice* voice = m_impl->LoopVoice(key);
        if (voice == nullptr) {
            // Nothing playing for this emitter yet — start it: a copy of the
            // prototype, set looping so it tiles until stopped rather than ending.
            // Pitch is rolled once, here, not per frame — re-rolling it every tick
            // would warble a held note.
            voice = m_impl->FreeVoice();
            if (voice == nullptr) {
                return;
            }
            if (ma_sound_init_copy(&m_impl->engine, &def.prototype, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                   nullptr, &voice->sound) != MA_SUCCESS) {
                return;
            }
            voice->active = true;
            voice->looping = true;
            voice->key = key;
            ma_sound_set_looping(&voice->sound, MA_TRUE);
            std::uniform_real_distribution<float> pitchDist(1.0f - def.pitchVariance,
                                                            1.0f + def.pitchVariance);
            ma_sound_set_pitch(&voice->sound, pitchDist(m_impl->rng));
            ma_sound_start(&voice->sound);
        }

        // Refreshed this frame, so Update keeps it; and its gain and pan are
        // re-applied from the emitter's current position, so a held loop tracks
        // the ship and the camera — unlike a one-shot, which bakes both at the
        // start (see Play for the pan derivation).
        voice->refreshed = true;
        float const across = position.x - m_impl->listener.x;
        float const pan = kMaxPan * std::clamp(across / m_impl->listenerHalfWidthM, -1.0f, 1.0f);
        ma_sound_set_volume(&voice->sound, def.volume * gain);
        ma_sound_set_pan(&voice->sound, pan);
    }

    void Audio::Update() {
        if (!m_impl->engineReady) {
            return;
        }
        // A voice holds a cloned data source, so a finished one has to be
        // uninitialised to give the memory back rather than just marked free.
        // A one-shot is finished when it reaches its end; a held loop is finished
        // when a frame went by without a HoldLoop to refresh it — the emitter
        // stopped firing, lost its mark, or was destroyed.
        for (auto& voice : m_impl->voices) {
            if (!voice.active) {
                continue;
            }
            bool const done =
                voice.looping ? !voice.refreshed : bool(ma_sound_at_end(&voice.sound));
            voice.refreshed = false;
            if (done) {
                ma_sound_uninit(&voice.sound);
                voice.active = false;
                voice.looping = false;
                voice.key = 0;
            }
        }
    }
}
