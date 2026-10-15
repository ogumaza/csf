// SPDX-License-Identifier: MIT

#include "axfx/math.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include "ntdlib/fused.h"

#include "axfx/util.h"

namespace csf::axfx
{
namespace
{

constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr float kLn2 = 0.6931471824645996f; // 0x3f317218

uint32_t Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

float FromBits(uint32_t bits)
{
    return std::bit_cast<float>(bits);
}

// Whether a float is an infinity by the library's test: the bits less the sign are 0x7f800000.
bool IsInfinity(float value)
{
    return (Bits(value) << 1) == 0xff000000u;
}

// 2 to the power of `exponent` times `value`. The library scales its results by adding `exponent` to the exponent field
// of the bits.
float Scale(float value, int32_t exponent)
{
    return FromBits(Bits(value) + (static_cast<uint32_t>(exponent) << 23));
}

// The polynomial for 2^f on 0 to 1 that Exp2 and Exp share, without its last two terms (0x02023b38 and 0x02023cfc).
float Exp2Polynomial(float f)
{
    float sum = Fmadds(0.0018775766948238015f, f, 0.00898933969438076f);
    sum = Fmadds(sum, f, 0.05582631751894951f);

    return Fmadds(sum, f, 0.24015361070632935f);
}

} // namespace

float Pow(float x, float y)
{
    if (!(x > 2.0f))
    {
        if (x == 1.0f || y == 0.0f)
        {
            return 1.0f;
        }

        if (x == 2.0f)
        {
            return Exp2(y);
        }

        if (x == 0.5f)
        {
            return Exp2(-y);
        }

        if (x == 0.0f)
        {
            return y > 0.0f ? 0.0f : kInfinity;
        }
    }

    // A negative power is the positive power of the reciprocal.
    if (y < 0.0f)
    {
        y = -y;
        x = 1.0f / x;
    }

    // A whole power up to 2^31 multiplies the squares of x. The library compares y with the whole number in single
    // precision (0x020244e4). A power of 2^31 then counts as 2^31 - 1.
    if (!(y > 2147483648.0f))
    {
        int32_t whole = ntdlib::PowerPcToInt32(y);
        if (y == static_cast<float>(whole))
        {
            float result = 1.0f;
            while (whole > 0)
            {
                if ((whole & 1) != 0)
                {
                    result = result * x;
                }

                x = x * x;
                whole >>= 1;
            }

            return result;
        }
    }

    if (x < 0.0f)
    {
        return 0.0f;
    }

    if (y > 1e20f)
    {
        if (IsInfinity(y) && (static_cast<int32_t>(Bits(y)) >> 30) != 0)
        {
            return x > 1.0f ? y : 0.0f;
        }

        if (x > 1.0f)
        {
            return kInfinity;
        }
    }

    return Exp(y * Log(x));
}

float Log(float x)
{
    if (!(x > 0.0f))
    {
        return x == 0.0f ? -kInfinity : std::numeric_limits<float>::quiet_NaN();
    }

    const uint32_t bits = Bits(x);
    if (bits >= 0x7f800000u)
    {
        return x;
    }

    // x = m * 2^e with m from sqrt(1/2) to sqrt(2).
    int32_t exponent = static_cast<int32_t>(bits >> 23) - 126;
    uint32_t mantissa = (bits & 0x7fffffu) | 0x3f000000u;
    if (mantissa < 0x3f3504f3u)
    {
        exponent--;
        mantissa += 0x00800000u;
    }

    const float m = FromBits(mantissa);
    const float s = (m - 1.0f) / (m + 1.0f);
    const float z = s * s;
    const float numerator = Fmadds(0.8955405950546265f, z, -3.31355619430542f) * s;
    const float fraction = numerator / (z + -1.6567779779434204f);

    return Fmadds(static_cast<float>(exponent), kLn2, fraction);
}

float Exp(float x)
{
    int32_t exponent = 0;
    float f = x;
    float sum = 0.0f;
    float linear = 0.0f; // the polynomial's coefficient of f
    if (!(-kLn2 > x) && !(x > kLn2))
    {
        // e^x on -ln 2 to ln 2, from e^f on 0 to ln 2.
        if (x == 0.0f)
        {
            return 1.0f;
        }

        if (x < 0.0f)
        {
            f = x + kLn2;
            exponent = -1;
        }

        sum = Fmadds(0.011734656058251858f, f, 0.038942690938711166f);
        sum = Fmadds(sum, f, 0.16763418912887573f);
        sum = Fmadds(sum, f, 0.49984827637672424f);
        linear = 1.000008463859558f;
    }
    else
    {
        if (!(x < 88.72283935546875f))
        {
            return kInfinity;
        }

        if (x < -87.3365478515625f)
        {
            return 0.0f;
        }

        // e^x as 2^(x log2 e), from 2^f on 0 to 1.
        f = x * 1.4426950216293335f;
        exponent = ntdlib::PowerPcToInt32(f);
        f = f - static_cast<float>(exponent);
        if (f < 0.0f)
        {
            f = f + 1.0f;
            exponent--;
        }

        sum = Exp2Polynomial(f);
        linear = 0.6931530833244324f;
    }

    sum = Fmadds(sum, f, linear);
    sum = Fmadds(sum, f, 0.9999999403953552f);

    return Scale(sum, exponent);
}

float Exp2(float x)
{
    if (!(x < 128.0f))
    {
        return kInfinity;
    }

    int32_t exponent = ntdlib::PowerPcToInt32(x);
    if (!(x > 0.0f))
    {
        if (x == 0.0f)
        {
            return 1.0f;
        }

        if (!(x >= -126.0f))
        {
            // Below 2^-126 the library gives the subnormal 2^exponent, or 0 below 2^-149.
            const int32_t shift = static_cast<int32_t>(static_cast<uint32_t>(exponent) + 149u);
            return shift >= 0 ? FromBits(1u << shift) : 0.0f;
        }
    }

    float f = x - static_cast<float>(exponent);
    if (f == 0.0f)
    {
        return FromBits(static_cast<uint32_t>(exponent + 127) << 23);
    }

    if (f < 0.0f)
    {
        f = f + 1.0f;
        exponent--;
    }

    float sum = Exp2Polynomial(f);
    sum = Fmadds(sum, f, 0.6931530833244324f);
    sum = Fmadds(sum, f, 0.9999999403953552f);

    return Scale(sum, exponent);
}

} // namespace csf::axfx
