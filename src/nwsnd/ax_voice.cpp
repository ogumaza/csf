// SPDX-License-Identifier: MIT

#include "nwsnd/ax_voice.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>

namespace csf::nwsnd
{
namespace
{

// nw::snd is set up for AX's 32 kHz renderer: frames of 96 samples (set when the sound system starts, 0x0229B284).
constexpr int32_t kSamplesPerFrame = 96;
constexpr float kOutputRate = 32000.0f;

// AX's sample format for each of nw::snd's formats (the table at 0x10092BAC).
ax::SampleFormat AxFormat(SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return ax::SampleFormat::kPcm8;
    case SampleFormat::kPcm16:
        return ax::SampleFormat::kPcm16;
    default:
        return ax::SampleFormat::kAdpcm;
    }
}

// A gain as AX's 1.15 volume, held to 0 and 65535 (GetVolumeU16, 0x022A30DC).
uint16_t VolumeU16(float gain)
{
    const float volume = gain * 32768.0f;
    const float held = volume > 65535.0f ? 65535.0f : (volume >= 0.0f ? volume : 0.0f);

    return static_cast<uint16_t>(util::ToInt32(held));
}

// Ramps a volume to `target` over a frame (UpdateDelta, 0x022A382C). Nothing happens if the volume is there and still.
// Otherwise the ramp starts from where the last ramp reached. The step is the one that reaches the target at the
// frame's end. The step is held to 16 bits. When the step rounds to 0, the volume goes straight to the target. Returns
// whether the volume changed.
bool UpdateDelta(uint16_t target, ax::VolumeRamp& ramp)
{
    if (ramp.delta == 0 && ramp.volume == target)
    {
        return false;
    }

    const int32_t reached = ramp.volume + ramp.delta * kSamplesPerFrame;
    const int32_t step = (target - reached) / kSamplesPerFrame;
    if (step == 0)
    {
        ramp = {target, 0};
        return true;
    }
    ramp.volume = static_cast<uint16_t>(reached);
    ramp.delta = static_cast<int16_t>(std::clamp(step, -32768, 32767));

    return true;
}

// A device's mixes from an OutputMix (CalcSetupTvMix, 0x022A3120, and CalcSetupDrcMix, 0x022A3400): each channel's
// buses at their volumes. There are no ramps.
void SetupMix(std::span<ax::System::ChannelMix> mix, const OutputMix& send, int channels)
{
    for (int channel = 0; channel < channels; ++channel)
    {
        for (int bus = 0; bus < ax::kBusCount; ++bus)
        {
            mix[channel][bus] = {VolumeU16(send[bus][channel]), 0};
        }
    }
}

// Ramps a device's mixes to an OutputMix's volumes (CalcUpdateTvMix, 0x022A3F40, and CalcUpdateDrcMix, 0x022A43CC). The
// channels from `first` on ramp to 0. Returns whether any changed.
bool UpdateMix(std::span<ax::System::ChannelMix> mix, const OutputMix& send, int channels, int first_silent)
{
    bool changed = false;
    for (int channel = 0; channel < channels; ++channel)
    {
        for (int bus = 0; bus < ax::kBusCount; ++bus)
        {
            const uint16_t target = channel < first_silent ? VolumeU16(send[bus][channel]) : 0;
            changed |= UpdateDelta(target, mix[channel][bus]);
        }
    }

    return changed;
}

} // namespace

void AxVoice::Initialize(ax::Voice* voice)
{
    ax_voice_ = voice;
    sample_rate_ = 32000;
    loop_count_ = 0;
    format_ = SampleFormat::kPcm16;
    head_ = nullptr;
    state_ = State::kStop;
    next_in_loop_ = false;
    started_with_next_ = false;
    play_position_ = 0;
    tail_ = nullptr;
}

void AxVoice::Finalize()
{
    if (in_list_)
    {
        owner_->EraseFromList(*this);
    }
    FreeAllWaveBuffer();
    if (dispose_)
    {
        const DisposeCallback dispose = std::exchange(dispose_, {});
        dispose(*this);
    }
    ax_voice_ = nullptr;
}

void AxVoice::FreeAllWaveBuffer()
{
    for (WaveBuffer* buffer = head_; buffer; buffer = buffer->next)
    {
        buffer->status = WaveBuffer::Status::kDone;
    }
    head_ = nullptr;
    tail_ = nullptr;
}

void AxVoice::AppendWaveBuffer(WaveBuffer& buffer)
{
    buffer.next = nullptr;
    buffer.status = WaveBuffer::Status::kWait;
    if (!tail_)
    {
        head_ = &buffer;
    }
    else
    {
        tail_->next = &buffer;
    }
    tail_ = &buffer;
}

void AxVoice::SetPriority(uint32_t priority)
{
    priority_ = priority;
    if (in_list_)
    {
        owner_->EraseFromList(*this);
        owner_->AppendToList(*this);
    }
}

uint32_t AxVoice::OffsetOf(uint32_t sample) const
{
    // ADPCM counts nibbles. Two nibbles of each 8-byte frame are the frame's header.
    if (format_ != SampleFormat::kDspAdpcm)
    {
        return sample;
    }

    return sample / 14 * 16 + sample % 14 + 2;
}

void AxVoice::SetupNextWaveBuffer(const WaveBuffer& buffer)
{
    // A buffer that comes with an ADPCM context is a new wave, and the loop takes the context. A stream's next buffer
    // continues the history. That buffer's predictor and scale come from its first frame's header.
    if (buffer.adpcm_context)
    {
        system_->SetVoiceType(*ax_voice_, ax::VoiceType::kNormal);
        system_->SetVoiceAdpcmLoop(
            *ax_voice_, {buffer.adpcm_context->predictor_scale, buffer.adpcm_context->yn1, buffer.adpcm_context->yn2});
        return;
    }

    system_->SetVoiceType(*ax_voice_, ax::VoiceType::kStream);
    system_->SetVoiceAdpcmLoop(*ax_voice_, {memory_->Read8(buffer.address), 0, 0});
}

void AxVoice::SetupOffset(const WaveBuffer& buffer)
{
    // The voice plays from the buffer's start offset to its last sample. The loop is at the buffer's start. A buffer
    // with another after it loops into that one. The loop count shows when it has.
    ax::Offsets offsets;
    offsets.format = AxFormat(format_);
    offsets.current = OffsetOf(buffer.sample_offset);
    offsets.end = OffsetOf(buffer.sample_length - 1);
    offsets.loop = OffsetOf(0);
    offsets.samples = buffer.address;
    if (buffer.next)
    {
        offsets.loop_flag = 1;
        SetupNextWaveBuffer(*buffer.next);
        next_in_loop_ = true;
        started_with_next_ = true;
    }
    else
    {
        offsets.loop_flag = buffer.loop ? 1 : 0;
        started_with_next_ = false;
        next_in_loop_ = false;
    }
    system_->SetVoiceOffsets(*ax_voice_, offsets);
}

void AxVoice::SetupAdpcm(const WaveBuffer& buffer)
{
    // PCM gets the identity. The gain is 0x100 or 0x800 (the blocks at 0x10092C98 and 0x10092CC0).
    ax::Adpcm adpcm;
    switch (format_)
    {
    case SampleFormat::kPcm8:
        adpcm.gain = 0x100;
        break;
    case SampleFormat::kPcm16:
        adpcm.gain = 0x800;
        break;
    case SampleFormat::kDspAdpcm:
        adpcm.coefs = coefs_;
        if (buffer.adpcm_context)
        {
            adpcm.predictor_scale = buffer.adpcm_context->predictor_scale;
            adpcm.yn1 = buffer.adpcm_context->yn1;
            adpcm.yn2 = buffer.adpcm_context->yn2;
        }
        break;
    }
    system_->SetVoiceAdpcm(*ax_voice_, adpcm);
}

void AxVoice::SetupTvMix(OutputMode mode)
{
    // In stereo the surround channels get nothing.
    tv_mix_ = {};
    SetupMix(tv_mix_, param_.tv, mode == OutputMode::kSurround ? ax::kTvChannels : 2);
    system_->SetVoiceDeviceMix(*ax_voice_, ax::System::Device::kTv, 0, tv_mix_);
}

void AxVoice::SetupDrcMix(OutputMode mode)
{
    // The first GamePad's channels start silent, and in surround the second GamePad's front channels do too. In stereo,
    // the main and aux buses reach the first GamePad's front channels. In surround, the buses reach the rear channels
    // too. The main bus may then go to the second GamePad's front channels instead.
    const bool surround = mode == OutputMode::kSurround;
    drc_mix_ = {};
    drc2_mix_[0] = {};
    drc2_mix_[1] = {};
    SetupMix(drc_mix_, param_.drc, surround ? ax::kDrcChannels : 2);
    if (surround && param_.drc_main_to_second)
    {
        drc2_mix_ = {};
        for (int channel = 0; channel < 2; ++channel)
        {
            drc2_mix_[channel][0] = drc_mix_[channel][0];
            drc_mix_[channel][0] = {};
        }
    }
    system_->SetVoiceDeviceMix(*ax_voice_, ax::System::Device::kDrc, 0, drc_mix_);
    if (surround && param_.drc_main_to_second)
    {
        system_->SetVoiceDeviceMix(*ax_voice_, ax::System::Device::kDrc, 1, drc2_mix_);
    }
}

void AxVoice::UpdateVe(bool setup)
{
    const uint16_t volume = VolumeU16(param_.volume);
    if (setup)
    {
        ve_ = {volume, 0};
        system_->SetVoiceVe(*ax_voice_, ve_);
        return;
    }

    if (UpdateDelta(volume, ve_))
    {
        system_->SetVoiceVe(*ax_voice_, ve_);
    }
}

void AxVoice::UpdateSrc(bool setup)
{
    // The ratio of the sample's rate to the renderer's rate, times the pitch.
    const float ratio = param_.pitch * static_cast<float>(sample_rate_) / kOutputRate;
    if (!setup)
    {
        if (ratio != ratio_)
        {
            system_->SetVoiceSrcRatio(*ax_voice_, ratio);
            ratio_ = ratio;
        }
        return;
    }

    // At the start: the ratio in 16.16, held under 8, and the filter. The 4-tap filter is 8 kHz above a ratio of 4/3,
    // 12 kHz above 1 and 16 kHz otherwise. The choice holds whatever the pitch does later.
    const float fixed = 65536.0f * ratio;
    const uint32_t bits = fixed < 2147483648.0f
                              ? static_cast<uint32_t>(util::ToInt32(fixed))
                              : static_cast<uint32_t>(util::ToInt32(fixed - 2147483648.0f)) + 0x80000000u;
    ax::Src src;
    src.ratio = bits >> 16 < 8 ? bits : 0x80000;
    uint32_t type = 0;
    switch (param_.interpolation)
    {
    case 0:
        type = ratio > 0x1.555556p+0f ? 2 : ratio > 1.0f ? 3 : 4;
        break;
    case 1:
        type = 1;
        break;
    default:
        type = 0;
        break;
    }
    system_->SetVoiceSrc(*ax_voice_, src);
    system_->SetVoiceSrcType(*ax_voice_, type);
    ratio_ = ratio;
}

void AxVoice::UpdateTvMix(OutputMode mode)
{
    const bool surround = mode == OutputMode::kSurround;
    if (UpdateMix(tv_mix_, param_.tv, ax::kTvChannels, surround ? ax::kTvChannels : 2))
    {
        system_->SetVoiceDeviceMix(*ax_voice_, ax::System::Device::kTv, 0, tv_mix_);
    }
}

void AxVoice::UpdateDrcMix(OutputMode mode)
{
    // The first GamePad's front channels, and its rear ones in surround. The rear channels ramp to 0 in stereo. In
    // surround, the main bus may go to the second GamePad's front channels instead. The first GamePad's front main
    // entries are then left as they are.
    const bool surround = mode == OutputMode::kSurround;
    const OutputMix& send = param_.drc;
    bool changed = false;
    const auto ramp = [&changed](ax::VolumeRamp& entry, float gain)
    {
        changed |= UpdateDelta(VolumeU16(gain), entry);
    };
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int bus = 1; bus < ax::kBusCount; ++bus)
        {
            ramp(drc_mix_[channel][bus], send[bus][channel]);
        }
    }
    for (int channel = 2; channel < ax::kDrcChannels; ++channel)
    {
        for (int bus = 0; bus < ax::kBusCount; ++bus)
        {
            ramp(drc_mix_[channel][bus], surround ? send[bus][channel] : 0.0f);
        }
    }
    std::array<ax::System::ChannelMix, 4>& front = surround && param_.drc_main_to_second ? drc2_mix_ : drc_mix_;
    for (int channel = 0; channel < 2; ++channel)
    {
        ramp(front[channel][0], send[0][channel]);
    }
    if (!changed)
    {
        return;
    }

    system_->SetVoiceDeviceMix(*ax_voice_, ax::System::Device::kDrc, 0, drc_mix_);
    if (surround && param_.drc_main_to_second)
    {
        system_->SetVoiceDeviceMix(*ax_voice_, ax::System::Device::kDrc, 1, drc2_mix_);
    }
}

void AxVoice::UpdateMonoFilter(bool setup)
{
    // The one-pole low-pass. The whole filter is set when it's switched, and only its coefficients while it stays on.
    const uint16_t on = param_.lpf_on ? 1 : 0;
    if (setup || on != lpf_on_)
    {
        ax::Lpf lpf;
        lpf.on = on;
        if (on)
        {
            const auto [a0, b0] = system_->ComputeLpfCoefs(param_.lpf_freq);
            lpf.a0 = static_cast<int16_t>(a0);
            lpf.b0 = static_cast<int16_t>(b0);
        }
        system_->SetVoiceLpf(*ax_voice_, lpf);
        lpf_on_ = on;
        return;
    }

    if (on)
    {
        const auto [a0, b0] = system_->ComputeLpfCoefs(param_.lpf_freq);
        system_->SetVoiceLpfCoefs(*ax_voice_, static_cast<int16_t>(a0), static_cast<int16_t>(b0));
    }
}

void AxVoice::UpdateBiquadFilter(bool setup)
{
    // The biquad. The whole filter is set when it's switched (on is 2 here), and only its coefficients while it stays
    // on.
    const uint16_t on = param_.biquad_on ? 2 : 0;
    const std::array<int16_t, 5>& c = param_.biquad;
    if (setup || on != biquad_on_)
    {
        ax::Biquad biquad;
        biquad.on = on;
        if (on)
        {
            biquad.b0 = c[0];
            biquad.b1 = c[1];
            biquad.b2 = c[2];
            biquad.a1 = c[3];
            biquad.a2 = c[4];
        }
        system_->SetVoiceBiquad(*ax_voice_, biquad);
        biquad_on_ = on;
        return;
    }

    if (on)
    {
        system_->SetVoiceBiquadCoefs(*ax_voice_, c[0], c[1], c[2], c[3], c[4]);
    }
}

void AxVoice::SetupVoice(const WaveBuffer& buffer, const std::array<OutputMode, 2>& modes)
{
    SetupOffset(buffer);
    SetupAdpcm(buffer);
    SetupTvMix(modes[0]);
    SetupDrcMix(modes[1]);
    UpdateVe(true);
    UpdateSrc(true);
    UpdateMonoFilter(true);
    UpdateBiquadFilter(true);
}

void AxVoice::UpdateParams(const std::array<OutputMode, 2>& modes)
{
    UpdateVe(false);
    UpdateSrc(false);
    UpdateTvMix(modes[0]);
    UpdateDrcMix(modes[1]);
    UpdateMonoFilter(false);
    UpdateBiquadFilter(false);
}

void AxVoice::UpdateStatePlay(bool running, const std::array<OutputMode, 2>& modes)
{
    if (!running)
    {
        // A voice that has stopped finishes its buffer and starts the next one waiting.
        WaveBuffer* buffer = head_;
        if (!buffer)
        {
            return;
        }
        if (buffer->status == WaveBuffer::Status::kPlay)
        {
            head_ = buffer->next;
            if (tail_ == buffer)
            {
                tail_ = nullptr;
            }
            buffer->status = WaveBuffer::Status::kDone;
            buffer = head_;
            if (!buffer)
            {
                return;
            }
        }
        if (buffer->status != WaveBuffer::Status::kWait)
        {
            return;
        }
        SetupVoice(*buffer, modes);
        system_->SetVoiceState(*ax_voice_, ax::VoiceState::kRun);
        buffer->status = WaveBuffer::Status::kPlay;
        return;
    }

    // A voice plays with a buffer. The game's code relies on that.
    WaveBuffer* buffer = head_;
    if (!buffer)
    {
        UpdateParams(modes);
        return;
    }
    if (!next_in_loop_)
    {
        // A buffer queued behind the current one becomes its loop. The offset here is the buffer's start sample as it
        // stands. Elsewhere, the offset is turned into the format's units.
        if (WaveBuffer* next = buffer->next)
        {
            system_->SetVoiceLoopOffsetEx(*ax_voice_, next->sample_offset, next->address);
            system_->SetVoiceLoop(*ax_voice_, true);
            SetupNextWaveBuffer(*next);
            next_in_loop_ = true;
        }
        UpdateParams(modes);
        return;
    }

    // When the loop count moves, the voice has reached the next buffer. That buffer becomes the current one. A voice
    // that started with a next buffer sets up its loop here. That happens a frame after the voice's start.
    const uint32_t count = system_->GetVoiceLoopCount(*ax_voice_);
    if (count == loop_count_ && !started_with_next_)
    {
        UpdateParams(modes);
        return;
    }
    next_in_loop_ = false;
    if (!started_with_next_ && buffer->next)
    {
        buffer->status = WaveBuffer::Status::kDone;
        head_ = buffer->next;
        buffer = head_;
        buffer->status = WaveBuffer::Status::kPlay;
        system_->SetVoiceEndOffsetEx(*ax_voice_, OffsetOf(buffer->sample_length - 1), buffer->address);
    }
    started_with_next_ = false;

    // The buffer after it is the loop, or else the buffer loops on itself if it loops at all.
    if (WaveBuffer* next = buffer->next)
    {
        system_->SetVoiceLoopOffsetEx(*ax_voice_, OffsetOf(next->sample_offset), next->address);
        system_->SetVoiceLoop(*ax_voice_, true);
        SetupNextWaveBuffer(*next);
        next_in_loop_ = true;
    }
    else
    {
        system_->SetVoiceLoopOffsetEx(*ax_voice_, OffsetOf(0), buffer->address);
        system_->SetVoiceLoop(*ax_voice_, buffer->loop);
    }
    loop_count_ = count;
    UpdateParams(modes);
}

void AxVoice::UpdateStateStop(bool running)
{
    if (running)
    {
        system_->SetVoiceState(*ax_voice_, ax::VoiceState::kStop);
    }
}

void AxVoice::UpdatePlayPosition()
{
    // The samples played from the current buffer's start, past an ADPCM frame's header. The count comes from AX's
    // position at the start of the last frame, and goes no further than the buffer's end.
    WaveBuffer* buffer = head_;
    if (!buffer || buffer->status != WaveBuffer::Status::kPlay)
    {
        return;
    }

    uint32_t position = system_->GetVoiceOffsetsEx(*ax_voice_, buffer->address).current;
    const ax::Offsets offsets = system_->GetVoiceOffsets(*ax_voice_);
    if (position > offsets.end)
    {
        position = offsets.end;
        if (format_ == SampleFormat::kDspAdpcm && (position & 15) <= 1)
        {
            position -= (position & 15) + 1;
        }
    }
    if (format_ == SampleFormat::kDspAdpcm)
    {
        const uint32_t nibble = position & 15;
        position = (position >> 4) * 14 + (nibble >= 2 ? nibble - 2 : nibble);
    }
    play_position_ = position;
}

void AxVoice::UpdateState(const std::array<OutputMode, 2>& modes)
{
    const bool running = ax_voice_->state == ax::VoiceState::kRun;
    switch (state_)
    {
    case State::kPlay:
        UpdateStatePlay(running, modes);
        break;
    case State::kStop:
        UpdateStateStop(running);
        break;
    }
    UpdatePlayPosition();
}

AxVoices::AxVoices(ax::System& system, const ax::Memory& memory) : system_(&system)
{
    for (AxVoice& voice : voices_)
    {
        voice.system_ = system_;
        voice.memory_ = &memory;
        voice.owner_ = this;
    }
}

AxVoice* AxVoices::AllocVoice(uint32_t priority, AxVoice::DisposeCallback dispose)
{
    // AX's priority 17, above the 16 that nw::snd keeps its voices at. Priority 255 gets AX's highest. When AX takes a
    // voice away, the voice's AxVoice loses it (VoiceCallback, 0x022A2B38).
    const auto taken = [this](ax::Voice& voice, uint32_t, uint32_t)
    {
        voices_[voice.index].ax_voice_ = nullptr;
    };
    ax::Voice* ax_voice = system_->AcquireVoice(priority == 255 ? 31 : 17, taken, 0);
    if (!ax_voice)
    {
        return nullptr;
    }
    if (priority != 255)
    {
        system_->SetVoicePriority(*ax_voice, 16);
    }

    AxVoice& voice = voices_[ax_voice->index];
    voice.Finalize();
    voice.Initialize(ax_voice);
    voice.dispose_ = std::move(dispose);
    voice.priority_ = priority;
    AppendToList(voice);

    return &voice;
}

void AxVoices::Free(AxVoice& voice)
{
    if (ax::Voice* ax_voice = std::exchange(voice.ax_voice_, nullptr))
    {
        system_->FreeVoice(*ax_voice);
    }
    voice.Finalize();
}

void AxVoices::UpdateAllVoices(const std::array<OutputMode, 2>& modes)
{
    if (system_->UserIsProtected())
    {
        return;
    }

    for (AxVoice& voice : voices_)
    {
        if (voice.ax_voice_)
        {
            voice.UpdateState(modes);
        }
    }
}

void AxVoices::UpdatePriorityList()
{
    system_->UserBegin();
    for (AxVoice* voice : priority_list_)
    {
        if (voice->ax_voice_)
        {
            system_->SetVoicePriority(*voice->ax_voice_, 16);
        }
    }
    system_->UserEnd();
}

void AxVoices::AppendToList(AxVoice& voice)
{
    // After every voice of the same priority or lower.
    if (voice.in_list_)
    {
        return;
    }

    auto at = priority_list_.end();
    while (at != priority_list_.begin() && (*std::prev(at))->priority_ > voice.priority_)
    {
        --at;
    }
    priority_list_.insert(at, &voice);
    voice.in_list_ = true;
}

void AxVoices::EraseFromList(AxVoice& voice)
{
    if (voice.in_list_)
    {
        priority_list_.remove(&voice);
        voice.in_list_ = false;
    }
}

} // namespace csf::nwsnd
