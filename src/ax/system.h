// SPDX-License-Identifier: MIT

// How CSF runs AX, the Wii U system's sound library: the 96 voices and their priorities, the calls a game makes to set
// them up, and the frame itself. The calls change the game's copy of a voice's parameter block. The changes wait in the
// voice's sync word until the next frame. Each frame copies the changes to the renderer's copy and renders the voices
// into the devices' buses (see render.h). The frame then returns the aux buses through their callbacks and runs the
// frame callbacks. Last, the frame converts each device's output to 48 kHz for the final-mix callbacks and the remix
// matrices. The conversion uses the system library's FIR filter, or linear interpolation where the game selects it. On
// the console, all of this but the rendering is PowerPC code in the system library. The aux buses come back two frames
// after the voices sent them. That delay comes from the rotation of the library's buffers.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "ax/memory.h"
#include "ax/pb.h"
#include "ax/render.h"
#include "ax/voice.h"

namespace csf::ax
{

class System
{
public:
    // The game links snd_core or sndcore2. snd_core has only the 32 kHz renderer. sndcore2 also computes low-pass
    // coefficients for the renderer's rate. Each library uploads a microcode of its own. CSF renders both libraries'
    // voices with sndcore2's microcode (see render.h). snd_core's microcode mixes with the same arithmetic.
    // It chooses the buses and channels from other fields of the voice's block.
    enum class Library
    {
        kSndCore,
        kSndCore2,
    };

    enum class Device
    {
        kTv = 0,
        kDrc = 1,
        kRemote = 2,
    };

    // A channel's volume and delta for each of the four buses (main, A, B, C). AXSetVoiceDeviceMix takes them in this
    // form.
    using ChannelMix = std::array<VolumeRamp, kBusCount>;

    // Processes one aux bus of one device in place: a buffer of `samples` 32-bit samples in 16-bit scale for each
    // channel.
    using AuxCallback = std::function<void(std::span<int32_t* const> channels, uint32_t samples)>;

    // What a device's final-mix callback gets: a buffer per channel (16-bit scale), all the device's channels for each
    // of its devices in turn. The callback may change the samples, and `channels_out`, the remix matrix's input count.
    struct FinalMix
    {
        std::span<int32_t* const> channels;
        uint16_t channels_in = 0;
        uint16_t samples = 0;
        uint16_t devices = 0;
        uint16_t channels_out = 0;
    };

    using FinalMixCallback = std::function<void(FinalMix& mix)>;

    explicit System(const Memory& memory);

    // AXInit, or AXInitWithParams with the 48 kHz renderer.
    void Init(Library library, bool renderer_48k = false);

    int SamplesPerFrame() const
    {
        return samples_per_frame_;
    }

    // AXAcquireVoice: a free voice at `priority` (1 to 31), or else the oldest voice of the lowest priority below it.
    // That voice's callback runs first. Returns nullptr if every voice has `priority` or more.
    Voice* AcquireVoice(uint32_t priority, Voice::Callback callback = {}, uint32_t context = 0);

    void FreeVoice(Voice& voice);

    // Moves the voice to the newest end of its new priority's voices.
    void SetVoicePriority(Voice& voice, uint32_t priority);

    void SetVoiceState(Voice& voice, VoiceState state);
    void SetVoiceType(Voice& voice, VoiceType type);

    // AXSetVoiceOffsets: for PCM, the call also makes the ADPCM block the identity. The identity's gain is 0x800
    // (PCM16) or 0x100 (PCM8). Nothing happens if the sample base is 0.
    void SetVoiceOffsets(Voice& voice, const Offsets& offsets);

    void SetVoiceLoop(Voice& voice, bool loop);
    void SetVoiceLoopOffset(Voice& voice, uint32_t offset);
    void SetVoiceEndOffset(Voice& voice, uint32_t offset);

    // AXSetVoiceLoopOffsetEx and AXSetVoiceEndOffsetEx: an offset from a new sample base. Later offsets count from that
    // base.
    void SetVoiceLoopOffsetEx(Voice& voice, uint32_t offset, uint32_t samples);
    void SetVoiceEndOffsetEx(Voice& voice, uint32_t offset, uint32_t samples);
    void SetVoiceCurrentOffset(Voice& voice, uint32_t offset);

    // AXGetVoiceOffsets: the offsets the game set, with the positions as they stood at the start of the last frame.
    Offsets GetVoiceOffsets(const Voice& voice) const;

    // AXGetVoiceOffsetsEx: the offsets from a new sample base. The voice keeps both the base and the offsets.
    Offsets GetVoiceOffsetsEx(Voice& voice, uint32_t samples);

    // AXGetVoiceLoopCount: the loops that the calls have counted since the voice was acquired. AX has no record of
    // loops. Each call counts one loop if the position has gone back since the last call. For a loop no longer than a
    // frame, a call counts as many loops as a frame from the last position could hold.
    uint32_t GetVoiceLoopCount(Voice& voice);

    void SetVoiceAdpcm(Voice& voice, const Adpcm& adpcm);

    // A stream's loop history isn't kept. The stream's buffer is refilled as it plays.
    void SetVoiceAdpcmLoop(Voice& voice, const AdpcmLoop& loop);

    void SetVoiceSrc(Voice& voice, const Src& src);

    // AXSetVoiceSrcRatio: the ratio in 16.16, from 0 to 8.
    void SetVoiceSrcRatio(Voice& voice, float ratio);

    // AXSetVoiceSrcType: 0 none, 1 linear, 2 to 4 the 8, 12 and 16 kHz 4-tap filters.
    void SetVoiceSrcType(Voice& voice, uint32_t type);

    void SetVoiceVe(Voice& voice, VolumeRamp ve);

    // AXSetVoiceDeviceMix: a ChannelMix for each of the device's channels (6 for the TV, 4 for a GamePad). Returns 0,
    // or -1 for an unknown device and -2 for an index the device doesn't have.
    int SetVoiceDeviceMix(Voice& voice, Device device, uint32_t index, std::span<const ChannelMix> mix);

    void SetVoiceLpf(Voice& voice, const Lpf& lpf);
    void SetVoiceLpfCoefs(Voice& voice, int16_t a0, int16_t b0);
    void SetVoiceBiquad(Voice& voice, const Biquad& biquad);
    void SetVoiceBiquadCoefs(Voice& voice, int16_t b0, int16_t b1, int16_t b2, int16_t a1, int16_t a2);

    // AXComputeLpfCoefs: a0 and b0 for a cutoff of `frequency` Hz.
    std::pair<uint16_t, uint16_t> ComputeLpfCoefs(uint32_t frequency) const;

    // AXIsVoiceRunning: the state that the game's copy shows. That copy is a frame behind the renderer.
    bool IsVoiceRunning(const Voice& voice) const
    {
        return voice.game.state == VoiceState::kRun;
    }

    // Every voice, by its index.
    const std::array<Voice, kVoiceCount>& Voices() const
    {
        return voices_;
    }

    // Protection holds a voice's changes back from the renderer until the protection ends. AXUserBegin protects every
    // voice that the game changes after it. Each of these functions returns the count, or -2 inside a frame. UserEnd
    // and VoiceEnd return -3 when nothing was begun.
    int UserBegin();
    int UserEnd();

    // AXUserIsProtected.
    bool UserIsProtected() const
    {
        return user_count_ != 0;
    }

    int VoiceBegin(Voice& voice);
    int VoiceEnd(Voice& voice);

    void RegisterFrameCallback(std::function<void()> callback)
    {
        frame_callback_ = std::move(callback);
    }

    // AXRegisterAuxCallback: `bus` is 1, 2 or 3 for A, B and C. A bus without a callback isn't returned at all. What
    // voices send to it is lost. Returns 0, or -1 for a device without aux buses and -2 for an index it doesn't have.
    int RegisterAuxCallback(Device device, uint32_t index, int bus, AuxCallback callback);

    // AXSetAuxReturnVolume: the 1.15 gain at which an aux bus comes back into the device's main bus.
    void SetAuxReturnVolume(Device device, uint32_t index, int bus, uint16_t volume);

    void RegisterDeviceFinalMixCallback(Device device, FinalMixCallback callback);

    // AXSetDeviceUpsampleStage: whether a 32 kHz renderer's output reaches 48 kHz after the final-mix callback rather
    // than before it.
    void SetDeviceUpsampleStage(Device device, bool after_final_mix);

    // AXSetDeviceLinearUpsampler: whether a device converts a 32 kHz renderer's output to 48 kHz by linear
    // interpolation rather than with the FIR filter. Every device starts with the FIR filter. `index` is the GamePad's
    // index, or 0 for the TV. Returns 0, or -1 for an unknown device and -2 for an index it doesn't have.
    int SetDeviceLinearUpsampler(Device device, uint32_t index, bool linear);

    // AXSetDeviceRemixMatrix: `out` rows of `in` weights. The final mix stage applies the matrix whose `in` is the
    // final-mix callback's output channel count and whose `out` is the device's channel count (2, for stereo). A matrix
    // with the same counts replaces an older one. Returns 0, or -1 for an unknown device.
    int SetDeviceRemixMatrix(Device device, uint32_t in, uint32_t out, std::span<const float> matrix);

    // Generates the next frame.
    void RenderFrame();

    // The last frame at each stage: the voices' mix and the main buses after the aux returns (s32, 16-bit scale); each
    // device's output at the renderer's rate (s32, 24-bit scale) and at 48 kHz after the final mix stage (s32, 16-bit
    // scale, not yet clamped); and the TV's stereo output, 144 interleaved pairs of 16-bit samples.
    const Buses& VoiceBuses() const
    {
        return buses_;
    }

    const Buses& MainBuses() const
    {
        return main_;
    }

    std::span<const int32_t> TvOutput(int channel) const
    {
        return {output_.tv[channel].data(), static_cast<std::size_t>(samples_per_frame_)};
    }

    std::span<const int32_t> DrcOutput(int drc, int channel) const
    {
        return {output_.drc[drc][channel].data(), static_cast<std::size_t>(samples_per_frame_)};
    }

    std::span<const int32_t> Tv48(int channel) const
    {
        return output48_.tv[channel];
    }

    std::span<const int32_t> Drc48(int drc, int channel) const
    {
        return output48_.drc[drc][channel];
    }

    std::span<const int16_t> Output() const
    {
        return stereo_;
    }

private:
    using Samples = std::array<int32_t, kOutputSamplesPerFrame>;

    // An aux bus: its callback, its return volume and the three buffers it rotates through. Each buffer holds the
    // channels' samples one channel after another.
    struct AuxBus
    {
        AuxCallback callback;
        uint16_t return_volume = kUnityVolume;
        uint16_t dsp_return_volume = kUnityVolume; // the return volume the DSP used last
        std::array<std::vector<int32_t>, 3> buffers;
    };

    struct RemixMatrix
    {
        uint32_t in = 0;
        uint32_t out = 0;
        std::vector<float> weights;
    };

    // The FIR upsampler's history of one channel: its last ten inputs, in pairs from the latest. Each pair has its
    // later input first.
    using FirHistory = std::array<float, 10>;

    // A device type's final mix stage: the TV's one device, or the two GamePads. The two GamePads share a callback and
    // an upsample stage. Each device's channels keep a history for each upsampler.
    struct Mixer
    {
        int channels = 0;
        int devices = 0;
        bool upsample_after = false;
        std::array<bool, kDrcCount> linear{}; // per device
        FinalMixCallback callback;
        std::vector<RemixMatrix> matrices;
        std::array<int32_t, kDrcCount * kDrcChannels> history{}; // the linear upsampler's last input
        std::array<FirHistory, kDrcCount * kDrcChannels> fir{};
    };

    struct Outputs
    {
        std::array<Samples, kTvChannels> tv{};
        std::array<std::array<Samples, kDrcChannels>, kDrcCount> drc{};
    };

    void ResetVoice(Voice& voice);
    void Touch(Voice& voice);
    void Sync(Voice& voice);
    void RemoveFromPriority(Voice& voice);
    AuxBus* FindAuxBus(Device device, uint32_t index, int bus);
    void ReturnAux(int frame);
    void RunAuxCallbacks(int frame);
    void FinalMixStage();
    static void Upsample(Mixer& mixer, std::size_t channel, std::span<const int32_t> in, std::span<int32_t> out,
                         bool shift);
    static void UpsampleLinear(std::span<const int32_t> in, std::span<int32_t> out, int32_t& history, bool shift);
    static void UpsampleFir(std::span<const int32_t> in, std::span<int32_t> out, FirHistory& history, bool shift);
    static void Remix(const Mixer& mixer, uint16_t channels_in, std::span<Samples* const> channels);

    const Memory* memory_;
    Library library_ = Library::kSndCore;
    int samples_per_frame_ = kSamplesPerFrame32k;
    bool in_frame_ = false;
    int frame_ = 0;
    uint32_t default_mixer_select_ = 1;

    std::array<Voice, kVoiceCount> voices_;
    std::vector<Voice*> order_;                                        // the voices the frame renders, last first
    std::vector<Voice*> free_;                                         // a stack: the top is acquired next
    std::array<std::vector<Voice*>, kPriorityMax + 1> priority_lists_; // oldest first
    uint32_t user_count_ = 0;

    std::function<void()> frame_callback_;
    std::array<std::array<AuxBus, 3>, 1 + kDrcCount> aux_; // TV, then the GamePads; buses A, B, C
    std::array<Mixer, 2> mixers_;                          // the TV's mixer, then the GamePads' mixer

    Buses buses_{};
    Buses main_{};
    Outputs output_;
    Outputs output48_;
    std::array<int16_t, 2 * kOutputSamplesPerFrame> stereo_{};
};

} // namespace csf::ax
