// SPDX-License-Identifier: MIT

// The pieces of music whose tracks a game's code turns up and down as the game is played, and the states they play in.
// A rip of such a piece plays it in the state that the game starts it in. Each later state gets a rip too. The game
// sets the tracks' volumes through the sound's handle (nw::snd's SetTrackVolume). The rip's descriptor holds them
// (docs/csf.md, "Track volumes"). A piece is an ntdlib::sar::LayeredSound: its label in the sound archive and its
// states (ntdlib::sar::TrackState). The game starts the piece in the first state.

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/game_rules.h"

namespace csf::rip
{

// The layered pieces of the sound archive whose SHA-1 is `archive_sha1`, in 40 lowercase hex digits: the sequences
// whose start or later states turn tracks down in ntdlib's lists of sequence starts
// (ntdlib/cafe/snd/sequence_starts.h). None for any other archive.
std::vector<ntdlib::sar::LayeredSound> LayeredPieces(std::string_view archive_sha1);

// The jobs that rip `selected` in the states of `pieces`. A piece's job takes the piece's first state. The jobs of its
// later states follow it, under the piece's label, a space and the state's name. The other jobs stay as they are.
std::vector<ntdlib::rip::Job> WithTrackStates(std::span<const ntdlib::sar::LayeredSound> pieces,
                                              const std::vector<ntdlib::rip::Job>& selected);

} // namespace csf::rip
