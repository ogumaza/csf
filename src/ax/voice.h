// SPDX-License-Identifier: MIT

// An AX voice (AXVPB): the structure that a game holds for a voice it has acquired. The structure has the voice's
// index, state and priority, and the owner's callback for when AX takes the voice away. It also has the sync word (a
// record of which parts of the game's copy of the parameter block changed), the offsets that the game last set, and the
// two copies of the block.

#pragma once

#include <cstdint>
#include <functional>

#include "ax/pb.h"

namespace csf::ax
{

// Where a voice's samples are (AXPBOFFSET). The offsets count from the sample base: bytes for PCM8, samples for PCM16
// and nibbles for ADPCM. In ADPCM, a frame's first sample is at offset 2 after its header.
struct Offsets
{
    SampleFormat format = SampleFormat::kAdpcm;
    uint16_t loop_flag = 0;
    uint32_t loop = 0;
    uint32_t end = 0; // the last sample played
    uint32_t current = 0;
    uint32_t samples = 0; // the sample base address
};

struct Voice
{
    // Called when AX takes the voice away for one of higher priority. The call passes the context that the owner
    // acquired the voice with, and the reason (0).
    using Callback = std::function<void(Voice& voice, uint32_t context, uint32_t reason)>;

    uint32_t index = 0;
    VoiceState state = VoiceState::kStop; // the game's view, refreshed from the renderer every frame
    Callback callback;
    uint32_t context = 0;
    uint32_t sync = 0;
    Offsets offsets;

    // Protection (AXVoiceBegin, AXUserBegin). It holds the game's changes back from the renderer.
    bool is_protected = false;
    uint32_t protect_count = 0;

    // What AXGetVoiceLoopCount last recorded: the position, and the loops counted since the voice was acquired.
    uint32_t last_position = 0;
    uint32_t loop_count = 0;

    Pb game;
    Pb renderer;
};

} // namespace csf::ax
