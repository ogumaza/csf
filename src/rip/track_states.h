// SPDX-License-Identifier: MIT

// The pieces of music whose tracks a game's code turns up and down as the game is played, and the states they play in.
// A rip of such a piece plays it in the state that the game starts it in. Each later state gets a rip too. The game
// sets the tracks' volumes through the sound's handle (nw::snd's SetTrackVolume). The rip's descriptor holds them
// (docs/csf.md, "Track volumes").

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/rip/archive.h"

namespace csf::rip
{

// A state of a piece: the words that the state's rip adds to the piece's label, and the tracks that the game's code has
// turned down to 0, a bit for each. The game leaves the other tracks at 1. The state that the piece starts in adds no
// words.
struct TrackState
{
    std::string_view name;
    uint16_t muted = 0;
};

// A piece whose tracks the game's code turns up and down: its label in the sound archive, and its states. The first is
// the state that the game starts the piece in.
struct LayeredPiece
{
    std::string_view label;
    std::vector<TrackState> states;
};

// The layered pieces of the sound archive `archive`, which a table recognizes by the archive's size and SHA-1. None for
// any other archive.
std::span<const LayeredPiece> LayeredPieces(std::span<const uint8_t> archive);

// The volumes of the tracks in a state whose muted tracks are `muted`: 0 for each track whose bit is set, and 1 for
// the rest.
ntdlib::rip::TrackVolumes TrackVolumesOf(uint16_t muted);

// The jobs that rip `selected` in the states of `pieces`. A piece's job takes the piece's first state. The jobs of its
// later states follow it, under the piece's label, a space and the state's name. The other jobs stay as they are.
std::vector<ntdlib::rip::Job> WithTrackStates(std::span<const LayeredPiece> pieces,
                                              const std::vector<ntdlib::rip::Job>& selected);

} // namespace csf::rip
