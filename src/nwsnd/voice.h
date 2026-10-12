// SPDX-License-Identifier: MIT

// nw::snd's MultiVoice and its manager (driver::MultiVoice and driver::MultiVoiceManager) in NintendoWare for Cafe:
// ntdlib's model of them, with CSF's policy (see sequence.h), and the data they work with. A MultiVoice plays each
// channel of a wave on a voice of AX (see renderer.h). Each comment gives the address of the routine that a type
// follows in Animal Crossing: amiibo Festival's executable.

#pragma once

#include <array>
#include <cstdint>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/voice.h"

#include "nwsnd/ax_voice.h"
#include "nwsnd/util.h"

namespace csf::nwsnd
{

using ntdlib::nwsnd::VoiceId;

// A wave as nw::snd reads it from its file (WaveInfo, filled by WaveFileReader, 0x022B85B0): the sample format, the
// loop, the rate, the loop's start and the end (also the length when the wave doesn't loop), and each channel's
// samples. Each channel has its samples' address in AX's memory, the DSP-ADPCM coefficients, and the decoder's state at
// the start and at the loop.
struct WaveInfo
{
    struct Channel
    {
        uint32_t address = 0;
        std::array<int16_t, 16> coefs{};
        AdpcmContext context;
        AdpcmContext loop_context;
    };

    SampleFormat format = SampleFormat::kDspAdpcm;
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t original_loop_start = 0;
    std::array<Channel, 2> channels;
};

// What the MultiVoices read of nw::snd's HardwareManager, at the values that a game's sound system starts with. The
// fields are each device's output mode as nw::snd mixes for it, and as the system reports it (the TV's mode, then the
// GamePad's mode). The other fields are the devices that each bit of an output line reaches (bit 0 the TV, 1 to 4 the
// Wii Remotes, 5 and 6 the GamePads), and the output volume (GetOutputVolume, 0x0229BF58).
struct HardwareSettings
{
    std::array<OutputMode, 2> output_mode{OutputMode::kStereo, OutputMode::kStereo};
    std::array<OutputMode, 2> end_user_output_mode{OutputMode::kStereo, OutputMode::kStereo};
    std::array<uint8_t, 32> output_line_devices{0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40};
    float output_volume = 1.0f;
};

// A device's output parameters (OutputParam): its volume, whether the channels mix by pan (0) or by the mix parameters
// (1), each wave channel's mix parameters (front left and right, rear left and right, front centre and LFE), the pan,
// the surround pan, and the main and effect sends. The main send is an offset from 1.
struct OutputParam
{
    float volume = 1.0f;
    uint32_t mix_mode = 0;
    std::array<std::array<float, 6>, 2> mix_parameter{
        {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}}};
    float pan = 0.0f;
    float span = 0.0f;
    float main_send = 0.0f;
    std::array<float, 3> fx_send{};
};

// The output parameters for the TV and for each GamePad (DRC).
struct Outputs
{
    OutputParam tv;
    std::array<OutputParam, 2> drc;
};

struct SequencePolicy;

using MultiVoice = ntdlib::nwsnd::MultiVoice<SequencePolicy>;
using MultiVoiceManager = ntdlib::nwsnd::MultiVoiceManager<SequencePolicy>;

} // namespace csf::nwsnd
