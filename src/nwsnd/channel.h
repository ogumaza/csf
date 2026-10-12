// SPDX-License-Identifier: MIT

// nw::snd's channels (driver::Channel and driver::ChannelManager) in NintendoWare for Cafe: ntdlib's model of them,
// with CSF's policy (see sequence.h). A channel is a note on a MultiVoice.

#pragma once

#include <cstdint>

#include "ntdlib/nwsnd/channel.h"

namespace csf::nwsnd
{

struct SequencePolicy;

using Channel = ntdlib::nwsnd::Channel<SequencePolicy>;
using ChannelManager = ntdlib::nwsnd::ChannelManager<SequencePolicy>;

// What a track passes to its bank when it plays a note (NoteOnInfo): the program, the key and the velocity (scaled by
// the track's velocity range), the length in ticks or -1, the track's initial pan, the channel's priority, and the
// track to notify when the channel ends.
struct NoteOnInfo
{
    uint32_t prg_no = 0;
    int key = 60;
    int velocity = 127;
    int32_t length = -1;
    int init_pan = 0;
    uint32_t priority = 64;

    // The channel's owner: the player's place among the engine's players, and the track. The engine passes the
    // channel's callback on to it.
    int owner_player = -1;
    int owner_track = -1;
};

} // namespace csf::nwsnd
