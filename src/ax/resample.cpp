// SPDX-License-Identifier: MIT

#include "ax/resample.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/dsp/accumulator.h"
#include "ntdlib/dsp/coef_rom.h"

namespace csf::ax
{
namespace
{

// The microcode reads each result from the accumulator: its middle word saturated to 16 bits (MiddleWord).
using ntdlib::dsp::MiddleWord;

// Linear interpolation between the two oldest samples, with weights in 0.16.
int16_t Linear(const std::array<int16_t, 4>& last, uint32_t frac)
{
    return MiddleWord(int64_t{last[0]} * (0x10000 - frac) + int64_t{last[1]} * frac);
}

// The 4-tap filter `filter`. The coefficients for the position's top 7 bits multiply the four samples, oldest first.
// Each product is doubled. The coefficients are in the DSP's coefficient ROM: four for each of 128 phases, for the 8,
// 12 and 16 kHz filters in turn. No file holds the Wii U's ROM. kCoefficientRom is the GameCube's and Wii's, the same
// in every console. The Wii U's DSP is the Wii's DSP, extended. The Wii U's AX microcode runs the Wii's resampler
// unchanged, and reads the filters at 0x3000, 0x3200 and 0x3400, where the Wii's reads them at 0x1000, 0x1200 and
// 0x1400. Wii games' microcode reads the Wii's table, and the console runs Wii games. So the Wii's values are taken to
// be the Wii U's.
int16_t FourTap(uint16_t filter, const std::array<int16_t, 4>& last, uint32_t frac)
{
    const std::size_t first = std::size_t{filter} * 512 + ((frac >> 9) & 127) * 4;
    int64_t sum = 0;
    for (std::size_t i = 0; i < last.size(); ++i)
    {
        const auto coefficient = static_cast<int16_t>(ntdlib::dsp::kCoefficientRom[first + i]);
        sum += 2 * int64_t{coefficient} * last[i];
    }

    return MiddleWord(sum);
}

} // namespace

int16_t Resample(SrcMode mode, uint16_t filter, const std::array<int16_t, 4>& last, uint32_t frac)
{
    return mode == SrcMode::kFourTap ? FourTap(filter, last, frac) : Linear(last, frac);
}

} // namespace csf::ax
