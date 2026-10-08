// SPDX-License-Identifier: MIT

#include "ax/system.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace csf::ax
{
namespace
{

int16_t ClampToInt16(int32_t x)
{
    return static_cast<int16_t>(std::clamp(x, -32768, 32767));
}

// A float converted to a 32-bit integer toward zero. A value out of range gives 0x80000000.
int32_t FloatToInt32(float x)
{
    if (!(x >= -2147483648.0f && x < 2147483648.0f))
    {
        return INT32_MIN;
    }

    return static_cast<int32_t>(x);
}

// The renderer's addresses keep 29 bits of the byte address: 29 for PCM8's bytes, 28 for PCM16's words and 30 for
// ADPCM's nibbles.
uint32_t AddressMask(SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return 0x1FFFFFFF;
    case SampleFormat::kPcm16:
        return 0x0FFFFFFF;
    default:
        return 0x3FFFFFFF;
    }
}

// The address in the format's units of the sample `offset` units past the sample base `base`.
uint32_t AddressOf(SampleFormat format, uint32_t base, uint32_t offset)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return base + offset;
    case SampleFormat::kPcm16:
        return base / 2 + offset;
    default:
        return 2 * base + offset;
    }
}

// The FIR upsampler's taps, from the system library's data (sndcore2 0x100056E8 and snd_core 0x10004D68 in version
// 15702 of the OS, and the same in earlier versions). There are three phases of 12 taps. Every two inputs give three
// outputs, and each phase gives one of them. Phase p gives output 3k + p from inputs 2k + 1 down to 2k - 10. The first
// tap is for the latest input. Each phase's taps add up to 1 within 0.001.
// clang-format off
constexpr std::array<float, 36> kFirTaps = {
    0.0f,              -0x1.29521p-8f,    0x1.69a54p-7f,     -0x1.0f891p-5f,    0x1.4d715p-4f,     -0x1.863184p-3f,
    0x1.79054ep-1f,    0x1.02b344p-1f,    -0x1.34c8aep-3f,   0x1.f82c7ep-5f,    -0x1.79083cp-6f,   0x1.c9e4a4p-8f,
    0.0f,              0x1.3a63a8p-9f,    -0x1.3a883ep-7f,   0x1.ef034ap-6f,    -0x1.503674p-4f,   0x1.100f26p-2f,
    0x1.d676cp-1f,     -0x1.6c0b82p-3f,   0x1.43f1ap-4f,     -0x1.1d7ef4p-5f,   0x1.9fc89cp-7f,    -0x1.3afe62p-8f,
    -0x1.f5e68p-9f,    0x1.4bbefep-7f,    -0x1.8f5756p-6f,   0x1.8c31a4p-5f,    -0x1.8b981ep-4f,   0x1.050054p+0f,
    0x1.c8094ap-5f,    -0x1.2a2e28p-7f,   -0x1.54a516p-10f,  0x1.3dc1d2p-9f,    -0x1.715fe2p-10f,  0.0f,
};
// clang-format on

} // namespace

System::System(const Memory& memory) : memory_(&memory)
{
    Init(Library::kSndCore);
}

void System::Init(Library library, bool renderer_48k)
{
    library_ = library;
    samples_per_frame_ = library == Library::kSndCore2 && renderer_48k ? kSamplesPerFrame48k : kSamplesPerFrame32k;
    in_frame_ = false;
    frame_ = 0;
    default_mixer_select_ = 1;
    user_count_ = 0;

    // Every voice free, in index order. The first voice acquired is then voice 95.
    free_.clear();
    for (std::vector<Voice*>& list : priority_lists_)
    {
        list.clear();
    }
    for (int i = 0; i < kVoiceCount; ++i)
    {
        voices_[i] = Voice{};
        voices_[i].index = static_cast<uint32_t>(i);
        free_.push_back(&voices_[i]);
    }

    frame_callback_ = {};
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        const int channels = device == 0 ? kTvChannels : kDrcChannels;
        for (AuxBus& aux : aux_[device])
        {
            aux = AuxBus{};
            for (std::vector<int32_t>& buffer : aux.buffers)
            {
                buffer.assign(static_cast<std::size_t>(channels) * kMaxSamplesPerFrame, 0);
            }
        }
    }
    device_volume_.fill(kUnityVolume);
    dsp_device_volume_.fill(kUnityVolume);
    mixers_[0] = Mixer{};
    mixers_[0].channels = kTvChannels;
    mixers_[0].devices = 1;
    mixers_[1] = Mixer{};
    mixers_[1].channels = kDrcChannels;
    mixers_[1].devices = kDrcCount;

    buses_ = {};
    main_ = {};
    output_ = {};
    output48_ = {};
    stereo_ = {};
}

Voice* System::AcquireVoice(uint32_t priority, Voice::Callback callback, uint32_t context)
{
    if (priority == kPriorityFree || priority > kPriorityMax)
    {
        return nullptr;
    }

    Voice* voice = nullptr;
    if (!free_.empty())
    {
        voice = free_.back();
        free_.pop_back();
    }
    else
    {
        // None free: the oldest voice of the lowest priority below the request. The owner of that voice is notified
        // first.
        for (uint32_t lower = 1; lower < priority && !voice; ++lower)
        {
            if (!priority_lists_[lower].empty())
            {
                voice = priority_lists_[lower].front();
            }
        }
        if (!voice)
        {
            return nullptr;
        }

        if (voice->state != VoiceState::kStop)
        {
            voice->depop = true;
        }
        if (voice->callback)
        {
            voice->callback(*voice, voice->context, 0);
        }
        RemoveFromPriority(*voice);
    }

    voice->priority = priority;
    priority_lists_[priority].push_back(voice);
    voice->callback = std::move(callback);
    voice->context = context;
    ResetVoice(*voice);

    return voice;
}

void System::FreeVoice(Voice& voice)
{
    if (voice.priority == kPriorityFree)
    {
        return;
    }

    voice.is_protected = false;
    voice.protect_count = 0;
    RemoveFromPriority(voice);
    if (voice.state != VoiceState::kStop)
    {
        voice.depop = true;
    }
    ResetVoice(voice);
    voice.callback = {};
    voice.priority = kPriorityFree;
    free_.push_back(&voice);
}

void System::SetVoicePriority(Voice& voice, uint32_t priority)
{
    if (voice.priority == kPriorityFree || priority == kPriorityFree || priority > kPriorityMax)
    {
        return;
    }

    RemoveFromPriority(voice);
    voice.priority = priority;
    priority_lists_[priority].push_back(&voice);
}

void System::SetVoiceState(Voice& voice, VoiceState state)
{
    if (state == voice.state)
    {
        return;
    }

    Touch(voice);
    voice.state = state;
    voice.game.state = state;
    voice.sync |= sync::kState;
    if (state == VoiceState::kStop)
    {
        voice.depop = true;
    }
}

void System::SetVoiceType(Voice& voice, VoiceType type)
{
    Touch(voice);
    voice.game.type = type;
    voice.sync |= sync::kType;
}

void System::SetVoiceOffsets(Voice& voice, const Offsets& offsets)
{
    const uint32_t base = offsets.samples;
    if (base == 0)
    {
        return;
    }

    Touch(voice);
    voice.offsets = offsets;
    uint32_t extension = 0;
    switch (offsets.format)
    {
    case SampleFormat::kPcm8:
        extension = (base + offsets.current) >> 29;
        break;
    case SampleFormat::kPcm16:
        extension = (base + 2 * offsets.current) >> 29;
        break;
    case SampleFormat::kAdpcm:
        extension = (base + offsets.current / 2) >> 29;
        break;
    default:
        return;
    }

    Pb& game = voice.game;
    const uint32_t mask = AddressMask(offsets.format);
    game.loop_flag = offsets.loop_flag;
    game.format = offsets.format;
    game.extension = static_cast<uint16_t>(extension);
    game.loop_address = AddressOf(offsets.format, base, offsets.loop) & mask;
    game.end_address = AddressOf(offsets.format, base, offsets.end) & mask;
    game.current_address = AddressOf(offsets.format, base, offsets.current) & mask;
    if (offsets.format != SampleFormat::kAdpcm)
    {
        game.adpcm = Adpcm{};
        game.adpcm.gain = offsets.format == SampleFormat::kPcm8 ? 0x0100 : 0x0800;
        voice.sync |= sync::kAdpcm;
    }

    voice.sync &= ~(sync::kLoopFlag | sync::kLoopAddress | sync::kEndAddress | sync::kCurrentAddress);
    voice.sync |= sync::kAddresses;
}

void System::SetVoiceLoop(Voice& voice, bool loop)
{
    Touch(voice);
    voice.offsets.loop_flag = loop;
    voice.game.loop_flag = loop;
    voice.sync |= sync::kLoopFlag;
}

void System::SetVoiceLoopOffset(Voice& voice, uint32_t offset)
{
    Touch(voice);
    voice.game.loop_address = AddressOf(voice.game.format, voice.offsets.samples & 0x1FFFFFFF, offset);
    voice.offsets.loop = offset;
    voice.sync |= sync::kLoopAddress;
}

void System::SetVoiceEndOffset(Voice& voice, uint32_t offset)
{
    Touch(voice);
    voice.game.end_address = AddressOf(voice.game.format, voice.offsets.samples & 0x1FFFFFFF, offset);
    voice.offsets.end = offset;
    voice.sync |= sync::kEndAddress;
}

void System::SetVoiceLoopOffsetEx(Voice& voice, uint32_t offset, uint32_t samples)
{
    voice.offsets.samples = samples;
    SetVoiceLoopOffset(voice, offset);
}

void System::SetVoiceEndOffsetEx(Voice& voice, uint32_t offset, uint32_t samples)
{
    voice.offsets.samples = samples;
    SetVoiceEndOffset(voice, offset);
}

void System::SetVoiceCurrentOffset(Voice& voice, uint32_t offset)
{
    Touch(voice);
    voice.game.current_address = AddressOf(voice.game.format, voice.offsets.samples & 0x1FFFFFFF, offset);
    voice.offsets.current = offset;
    voice.sync |= sync::kCurrentAddress;
}

Offsets System::GetVoiceOffsets(const Voice& voice) const
{
    // The positions in the game's copy, with the extension's three bits, less the sample base, in the units of the
    // format the offsets were set with.
    Offsets offsets = voice.offsets;
    const uint32_t base = offsets.samples;
    const uint32_t high = static_cast<uint32_t>(voice.game.extension) << 29;
    const auto offset_of = [&](uint32_t address)
    {
        switch (offsets.format)
        {
        case SampleFormat::kPcm8:
            return (address | high) - base;
        case SampleFormat::kPcm16:
            return (address | high / 2) - base / 2;
        default:
            return (address | high * 2) - 2 * base;
        }
    };
    offsets.loop = offset_of(voice.game.loop_address);
    offsets.end = offset_of(voice.game.end_address);
    offsets.current = offset_of(voice.game.current_address);

    return offsets;
}

Offsets System::GetVoiceOffsetsEx(Voice& voice, uint32_t samples)
{
    voice.offsets.samples = samples;
    voice.offsets = GetVoiceOffsets(voice);

    return voice.offsets;
}

uint32_t System::GetVoiceLoopCount(Voice& voice)
{
    // sndcore2 0x020121D8 and snd_core 0x0200DC30 in version 15702 of the OS, on the game's copy. For either renderer,
    // a frame is 96 samples in snd_core and 144 in sndcore2. The loop's length in output samples wraps for a loop of
    // 2^16 units or more. So a long loop can count as a short one. The first call after the voice is acquired measures
    // from position 0.
    const Pb& game = voice.game;
    const uint32_t loop = game.loop_address;
    const uint32_t end = game.end_address;
    const uint32_t current = game.current_address;
    const uint32_t last = std::exchange(voice.last_position, current);
    const uint32_t ratio = game.src_mode == SrcMode::kNone ? 0x10000 : game.src.ratio;
    const uint32_t frame = library_ == Library::kSndCore ? kSamplesPerFrame32k : kOutputSamplesPerFrame;
    if (ratio != 0 && end > loop)
    {
        const uint32_t length = ((end - loop) << 16) / ratio;
        if (length <= frame)
        {
            // A division by zero gives zero on the console's CPU.
            voice.loop_count += length != 0 ? (last - loop + frame) / length : 0;

            return voice.loop_count;
        }
    }

    if (current >= loop && end >= last && (last > current || loop > end))
    {
        ++voice.loop_count;
    }

    return voice.loop_count;
}

void System::SetVoiceAdpcm(Voice& voice, const Adpcm& adpcm)
{
    Touch(voice);
    voice.game.adpcm = adpcm;
    voice.sync |= sync::kAdpcm;
}

void System::SetVoiceAdpcmLoop(Voice& voice, const AdpcmLoop& loop)
{
    Touch(voice);
    voice.game.adpcm_loop.predictor_scale = loop.predictor_scale;
    if (voice.game.type == VoiceType::kNormal)
    {
        voice.game.adpcm_loop.yn1 = loop.yn1;
        voice.game.adpcm_loop.yn2 = loop.yn2;
    }
    voice.sync |= sync::kAdpcmLoop;
}

void System::SetVoiceSrc(Voice& voice, const Src& src)
{
    Touch(voice);
    voice.game.src = src;
    voice.sync = (voice.sync | sync::kSrc) & ~sync::kSrcRatio;
}

void System::SetVoiceSrcRatio(Voice& voice, float ratio)
{
    Touch(voice);
    const auto fixed = static_cast<uint32_t>(std::clamp(FloatToInt32(ratio * 65536.0f), 0, 0x80000));
    if (fixed != voice.game.src.ratio)
    {
        voice.game.src.ratio = fixed;
        voice.sync |= sync::kSrcRatio;
    }
}

void System::SetVoiceSrcType(Voice& voice, uint32_t type)
{
    Touch(voice);
    if (type == 0)
    {
        voice.game.src_mode = SrcMode::kNone;
    }
    else if (type == 1)
    {
        voice.game.src_mode = SrcMode::kLinear;
    }
    else if (type <= 4)
    {
        voice.game.src_mode = SrcMode::kFourTap;
        voice.game.four_tap_filter = static_cast<uint16_t>(type - 2);
    }
    voice.sync |= sync::kSrcType;
}

void System::SetVoiceVe(Voice& voice, VolumeRamp ve)
{
    Touch(voice);
    voice.game.ve = ve;
    voice.sync |= sync::kVe;
}

int System::SetVoiceDeviceMix(Voice& voice, Device device, uint32_t index, std::span<const ChannelMix> mix)
{
    std::size_t channels = 0;
    switch (device)
    {
    case Device::kTv:
        channels = kTvChannels;
        if (index != 0)
        {
            return -2;
        }
        break;
    case Device::kDrc:
        channels = kDrcChannels;
        if (index >= kDrcCount)
        {
            return -2;
        }
        break;
    case Device::kRemote:
        // The remotes' speakers aren't rendered.
        return index < 4 ? 0 : -2;
    default:
        return -1;
    }
    if (mix.size() < channels)
    {
        return -3;
    }

    // A channel takes part in a bus unless both its volume and its delta are 0. The channel's mask bits indicate
    // whether it ramps.
    Touch(voice);
    for (int bus = 0; bus < kBusCount; ++bus)
    {
        uint16_t mask = 0;
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            const VolumeRamp& ramp = mix[channel][bus];
            const uint16_t bits = ramp.volume == 0 && ramp.delta == 0 ? 0 : ramp.delta != 0 ? 3 : 1;
            mask |= static_cast<uint16_t>(bits << (2 * channel));
            if (device == Device::kTv)
            {
                voice.game.tv_mix[channel][bus] = ramp;
            }
            else
            {
                voice.game.drc_mix[index][channel][bus] = ramp;
            }
        }
        if (device == Device::kTv)
        {
            voice.game.tv_mask[bus] = mask;
        }
        else
        {
            voice.game.drc_mask[index][bus] = mask;
        }
    }
    voice.sync |= sync::kMixMasks | sync::kMix;

    return 0;
}

void System::SetVoiceLpf(Voice& voice, const Lpf& lpf)
{
    Touch(voice);
    voice.game.lpf = lpf;
    voice.sync |= sync::kLpf;
}

void System::SetVoiceLpfCoefs(Voice& voice, int16_t a0, int16_t b0)
{
    Touch(voice);
    voice.game.lpf.a0 = a0;
    voice.game.lpf.b0 = b0;
    voice.sync |= sync::kLpfCoefs;
}

void System::SetVoiceBiquad(Voice& voice, const Biquad& biquad)
{
    Touch(voice);
    voice.game.biquad = biquad;
    voice.sync |= sync::kBiquad;
}

void System::SetVoiceBiquadCoefs(Voice& voice, int16_t b0, int16_t b1, int16_t b2, int16_t a1, int16_t a2)
{
    Touch(voice);
    Biquad& biquad = voice.game.biquad;
    biquad.b0 = b0;
    biquad.b1 = b1;
    biquad.b2 = b2;
    biquad.a1 = a1;
    biquad.a2 = a2;
    voice.sync |= sync::kBiquadCoefs;
}

std::pair<uint16_t, uint16_t> System::ComputeLpfCoefs(uint32_t frequency) const
{
    // b0 is the pole of a one-pole low-pass filter at the cutoff. a0 + b0 is unity. snd_core uses 32 kHz. sndcore2
    // takes the renderer's rate and computes k k - 1 in one fused step.
    const float rate = library_ == Library::kSndCore2 ? static_cast<float>(samples_per_frame_ * 1000 / 3) : 32000.0f;
    const float w = 6.2831855f * static_cast<float>(frequency) / rate;
    const float k = 2.0f - std::cos(w);
    const float k2 = library_ == Library::kSndCore2 ? std::fma(k, k, -1.0f) : k * k - 1.0f;
    const auto b0 = static_cast<uint16_t>(FloatToInt32((k - std::sqrt(k2)) * 32768.0f));

    return {static_cast<uint16_t>(32767 - b0), b0};
}

int System::UserBegin()
{
    if (in_frame_)
    {
        return -2;
    }

    return static_cast<int>(++user_count_);
}

int System::UserEnd()
{
    if (in_frame_)
    {
        return -2;
    }
    if (user_count_ == 0)
    {
        return -3;
    }

    if (--user_count_ == 0)
    {
        for (Voice& voice : voices_)
        {
            if (voice.is_protected && voice.protect_count == 0)
            {
                voice.is_protected = false;
            }
        }
    }

    return static_cast<int>(user_count_);
}

int System::VoiceBegin(Voice& voice)
{
    if (in_frame_)
    {
        return -2;
    }

    voice.is_protected = true;

    return static_cast<int>(++voice.protect_count);
}

int System::VoiceEnd(Voice& voice)
{
    if (in_frame_)
    {
        return -2;
    }
    if (!voice.is_protected)
    {
        return -3;
    }

    if (voice.protect_count > 0)
    {
        --voice.protect_count;
    }
    if (voice.protect_count == 0 && user_count_ == 0)
    {
        voice.is_protected = false;
    }

    return static_cast<int>(voice.protect_count);
}

int System::RegisterAuxCallback(Device device, uint32_t index, int bus, AuxCallback callback)
{
    AuxBus* aux = FindAuxBus(device, index, bus);
    if (!aux)
    {
        return device == Device::kTv || device == Device::kDrc ? -2 : -1;
    }

    aux->callback = std::move(callback);
    if (!aux->callback)
    {
        for (std::vector<int32_t>& buffer : aux->buffers)
        {
            std::fill(buffer.begin(), buffer.end(), 0);
        }
    }

    return 0;
}

void System::SetAuxReturnVolume(Device device, uint32_t index, int bus, uint16_t volume)
{
    if (AuxBus* aux = FindAuxBus(device, index, bus))
    {
        aux->return_volume = volume;
    }
}

void System::RegisterDeviceFinalMixCallback(Device device, FinalMixCallback callback)
{
    if (device == Device::kTv || device == Device::kDrc)
    {
        mixers_[device == Device::kTv ? 0 : 1].callback = std::move(callback);
    }
}

void System::SetDeviceUpsampleStage(Device device, bool after_final_mix)
{
    if (device == Device::kTv || device == Device::kDrc)
    {
        mixers_[device == Device::kTv ? 0 : 1].upsample_after = after_final_mix;
    }
}

int System::SetDeviceLinearUpsampler(Device device, uint32_t index, bool linear)
{
    if (device != Device::kTv && device != Device::kDrc)
    {
        return -1;
    }

    Mixer& mixer = mixers_[device == Device::kTv ? 0 : 1];
    if (index >= static_cast<uint32_t>(mixer.devices))
    {
        return -2;
    }

    mixer.linear[index] = linear;

    return 0;
}

int System::SetDeviceRemixMatrix(Device device, uint32_t in, uint32_t out, std::span<const float> matrix)
{
    if (device != Device::kTv && device != Device::kDrc)
    {
        return -1;
    }

    Mixer& mixer = mixers_[device == Device::kTv ? 0 : 1];
    if (in == 0 || in > static_cast<uint32_t>(mixer.channels) || out == 0 || matrix.size() < in * out)
    {
        return -7;
    }

    RemixMatrix remix{in, out, std::vector<float>(matrix.begin(), matrix.begin() + in * out)};
    for (RemixMatrix& old : mixer.matrices)
    {
        if (old.in == in && old.out == out)
        {
            old = std::move(remix);

            return 0;
        }
    }
    mixer.matrices.push_back(std::move(remix));

    return 0;
}

void System::SetDeviceVolume(Device device, uint32_t index, uint16_t volume)
{
    if (device == Device::kTv && index == 0)
    {
        device_volume_[0] = volume;
    }
    else if (device == Device::kDrc && index < kDrcCount)
    {
        device_volume_[1 + index] = volume;
    }
}

void System::RenderFrame()
{
    in_frame_ = true;
    const int n = samples_per_frame_;

    // The voices to render are those that are playing or have changes waiting. They're taken by priority from the
    // highest, oldest first within each priority. Each voice takes its changes now. The voices are rendered in the
    // opposite order. A free voice is stopped in both copies.
    order_.clear();
    for (uint32_t priority = kPriorityMax; priority >= 1; --priority)
    {
        for (Voice* voice : priority_lists_[priority])
        {
            if (voice->game.state == VoiceState::kRun || voice->sync)
            {
                Sync(*voice);
                order_.push_back(voice);
            }
        }
    }
    for (Voice* voice : free_)
    {
        voice->state = VoiceState::kStop;
        voice->game.state = VoiceState::kStop;
        voice->renderer.state = VoiceState::kStop;
    }

    buses_ = {};
    for (auto voice = order_.rbegin(); voice != order_.rend(); ++voice)
    {
        RenderVoice((*voice)->renderer, *memory_, n, buses_);
    }

    // The aux buses go out and come back. Then the game's callbacks run.
    main_ = buses_;
    ReturnAux(frame_);
    RunAuxCallbacks(frame_);
    if (frame_callback_)
    {
        frame_callback_();
    }

    // Each device's main bus, at the device's volume, becomes the device's output. The volume moves across the frame
    // from the one the DSP used last.
    const auto samples = static_cast<std::size_t>(n);
    for (int channel = 0; channel < kTvChannels; ++channel)
    {
        OutputBus({main_.tv[0][channel].data(), samples}, dsp_device_volume_[0], device_volume_[0],
                  {output_.tv[channel].data(), samples});
    }
    for (int drc = 0; drc < kDrcCount; ++drc)
    {
        for (int channel = 0; channel < kDrcChannels; ++channel)
        {
            OutputBus({main_.drc[drc][0][channel].data(), samples}, dsp_device_volume_[1 + drc],
                      device_volume_[1 + drc], {output_.drc[drc][channel].data(), samples});
        }
    }
    dsp_device_volume_ = device_volume_;

    // The final mix stage, then the TV's stereo output, clamped to 16 bits (sndcore2 0x02008078 and 0x020092BC in
    // version 15702 of the OS). Between the two the console also runs the post-final-mix callback, the GamePad's
    // virtual surround, the OS's transition fade and the compressor. None of those is modelled.
    FinalMixStage();
    for (int i = 0; i < kOutputSamplesPerFrame; ++i)
    {
        stereo_[2 * i] = ClampToInt16(output48_.tv[0][i]);
        stereo_[2 * i + 1] = ClampToInt16(output48_.tv[1][i]);
    }

    ++frame_;
    in_frame_ = false;
}

void System::ResetVoice(Voice& voice)
{
    // What every acquired voice starts with. The rest of the game's copy (the addresses, the ADPCM state, the envelope,
    // the mix volumes, the ratio, the filters' coefficients) keeps the last owner's values until the game sets it. The
    // renderer's copy keeps all of the last owner's values until the changes reach it.
    voice.state = VoiceState::kStop;
    voice.sync = sync::kState | sync::kItd | sync::kSrc | sync::kLpf | sync::kBiquad | sync::kRmtOn | sync::kRmtSrc |
                 sync::kRmtIir;
    voice.mixer_select = default_mixer_select_;
    voice.last_position = 0;
    voice.loop_count = 0;

    Pb& game = voice.game;
    game.state = VoiceState::kStop;
    game.lpf.on = 0;
    game.biquad.on = 0;
    game.rmt_on = 0;
    game.mixer_select = static_cast<uint16_t>(default_mixer_select_);
    game.src_mode = SrcMode::kFourTap;
    game.src.frac = 0;
    game.src.last = {};
    game.tv_mask = {};
    game.drc_mask = {};
}

void System::Touch(Voice& voice)
{
    // While the game has begun protection, the voices it changes are protected too.
    if (user_count_ > 0 && !in_frame_)
    {
        voice.is_protected = true;
    }
}

void System::Sync(Voice& voice)
{
    // The renderer's progress reaches the game's copy. If the game has changed the same thing since, the game's change
    // stays.
    Pb& game = voice.game;
    Pb& renderer = voice.renderer;
    const uint32_t bits = voice.sync;
    if (!(bits & sync::kState))
    {
        game.state = renderer.state;
        voice.state = renderer.state;
    }
    if (!(bits & (sync::kAddresses | sync::kCurrentAddress)))
    {
        game.current_address = renderer.current_address;
    }
    if (!(bits & sync::kVe))
    {
        game.ve.volume = renderer.ve.volume;
    }
    if (!(bits & sync::kAdpcm))
    {
        game.adpcm = renderer.adpcm;
    }
    if (!(bits & sync::kSrc))
    {
        game.src.frac = renderer.src.frac;
        game.src.last = renderer.src.last;
    }

    // A protected voice's changes wait.
    if (voice.is_protected)
    {
        return;
    }

    // The game's changes reach the renderer's copy. Where two bits cover the same fields, the narrower one wins.
    if (bits & sync::kSrcType)
    {
        renderer.src_mode = game.src_mode;
        renderer.four_tap_filter = game.four_tap_filter;
    }
    if (bits & sync::kMixMasks)
    {
        renderer.tv_mask = game.tv_mask;
        renderer.drc_mask = game.drc_mask;
    }
    if (bits & sync::kState)
    {
        renderer.state = game.state;
    }
    if (bits & sync::kType)
    {
        renderer.type = game.type;
    }
    if (bits & sync::kMix)
    {
        renderer.tv_mix = game.tv_mix;
        renderer.drc_mix = game.drc_mix;
    }
    if (bits & sync::kVeDelta)
    {
        renderer.ve.delta = game.ve.delta;
    }
    else if (bits & sync::kVe)
    {
        renderer.ve = game.ve;
    }
    if (bits & sync::kAddresses)
    {
        renderer.loop_flag = game.loop_flag;
        renderer.format = game.format;
        renderer.extension = game.extension;
        renderer.loop_address = game.loop_address;
        renderer.end_address = game.end_address;
        renderer.current_address = game.current_address;
    }
    else
    {
        if (bits & sync::kLoopFlag)
        {
            renderer.loop_flag = game.loop_flag;
        }
        if (bits & sync::kLoopAddress)
        {
            renderer.loop_address = game.loop_address;
        }
        if (bits & sync::kEndAddress)
        {
            renderer.end_address = game.end_address;
        }
        if (bits & sync::kCurrentAddress)
        {
            renderer.current_address = game.current_address;
        }
    }
    if (bits & sync::kAdpcm)
    {
        renderer.adpcm = game.adpcm;
    }
    if (bits & sync::kSrc)
    {
        renderer.src = game.src;
    }
    else if (bits & sync::kSrcRatio)
    {
        renderer.src.ratio = game.src.ratio;
    }
    if (bits & sync::kAdpcmLoop)
    {
        renderer.adpcm_loop = game.adpcm_loop;
    }
    if (bits & sync::kLpfCoefs)
    {
        renderer.lpf.a0 = game.lpf.a0;
        renderer.lpf.b0 = game.lpf.b0;
    }
    else if (bits & sync::kLpf)
    {
        renderer.lpf = game.lpf;
    }
    if (bits & sync::kBiquadCoefs)
    {
        renderer.biquad.b0 = game.biquad.b0;
        renderer.biquad.b1 = game.biquad.b1;
        renderer.biquad.b2 = game.biquad.b2;
        renderer.biquad.a1 = game.biquad.a1;
        renderer.biquad.a2 = game.biquad.a2;
    }
    else if (bits & sync::kBiquad)
    {
        renderer.biquad = game.biquad;
    }
    if (bits & sync::kRmtOn)
    {
        renderer.rmt_on = game.rmt_on;
    }

    voice.sync = 0;
    voice.depop = false;
}

void System::RemoveFromPriority(Voice& voice)
{
    std::vector<Voice*>& list = priority_lists_[voice.priority];
    list.erase(std::remove(list.begin(), list.end(), &voice), list.end());
}

System::AuxBus* System::FindAuxBus(Device device, uint32_t index, int bus)
{
    if (bus < 0 || bus > 2)
    {
        return nullptr;
    }
    if (device == Device::kTv && index == 0)
    {
        return &aux_[0][bus];
    }
    if (device == Device::kDrc && index < kDrcCount)
    {
        return &aux_[1 + index][bus];
    }

    return nullptr;
}

void System::ReturnAux(int frame)
{
    // The DSP writes each aux bus that has a callback into one of three buffers. At the bus's return volume, the DSP
    // adds back into the main bus the buffer that the callbacks processed a frame ago. That buffer holds what the
    // voices sent two frames ago. The volume moves across the frame from the one the DSP used last.
    const int n = samples_per_frame_;
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        const int channels = device == 0 ? kTvChannels : kDrcChannels;
        for (int bus = 0; bus < 3; ++bus)
        {
            AuxBus& aux = aux_[device][bus];
            if (!aux.callback)
            {
                continue;
            }

            std::vector<int32_t>& out = aux.buffers[frame % 3];
            const std::vector<int32_t>& back = aux.buffers[(frame + 1) % 3];
            const auto samples = static_cast<std::size_t>(n);
            for (int channel = 0; channel < channels; ++channel)
            {
                const Buses::Channel& sent =
                    device == 0 ? buses_.tv[1 + bus][channel] : buses_.drc[device - 1][1 + bus][channel];
                Buses::Channel& main = device == 0 ? main_.tv[0][channel] : main_.drc[device - 1][0][channel];
                const std::size_t at = static_cast<std::size_t>(channel) * kMaxSamplesPerFrame;
                std::copy_n(sent.begin(), samples, out.begin() + static_cast<std::ptrdiff_t>(at));
                csf::ax::ReturnAux({back.data() + at, samples}, aux.dsp_return_volume, aux.return_volume,
                                   {main.data(), samples});
            }
            aux.dsp_return_volume = aux.return_volume;
        }
    }
}

void System::RunAuxCallbacks(int frame)
{
    // Each callback processes what the voices sent its bus in the last frame: TV A, B, C, then each GamePad's buses.
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        const int channels = device == 0 ? kTvChannels : kDrcChannels;
        for (AuxBus& aux : aux_[device])
        {
            if (!aux.callback)
            {
                continue;
            }

            std::vector<int32_t>& buffer = aux.buffers[(frame + 2) % 3];
            std::array<int32_t*, kTvChannels> channel_buffers{};
            for (int channel = 0; channel < channels; ++channel)
            {
                channel_buffers[channel] = buffer.data() + static_cast<std::size_t>(channel) * kMaxSamplesPerFrame;
            }
            aux.callback(std::span<int32_t* const>(channel_buffers.data(), static_cast<std::size_t>(channels)),
                         static_cast<uint32_t>(samples_per_frame_));
        }
    }
}

void System::FinalMixStage()
{
    const int n = samples_per_frame_;
    const bool upsample = n == kSamplesPerFrame32k;
    for (std::size_t m = 0; m < mixers_.size(); ++m)
    {
        // The device's channels, all of each device in turn.
        Mixer& mixer = mixers_[m];
        std::array<Samples*, kTvChannels + kDrcCount * kDrcChannels> in{};
        std::array<Samples*, kTvChannels + kDrcCount * kDrcChannels> out{};
        const std::size_t count = static_cast<std::size_t>(mixer.channels) * mixer.devices;
        for (std::size_t c = 0; c < count; ++c)
        {
            in[c] = m == 0 ? &output_.tv[c] : &output_.drc[c / kDrcChannels][c % kDrcChannels];
            out[c] = m == 0 ? &output48_.tv[c] : &output48_.drc[c / kDrcChannels][c % kDrcChannels];
        }

        // To 16-bit scale and 48 kHz, before the final-mix callback or after it.
        const bool before = upsample && !mixer.upsample_after;
        std::array<Samples, kTvChannels + kDrcCount * kDrcChannels> scratch;
        std::array<int32_t*, kTvChannels + kDrcCount * kDrcChannels> pointers{};
        for (std::size_t c = 0; c < count; ++c)
        {
            if (before)
            {
                Upsample(mixer, c, std::span<const int32_t>(in[c]->data(), n), *out[c], true);
                pointers[c] = out[c]->data();
            }
            else
            {
                Samples& shifted = upsample ? scratch[c] : *out[c];
                for (int i = 0; i < n; ++i)
                {
                    shifted[i] = (*in[c])[i] >> 8;
                }
                pointers[c] = shifted.data();
            }
        }

        FinalMix final_mix{std::span<int32_t* const>(pointers.data(), count), static_cast<uint16_t>(mixer.channels),
                           static_cast<uint16_t>(before || !upsample ? kOutputSamplesPerFrame : n),
                           static_cast<uint16_t>(mixer.devices), static_cast<uint16_t>(mixer.channels)};
        if (mixer.callback)
        {
            mixer.callback(final_mix);
        }

        if (upsample && !before)
        {
            for (std::size_t c = 0; c < count; ++c)
            {
                Upsample(mixer, c, std::span<const int32_t>(scratch[c].data(), n), *out[c], false);
            }
        }

        for (int device = 0; device < mixer.devices; ++device)
        {
            Remix(mixer, final_mix.channels_out,
                  std::span<Samples* const>(out.data() + static_cast<std::size_t>(device) * mixer.channels,
                                            static_cast<std::size_t>(mixer.channels)));
        }
    }
}

void System::Upsample(Mixer& mixer, std::size_t channel, std::span<const int32_t> in, std::span<int32_t> out,
                      bool shift)
{
    if (mixer.linear[channel / static_cast<std::size_t>(mixer.channels)])
    {
        UpsampleLinear(in, out, mixer.history[channel], shift);
    }
    else
    {
        UpsampleFir(in, out, mixer.fir[channel], shift);
    }
}

void System::UpsampleLinear(std::span<const int32_t> in, std::span<int32_t> out, int32_t& history, bool shift)
{
    // Three outputs for every two inputs, spaced evenly and two thirds of an input behind (sndcore2 0x02007B2C in
    // version 15702 of the OS). The middle output is the first input. The other two weigh two inputs by 2/3 and 1/3 in
    // single precision.
    const float two_thirds = std::bit_cast<float>(0x3F2AAAABu);
    const float one_third = std::bit_cast<float>(0x3EAAAAAAu);
    const int bits = shift ? 8 : 0;

    // The product by 2/3 is fused with the sum.
    const auto weigh = [two_thirds](float x, float third)
    {
        return std::fma(x, two_thirds, third);
    };

    for (std::size_t k = 0; 2 * k + 1 < in.size(); ++k)
    {
        const float third_of_first = static_cast<float>(in[2 * k]) * one_third;
        out[3 * k] = FloatToInt32(weigh(static_cast<float>(history), third_of_first)) >> bits;
        out[3 * k + 1] = in[2 * k] >> bits;
        out[3 * k + 2] = FloatToInt32(weigh(static_cast<float>(in[2 * k + 1]), third_of_first)) >> bits;
        history = in[2 * k + 1];
    }
}

void System::UpsampleFir(std::span<const int32_t> in, std::span<int32_t> out, FirHistory& history, bool shift)
{
    // sndcore2 0x02007C84 and snd_core 0x02006140 in version 15702 of the OS, in paired singles. The inputs go in
    // pairs. Each pair holds the later input first. Each phase sums its taps on the pairs' later inputs apart from its
    // taps on the earlier inputs. Each sum starts from the latest pair and takes a product and five fused
    // multiply-adds. Then the phase adds the two sums.
    const int bits = shift ? 8 : 0;
    for (std::size_t k = 0; 2 * k + 1 < in.size(); ++k)
    {
        std::array<float, 12> inputs;
        inputs[0] = static_cast<float>(in[2 * k + 1]);
        inputs[1] = static_cast<float>(in[2 * k]);
        std::copy(history.begin(), history.end(), inputs.begin() + 2);

        for (std::size_t phase = 0; phase < 3; ++phase)
        {
            const float* taps = &kFirTaps[12 * phase];
            float later = taps[0] * inputs[0];
            float earlier = taps[1] * inputs[1];
            for (std::size_t j = 2; j < inputs.size(); j += 2)
            {
                later = std::fma(taps[j], inputs[j], later);
                earlier = std::fma(taps[j + 1], inputs[j + 1], earlier);
            }
            out[3 * k + phase] = FloatToInt32(later + earlier) >> bits;
        }

        std::copy(inputs.begin(), inputs.begin() + history.size(), history.begin());
    }
}

void System::Remix(const Mixer& mixer, uint16_t channels_in, std::span<Samples* const> channels)
{
    // The matrix for the final-mix callback's output channels and the device's two channels, applied in place. Each
    // output is the sum of the inputs' weighted values. Each weighted value is truncated to an integer (sndcore2
    // 0x02008B00 in version 15702 of the OS).
    constexpr uint32_t kDeviceChannels = 2;
    const RemixMatrix* matrix = nullptr;
    for (const RemixMatrix& candidate : mixer.matrices)
    {
        if (candidate.in == channels_in && candidate.out == kDeviceChannels && candidate.in <= channels.size())
        {
            matrix = &candidate;
            break;
        }
    }
    if (!matrix)
    {
        return;
    }

    for (int i = 0; i < kOutputSamplesPerFrame; ++i)
    {
        std::array<float, kTvChannels> in{};
        for (uint32_t k = 0; k < matrix->in; ++k)
        {
            in[k] = static_cast<float>((*channels[k])[i]);
        }
        for (uint32_t j = 0; j < matrix->out; ++j)
        {
            uint32_t sum = 0;
            for (uint32_t k = 0; k < matrix->in; ++k)
            {
                sum += static_cast<uint32_t>(FloatToInt32(in[k] * matrix->weights[j * matrix->in + k]));
            }
            (*channels[j])[i] = static_cast<int32_t>(sum);
        }
    }
}

} // namespace csf::ax
