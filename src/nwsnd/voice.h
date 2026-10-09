// SPDX-License-Identifier: MIT

// nw::snd's MultiVoice and its manager (driver::MultiVoice and driver::MultiVoiceManager) in NintendoWare for Cafe. A
// MultiVoice plays each channel of a wave on a separate voice. Here a voice (an internal::Voice) is the AxVoice it
// wraps and the parameters it passes to that voice each frame. A MultiVoice keeps the parameters that a Channel sets
// each frame, and works out each voice's volume, pitch, mix and filters when they change. Each function's comment gives
// the address of the routine it follows in Animal Crossing: amiibo Festival's executable.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "nwsnd/ax_voice.h"
#include "nwsnd/util.h"

namespace csf::nwsnd
{

// What the MultiVoices read of nw::snd's HardwareManager, at the values that a game's sound system starts with. The
// fields are each device's output mode as nw::snd mixes for it, and as the system reports it (the TV's mode, then the
// GamePad's mode). The other fields are the devices that each bit of an output line reaches (bit 0 the TV, 1 to 4 the
// Wii Remotes, 5 and 6 the GamePads), and the output volume (GetOutputVolume, 0x0229BF58).
struct HardwareSettings
{
    std::array<OutputMode, 2> output_mode{OutputMode::kStereo, OutputMode::kStereo};
    std::array<OutputMode, 2> end_user_output_mode{OutputMode::kStereo, OutputMode::kStereo};
    std::array<uint8_t, 32> output_line_devices{0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40};
    float output_volume = 1.0f;
};

// A device's output parameters (OutputParam): its volume, whether the channels mix by pan (0) or by the mix parameters
// (1), each wave channel's mix parameters (front left and right, rear left and right, front centre and LFE), the pan,
// the surround pan, and the main and effect sends. The main send is an offset from 1.
struct OutputParam
{
    float volume = 1.0f;
    uint32_t mix_mode = 0;
    std::array<std::array<float, 6>, 2> mix_parameter{
        {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}}};
    float pan = 0.0f;
    float span = 0.0f;
    float main_send = 0.0f;
    std::array<float, 3> fx_send{};
};

class MultiVoiceManager;

class MultiVoice
{
public:
    // Why a MultiVoice calls its owner back (VoiceCallbackStatus): its wave has played to its end (0), it was cancelled
    // (1), the manager dropped it for a voice of higher priority (2), or AX took one of its voices away (3).
    enum class CallbackStatus : uint32_t
    {
        kFinishWave = 0,
        kCancel = 1,
        kDropVoice = 2,
        kDropDsp = 3,
    };

    using Callback = std::function<void(MultiVoice& voice, CallbackStatus status)>;

    // Start sets the voices to start at the next update (0x0229887C). Stop stops them now (0x02298B38).
    void Start();
    void Stop();

    // Gives the voices back to AX and the MultiVoice back to its manager (0x02298E8C).
    void Free();

    void SetSampleFormat(SampleFormat format);                             // 0x02298F38
    void SetSampleRate(uint32_t rate);                                     // 0x02298F70
    void SetInterpolationType(uint8_t type);                               // 0x02298FA0
    void SetAdpcmParam(int channel, const std::array<int16_t, 16>& coefs); // 0x02298FD0

    // Queues a buffer on a channel's voice. The last buffer queued with `last` is the one whose end ends the voice
    // (0x022AAAF4).
    void AppendWaveBuffer(int channel, WaveBuffer& buffer, bool last);

    void SetVolume(float volume);                        // 0x022AA3C8: held to 0 and up
    void SetPitch(float pitch);                          // 0x022AA3F4
    void SetPanMode(uint32_t mode);                      // 0x022AA414
    void SetPanCurve(uint32_t curve);                    // 0x022AA434
    void SetLpfFreq(float lpf);                          // 0x022AA454
    void SetBiquadFilter(int type, float value);         // 0x022AA474: the value held to 0 and 1
    void SetPriority(uint32_t priority);                 // 0x022AA5D0
    void SetFrontBypass(bool bypass);                    // 0x022AA64C
    void SetOutputLine(uint32_t line);                   // 0x022AA6D0
    void SetTvParam(const OutputParam& param);           // 0x022AA844
    void SetDrcParam(int drc, const OutputParam& param); // 0x022AA84C

    int ChannelCount() const
    {
        return channel_count_;
    }

    uint32_t Priority() const
    {
        return priority_;
    }

    float Volume() const
    {
        return volume_;
    }

    float Pitch() const
    {
        return pitch_;
    }

private:
    friend class MultiVoiceManager;

    // A wave channel's voice (internal::Voice): the AxVoice it plays on, and what it passes to that voice.
    struct Voice
    {
        uint32_t priority = 0;
        AxVoice::State state = AxVoice::State::kStop;
        VoiceParam param;
        SampleFormat format = SampleFormat::kPcm16;
        uint32_t sample_rate = 32000;
        std::array<int16_t, 16> coefs{};
        AxVoice* ax = nullptr;
    };

    // The changes that wait for the next update (+0x380): the start, and the calculations that a parameter change
    // requires.
    static constexpr uint16_t kSyncStart = 0x01;
    static constexpr uint16_t kSyncSrc = 0x04;
    static constexpr uint16_t kSyncMix = 0x08;
    static constexpr uint16_t kSyncLpf = 0x10;
    static constexpr uint16_t kSyncBiquad = 0x20;
    static constexpr uint16_t kSyncVe = 0x40;

    // A channel's gains before its device's volume and sends (PreMixVolume): front left and right, rear left and right,
    // front centre, LFE, and the centre of the two fronts.
    using PreMixVolume = std::array<float, 7>;

    bool Alloc(int channels, uint32_t priority, Callback callback); // 0x022AA2B0
    void InitParam(Callback callback);                              // 0x022AA1C0

    // Each frame: first the calculations that the changes require (Calc, 0x0229D168). Then the start and the parameters
    // passed to the voices (Update, 0x0229D3AC).
    void Calc();
    void Update();

    // Each frame before the sound frame's other work: the end of the wave, or AX taking a voice away (0x0229BCA0).
    void UpdateVoiceStatus();
    bool IsPlayFinished() const; // 0x0229BC2C

    void CalcSrc();          // 0x0229BF24
    void CalcVe();           // 0x0229C028
    void CalcMix();          // 0x0229CCA8
    void CalcLpf();          // 0x0229CEF0
    void CalcBiquadFilter(); // 0x0229CFBC

    // A channel's gains for a device (0 the TV, 1 a GamePad) by its pan and surround pan, or its mix parameters
    // (0x0229C2F4).
    PreMixVolume CalcPreMixVolume(const OutputParam& param, int channel, int device) const;

    // A device's sends: its volume, sends and the gains, or nothing when the voice's output line doesn't reach it
    // (CalcMixImpl, 0x0229C8F8). `device_bit` is the device's bit in an output line.
    OutputMix CalcMixImpl(int device_bit, const OutputParam& param, const PreMixVolume& gains) const;

    void RunAllSdkVoice();  // 0x0229D2D4
    void StopAllSdkVoice(); // 0x02298ACC

    void SetVoiceState(Voice& voice, AxVoice::State state);        // Voice::SetState, 0x0229897C
    void FreeVoice(Voice& voice);                                  // Voice::Free, 0x02298DCC
    void SetOutputParam(const OutputParam& from, OutputParam& to); // SetOutputParamImpl, 0x022AA6F0

    MultiVoiceManager* manager_ = nullptr;
    std::array<Voice, 2> voices_;
    int channel_count_ = 0;
    Callback callback_;
    bool active_ = false;               // +0x374
    bool started_ = false;              // +0x375
    bool sdk_started_ = false;          // +0x376: the voices have been started on AX
    WaveBuffer* last_buffer_ = nullptr; // +0x37C
    uint16_t sync_ = 0;                 // +0x380
    uint8_t biquad_type_ = 0;           // +0x382
    bool front_bypass_ = false;         // +0x384
    float volume_ = 1.0f;               // +0x388
    float pitch_ = 1.0f;                // +0x38C
    uint32_t pan_mode_ = 0;             // +0x390
    uint32_t pan_curve_ = 0;            // +0x394
    float lpf_freq_ = 1.0f;             // +0x398
    float biquad_value_ = 0.0f;         // +0x39C
    uint32_t priority_ = 0;             // +0x3A0
    uint32_t output_line_ = 1;          // +0x3A4
    OutputParam tv_;                    // +0x3A8
    std::array<OutputParam, 2> drc_;    // +0x3F8
    SampleFormat format_ = SampleFormat::kPcm16;
};

// The MultiVoices, a fixed number of them: those in use in a list from the lowest priority up, the newest last among
// equals, and the others in a queue.
class MultiVoiceManager
{
public:
    // `count` is nw::snd's voice count, the .data word at 0x101AA478 (96 unless the game changes it before the sound
    // system starts).
    MultiVoiceManager(AxVoices& ax, const HardwareSettings& hardware, int count);

    MultiVoiceManager(const MultiVoiceManager&) = delete;
    MultiVoiceManager& operator=(const MultiVoiceManager&) = delete;

    // A MultiVoice for `channels` channels, or nullptr (AllocVoice, 0x022AAC0C). When every MultiVoice is in use, the
    // lowest one of a lower or equal priority is dropped.
    MultiVoice* AllocVoice(int channels, uint32_t priority, MultiVoice::Callback callback);

    // Drops the MultiVoice of the lowest priority, the oldest among equals. Nothing is dropped if that MultiVoice's
    // priority is above `priority`. Returns the dropped MultiVoice's channel count, or 0 if none was dropped
    // (0x022AAB50).
    int DropLowestPriorityVoice(uint32_t priority);

    void UpdateAllVoiceStatus(); // 0x0229BE08
    void UpdateAllVoices();      // 0x0229D528

    int ActiveCount() const
    {
        return static_cast<int>(active_.size());
    }

private:
    friend class MultiVoice;

    void AppendVoiceList(MultiVoice& voice);     // 0x022AA4F8
    void RemoveVoiceList(MultiVoice& voice);     // 0x02298E30
    void ChangeVoicePriority(MultiVoice& voice); // 0x022AA584

    AxVoices* ax_;
    const HardwareSettings* hardware_;
    std::vector<std::unique_ptr<MultiVoice>> voices_;
    std::list<MultiVoice*> active_;
    std::list<MultiVoice*> free_;
};

} // namespace csf::nwsnd
