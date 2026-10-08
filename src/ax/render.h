// SPDX-License-Identifier: MIT

// AX's renderer, the part that the DSP does on the console. The system's sound library uploads a microcode to the DSP
// for it. CSF follows the microcode's arithmetic step by step. The microcode is sndcore2's 32 kHz image in version
// 15702 of the OS (0x1240 bytes at 0x10007D20). The addresses in these files are in that image. All of it is integer
// arithmetic. A voice's samples stay 16-bit through the envelope and the filters. The buses hold 32-bit sums.
//
// Each frame, the DSP renders the running voices into the devices' buses (0x01E5). The accelerator fetches and decodes
// a voice's samples. The microcode converts them to the renderer's rate (see resample.h) and applies the volume
// envelope, the low-pass filter and the biquad filter, in that order. The microcode then adds the samples into each bus
// and channel that the voice's masks name. After the voices, the DSP adds each aux bus's return into the device's main
// bus (0x042A). Last, the DSP scales each device's main bus by the device's volume (0x0517). The envelope and a mix's
// volume start at their old values with the first sample. The aux returns and the devices' volumes reach their new
// values with the last sample.
//
// sndcore2's 48 kHz renderer has a microcode of its own. That microcode renders a frame in two halves. CSF renders a
// 48 kHz frame with the 32 kHz microcode's arithmetic. The aux returns and the devices' volumes ramp by the 48 kHz
// microcode's step.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ax/memory.h"
#include "ax/pb.h"

namespace csf::ax
{

// One frame of the devices' buses as the DSP keeps them: 32-bit samples in 16-bit scale, indexed
// [bus][channel][sample].
struct Buses
{
    using Channel = std::array<int32_t, kMaxSamplesPerFrame>;

    std::array<std::array<Channel, kTvChannels>, kBusCount> tv;
    std::array<std::array<std::array<Channel, kDrcChannels>, kBusCount>, kDrcCount> drc;
};

// Renders `samples` samples of the voice whose renderer's copy is `pb` into `buses`. `pb` is updated as the voice
// plays. A voice that isn't running at the start of the frame is skipped. Its block keeps its values.
void RenderVoice(Pb& pb, const Memory& memory, int samples, Buses& buses);

// Adds an aux bus's return into one channel of the device's main bus. The volume goes from `from` to `to` across the
// frame (0x04C5).
void ReturnAux(std::span<const int32_t> returned, uint16_t from, uint16_t to, std::span<int32_t> main);

// One channel of a device's output, in 24-bit scale: the main bus at the device's volume (0x0517). The volume goes from
// `from` to `to` across the frame.
void OutputBus(std::span<const int32_t> bus, uint16_t from, uint16_t to, std::span<int32_t> out);

} // namespace csf::ax
