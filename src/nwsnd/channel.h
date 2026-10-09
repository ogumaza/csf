// SPDX-License-Identifier: MIT

// nw::snd's channels (driver::Channel and driver::ChannelManager) in NintendoWare for Cafe. A channel is a note on a
// MultiVoice. The channel has an envelope, four LFOs, a pitch sweep and the parameters that its sequence track sets.
// Every sound frame, the channel works these out into the MultiVoice's parameters. Each function's comment gives the
// address of the routine it follows in Animal Crossing: amiibo Festival's executable.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "nwsnd/ax_voice.h"
#include "nwsnd/envelope.h"
#include "nwsnd/util.h"
#include "nwsnd/voice.h"

namespace csf::nwsnd
{

// A wave as nw::snd reads it from its file (WaveInfo, filled by WaveFileReader, 0x022B85B0): the sample format, the
// loop, the rate, the loop's start and the end (also the length when the wave doesn't loop), and each channel's
// samples. Each channel has its samples' address in AX's memory, the DSP-ADPCM coefficients, and the decoder's state at
// the start and at the loop.
struct WaveInfo
{
    struct Channel
    {
        uint32_t address = 0;
        std::array<int16_t, 16> coefs{};
        AdpcmContext context;
        AdpcmContext loop_context;
    };

    SampleFormat format = SampleFormat::kDspAdpcm;
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t original_loop_start = 0;
    std::array<Channel, 2> channels;
};

class ChannelManager;

class Channel
{
public:
    // Why a channel calls its owner back (ChannelCallbackStatus): it was stopped (0), its voice was dropped (1), its
    // wave played to its end (2), or it was cancelled (3).
    enum class CallbackStatus : uint32_t
    {
        kStopped = 0,
        kDrop = 1,
        kFinish = 2,
        kCancel = 3,
    };

    using Callback = std::function<void(Channel& channel, CallbackStatus status)>;

    // Plays a wave on the channel's MultiVoice, from `start_offset` (Start, 0x022B9948).
    void Start(const WaveInfo& wave, int32_t length, uint32_t start_offset);

    // Each sound frame: the volume, pitch and output parameters, from the note, the track, the envelope and the LFOs
    // (the nameless 0x022C42C8). With `periodic`, the envelope, LFOs and sweep advance by a frame.
    void Update(bool periodic, Random& random);

    void Stop();    // 0x022C4278
    void Release(); // 0x022B6028
    void NoteOff(); // 0x022B6E2C: unless the note ignores note-off

    // Calls the owner's callback, once (CallChannelCallback, 0x022B8BA0).
    void CallChannelCallback(CallbackStatus status);

    // Whether the wave loops.
    bool WaveLoops() const
    {
        return loop_;
    }

    // A pitch sweep from `pitch` semitones away to the note's key over `ticks` (SetSweepParam, 0x022C487C).
    void SetSweepParam(float pitch, int32_t ticks, bool auto_update)
    {
        sweep_pitch = pitch;
        sweep_length = ticks;
        sweep_counter = 0;
        auto_sweep = auto_update;
    }

    // 0x022B6E68.
    void UpdateSweep(int32_t count)
    {
        sweep_counter += count;
        if (sweep_counter > sweep_length)
        {
            sweep_counter = sweep_length;
        }
    }

    CurveAdshr envelope;                // +0x10
    std::array<CurveLfo, 4> lfos;       // +0x2C
    std::array<uint8_t, 4> lfo_types{}; // +0xAC: 0 pitch, 1 volume, 2 pan
    bool active = false;                // +0xB1
    bool allocated = false;             // +0xB2
    bool auto_sweep = true;             // +0xB3
    bool release_priority_fix = false;  // +0xB4
    bool ignore_note_off = false;       // +0xB5
    uint8_t biquad_type = 0;            // +0xB6
    float track_volume = 1.0f;          // +0xB8
    float user_pitch = 1.0f;            // +0xBC
    float lpf = 0.0f;                   // +0xC0
    float biquad_value = 0.0f;          // +0xC4
    uint32_t output_line = 1;           // +0xC8
    OutputParam tv;                     // +0xCC
    std::array<OutputParam, 2> drc;     // +0x11C
    float bend = 0.0f;                  // +0x1EC: semitones
    float sweep_pitch = 0.0f;           // +0x1F0
    int32_t sweep_counter = 0;          // +0x1F4
    int32_t sweep_length = 0;           // +0x1F8
    float init_pan = 0.0f;              // +0x1FC
    float init_span = 0.0f;             // +0x200
    float tune = 1.0f;                  // +0x204
    uint8_t silence_from = 255;         // +0x208: a ramp of the volume, in 255ths, that silences a track
    uint8_t silence_to = 255;           // +0x209
    uint16_t silence_duration = 0;      // +0x20A: frames
    uint16_t silence_counter = 0;       // +0x20C
    float cached_pitch = 0.0f;          // +0x210
    float cached_ratio = 1.0f;          // +0x214
    int32_t length = 0;                 // +0x218: ticks, or negative for none
    uint32_t pan_mode = 0;              // +0x21C
    uint32_t pan_curve = 0;             // +0x220
    uint8_t key = 60;                   // +0x224
    uint8_t original_key = 60;          // +0x225
    uint8_t key_group = 0;              // +0x226
    uint8_t interpolation_type = 0;     // +0x227
    float init_volume = 1.0f;           // +0x228
    float velocity_volume = 1.0f;       // +0x22C
    Callback callback;                  // +0x230
    MultiVoice* voice = nullptr;        // +0x238
    Channel* next_in_track = nullptr;   // +0x23C

private:
    friend class ChannelManager;

    void InitParam(Callback owner);                                          // 0x022B8E28
    void AppendWaveBuffer(const WaveInfo& wave, uint32_t start_offset);      // 0x022B96FC
    void OnVoiceEvent(MultiVoice& voice, MultiVoice::CallbackStatus status); // VoiceCallbackFunc, 0x022B8BF0

    // The sweep's pitch now, in semitones (GetSweepValue, 0x022C4204).
    float GetSweepValue() const;

    std::array<std::array<WaveBuffer, 2>, 2> wave_buffers_; // +0x240: each wave channel's start and loop
    std::array<AdpcmContext, 2> contexts_;                  // +0x2C0
    std::array<AdpcmContext, 2> loop_contexts_;             // +0x2CC
    uint32_t start_offset_ = 0;                             // +0x2DC
    uint32_t loop_start_ = 0;                               // +0x2E0
    uint32_t original_loop_start_ = 0;                      // +0x2E4
    bool loop_ = false;                                     // +0x2E8
};

// What a track passes to its bank when it plays a note (NoteOnInfo): the program, the key and the velocity (scaled by
// the track's velocity range), the length in ticks or -1, the track's initial pan, the channel's priority, and the
// track to notify when the channel ends.
struct NoteOnInfo
{
    uint32_t prg_no = 0;
    int key = 60;
    int velocity = 127;
    int32_t length = -1;
    int init_pan = 0;
    uint32_t priority = 64;
    Channel::Callback callback;
};

// The channels: a pool of a fixed number, and a list of those in use, in the order they were allocated. They update in
// that order. nw::snd's pool lays its channels out one after another, and hands out the last freed first. Before any
// has been freed, the pool hands out its last channel first (PoolImpl's CreateImpl, AllocImpl and FreeImpl: 0x0228EADC,
// 0x022901CC and 0x022901F0).
class ChannelManager
{
public:
    // `count` is nw::snd's channel count, the same .data word as its voice count (0x101AA478).
    ChannelManager(MultiVoiceManager& voices, int count);

    ChannelManager(const ChannelManager&) = delete;
    ChannelManager& operator=(const ChannelManager&) = delete;

    // A channel with a MultiVoice for `channels` channels at `priority`, or nullptr (Channel::AllocChannel,
    // 0x022B9000).
    Channel* AllocChannel(int channels, uint32_t priority, Channel::Callback callback);

    // Each sound frame: every channel's update, and those that have stopped are freed (0x022C97B4).
    void UpdateAllChannel(Random& random);

    int ActiveCount() const
    {
        return static_cast<int>(active_.size());
    }

    // The channels in use, in the list's order.
    const std::list<Channel*>& InUse() const
    {
        return active_;
    }

    // A channel's place in the pool, from 0, or -1 if it isn't in use.
    int Index(const Channel& channel) const;

    // Whether any channel's envelope is in its attack, hold, decay or release, rather than settled at its sustain level
    // (for the ripper's length analysis).
    bool AnyChanging() const;

    // Takes a channel out of the list and frees it (0x022B8D40). Channel::FreeChannel (0x022C41D0) does the same when a
    // track stops a note.
    void Free(Channel& channel);

private:
    MultiVoiceManager* voices_;
    std::vector<std::unique_ptr<Channel>> pool_; // each place's channel while it's in use
    std::vector<int> free_;                      // the free places, the next to hand out last
    std::list<Channel*> active_;
};

} // namespace csf::nwsnd
