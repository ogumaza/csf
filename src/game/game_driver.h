// SPDX-License-Identifier: MIT

// Game mode's drivers. A driver runs a game's sound code on the emulated system, starts one of its sounds and renders
// it. The process descriptor names the driver (docs/csf.md, "Process descriptor"), and the descriptor's profile gives
// the addresses and settings in the game's code that the driver requires.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cafe/rpl.h"

namespace csf::game
{

// A frame of AX is 3 ms: 186,468.75 ticks of 62,156,250 a second. So the drivers count frames in quarters of a tick.
inline constexpr uint64_t kFrameQuarterTicks = 745'875;

class GameDriver
{
public:
    virtual ~GameDriver() = default;

    // Starts the sound `sound`, the ID in the set's SND chunk, with the driver's main sound file at `path` (under
    // /vol/content). A JAudio driver loads the wave groups `waves` first, each the wave bank in the high half and the
    // group in the low half. Throws std::runtime_error if the game's code refuses.
    virtual void Start(const std::string& path, uint32_t sound, std::span<const uint32_t> waves) = 0;

    // Runs one frame of AX, 3 ms, and returns the TV's output: 144 stereo pairs at 48 kHz.
    virtual std::span<const int16_t> Frame() = 0;

    // Whether the sound has ended.
    virtual bool IsFinished() = 0;
};

// The driver `driver` (a GameDriverKind) with the profile `words`, for the game whose executable is `executable`.
// `files` are the title's files that the game opens, by their paths under /vol/content. The driver copies the
// executable into the game's memory. `executable` may then be freed. Returns nullptr and sets `error` if this player
// has no such driver, or if the profile's words don't fit it. Throws std::runtime_error if the game's code fails.
std::unique_ptr<GameDriver> MakeGameDriver(uint32_t driver, const std::vector<uint32_t>& words,
                                           const cafe::Rpl& executable,
                                           std::map<std::string, std::vector<uint8_t>> files, std::string& error);

} // namespace csf::game
