// SPDX-License-Identifier: MIT

// What the effects' models share: the library's conversions between integers and floats, and the step of a line's
// position.

#pragma once

#include <cmath>
#include <cstdint>

#include "ntdlib/fused.h"

namespace csf::axfx
{

// The library's conversion of a float to an unsigned integer: fctiwz below 2^31, and from 2^31 fctiwz of the value
// less 2^31, with 2^31 added back. Each place in the library tests the value in a way that takes a NaN the second way.
// fctiwz gives 0x80000000 for it, and the NaN comes out as 0.
inline uint32_t ToUnsigned(float value)
{
    if (value < 2147483648.0f)
    {
        return static_cast<uint32_t>(ntdlib::PowerPcToInt32(value));
    }

    return static_cast<uint32_t>(ntdlib::PowerPcToInt32(value - 2147483648.0f)) + 0x80000000u;
}

// The library's conversions of an integer to a float: through a double (exact), then rounded to single precision.
inline float ToFloat(uint32_t value)
{
    return static_cast<float>(value);
}

inline float ToFloat(int32_t value)
{
    return static_cast<float>(value);
}

// The PowerPC's fmadds: a * c + b, rounded once to single precision. Game mode's interpreter rounds it to double
// precision first, then to single. The two differ only where the result in double precision lies halfway between two
// floats. Which of the two the Wii U's CPU does isn't known.
inline float Fmadds(float a, float c, float b)
{
    return std::fma(a, c, b);
}

// The PowerPC's fnmsubs: -(a * c - b), rounded once, then negated. An exact zero comes out as -0.
inline float NegatedFms(float a, float c, float b)
{
    return ntdlib::NegatedFma(a, c, -b);
}

// Moves a line's position on by a sample. The position wraps to 0 at the line's length.
inline void Step(uint32_t& position, uint32_t length)
{
    position++;
    if (position >= length)
    {
        position = 0;
    }
}

// The renderer's samples per millisecond: the rate divided by 1,000 (mulhwu and srwi).
inline uint32_t SamplesPerMs(uint32_t rate)
{
    return rate / 1000;
}

} // namespace csf::axfx
