// SPDX-License-Identifier: MIT

// WAV and FLAC writers for the command-line tools (16-bit stereo).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace csf
{

// Writes interleaved 16-bit stereo samples to a .wav, or to a .flac when the name ends in .flac (in any case). Returns
// an error message on failure.
std::optional<std::string> WriteAudio(const std::string& path, const std::vector<int16_t>& pcm, uint32_t rate);

// Returns the error that WriteAudio would give for `frames` stereo frames because the file `path` names can't hold
// them, or nullopt if it can. A WAV file's sizes are 32-bit. A WAV file therefore holds a little over 6 hours at
// 48,000 Hz.
std::optional<std::string> CheckAudioLength(const std::string& path, uint64_t frames);

} // namespace csf
