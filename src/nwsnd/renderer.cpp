// SPDX-License-Identifier: MIT

#include "nwsnd/renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"

namespace csf::nwsnd
{
namespace
{

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

AxRenderer::AxRenderer(AxVoices& ax, const HardwareSettings& hardware, int voice_count)
    : ax_(&ax), hardware_(&hardware), buffers_(static_cast<std::size_t>(voice_count))
{
}

int AxRenderer::Acquire(uint32_t priority, Voice& voice)
{
    const auto dispose = [&voice](AxVoice&)
    {
        voice.renderer = -1;
    };
    AxVoice* ax = ax_->AllocVoice(priority, dispose);
    return ax ? ax_->IndexOf(*ax) : -1;
}

void AxRenderer::Free(int renderer)
{
    ax_->Free(ax_->At(renderer));
}

bool AxRenderer::IsAvailable(int renderer) const
{
    return ax_->At(renderer).IsAvailable();
}

void AxRenderer::SetPriority(int renderer, uint32_t priority)
{
    ax_->At(renderer).SetPriority(priority);
}

void AxRenderer::SetState(int renderer, const Voice& voice)
{
    AxVoice& ax = ax_->At(renderer);
    const auto state = static_cast<AxVoice::State>(voice.state);
    ax.SetState(state);
    if (state == AxVoice::State::kPlay)
    {
        ax.SetSampleRate(voice.sample_rate);
        ax.SetSampleFormat(voice.format);
        ax.SetAdpcmCoefs(voice.coefs);
    }
    else if (state == AxVoice::State::kStop)
    {
        ax.FreeAllWaveBuffer();
    }
}

void AxRenderer::SetParam(int renderer, const VoiceParam& param)
{
    ax_->At(renderer).Param() = param;
}

void AxRenderer::AppendWave(VoiceId place, std::span<Voice> voices, const WaveInfo& wave)
{
    // Notes always start at the wave's start. A start offset would have CalcOffsetAdpcmParam decode up to it.
    Buffers& buffers = buffers_[static_cast<std::size_t>(place)];
    buffers.last = nullptr;
    const bool adpcm = wave.format == SampleFormat::kDspAdpcm;
    const uint32_t loop_start_bytes = FrameToByte(wave.loop_start, wave.format);
    for (std::size_t channel = 0; channel < voices.size(); ++channel)
    {
        const WaveInfo::Channel& data = wave.channels[channel];
        if (adpcm)
        {
            buffers.contexts[channel] = data.context;
            if (wave.loop)
            {
                buffers.loop_contexts[channel] = data.loop_context;
            }

            // MultiVoice::SetAdpcmParam (0x02298FD0).
            voices[channel].coefs = data.coefs;
        }

        WaveBuffer& start = buffers.wave_buffers[channel][0];
        start.address = data.address;
        start.loop = false;
        start.sample_length = wave.loop_end;
        start.adpcm_context = adpcm ? &buffers.contexts[channel] : nullptr;

        if (wave.loop)
        {
            WaveBuffer& loop = buffers.wave_buffers[channel][1];
            loop.address = data.address + loop_start_bytes;
            loop.loop = true;
            loop.sample_length = wave.loop_end - wave.loop_start;
            loop.adpcm_context = adpcm ? &buffers.loop_contexts[channel] : nullptr;
            AppendWaveBuffer(buffers, voices[channel], start, false);
            AppendWaveBuffer(buffers, voices[channel], loop, true);
        }
        else
        {
            AppendWaveBuffer(buffers, voices[channel], start, true);
        }
    }
}

void AxRenderer::AppendWaveBuffer(Buffers& buffers, const Voice& voice, WaveBuffer& buffer, bool last)
{
    // Voice::AppendWaveBuffer (0x022A5A8C) queues the buffer on AX's voice. Nothing is queued if the Voice no longer
    // has an AX voice.
    if (voice.renderer >= 0)
    {
        ax_->At(voice.renderer).AppendWaveBuffer(buffer);
    }

    if (last)
    {
        buffers.last = &buffer;
    }
}

bool AxRenderer::IsPlayFinished(VoiceId place) const
{
    const WaveBuffer* last = buffers_[static_cast<std::size_t>(place)].last;
    if (!last)
    {
        return false;
    }

    return last->status != WaveBuffer::Status::kWait && last->status != WaveBuffer::Status::kPlay;
}

void AxRenderer::CalcMix(MultiVoice& voice) const
{
    const Outputs& outputs = voice.GetOutputs();
    const std::span<Voice> voices = voice.Voices();
    for (std::size_t i = 0; i < voices.size(); ++i)
    {
        const int channel = static_cast<int>(i);
        voices[i].param.tv = CalcMixImpl(voice, 0, outputs.tv, CalcPreMixVolume(voice, outputs.tv, channel, 0));

        // Each GamePad. The voices' parameters have no room for the second GamePad here: AxVoice plays the first
        // GamePad's mix. The Wii Remotes' mix (CalcRemoteMix) isn't modelled.
        voices[i].param.drc =
            CalcMixImpl(voice, 5, outputs.drc[0], CalcPreMixVolume(voice, outputs.drc[0], channel, 1));
    }
}

void AxRenderer::CalcLpf(MultiVoice& voice) const
{
    const uint16_t frequency = util::CalcLpfFreq(voice.LpfFreq());
    for (Voice& each : voice.Voices())
    {
        // Voice::SetMonoFilter (0x0229CEDC): turning the filter off leaves its frequency.
        if (frequency >= 16000)
        {
            each.param.lpf_on = false;
        }
        else
        {
            each.param.lpf_on = true;
            each.param.lpf_freq = frequency;
        }
    }
}

void AxRenderer::CalcBiquadFilter(MultiVoice& voice) const
{
    // Voice::SetBiquadFilter (0x0229CF84): turning the filter off leaves its coefficients. A value above 0 takes the
    // preset's coefficients (ntdlib's BiquadPresetCoefficients). Types 1 to 5 are the presets. A game registers any
    // other types. Here those types turn the filter off.
    const int16_t* coefs = voice.BiquadValue() > 0.0f
                               ? ntdlib::nwsnd::BiquadPresetCoefficients<ntdlib::nwsnd::CafePolicy>(voice.BiquadType(),
                                                                                                    voice.BiquadValue())
                               : nullptr;
    for (Voice& each : voice.Voices())
    {
        each.param.biquad_on = coefs != nullptr;
        if (coefs != nullptr)
        {
            std::copy_n(coefs, 5, each.param.biquad.begin());
        }
    }
}

AxRenderer::PreMixVolume AxRenderer::CalcPreMixVolume(const MultiVoice& voice, const OutputParam& param, int channel,
                                                      int device) const
{
    const HardwareSettings& hardware = *hardware_;
    const OutputMode mode = hardware.output_mode[device];
    const OutputMode end_user = hardware.end_user_output_mode[device];

    float front_left = 0.0f, front_right = 0.0f, rear_left = 0.0f, rear_right = 0.0f;
    float front_center = 0.0f, lfe = 0.0f;
    if (param.mix_mode != 0)
    {
        const std::array<float, 6>& mix = param.mix_parameter[channel];
        front_left = mix[0];
        front_right = mix[1];
        rear_left = mix[2];
        rear_right = mix[3];
        front_center = mix[4];
        lfe = mix[5];
    }
    else
    {
        const PanInfo info = ntdlib::nwsnd::PanInfoOf(voice.PanCurve());
        float left = 0.0f, right = 0.0f;
        float front = 0.0f, rear = 0.0f;
        if (mode == OutputMode::kMono)
        {
            left = util::CalcPanRatio(0.0f, info, OutputMode::kMono);
            right = left;
        }
        else if (mode == OutputMode::kStereo || mode == OutputMode::kSurround)
        {
            if (voice.ChannelCount() > 1 && voice.PanMode() == 1)
            {
                // Balance: each channel keeps its side.
                if (channel == 0)
                {
                    left = util::CalcPanRatio(param.pan, info, mode);
                }
                else if (channel == 1)
                {
                    right = util::CalcPanRatio(-param.pan, info, mode);
                }
            }
            else
            {
                // Dual: a stereo wave's channels start at the sides and move with the pan.
                float pan = param.pan;
                if (voice.ChannelCount() == 2)
                {
                    if (channel == 0)
                    {
                        pan = pan - 1.0f;
                    }
                    else if (channel == 1)
                    {
                        pan = pan + 1.0f;
                    }
                }
                left = util::CalcPanRatio(pan, info, mode);
                right = util::CalcPanRatio(-pan, info, mode);
            }
        }

        // Nothing here uses Dolby Pro Logic II output. It gets no gains at all: the game's code leaves them 0.
        if (mode == OutputMode::kSurround)
        {
            front = util::CalcSurroundPanRatio(param.span, info);
            rear = util::CalcSurroundPanRatio(2.0f - param.span, info);
        }
        else if (mode != OutputMode::kDpl2)
        {
            front = util::CalcSurroundPanRatio(0.0f, info);
            rear = util::CalcSurroundPanRatio(2.0f, info);
        }
        rear_left = rear * left;
        front_right = front * right;
        front_left = front * left;
        rear_right = rear * right;
    }

    // The device's mode as the system reports it: a mono device gets the fronts' centre, a stereo one folds a surround
    // mix's rear channels into the front.
    if (end_user == OutputMode::kMono)
    {
        if (mode == OutputMode::kStereo)
        {
            const float mono = (front_left + front_right) * 0.5f;
            return {mono, mono, rear_left, rear_right, front_center, lfe};
        }
        if (mode == OutputMode::kSurround)
        {
            const float mono = ((front_left + rear_left) + (front_right + rear_right)) * 0.5f;
            return {mono, mono, 0.0f, 0.0f, front_center, lfe};
        }
    }
    else if (end_user == OutputMode::kStereo && mode == OutputMode::kSurround)
    {
        return {front_left + rear_left, front_right + rear_right, 0.0f, 0.0f, front_center, lfe};
    }

    return {front_left, front_right, rear_left, rear_right, front_center, lfe};
}

OutputMix AxRenderer::CalcMixImpl(const MultiVoice& voice, int device_bit, const OutputParam& param,
                                  const PreMixVolume& gains) const
{
    const HardwareSettings& hardware = *hardware_;
    uint32_t devices = 0;
    for (int line = 0; line < 32; ++line)
    {
        if ((voice.OutputLine() >> line) & 1)
        {
            devices |= hardware.output_line_devices[line];
        }
    }

    float main = 0.0f;
    std::array<float, 3> fx{};
    if (devices & (1u << device_bit))
    {
        const float send = param.main_send + 1.0f;
        main = send > 1.0f ? 1.0f : SequencePolicy::AtLeastZero(send);
        for (int bus = 0; bus < 3; ++bus)
        {
            fx[bus] = param.fx_send[bus] > 1.0f ? 1.0f : SequencePolicy::AtLeastZero(param.fx_send[bus]);
        }
    }

    OutputMix mix{};
    for (int channel = 0; channel < 6; ++channel)
    {
        const float gain = gains[channel] * param.volume;
        mix[0][channel] = main * gain;
        for (int bus = 0; bus < 3; ++bus)
        {
            mix[1 + bus][channel] = fx[bus] * gain;
        }
    }

    return mix;
}

} // namespace csf::nwsnd
