// SPDX-License-Identifier: MIT

// sndcore2, the Wii U system's AX, at a high level: the game's calls go to src/ax's model of AX. The model's renderer
// stands in for the DSP. A game holds each voice as an AXVPB. Here an AXVPB is a record in guest memory with the fields
// that sndcore2 keeps there and games read. The fields are the voice's index (+0x00), its state (+0x04: the game reads
// 1 there while the voice runs), and the context (+0x24) and callback (+0x48) for the call that AX makes when it drops
// the voice. NintendoWare finds its voices by the index. NintendoWare uses the state to detect when a voice has played
// its samples. Blocks of parameters (AXPBOFFSET, AXPBADPCM, AXPBSRC and the rest) are read in sndcore2's layouts. The
// Wii Remotes' speakers (and padscore's calls for them) aren't modelled. The aux buses' callbacks run a game's effects,
// and they aren't called yet. So what voices send to the aux buses is lost.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "ax/memory.h"
#include "ax/system.h"
#include "cafe/machine.h"

namespace csf::cafe
{

class Sndcore2
{
public:
    // Registers the handlers with `machine`. The machine must not have loaded its executable yet.
    explicit Sndcore2(Machine& machine);

    Sndcore2(const Sndcore2&) = delete;
    Sndcore2& operator=(const Sndcore2&) = delete;

    // After the executable is loaded: the voices' records, and AX's view of the guest's memory.
    void Start();

    // Starts AX with the 32 kHz renderer or the 48 kHz one. AXInit and AXInitWithParams do the same.
    void Init(bool renderer_48k);

    // Renders one frame of AX. The frame runs the game's frame callbacks.
    void RenderFrame()
    {
        system_.RenderFrame();
    }

    ax::System& System()
    {
        return system_;
    }

private:
    // The voice whose record is at `record`. Throws std::runtime_error for an address that isn't one.
    ax::Voice& VoiceAt(uint32_t record);

    uint32_t RecordOf(const ax::Voice& voice) const;

    // Sets the state in the voice's record (+0x04) from the voice's state: 1 while the voice runs. The game reads the
    // state there.
    void WriteState(const ax::Voice& voice);

    Machine& machine_;
    ax::Memory memory_;
    ax::System system_{memory_};
    uint32_t records_ = 0;
    std::array<ax::Voice*, ax::kVoiceCount> voices_{};
    std::vector<uint32_t> frame_callbacks_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> aux_; // callback, context
    std::map<uint32_t, uint32_t> final_mix_;                                                // by device
};

} // namespace csf::cafe
