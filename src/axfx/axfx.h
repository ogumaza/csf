// SPDX-License-Identifier: MIT

// The Wii U system's effects for AX's aux buses (AXFX in the system library snduser2), modelled on the library's code.
// A game registers an effect's callback for a bus. In each AX frame, the callback takes the bus's samples as 32-bit
// integers in 16-bit scale: AXGetInputSamplesPerFrame's count (96 for the 32 kHz renderer, 144 for the 48 kHz one) in
// each of three channels, left, right and surround. The callback replaces them with the effect's output. AX mixes the
// output back into the device's main bus two frames later (see ax::System).
//
// Each model follows the library's functions operation for operation. The floating-point effects compute in single
// precision. The library's code fuses some multiplies and adds into one instruction with one rounding (fmadds and
// fnmsubs). The models fuse the same ones (Fmadds and NegatedFms in util.h) and keep every other multiply and add
// apart. The build turns off the compiler's contraction of the rest (xsflib_project). Comments give the addresses in
// snduser2 of version 15702 of the system.
//
// Each effect's structure in the library holds the game's parameters, two pointers to other buses (busIn and busOut)
// and the effect's state. A model holds the same state. Its Process takes the buffers that the structure's busIn and
// busOut would point to.

#pragma once

#include <array>
#include <cstdint>

namespace csf::axfx
{

// The channels of an aux bus as the effects take them (AXFX_BUFFERUPDATE and AXFX_BUS), in that order.
enum Channel
{
    kLeft,
    kRight,
    kSurround,
    kChannelCount,
};

// A frame's samples on each channel of a bus, or nullptr in each for no bus.
using Buffers = std::array<int32_t*, kChannelCount>;

// What the effects ask AX: the renderer's rate (AXGetInputSamplesPerSec), when they set up, and a frame's samples
// (AXGetInputSamplesPerFrame), in each callback.
constexpr uint32_t kRate32k = 32000;
constexpr uint32_t kRate48k = 48000;

} // namespace csf::axfx
