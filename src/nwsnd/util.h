// SPDX-License-Identifier: MIT

// nw::snd's arithmetic helpers (nw::snd::internal::Util) in NintendoWare for Cafe. Each function's comment gives the
// address of the routine it follows in Animal Crossing: amiibo Festival's executable.

#pragma once

#include <cstdint>

namespace csf::nwsnd
{

// nw::snd's output modes (OutputMode).
enum class OutputMode : uint8_t
{
    kMono = 0,
    kStereo = 1,
    kSurround = 2,
    kDpl2 = 3,
};

// How a pan becomes a gain (Util::PanInfo).
struct PanInfo
{
    uint32_t curve = 0;          // 0 the square root, 1 the sine and cosine, 2 linear
    bool center_zero_db = false; // scaled so that the centre is unity
    bool clamp_to_unity = false; // held to 1 rather than 2
    bool front_bypass = false;   // the stereo curves, even for surround output
};

namespace util
{

// The PowerPC's conversion of a float to a 32-bit integer (fctiwz): toward zero, held to the integer's range, and the
// most negative integer for NaN.
int32_t ToInt32(float x);

// The C library's conversion of a float to an unsigned 64-bit integer (_f_ftoull, 0x0263486C): toward zero, and 0 for
// what's below 1 or NaN.
uint64_t ToUint64(float x);

// The pitch ratio for a pitch in 1/256 semitones (Util::CalcPitchRatio, 0x022A2640).
float CalcPitchRatio(int pitch);

// The gain for a level in dB, held to -90.4 and +6.0 (Util::CalcVolumeRatio, 0x022A2730).
float CalcVolumeRatio(float decibel);

// The gain for a pan from -1 (left) to 1 (right) (Util::CalcPanRatio, 0x0229C0DC). Surround output takes the surround
// curves unless the pan bypasses them.
float CalcPanRatio(float pan, const PanInfo& info, OutputMode mode);

// The gain for a surround pan from 0 (front) to 2 (rear) (Util::CalcSurroundPanRatio, 0x0229C228).
float CalcSurroundPanRatio(float span, const PanInfo& info);

// The cutoff in Hz for a low-pass value from 0 to 1 (Util::CalcLpfFreq, 0x0229CE40).
uint16_t CalcLpfFreq(float lpf);

} // namespace util

// nw::snd's random numbers (Util::CalcRandom, 0x0229EF64): x = x * 0x19660D + 0x3C6EF35F. Each number is x's top 16
// bits.
class Random
{
public:
    uint16_t Next()
    {
        x_ = x_ * 0x19660Du + 0x3C6EF35Fu;

        return static_cast<uint16_t>(x_ >> 16);
    }

    uint32_t State() const
    {
        return x_;
    }

private:
    uint32_t x_ = 0x12345678;
};

} // namespace csf::nwsnd
