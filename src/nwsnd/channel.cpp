// SPDX-License-Identifier: MIT

#include "nwsnd/channel.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace csf::nwsnd
{
namespace
{

// The game's 1/255 (0x100949F0), the float nearest to it.
const float kOneOver255 = std::bit_cast<float>(0x3B808081u);

// A frame count as bytes into a wave's samples (MultiVoice::FrameToByte, 0x022AA92C). DSP-ADPCM counts through its
// 8-byte frames of 14 samples. DspAdpcmFrameToNibbleAddress (0x022AA8FC) counts them as nibbles first.
uint32_t FrameToByte(uint32_t frames, SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return frames;
    case SampleFormat::kPcm16:
        return frames * 2;
    case SampleFormat::kDspAdpcm:
        {
            const uint32_t rest = frames % 14;
            const uint32_t nibble = frames / 14 * 16 + (rest != 0 ? rest + 2 : 0);
            return nibble >> 1;
        }
    }

    return 0;
}

} // namespace

// ----------------------------------------------------------------------------------------------------------- Channel

void Channel::InitParam(Callback owner)
{
    cached_ratio = 1.0f;
    pause = false;
    bend = 0.0f;
    tune = 1.0f;
    length = 0;
    key = 60;
    original_key = 60;
    biquad_value = 0.0f;
    user_pitch = 1.0f;
    track_volume = 1.0f;
    next_in_track = nullptr;
    release_priority_fix = false;
    loop_start_ = 0;
    callback = std::move(owner);
    cached_pitch = 0.0f;
    init_pan = 0.0f;
    lpf = 0.0f;
    start_offset_ = 0;
    original_loop_start_ = 0;
    biquad_type = 0;
    ignore_note_off = false;
    auto_sweep = true;
    loop_ = false;
    output_line = 1;
    for (OutputParam* param : {&tv, &drc[0], &drc[1]})
    {
        param->volume = 1.0f;
        param->mix_mode = 0;
        param->pan = 0.0f;
        param->span = 0.0f;
        param->main_send = 0.0f;
        param->fx_send = {};
    }
    silence_duration = 0;
    silence_counter = 0;
    silence_to = 255;
    sweep_pitch = 0.0f;
    silence_from = 255;
    sweep_counter = 0;
    sweep_length = 0;
    envelope.Initialize(CurveAdshr::kInitialDecibel);
    for (int i = 0; i < 4; ++i)
    {
        lfos[i].param = CurveLfoParam{};
        lfo_types[i] = 0;
    }
    pan_curve = 0;
    key_group = 0;
    pan_mode = 0;

    // GetNwInterpolationTypeFromHardwareManager (0x022B8DA0): the 4-tap filters, the sound system's default.
    interpolation_type = 0;
    velocity_volume = 1.0f;
    init_volume = 1.0f;
}

void Channel::Start(const WaveInfo& wave, int32_t note_length, uint32_t start_offset)
{
    length = note_length;
    for (CurveLfo& lfo : lfos)
    {
        lfo.Reset();
    }
    envelope.Reset(CurveAdshr::kInitialDecibel);
    sweep_counter = 0;
    voice->SetSampleFormat(wave.format);
    voice->SetSampleRate(wave.sample_rate);
    voice->SetInterpolationType(interpolation_type);
    AppendWaveBuffer(wave, start_offset);
    voice->Start();
    active = true;
}

void Channel::AppendWaveBuffer(const WaveInfo& wave, uint32_t start_offset)
{
    // The wave from its start (DSP-ADPCM from the frame it lies in) to its end, and for a wave that loops, its loop
    // after that, for ever.
    loop_ = wave.loop;
    loop_start_ = wave.loop_start;
    original_loop_start_ = wave.original_loop_start;
    if (start_offset != 0 && wave.format == SampleFormat::kDspAdpcm)
    {
        start_offset = start_offset / 14 * 14;
    }
    start_offset_ = start_offset;
    const uint32_t loop_start_bytes = FrameToByte(loop_start_, wave.format);

    for (int channel = 0; channel < voice->ChannelCount(); ++channel)
    {
        const WaveInfo::Channel& data = wave.channels[channel];
        uint32_t address = data.address;
        if (start_offset != 0)
        {
            address += FrameToByte(start_offset, wave.format);
        }
        if (wave.format == SampleFormat::kDspAdpcm)
        {
            // A start offset would have CalcOffsetAdpcmParam decode up to it. Notes always start at 0.
            contexts_[channel] = data.context;
            if (wave.loop)
            {
                loop_contexts_[channel] = data.loop_context;
            }
            voice->SetAdpcmParam(channel, data.coefs);
        }

        WaveBuffer& start = wave_buffers_[channel][0];
        start.address = address;
        start.loop = false;
        start.sample_length = wave.loop_end - start_offset;
        start.adpcm_context = wave.format == SampleFormat::kDspAdpcm ? &contexts_[channel] : nullptr;
        if (wave.loop)
        {
            WaveBuffer& loop = wave_buffers_[channel][1];
            loop.address = data.address + loop_start_bytes;
            loop.loop = true;
            loop.sample_length = wave.loop_end - loop_start_;
            loop.adpcm_context = wave.format == SampleFormat::kDspAdpcm ? &loop_contexts_[channel] : nullptr;
            voice->AppendWaveBuffer(channel, start, false);
            voice->AppendWaveBuffer(channel, loop, true);
        }
        else
        {
            voice->AppendWaveBuffer(channel, start, true);
        }
    }
}

float Channel::GetSweepValue() const
{
    if (sweep_pitch == 0.0f || sweep_counter >= sweep_length)
    {
        return 0.0f;
    }

    return (sweep_pitch * static_cast<float>(sweep_length - sweep_counter)) / static_cast<float>(sweep_length);
}

void Channel::Update(bool periodic, Random& random)
{
    if (!active)
    {
        return;
    }
    if (pause)
    {
        periodic = false;
    }

    // The silence ramp, then the volume from the velocity, the region and the track.
    uint32_t silence = silence_to;
    if (silence_counter < silence_duration)
    {
        ++silence_counter;
        if (silence_counter < silence_duration)
        {
            const int32_t change = (static_cast<int32_t>(silence_to) - static_cast<int32_t>(silence_from)) *
                                   static_cast<int32_t>(silence_counter) / static_cast<int32_t>(silence_duration);
            silence = static_cast<uint8_t>(silence_from + change);
        }
    }
    float volume = velocity_volume * velocity_volume;
    volume = volume * init_volume;
    volume = volume * track_volume;
    volume = volume * static_cast<float>(silence);
    volume = volume * kOneOver255;

    if (envelope.GetStatus() == CurveAdshr::Status::kRelease && envelope.GetValue() < CurveAdshr::kInitialDecibel)
    {
        Stop();
        return;
    }

    // The pitch: the key, the bend, the sweep and the pitch LFOs. The result is a ratio, kept for the next frame.
    float pitch = static_cast<float>(static_cast<int32_t>(key) - static_cast<int32_t>(original_key)) + bend;
    pitch = pitch + GetSweepValue();
    for (int i = 0; i < 4; ++i)
    {
        if (lfo_types[i] == 0)
        {
            pitch = pitch + lfos[i].GetValue(random);
        }
    }
    float ratio;
    if (pitch == cached_pitch)
    {
        ratio = cached_ratio * tune;
    }
    else
    {
        const float semitones = util::CalcPitchRatio(util::ToInt32(pitch * 256.0f));
        ratio = semitones * tune;
        cached_pitch = pitch;
        cached_ratio = semitones;
    }
    ratio = ratio * user_pitch;

    // Each device's output parameters, with the note's pan and surround pan, and the pan LFOs. The pan LFOs run once
    // for each device.
    OutputParam tv_param = tv;
    float pan = tv.pan + init_pan;
    for (int i = 0; i < 4; ++i)
    {
        if (lfo_types[i] == 2)
        {
            pan = pan + lfos[i].GetValue(random);
        }
    }
    tv_param.pan = pan;
    tv_param.span = tv.span + init_span;
    std::array<OutputParam, 2> drc_param = drc;
    for (OutputParam& param : drc_param)
    {
        float drc_pan = param.pan + init_pan;
        for (int i = 0; i < 4; ++i)
        {
            if (lfo_types[i] == 2)
            {
                drc_pan = drc_pan + lfos[i].GetValue(random);
            }
        }
        param.pan = drc_pan;
        param.span = param.span + init_span;
    }

    // A frame of 3 ms passes.
    if (periodic)
    {
        if (auto_sweep)
        {
            UpdateSweep(3);
        }
        for (CurveLfo& lfo : lfos)
        {
            lfo.Update(3);
        }
        envelope.Update(3);
    }

    const float lpf_freq = lpf + 1.0f;
    volume = volume * util::CalcVolumeRatio(envelope.GetValue());
    for (int i = 0; i < 4; ++i)
    {
        if (lfo_types[i] == 1)
        {
            volume = volume * util::CalcVolumeRatio(lfos[i].GetValue(random) * 6.0f);
        }
    }

    if (voice)
    {
        voice->SetVolume(volume);
        voice->SetPitch(ratio);
        voice->SetPanMode(pan_mode);
        voice->SetPanCurve(pan_curve);
        voice->SetLpfFreq(lpf_freq);
        voice->SetBiquadFilter(biquad_type, biquad_value);
        voice->SetOutputLine(output_line);
        voice->SetTvParam(tv_param);
        voice->SetDrcParam(0, drc_param[0]);
        voice->SetDrcParam(1, drc_param[1]);
    }
}

void Channel::Stop()
{
    if (!voice)
    {
        return;
    }
    voice->Stop();
    voice->Free();
    voice = nullptr;
    active = false;
    pause = false;
}

void Channel::Release()
{
    if (envelope.GetStatus() != CurveAdshr::Status::kRelease)
    {
        if (voice && !release_priority_fix)
        {
            voice->SetPriority(1);
        }
        envelope.SetStatus(CurveAdshr::Status::kRelease);
    }
    pause = false;
}

void Channel::NoteOff()
{
    if (!ignore_note_off)
    {
        Release();
    }
}

void Channel::CallChannelCallback(CallbackStatus status)
{
    if (callback)
    {
        const Callback owner = callback;
        owner(*this, status);
        callback = nullptr;
    }
}

void Channel::OnVoiceEvent(MultiVoice& dropped, MultiVoice::CallbackStatus status)
{
    switch (status)
    {
    case MultiVoice::CallbackStatus::kFinishWave:
        dropped.Free();
        CallChannelCallback(CallbackStatus::kFinish);
        break;
    case MultiVoice::CallbackStatus::kCancel:
        dropped.Free();
        CallChannelCallback(CallbackStatus::kCancel);
        break;
    case MultiVoice::CallbackStatus::kDropVoice:
    case MultiVoice::CallbackStatus::kDropDsp:
        CallChannelCallback(CallbackStatus::kDrop);
        break;
    default:
        CallChannelCallback(CallbackStatus::kFinish);
        break;
    }
    voice = nullptr;
    allocated = false;
    pause = false;
    active = false;
}

// ---------------------------------------------------------------------------------------------------- ChannelManager

ChannelManager::ChannelManager(MultiVoiceManager& voices, int count) : voices_(&voices), pool_(count)
{
    for (int i = 0; i < count; ++i)
    {
        free_.push_back(i);
    }
}

Channel* ChannelManager::AllocChannel(int channels, uint32_t priority, Channel::Callback callback)
{
    // A channel from the pool (ChannelManager::Alloc, 0x022B8A14), at the end of the list, then its MultiVoice.
    if (free_.empty())
    {
        return nullptr;
    }
    const int index = free_.back();
    free_.pop_back();
    pool_[index] = std::make_unique<Channel>();
    Channel* channel = pool_[index].get();
    active_.push_back(channel);
    channel->allocated = true;

    MultiVoice* voice =
        voices_->AllocVoice(channels, priority, [channel](MultiVoice& dropped, MultiVoice::CallbackStatus status)
                            { channel->OnVoiceEvent(dropped, status); });
    if (!voice)
    {
        Free(*channel);
        return nullptr;
    }
    channel->voice = voice;
    channel->InitParam(std::move(callback));

    return channel;
}

void ChannelManager::UpdateAllChannel(Random& random)
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        Channel& channel = **it++;
        channel.Update(true, random);
        if (!channel.active)
        {
            channel.CallChannelCallback(Channel::CallbackStatus::kStopped);
            Free(channel);
        }
    }
}

int ChannelManager::Index(const Channel& channel) const
{
    for (std::size_t i = 0; i < pool_.size(); ++i)
    {
        if (pool_[i].get() == &channel)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

bool ChannelManager::AnyChanging() const
{
    const auto changing = [](const Channel* channel)
    {
        return channel->envelope.GetStatus() != CurveAdshr::Status::kSustain;
    };

    return std::any_of(active_.begin(), active_.end(), changing);
}

void ChannelManager::Free(Channel& channel)
{
    const int index = Index(channel);
    if (index < 0)
    {
        return;
    }

    active_.remove(&channel);
    free_.push_back(index);
    pool_[index].reset();
}

} // namespace csf::nwsnd
