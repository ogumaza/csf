// SPDX-License-Identifier: MIT

// nw::snd's voices on AX (AxVoice) in NintendoWare for Cafe. Each AxVoice wraps one AX voice and plays a queue of wave
// buffers on it. Each frame, the voice turns the parameters that the layers above compute into the AX calls that set
// them. The AX calls ramp the volumes across the frame. Each function's comment gives the address of the routine it
// follows in Animal Crossing: amiibo Festival's executable. The Wii Remote's speakers aren't modelled.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>

#include "ax/memory.h"
#include "ax/system.h"
#include "nwsnd/util.h"

namespace csf::nwsnd
{

// nw::snd's sample formats (SampleFormat).
enum class SampleFormat : uint32_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kDspAdpcm = 2,
};

// The ADPCM decoder's state at a buffer's start (AdpcmContext).
struct AdpcmContext
{
    uint16_t predictor_scale = 0;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
};

// A block of samples a voice plays (WaveBuffer). A voice plays its queue in order. A buffer that loops plays until the
// next buffer is queued. The voice marks each buffer done once it has moved on.
struct WaveBuffer
{
    enum class Status : uint32_t
    {
        kFree = 0,
        kWait = 1,
        kPlay = 2,
        kDone = 3,
    };

    uint32_t address = 0; // the samples' address in AX's memory
    uint32_t sample_length = 0;
    uint32_t sample_offset = 0; // where playback starts
    const AdpcmContext* adpcm_context = nullptr;
    bool loop = false;
    Status status = Status::kFree;
    WaveBuffer* next = nullptr;
};

// A voice's sends to one device's buses, [bus][channel]: the main bus, then aux A, B and C, each for the TV's six
// channels or a GamePad's four channels (OutputMix).
using OutputMix = std::array<std::array<float, 6>, 4>;

// What a voice plays with this frame (VoiceParam). The layers above compute it.
struct VoiceParam
{
    float volume = 1.0f;
    float pitch = 1.0f;
    OutputMix tv{};
    OutputMix drc{};
    bool lpf_on = false;
    bool biquad_on = false;
    bool drc_main_to_second = false; // in surround, the main bus goes to the second GamePad's front channels
    std::array<int16_t, 5> biquad{}; // b0, b1, b2, a1, a2
    uint16_t lpf_freq = 0;
    uint8_t interpolation = 0; // 0 the 4-tap filters, 1 linear, else none
};

class AxVoices;

class AxVoice
{
public:
    enum class State : uint32_t
    {
        kPlay = 0,
        kStop = 1,
    };

    // Called when the voice is freed or AX takes it away (the dispose callback).
    using DisposeCallback = std::function<void(AxVoice& voice)>;

    // Queues a buffer (AppendWaveBuffer, 0x022A2D08). The buffer starts waiting.
    void AppendWaveBuffer(WaveBuffer& buffer);

    // Moves the voice in the priority list (SetPriority, 0x022A2D90).
    void SetPriority(uint32_t priority);

    void SetState(State state)
    {
        state_ = state;
    }

    void SetSampleFormat(SampleFormat format)
    {
        format_ = format;
    }

    void SetSampleRate(uint32_t rate)
    {
        sample_rate_ = rate;
    }

    void SetAdpcmCoefs(const std::array<int16_t, 16>& coefs)
    {
        coefs_ = coefs;
    }

    VoiceParam& Param()
    {
        return param_;
    }

    bool IsAvailable() const
    {
        return ax_voice_ != nullptr;
    }

    // The AX voice it plays on, or nullptr once AX has taken it away.
    ax::Voice* SdkVoice() const
    {
        return ax_voice_;
    }

    // The samples played from the current buffer's start (the play position UpdatePlayPosition keeps).
    uint32_t PlayPosition() const
    {
        return play_position_;
    }

    WaveBuffer* CurrentBuffer() const
    {
        return head_;
    }

    // Marks every queued buffer done and empties the queue (FreeAllWaveBuffer, 0x0229889C). A voice's owner calls it
    // when it stops the voice.
    void FreeAllWaveBuffer();

private:
    friend class AxVoices;

    void Initialize(ax::Voice* voice); // 0x022A2B54
    void Finalize();                   // 0x02298BDC

    // Each frame, by the voice's state, then the play position (UpdateState, 0x022A4E10).
    void UpdateState(const std::array<OutputMode, 2>& modes);
    void UpdateStatePlay(bool running, const std::array<OutputMode, 2>& modes); // 0x022A48A8
    void UpdateStateStop(bool running);                                         // 0x022A4CAC
    void UpdatePlayPosition();                                                  // 0x022A4D48
    void UpdateParams(const std::array<OutputMode, 2>& modes);

    void SetupVoice(const WaveBuffer& buffer, const std::array<OutputMode, 2>& modes); // 0x022A3E3C
    void SetupOffset(const WaveBuffer& buffer);                                        // 0x022A2EC4
    void SetupAdpcm(const WaveBuffer& buffer);                                         // 0x022A2FA0
    void SetupNextWaveBuffer(const WaveBuffer& buffer);                                // 0x022A2E38
    void SetupTvMix(OutputMode mode);                                                  // 0x022A33AC
    void SetupDrcMix(OutputMode mode);                                                 // 0x022A36B0
    void UpdateVe(bool setup);                                                         // 0x022A38BC
    void UpdateSrc(bool setup);                                                        // 0x022A3954
    void UpdateTvMix(OutputMode mode);                                                 // 0x022A4370
    void UpdateDrcMix(OutputMode mode);                                                // 0x022A4704
    void UpdateMonoFilter(bool setup);                                                 // 0x022A3B8C
    void UpdateBiquadFilter(bool setup);                                               // 0x022A3C48

    // The offset in the format's units of a sample (CalcVoiceOffsetFromSampleCount, 0x022A2DD8).
    uint32_t OffsetOf(uint32_t sample) const;

    ax::System* system_ = nullptr;
    const ax::Memory* memory_ = nullptr;
    AxVoices* owner_ = nullptr;
    ax::Voice* ax_voice_ = nullptr;
    DisposeCallback dispose_;
    uint32_t priority_ = 0;
    bool in_list_ = false;

    WaveBuffer* head_ = nullptr;
    WaveBuffer* tail_ = nullptr;
    SampleFormat format_ = SampleFormat::kPcm16;
    uint32_t sample_rate_ = 32000;
    std::array<int16_t, 16> coefs_{};
    State state_ = State::kStop;
    VoiceParam param_;

    // What the voice last sent AX. The ramps and the checks for changes start from these values.
    std::array<ax::System::ChannelMix, 6> tv_mix_{};
    std::array<ax::System::ChannelMix, 4> drc_mix_{};
    std::array<ax::System::ChannelMix, 4> drc2_mix_{};
    ax::VolumeRamp ve_;
    float ratio_ = 0.0f;
    uint16_t lpf_on_ = 0;
    uint16_t biquad_on_ = 0;

    uint32_t play_position_ = 0;
    uint32_t loop_count_ = 0;
    bool next_in_loop_ = false;      // the next buffer is the loop, and a change in the loop count says it has begun
    bool started_with_next_ = false; // the voice started with a next buffer: its loop isn't set up yet
};

// The AxVoices, one for each AX voice, and the list that keeps AX's priorities in nw::snd's order.
class AxVoices
{
public:
    AxVoices(ax::System& system, const ax::Memory& memory);

    AxVoices(const AxVoices&) = delete;
    AxVoices& operator=(const AxVoices&) = delete;

    // Acquires an AX voice for an nw::snd priority (0 to 255; 255 takes AX's highest), and the AxVoice that wraps it
    // (AllocVoice, 0x022A2C44). Returns nullptr if AX has no voice to give.
    AxVoice* AllocVoice(uint32_t priority, AxVoice::DisposeCallback dispose);

    // Frees the AX voice and the AxVoice (Free, 0x02298C54).
    void Free(AxVoice& voice);

    // Each frame when the game isn't protecting voices: every voice's state (detail_UpdateAllVoices, 0x022A4EB8).
    // `modes` are the TV's and the GamePad's output modes.
    void UpdateAllVoices(const std::array<OutputMode, 2>& modes);

    // Each frame: AX's priority 16, in nw::snd's order from the lowest priority (detail_UpdatePriorityList,
    // 0x0229EB84). AX then takes the lowest first.
    void UpdatePriorityList();

private:
    friend class AxVoice;

    void AppendToList(AxVoice& voice);  // 0x022A2B94
    void EraseFromList(AxVoice& voice); // 0x02298B90

    ax::System* system_;
    std::array<AxVoice, ax::kVoiceCount> voices_;
    std::list<AxVoice*> priority_list_; // lowest first, and the newest last among equals
};

} // namespace csf::nwsnd
