// SPDX-License-Identifier: MIT

// sndcore2, the Wii U system's AX, at a high level: the game's calls go to src/ax's model of AX. The model's renderer
// stands in for the DSP. A game holds each voice as an AXVPB. Here an AXVPB is a record in guest memory with the fields
// that sndcore2 keeps there and games read. The fields are the voice's index (+0x00), its state (+0x04: the game reads
// 1 there while the voice runs), and the context (+0x24) and callback (+0x48) for the call that AX makes when it drops
// the voice. NintendoWare finds its voices by the index. NintendoWare uses the state to detect when a voice has played
// its samples. Blocks of parameters (AXPBOFFSET, AXPBADPCM, AXPBSRC and the rest) are read in sndcore2's layouts. The
// Wii Remotes' speakers (and padscore's calls for them) aren't modelled. The aux buses' callbacks run a game's effects,
// and they aren't called yet. So what voices send to the aux buses is lost.
//
// A game that links the older snd_core reaches the same handlers. Its AXInit starts the model as snd_core, with the
// 32 kHz renderer. snd_core's records and parameter blocks are taken in sndcore2's layouts, which nobody has checked
// against snd_core's. Only NintendoWare's voices read them. JAudio1 mixes its voices on the CPU and hands AX its mix.

#pragma once

#include <array>
#include <cstdint>
#include <map>
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

    // Starts AX for a game that links `library`, with the 32 kHz renderer or sndcore2's 48 kHz one. AXInit and
    // AXInitWithParams do the same.
    void Init(ax::System::Library library, bool renderer_48k);

    // Starts sndcore2's AX.
    void Init(bool renderer_48k)
    {
        Init(ax::System::Library::kSndCore2, renderer_48k);
    }

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
    // The most channels and samples that a device's final mix holds: the GamePads' two devices of four channels, and
    // the 48 kHz renderer's frame. The guest's data for the final-mix callback then fits in kFinalMixDataBytes.
    static constexpr uint32_t kFinalMixChannels = 8;
    static constexpr uint32_t kFinalMixSamples = 144;
    static constexpr uint32_t kFinalMixDataBytes =
        16 + kFinalMixChannels * 4 + kFinalMixChannels * kFinalMixSamples * 4;

    // Runs the guest's final-mix callback for `device` on the device's mix, as AX does each frame.
    void RunFinalMix(uint32_t device, uint32_t callback, ax::System::FinalMix& mix);

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
    std::array<uint32_t, 2> final_mix_data_{}; // each device's data for its final-mix callback, once allocated
};

} // namespace csf::cafe
