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

std::vector<LayeredPiece> LayeredPieces(std::string_view archive_sha1)
{
    namespace snd = ntdlib::cafe::snd;
    std::vector<LayeredPiece> pieces;
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
        LayeredPiece piece{label, {{"", Muted(start.tracks)}}};
        for (const snd::SequenceArrangement& state : later)
        {
            std::string_view name = state.suffix;
            if (name.starts_with(' '))
            {
                name.remove_prefix(1);
            }
            piece.states.push_back({name, Muted(state.tracks)});
        }
        pieces.push_back(std::move(piece));
    }

    return pieces;
}

std::vector<ntdlib::rip::Job> WithTrackStates(std::span<const LayeredPiece> pieces,
                                              const std::vector<ntdlib::rip::Job>& selected)
{
    std::vector<ntdlib::rip::Job> jobs;
    for (const ntdlib::rip::Job& job : selected)
    {
        const auto piece =
            std::find_if(pieces.begin(), pieces.end(), [&job](const LayeredPiece& p) { return p.label == job.label; });
        if (piece == pieces.end())
        {
            jobs.push_back(job);
            continue;
        }

        for (const TrackState& state : piece->states)
        {
            ntdlib::rip::Job in_state = job;
            if (!state.name.empty())
            {
                in_state.label += " ";
                in_state.label += state.name;
            }
            in_state.track_volumes = ntdlib::rip::TrackVolumesOf(state.muted);
            jobs.push_back(std::move(in_state));
        }
    }

    return jobs;
}

} // namespace csf::rip
