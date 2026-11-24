// SPDX-License-Identifier: MIT

#include "rip/track_states.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/sequence_starts.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/game_rules.h"

namespace csf::rip
{
namespace
{

// The tracks that a state doesn't play, a bit for each, from those that it plays.
constexpr uint16_t Muted(uint16_t playing)
{
    return static_cast<uint16_t>(~playing);
}

} // namespace

std::vector<ntdlib::sar::LayeredSound> LayeredPieces(std::string_view archive_sha1)
{
    namespace snd = ntdlib::cafe::snd;
    std::vector<ntdlib::sar::LayeredSound> pieces;
    for (const std::string_view label : snd::ListedSequences(archive_sha1))
    {
        const snd::SequenceStart start = snd::StartOfSequence(archive_sha1, label);
        const std::span<const snd::SequenceArrangement> later = snd::SequenceArrangements(archive_sha1, label);
        const auto turns_down = [](const snd::SequenceArrangement& state)
        {
            return state.tracks != 0xffff;
        };
        if (start.tracks == 0xffff && std::none_of(later.begin(), later.end(), turns_down))
        {
            continue;
        }

        // A later state's suffix starts with the space that WithTrackStates puts between the label and the name.
        ntdlib::sar::LayeredSound piece{std::string(label), {{"", Muted(start.tracks)}}};
        for (const snd::SequenceArrangement& state : later)
        {
            std::string_view name = state.suffix;
            if (name.starts_with(' '))
            {
                name.remove_prefix(1);
            }

            piece.states.push_back({std::string(name), Muted(state.tracks)});
        }

        pieces.push_back(std::move(piece));
    }

    return pieces;
}

std::vector<ntdlib::rip::Job> WithTrackStates(std::span<const ntdlib::sar::LayeredSound> pieces,
                                              const std::vector<ntdlib::rip::Job>& selected)
{
    const auto states_of = [pieces](const ntdlib::rip::Job& job)
    {
        const auto piece = std::ranges::find(pieces, job.label, &ntdlib::sar::LayeredSound::label);
        return piece == pieces.end() ? std::span<const ntdlib::sar::TrackState>()
                                     : std::span<const ntdlib::sar::TrackState>(piece->states);
    };
    const auto set_volumes = [](const ntdlib::sar::TrackState& state, ntdlib::rip::Job& job)
    {
        job.track_volumes = ntdlib::rip::TrackVolumesOf(state.muted);
    };
    return ntdlib::rip::WithStates(selected, states_of, set_volumes);
}

} // namespace csf::rip
