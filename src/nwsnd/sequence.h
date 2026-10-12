// SPDX-License-Identifier: MIT

// nw::snd's sequence playback in NintendoWare for Cafe: ntdlib's model of it (driver::SequenceTrack with its MmlParser,
// and driver::SequenceSoundPlayer), and of the channels that play its notes. CSF's policy gives the model the Wii U's
// arithmetic, and CSF's engine, waves, voices and outputs.

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"

#include "csf/sar.h"
#include "nwsnd/channel.h"
#include "nwsnd/voice.h"

namespace csf::nwsnd
{

class Engine;

// The sequence model's policy: the Wii U's arithmetic, and CSF's types.
struct SequencePolicy : ntdlib::nwsnd::CafePolicy
{
    using Channel = nwsnd::Channel;
    using Engine = nwsnd::Engine;
    using NoteOnInfo = nwsnd::NoteOnInfo;
    using SoundInfo = fsar::SoundInfo;
    using WaveInfo = nwsnd::WaveInfo;
    using Outputs = nwsnd::Outputs;
    using Receiver = void;
    using VoiceParam = nwsnd::VoiceParam;
    using SampleFormat = nwsnd::SampleFormat;

    // A channel's samples are where the wave archive's file has them, in AX's memory. The engine maps the whole file
    // there when a note first uses the wave archive.
    static void PlaceSamples(WaveInfo::Channel& channel, std::span<const uint8_t> samples,
                             const ntdlib::nwsnd::LoadedWaveArchive& archive)
    {
        channel.address = archive.address + static_cast<uint32_t>(samples.data() - archive.file.data());
    }

    // Each device's output parameters, the TV's first, then each GamePad's, from each of `outputs` in step.
    template <typename Function, typename... Each>
    static void ForEachOutput(const Function& function, Each&... outputs)
    {
        function(outputs.tv...);
        function(outputs.drc[0]...);
        function(outputs.drc[1]...);
    }

    // A voice's parameters when it's allocated (VoiceParam::Initialize, 0x022A254C): the TV's front channels at unity
    // on the main bus.
    static VoiceParam InitialVoiceParam()
    {
        VoiceParam param{};
        param.tv[0][0] = 1.0f;
        param.tv[0][1] = 1.0f;

        return param;
    }

    // Each device's output parameters combine the player's parameters, the track's parameters, and the bytecode's pan,
    // surround pan and sends.
    static Outputs CombineOutputs(const Outputs& player, const Outputs& track, float pan, float span, float main_send,
                                  const std::array<float, 3>& fx_send);

    static void SetOutputs(Channel& channel, const Outputs& outputs);

    // The track's front bypass goes to the channel's voices.
    static void SetFrontBypass(Engine& engine, Channel& channel, bool front_bypass);

    // The Wii U's channels have no receivers.
    static void SetReceiver(NoteOnInfo&, const Receiver*)
    {
    }
};

using SequenceTrack = ntdlib::nwsnd::SequenceTrack<SequencePolicy>;
using SequenceSoundPlayer = ntdlib::nwsnd::SequenceSoundPlayer<SequencePolicy>;

} // namespace csf::nwsnd
