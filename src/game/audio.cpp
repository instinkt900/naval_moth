#include "game/audio.h"

#include "game/camera.h"
#include "game/defs.h"

#include <miniaudio.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
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

        // Volume is two independent things multiplied together: how far the
        // sound is across the water, and how far the camera has pulled back from
        // it. They are kept separate because they answer different questions and
        // want different curves — folding the zoom into the distance (by giving
        // the listener an altitude, say) ties the zoom response to kSilenceM and
        // squashes it to nothing over the near half of the zoom range.

        // How far a sound carries across the sea: audible out to kSilenceM,
        // nothing beyond. There is no full-volume band around the listener, so
        // the fade starts the moment a sound is off-centre.
        constexpr float kSilenceM = 2500.0f;

        // What the zoom does to the volume. A sound plays as authored only at
        // kMaxZoom — right down on the action — and is scaled to kZoomGainFloor
        // when pulled all the way out to kMinZoom, which is a floor rather than
        // silence so a battle you have zoomed out to watch is still audible.
        // Raise the floor to flatten the effect; lower it to make surveying the
        // map quieter still.
        constexpr float kZoomGainFloor = 0.15f;

        // How hard a sound at the edge of the view is pushed to one side. Short
        // of 1 deliberately: a full balance would silence the opposite channel
        // outright, which is a strange thing to hear from a map camera looking
        // down on the whole engagement.
        constexpr float kMaxPan = 0.8f;

        // Volume for a sound `distance` metres from the listener. The fade is
        // squared rather than linear so sound drops away quickly as it leaves
        // the near field and then trails off, which reads as distance far better
        // than an even ramp does.
        float DistanceGain(float distance) {
            if (distance >= kSilenceM) {
                return 0.0f;
            }
            float const t = distance / kSilenceM;
            return (1.0f - t) * (1.0f - t);
        }

        // Volume for the camera being zoomed to `pixelsPerMeter`.
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

    struct Audio::Impl {
        // A sound loaded into the bank. `prototype` is decoded once and never
        // played: it is only ever cloned, which is what lets many shots share
        // one decoded copy of the audio. Only sounds that actually loaded get in
        // here, so a file that wouldn't open simply has no handle to find.
        struct Sound {
            ma_sound prototype;
            float volume = 1.0f;
            float pitchVariance = 0.0f;
        };

        // One playing sound. `active` says whether `sound` is initialised —
        // an inactive voice's ma_sound is untouched memory, so nothing may read
        // it.
        struct Voice {
            ma_sound sound;
            bool active = false;
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

        b2Vec2 listener{ 0.0f, 0.0f };   // where the camera looks (m)
        float zoomGain = 1.0f;           // volume scale for how far the camera has pulled back
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
        m_impl->listener = camera.center;
        m_impl->zoomGain = ZoomGain(camera.pixelsPerMeter);
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
        float const gain = m_impl->zoomGain * DistanceGain((position - m_impl->listener).Length());
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

    void Audio::Update() {
        if (!m_impl->engineReady) {
            return;
        }
        // A voice holds a cloned data source, so a finished one has to be
        // uninitialised to give the memory back rather than just marked free.
        for (auto& voice : m_impl->voices) {
            if (voice.active && ma_sound_at_end(&voice.sound)) {
                ma_sound_uninit(&voice.sound);
                voice.active = false;
            }
        }
    }
}
