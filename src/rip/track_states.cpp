// SPDX-License-Identifier: MIT

#include "rip/track_states.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>

#include "xsflib/sha1.h"

namespace csf::rip
{

namespace
{

// Every track but `playing`, a bit for each.
constexpr uint16_t AllBut(std::initializer_list<int> playing)
{
    uint16_t bits = 0xffff;
    for (const int t : playing)
    {
        bits = static_cast<uint16_t>(bits & ~(1u << t));
    }

    return bits;
}

// A sound archive that a table of layered pieces belongs to: its size, its SHA-1 and its pieces.
struct LayeredArchive
{
    std::size_t size = 0;
    std::string_view sha1;
    std::vector<LayeredPiece> pieces;
};

// Kirby and the Rainbow Curse's seven.bfsar (the USA release, executable 9b8810d629c51a5e544e960498fea84a536afcd6). The
// stage select plays BGM_SEQ_MENU_INTERACTIVE (item 0x0100030B) through areastageselect::BgmController. Its changeType
// (0x023EC428) takes an area type and gives the sequence's tracks that type's row of a table at 0x10058ABC: 1 for each
// track that plays and 0 for the rest. The first call starts the sound with the row already applied (forceCompleteFade,
// 0x023EC404). Later calls fade each track to its new volume by 1/30 a frame. The area view passes type 1 (0x023F4948).
// A world's list of stages passes the world's number and 1, and its boss stage the world's number and 8 (0x023FA074).
// The table's row 0 is the same as row 1, and no code passes type 0. The sequence has no track 9.
LayeredArchive RainbowCurse()
{
    LayeredArchive a;
    a.size = 23'733'264;
    a.sha1 = "813a15124d13b1978e7e3cb49f6648b9cb50d705";
    a.pieces.push_back({"BGM_SEQ_MENU_INTERACTIVE",
                        {{"", AllBut({7})},
                         {"world 1", AllBut({0, 10})},
                         {"world 2", AllBut({1})},
                         {"world 3", AllBut({2, 12})},
                         {"world 4", AllBut({3})},
                         {"world 5", AllBut({4})},
                         {"world 6", AllBut({5})},
                         {"world 7", AllBut({6})},
                         {"world 1 boss", AllBut({0, 8, 10})},
                         {"world 2 boss", AllBut({1, 11})},
                         {"world 3 boss", AllBut({2, 8, 12})},
                         {"world 4 boss", AllBut({3, 8})},
                         {"world 5 boss", AllBut({4, 11})},
                         {"world 6 boss", AllBut({5, 11})},
                         {"world 7 boss", AllBut({6, 8})}}});

    return a;
}

const std::vector<LayeredArchive>& Archives()
{
    static const std::vector<LayeredArchive> kArchives = {RainbowCurse()};

    return kArchives;
}

} // namespace

std::span<const LayeredPiece> LayeredPieces(std::span<const uint8_t> archive)
{
    std::string sha1;
    for (const LayeredArchive& a : Archives())
    {
        if (archive.size() != a.size)
        {
            continue;
        }
        if (sha1.empty())
        {
            sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(archive));
        }
        if (sha1 == a.sha1)
        {
            return a.pieces;
        }
    }

    return {};
}

ntdlib::rip::TrackVolumes TrackVolumesOf(uint16_t muted)
{
    ntdlib::rip::TrackVolumes volumes = ntdlib::rip::kFullTrackVolumes;
    for (std::size_t t = 0; t < volumes.size(); ++t)
    {
        if ((muted >> t & 1) != 0)
        {
            volumes[t] = 0.0f;
        }
    }

    return volumes;
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
            in_state.track_volumes = TrackVolumesOf(state.muted);
            jobs.push_back(std::move(in_state));
        }
    }

    return jobs;
}

} // namespace csf::rip
