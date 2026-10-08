// SPDX-License-Identifier: MIT

#include "ax/resample.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ax/four_tap.h"

namespace csf::ax
{
namespace
{

// The middle word of a sum, saturated to 16 bits when the sum is outside 32 bits' range. That's how the microcode reads
// its result from the accumulator.
int16_t MiddleWord(int64_t sum)
{
    if (sum > std::numeric_limits<int32_t>::max())
    {
        return 0x7FFF;
    }
    if (sum < std::numeric_limits<int32_t>::min())
    {
        return -0x8000;
    }

    return static_cast<int16_t>(sum >> 16);
}

// Linear interpolation between the two oldest samples, with weights in 0.16.
int16_t Linear(const std::array<int16_t, 4>& last, uint32_t frac)
{
    return MiddleWord(int64_t{last[0]} * (0x10000 - frac) + int64_t{last[1]} * frac);
}

// The 4-tap filter `filter`. The coefficients for the position's top 7 bits multiply the four samples, oldest first.
// Each product is doubled.
int16_t FourTap(uint16_t filter, const std::array<int16_t, 4>& last, uint32_t frac)
{
    const auto& coefficients = kFourTap[filter][(frac >> 9) & 127];
    int64_t sum = 0;
    for (std::size_t i = 0; i < last.size(); ++i)
    {
        sum += 2 * int64_t{coefficients[i]} * last[i];
    }

    return MiddleWord(sum);
}

} // namespace

int16_t Resample(SrcMode mode, uint16_t filter, const std::array<int16_t, 4>& last, uint32_t frac)
{
    return mode == SrcMode::kFourTap ? FourTap(filter, last, frac) : Linear(last, frac);
}

} // namespace csf::ax
