// SPDX-License-Identifier: MIT

// nw::snd's MultiVoices on AX in NintendoWare for Cafe: the renderer that ntdlib's model of the MultiVoices plays on
// (see ntdlib/nwsnd/voice.h). The renderer gives each wave channel's voice an AxVoice, lays a wave's buffers out in
// AX's memory, and works out each voice's mix and filters for the TV and the GamePad. Each function's comment gives the
// address of the routine it follows in Animal Crossing: amiibo Festival's executable.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "nwsnd/ax_voice.h"
#include "nwsnd/sequence.h"
#include "nwsnd/voice.h"

namespace csf::nwsnd
{

class AxRenderer
{
public:
    using Voice = MultiVoice::Voice;

    // `voice_count` is nw::snd's voice count: the MultiVoices whose buffers the renderer holds.
    AxRenderer(AxVoices& ax, const HardwareSettings& hardware, int voice_count);

    AxRenderer(const AxRenderer&) = delete;
    AxRenderer& operator=(const AxRenderer&) = delete;

    // An AxVoice for `voice` at `priority` (Voice::AllocVoice, 0x022A5970), or -1. When AX takes the voice away or it's
    // freed, `voice` loses it (AxVoiceDisposeCallback, 0x022A5908).
    int Acquire(uint32_t priority, Voice& voice);

    void Free(int renderer);
    bool IsAvailable(int renderer) const;
    void SetPriority(int renderer, uint32_t priority);

    // Starting gives AX's voice the wave's format, rate and coefficients. Stopping drops its buffers (Voice::SetState,
    // 0x0229897C).
    void SetState(int renderer, const Voice& voice);

    // Voice::UpdateParam (0x0229D24C).
    void SetParam(int renderer, const VoiceParam& param);

    // Queues each wave channel's buffers on its voice: the wave from its start to its end, and for a wave that loops,
    // the loop after that, for ever (Channel::AppendWaveBuffer, 0x022B96FC). The game's Channel holds the buffers (at
    // +0x240) and the decoder's states that they start from (at +0x2C0 and +0x2CC). Here the renderer holds them, for
    // the MultiVoice at `place`.
    void AppendWave(VoiceId place, std::span<Voice> voices, const WaveInfo& wave);

    // Whether the last buffer of the MultiVoice at `place` has played (part of IsPlayFinished, 0x0229BC2C).
    bool IsPlayFinished(VoiceId place) const;

    // The output volume (GetOutputVolume, 0x0229BF58).
    float OutputVolume() const
    {
        return hardware_->output_volume;
    }

    void CalcMix(MultiVoice& voice) const;          // 0x0229CCA8
    void CalcLpf(MultiVoice& voice) const;          // 0x0229CEF0
    void CalcBiquadFilter(MultiVoice& voice) const; // 0x0229CFBC

private:
    // A MultiVoice's buffers: each wave channel's start and loop, the decoder's states that they start from, and the
    // buffer whose end ends the voice (MultiVoice +0x37C).
    struct Buffers
    {
        std::array<std::array<WaveBuffer, 2>, 2> wave_buffers;
        std::array<AdpcmContext, 2> contexts;
        std::array<AdpcmContext, 2> loop_contexts;
        WaveBuffer* last = nullptr;
    };

    // A channel's gains before its device's volume and sends (PreMixVolume): front left and right, rear left and right,
    // front centre and LFE.
    using PreMixVolume = std::array<float, 6>;

    // Queues a buffer on a channel's voice. The last buffer queued with `last` is the one whose end ends the voice
    // (MultiVoice::AppendWaveBuffer, 0x022AAAF4).
    void AppendWaveBuffer(Buffers& buffers, const Voice& voice, WaveBuffer& buffer, bool last);

    // A channel's gains for a device (0 the TV, 1 a GamePad) by its pan and surround pan, or its mix parameters
    // (0x0229C2F4).
    PreMixVolume CalcPreMixVolume(const MultiVoice& voice, const OutputParam& param, int channel, int device) const;

    // A device's sends: its volume, sends and the gains, or nothing when the voice's output line doesn't reach it
    // (CalcMixImpl, 0x0229C8F8). `device_bit` is the device's bit in an output line.
    OutputMix CalcMixImpl(const MultiVoice& voice, int device_bit, const OutputParam& param,
                          const PreMixVolume& gains) const;

    AxVoices* ax_;
    const HardwareSettings* hardware_;
    std::vector<Buffers> buffers_;
};

} // namespace csf::nwsnd
