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

// The layered pieces of the sound archive whose SHA-1 is `archive_sha1`, in 40 lowercase hex digits: the sequences
// whose start or later states turn tracks down in ntdlib's lists of sequence starts
// (ntdlib/cafe/snd/sequence_starts.h). None for any other archive.
std::vector<LayeredPiece> LayeredPieces(std::string_view archive_sha1);

// The jobs that rip `selected` in the states of `pieces`. A piece's job takes the piece's first state. The jobs of its
// later states follow it, under the piece's label, a space and the state's name. The other jobs stay as they are.
std::vector<ntdlib::rip::Job> WithTrackStates(std::span<const LayeredPiece> pieces,
                                              const std::vector<ntdlib::rip::Job>& selected);

} // namespace csf::rip
