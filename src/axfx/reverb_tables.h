// SPDX-License-Identifier: MIT

// The reverbs' tables in the system library: the lengths of their lines in samples, the high-quality reverb's early
// taps and gains, and the comb gains' formula. The lengths are the same at both of the renderer's rates.

#pragma once

#include <cstdint>

#include "axfx/math.h"
#include "axfx/util.h"

namespace csf::axfx::reverb
{

// The longest pre-delay line that the models allocate: 2^23 samples, 32 MiB for each channel. The game's allocation
// would fail long before. The models' Init fails there too.
constexpr uint32_t kMaxPreDelayLength = 1u << 23;

constexpr uint32_t kEarlyModes = 8;
constexpr uint32_t kFusedModes = 6;

// The standard reverb's early line for each early mode (0x10004030). Init allocates the line with the last length.
constexpr uint32_t kStdEarlySizes[kEarlyModes] = {163, 317, 479, 641, 797, 967, 1123, 1283};

// The standard reverb's lengths for each fused mode: the two combs and the two allpasses (0x10004050). Init allocates
// the lines with the last row.
constexpr uint32_t kStdFilterSizes[kFusedModes + 1][4] = {
    {1789, 1999, 433, 149}, {149, 293, 251, 103},   {947, 1361, 433, 137},  {1279, 1531, 509, 149},
    {1531, 1847, 563, 179}, {1823, 2357, 571, 137}, {1823, 2357, 571, 179},
};

// The high-quality reverb's three early taps for each early mode: each tap's delay (0x10003cd4). The last tap's delay
// is the early line's length. Init allocates the line with the longest.
constexpr uint32_t kHiEarlySizes[kEarlyModes][3] = {
    {157, 479, 829},   {317, 809, 1117},  {479, 941, 1487},   {641, 1259, 1949},
    {797, 1667, 2579}, {967, 1901, 2903}, {1123, 2179, 3413}, {1279, 2477, 3889},
};

// The high-quality reverb's gains for the three taps (0x10003d34).
constexpr float kHiEarlyCoefs[kEarlyModes][3] = {
    {0.4f, -1.0f, 0.3f}, {0.5f, -0.95f, 0.3f}, {0.6f, -0.9f, 0.3f}, {0.75f, -0.85f, 0.3f},
    {-0.9f, 0.8f, 0.3f}, {-1.0f, 0.7f, 0.3f},  {-1.0f, 0.7f, 0.3f}, {-1.0f, 0.7f, 0.3f},
};

// The high-quality reverb's lengths for each fused mode: the three combs, the two allpasses and each channel's last
// allpass (0x10003d94). Init allocates the lines with the last row.
constexpr uint32_t kHiFilterSizes[kFusedModes + 1][8] = {
    {1789, 1999, 2333, 433, 149, 47, 73, 67}, {149, 293, 449, 251, 103, 47, 73, 67},
    {947, 1361, 1531, 433, 137, 47, 73, 67},  {1279, 1531, 1973, 509, 149, 47, 73, 67},
    {1531, 1847, 2297, 563, 179, 47, 73, 67}, {1823, 2357, 2693, 571, 137, 47, 73, 67},
    {1823, 2357, 2693, 571, 179, 47, 73, 67},
};

// The gain of a comb `length` samples long that decays by 60 dB in `time` seconds at `rate` (`rate` already a float):
// 10^(-3 length / (time rate)), each step in single precision, with the library's pow.
inline float CombCoef(uint32_t length, float time, float rate)
{
    return Pow(10.0f, (-3.0f * ToFloat(length)) / (time * rate));
}

} // namespace csf::axfx::reverb
