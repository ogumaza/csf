// SPDX-License-Identifier: MIT

// Checks CSF's model of AX two ways. Scenarios (voices of every format set up through the calls a game makes, with rate
// conversion, envelopes, filters, mixes to the TV and the GamePad, several priorities, changes made between frames and
// inside the frame callback, protection, the final-mix callback, the upsample stage and a remix matrix, at both
// renderer rates) must give the samples recorded for them at every stage. A hash of each stage's samples is compared
// with the hash in ax_test_hashes.h. The scenarios and their runner are in ax_scenarios.h. The rest of the test checks
// particular behaviours: the ADPCM history at a voice's start and at its loop, a one-shot voice's last sample, the
// interpolation, the 4-tap filters, the low-pass filter, both GamePads, each device's upsample stage, the aux buses'
// timing, the FIR upsampler and AXGetVoiceLoopCount.
//
// usage: ax_test [--dump <folder>]
//   --dump writes each scenario's stage lines.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ax/memory.h"
#include "ax/resample.h"
#include "ax/system.h"
#include "ax_scenarios.h"
#include "ax_test_hashes.h"

using xsflib::testing::Check;

namespace
{

using csf::ax::Memory;
using csf::ax::System;
using csf::ax::Voice;

namespace fs = std::filesystem;

using ax_scenarios::Hash;
using ax_scenarios::Runner;
using ax_scenarios::Scenario;
using ax_scenarios::Scenarios;

} // namespace

// One voice on CSF's model, playing `bytes` from 0x10000000 into the TV's left channel at unity, without rate
// conversion.
class Bench
{
public:
    Bench(std::vector<uint8_t> data, csf::ax::SampleFormat format, uint32_t loop_flag, uint32_t loop, uint32_t end,
          uint32_t current)
        : bytes(std::move(data))
    {
        memory.Map(0x10000000, bytes);
        voice = system.AcquireVoice(10);
        system.SetVoiceOffsets(*voice, {format, static_cast<uint16_t>(loop_flag), loop, end, current, 0x10000000});
        system.SetVoiceSrcType(*voice, 0);
        system.SetVoiceVe(*voice, {csf::ax::kUnityVolume, 0});
        std::vector<System::ChannelMix> mix(csf::ax::kTvChannels);
        mix[0][0].volume = csf::ax::kUnityVolume;
        system.SetVoiceDeviceMix(*voice, System::Device::kTv, 0, mix);
    }

    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    // Renders a frame and returns the TV's left main bus.
    std::vector<int32_t> Frame()
    {
        system.RenderFrame();
        const auto& bus = system.VoiceBuses().tv[0][0];
        return {bus.begin(), bus.begin() + system.SamplesPerFrame()};
    }

    // In the order they're made: the memory maps the bytes, and the model reads the memory.
    std::vector<uint8_t> bytes;
    Memory memory;
    System system{memory};
    Voice* voice = nullptr;
};

// An ADPCM frame whose samples, with coefficient pair 0 at (1, 0), all repeat the history.
csf::ax::Adpcm FlatAdpcm(int16_t yn1)
{
    csf::ax::Adpcm adpcm;
    adpcm.coefs[0] = 2048;
    adpcm.yn1 = yn1;

    return adpcm;
}

void TestAdpcmHistory()
{
    // A voice starts from the history that the game gives it. A one-shot voice plays its last sample, then nothing.
    Bench bench(std::vector<uint8_t>(8, 0), csf::ax::SampleFormat::kAdpcm, 0, 2, 15, 2);
    bench.system.SetVoiceAdpcm(*bench.voice, FlatAdpcm(1000));
    bench.system.SetVoiceState(*bench.voice, csf::ax::VoiceState::kRun);

    const std::vector<int32_t> frame = bench.Frame();

    Check(frame[0] == 1000 && frame[13] == 1000, "ADPCM starts from the game's history");
    Check(frame[14] == 0 && frame[95] == 0, "a one-shot ADPCM voice ends after its last sample");

    // At the loop start, a voice takes the history that the loop context gives it. A stream's history continues
    // instead.
    for (const auto type : {csf::ax::VoiceType::kNormal, csf::ax::VoiceType::kStream})
    {
        Bench loop(std::vector<uint8_t>(8, 0), csf::ax::SampleFormat::kAdpcm, 1, 2, 15, 2);
        loop.system.SetVoiceType(*loop.voice, type);
        loop.system.SetVoiceAdpcm(*loop.voice, FlatAdpcm(1000));
        loop.system.SetVoiceAdpcmLoop(*loop.voice, {0, 500, 0});
        loop.system.SetVoiceState(*loop.voice, csf::ax::VoiceState::kRun);

        const std::vector<int32_t> looped = loop.Frame();

        const bool stream = type == csf::ax::VoiceType::kStream;
        Check(looped[13] == 1000 && looped[14] == (stream ? 1000 : 500) && looped[95] == (stream ? 1000 : 500),
              stream ? "a stream's history continues through its loop" : "a loop takes the loop context's history");
    }
}

void TestPcm16()
{
    const std::vector<uint8_t> kBytes = {0, 100, 0, 200, 1, 44, 1, 144};

    // Without rate conversion, a one-shot voice plays its last sample.
    Bench once(kBytes, csf::ax::SampleFormat::kPcm16, 0, 0, 2, 0);
    once.system.SetVoiceState(*once.voice, csf::ax::VoiceState::kRun);

    const std::vector<int32_t> frame = once.Frame();

    Check(frame[0] == 100 && frame[1] == 200 && frame[2] == 300 && frame[3] == 0,
          "a one-shot PCM16 voice plays its last sample");

    // Linear interpolation goes from the oldest of the last four samples. So at a ratio of 1 it gives the sample three
    // before.
    Bench linear(kBytes, csf::ax::SampleFormat::kPcm16, 0, 0, 3, 0);
    linear.system.SetVoiceSrcType(*linear.voice, 1);
    linear.system.SetVoiceSrcRatio(*linear.voice, 1.0f);
    linear.system.SetVoiceState(*linear.voice, csf::ax::VoiceState::kRun);

    const std::vector<int32_t> interpolated = linear.Frame();

    Check(interpolated[2] == 0 && interpolated[3] == 100 && interpolated[4] == 200 && interpolated[6] == 400,
          "linear interpolation goes from the oldest of the last four samples");

    // Halfway between two samples, the interpolation rounds down.
    Check(csf::ax::Resample(csf::ax::SrcMode::kLinear, 0, {101, -100, 7, 9}, 0x8000) == 0 &&
              csf::ax::Resample(csf::ax::SrcMode::kLinear, 0, {-101, 100, 7, 9}, 0x8000) == -1,
          "linear interpolation rounds down");
}

void TestFourTap()
{
    using csf::ax::Resample;
    using csf::ax::SrcMode;

    // The sum is rounded down to 16 bits: the 8 kHz filter's first phase sums to 32751/32768.
    Check(Resample(SrcMode::kFourTap, 0, {10000, 10000, 10000, 10000}, 0) == 9994 &&
              Resample(SrcMode::kFourTap, 0, {-10000, -10000, -10000, -10000}, 0) == -9995,
          "a 4-tap filter rounds its sum down to 16 bits");

    // The phase is the position's top 7 bits. At phase 0, the 16 kHz filter takes nearly all of the second oldest
    // sample.
    const std::array<int16_t, 4> kSecond = {0, 10000, 0, 0};
    Check(Resample(SrcMode::kFourTap, 2, kSecond, 0x01FF) == 9960 &&
              Resample(SrcMode::kFourTap, 2, kSecond, 0x0200) == 9957,
          "a 4-tap filter's phase is the position's top 7 bits");

    // Each filter is symmetric about the middle of its four samples. So the taps meet the samples in order.
    bool symmetric = true;
    for (uint16_t filter = 0; filter < 3; ++filter)
    {
        for (uint32_t phase = 0; phase < 128; ++phase)
        {
            symmetric =
                symmetric && Resample(SrcMode::kFourTap, filter, {1000, -2000, 3000, 4000}, phase << 9) ==
                                 Resample(SrcMode::kFourTap, filter, {4000, 3000, -2000, 1000}, (127 - phase) << 9);
        }
    }

    Check(symmetric, "the 4-tap filters are symmetric");

    // A sum outside 32 bits' range saturates.
    Check(Resample(SrcMode::kFourTap, 2, {-32768, 32767, 32767, -32768}, 62 << 9) == 32767 &&
              Resample(SrcMode::kFourTap, 2, {32767, -32768, -32768, 32767}, 62 << 9) == -32768,
          "a 4-tap filter saturates");

    // Through a voice: a constant of 10000 enters the 8 kHz filter one sample at a time.
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 200; ++i)
    {
        bytes.insert(bytes.end(), {0x27, 0x10});
    }
    Bench bench(bytes, csf::ax::SampleFormat::kPcm16, 1, 0, 199, 0);
    bench.system.SetVoiceSrcType(*bench.voice, 2);
    bench.system.SetVoiceSrcRatio(*bench.voice, 1.0f);
    bench.system.SetVoiceState(*bench.voice, csf::ax::VoiceState::kRun);

    const std::vector<int32_t> frame = bench.Frame();

    Check(frame[0] == 0 && frame[1] == 2052 && frame[2] == 7980 && frame[3] == 9994 && frame[95] == 9994,
          "a voice's 4-tap filter takes its four newest samples, oldest first");
}

void TestLpf()
{
    // A step through the low-pass filter rises to the input without ringing, and continues across frames.
    Bench bench({0x27, 0x10}, csf::ax::SampleFormat::kPcm16, 1, 0, 0, 0);
    const auto [a0, b0] = bench.system.ComputeLpfCoefs(1000);
    bench.system.SetVoiceLpf(*bench.voice, {1, 0, static_cast<int16_t>(a0), static_cast<int16_t>(b0)});
    bench.system.SetVoiceState(*bench.voice, csf::ax::VoiceState::kRun);

    std::vector<int32_t> samples;
    for (int frame = 0; frame < 4; ++frame)
    {
        const std::vector<int32_t> next = bench.Frame();
        samples.insert(samples.end(), next.begin(), next.end());
    }

    bool rising = true;
    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        rising = rising && samples[i] >= samples[i - 1] && samples[i] <= 10000;
    }
    Check(rising && samples.back() > 9900, "the low-pass filter rises to a step's level without ringing");

    Check(a0 + b0 == 32767 && b0 > 0, "AXComputeLpfCoefs's coefficients add up to unity");

    Memory none;
    System at_48k(none);
    at_48k.Init(System::Library::kSndCore2, true);

    Check(at_48k.ComputeLpfCoefs(1000).second > b0 && bench.system.ComputeLpfCoefs(4000).second < b0,
          "AXComputeLpfCoefs's pole is nearer unity for a lower cutoff and a faster renderer");
}

void TestDevices()
{
    // The GamePads are mixed separately.
    Bench bench({0x03, 0xE8}, csf::ax::SampleFormat::kPcm16, 1, 0, 0, 0);
    std::vector<System::ChannelMix> mix(csf::ax::kDrcChannels);
    mix[0][0].volume = csf::ax::kUnityVolume;
    bench.system.SetVoiceDeviceMix(*bench.voice, System::Device::kDrc, 1, mix);
    mix[0][0].volume = 0;
    mix[1][0].volume = 0x4000;
    bench.system.SetVoiceDeviceMix(*bench.voice, System::Device::kDrc, 0, mix);
    bench.system.SetVoiceState(*bench.voice, csf::ax::VoiceState::kRun);

    bench.Frame();

    const csf::ax::Buses& buses = bench.system.VoiceBuses();
    Check(buses.drc[1][0][0][5] == 1000 && buses.drc[0][0][1][5] == 500 && buses.drc[0][0][0][5] == 0,
          "both GamePads are mixed, each into its channels");

    // Each device converts to 48 kHz at the stage set for it.
    std::vector<int32_t> before;
    for (int frame = 0; frame < 3; ++frame)
    {
        bench.Frame();
        before.insert(before.end(), bench.system.Drc48(1, 0).begin(), bench.system.Drc48(1, 0).end());
    }

    Bench after({0x03, 0xE8}, csf::ax::SampleFormat::kPcm16, 1, 0, 0, 0);
    mix[0][0].volume = csf::ax::kUnityVolume;
    after.system.SetVoiceDeviceMix(*after.voice, System::Device::kDrc, 1, mix);
    after.system.SetDeviceUpsampleStage(System::Device::kTv, true);
    after.system.SetVoiceState(*after.voice, csf::ax::VoiceState::kRun);

    std::vector<int32_t> unaffected;
    for (int frame = 0; frame < 4; ++frame)
    {
        after.Frame();
        if (frame > 0)
        {
            unaffected.insert(unaffected.end(), after.system.Drc48(1, 0).begin(), after.system.Drc48(1, 0).end());
        }
    }

    Check(before == unaffected, "the TV's upsample stage leaves the GamePads' alone");
}

void TestAux()
{
    // What a voice sends an aux bus reaches the callback a frame later and the main bus two frames later.
    Bench bench({0x03, 0xE8}, csf::ax::SampleFormat::kPcm16, 1, 0, 0, 0);
    std::vector<System::ChannelMix> mix(csf::ax::kTvChannels);
    mix[0][1].volume = csf::ax::kUnityVolume;
    bench.system.SetVoiceDeviceMix(*bench.voice, System::Device::kTv, 0, mix);
    std::vector<int32_t> seen;
    const auto aux_callback = [&seen](std::span<int32_t* const> channels, uint32_t)
    {
        seen.push_back(channels[0][10]);
    };
    bench.system.RegisterAuxCallback(System::Device::kTv, 0, 0, aux_callback);
    bench.system.SetVoiceState(*bench.voice, csf::ax::VoiceState::kRun);

    std::vector<int32_t> main;
    for (int frame = 0; frame < 4; ++frame)
    {
        bench.system.RenderFrame();
        main.push_back(bench.system.MainBuses().tv[0][0][10]);
    }

    Check(seen == std::vector<int32_t>{0, 1000, 1000, 1000}, "an aux callback receives the last frame's sends");
    Check(main == std::vector<int32_t>{0, 0, 1000, 1000}, "the aux bus returns two frames later");
}

void TestLoopCount()
{
    // AXGetVoiceLoopCount counts a loop each time the game's copy of the position has gone back since the last call.
    Memory memory;
    System system(memory);
    Voice& voice = *system.AcquireVoice(10);
    system.SetVoiceSrcType(voice, 0);
    system.SetVoiceOffsets(voice, {csf::ax::SampleFormat::kPcm16, 1, 1000, 5000, 1000, 0x10000000});

    std::vector<uint32_t> counts;
    for (const uint32_t offset : {1000u, 4000u, 1010u, 1020u, 4999u, 1000u})
    {
        system.SetVoiceCurrentOffset(voice, offset);
        counts.push_back(system.GetVoiceLoopCount(voice));
    }

    Check(counts == std::vector<uint32_t>{0, 0, 1, 1, 1, 2}, "a loop counts when the position goes back");

    // A loop no longer than a frame (144 output samples in sndcore2 and 96 in snd_core) counts as many loops as a frame
    // from the last position could hold. The first call after the voice is acquired measures from position 0. A loop of
    // 2^16 units or more can pass for a short one.
    const auto looping = [&system](uint32_t end, uint32_t current) -> Voice&
    {
        Voice& looping_voice = *system.AcquireVoice(10);
        system.SetVoiceSrcType(looping_voice, 0);
        system.SetVoiceOffsets(looping_voice, {csf::ax::SampleFormat::kPcm16, 1, 1000, end, current, 0x10000000});

        return looping_voice;
    };

    constexpr uint32_t kLoopAddress = 0x10000000 / 2 + 1000;
    const uint32_t in_snd_core = system.GetVoiceLoopCount(looping(1100, 1000));
    system.Init(System::Library::kSndCore2);
    Voice& short_loop = looping(1100, 1000);
    Voice& long_loop = looping(1000 + 65536 + 50, 1010);

    const uint32_t first = system.GetVoiceLoopCount(short_loop);
    system.SetVoiceCurrentOffset(short_loop, 1050);
    const uint32_t second = system.GetVoiceLoopCount(short_loop);
    system.GetVoiceLoopCount(long_loop);
    const uint32_t before = system.GetVoiceLoopCount(long_loop);
    const uint32_t after = system.GetVoiceLoopCount(long_loop);

    Check(in_snd_core == 0 && first == (0 - kLoopAddress + 144) / 100 && second == first + 1,
          "a short loop counts the loops a frame could hold");
    Check(after == before + (10 + 144) / 50, "a loop of 2^16 units or more can count as a short one");
}

void TestUpsampler()
{
    // By default, a device converts a 32 kHz renderer's output to 48 kHz with the FIR filter, before the final-mix
    // callback. A constant from the first sample rings, then settles where each phase's taps take it.
    Bench bench({0x03, 0xE8}, csf::ax::SampleFormat::kPcm16, 1, 0, 0, 0);
    bench.system.SetVoiceState(*bench.voice, csf::ax::VoiceState::kRun);

    bench.Frame();

    const std::span<const int32_t> step = bench.system.Tv48(0);
    Check(std::vector<int32_t>(step.begin(), step.begin() + 21) ==
              std::vector<int32_t>{-5,  2,    6,   -27,  23,   30,  -136, 206,  953, 1105, 947,
                                   999, 1016, 991, 1000, 1000, 999, 999,  1000, 999, 999},
          "the FIR upsampler's step response");

    // With the stage after the callback, the filter takes what the callback leaves. An impulse of 2^20 comes out as a
    // tap of each phase in turn, truncated. The taps on the pairs' earlier inputs come from an even input, and the
    // others from an odd one. The output continues into the next frame.
    Memory memory;
    System system(memory);
    system.SetDeviceUpsampleStage(System::Device::kTv, true);
    int pulse = 0;
    const auto put_pulse = [&pulse](System::FinalMix& mix)
    {
        for (int i = 0; i < mix.samples; ++i)
        {
            mix.channels[0][i] = i == pulse ? 1 << 20 : 0;
        }
    };
    system.RegisterDeviceFinalMixCallback(System::Device::kTv, put_pulse);

    const auto render_with_pulse = [&system, &pulse](int at)
    {
        pulse = at;
        system.RenderFrame();

        return std::vector<int32_t>(system.Tv48(0).begin(), system.Tv48(0).end());
    };

    const std::vector<int32_t> kEvenResponse = {-4757,  2515,    10615, -34756, 31680,  50712, -199779, 278588, 1069061,
                                                529818, -186391, -9541, 64534,  -36543, 2542,  7326,    -5039,  0};
    const std::vector<int32_t> kOddResponse = {0,      0,      -4015, 11572,   -10065, -25557, 85361,  -86070, -101272,
                                               772138, 963510, 58372, -158097, 82929,  -1362,  -24130, 13305,  -1477};

    const std::vector<int32_t> even = render_with_pulse(0);
    const std::vector<int32_t> odd = render_with_pulse(1);
    const std::vector<int32_t> last = render_with_pulse(95);
    const std::vector<int32_t> next = render_with_pulse(-1);

    Check(std::vector<int32_t>(even.begin(), even.begin() + 18) == kEvenResponse &&
              std::all_of(even.begin() + 18, even.end(), [](int32_t x) { return x == 0; }),
          "an impulse at an even input gives the taps on the earlier inputs");
    Check(std::vector<int32_t>(odd.begin(), odd.begin() + 18) == kOddResponse,
          "an impulse at an odd input gives the taps on the later inputs");
    Check(std::vector<int32_t>(last.end() - 3, last.end()) ==
                  std::vector<int32_t>(kOddResponse.begin(), kOddResponse.begin() + 3) &&
              std::vector<int32_t>(next.begin(), next.begin() + 15) ==
                  std::vector<int32_t>(kOddResponse.begin() + 3, kOddResponse.end()),
          "the FIR upsampler's history continues into the next frame");

    // A device can use linear interpolation instead.
    Check(system.SetDeviceLinearUpsampler(System::Device::kRemote, 0, true) == -1 &&
              system.SetDeviceLinearUpsampler(System::Device::kTv, 1, true) == -2 &&
              system.SetDeviceLinearUpsampler(System::Device::kDrc, csf::ax::kDrcCount, true) == -2 &&
              system.SetDeviceLinearUpsampler(System::Device::kTv, 0, true) == 0,
          "AXSetDeviceLinearUpsampler checks the device and its index");

    const std::vector<int32_t> linear = render_with_pulse(0);

    Check(std::vector<int32_t>(linear.begin(), linear.begin() + 4) == std::vector<int32_t>{349525, 1 << 20, 349525, 0},
          "the linear upsampler weighs an input by 1/3, 1 and 1/3");
}

int main(int argc, char** argv)
{
    std::string dump_folder;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (std::string(argv[i]) == "--dump")
        {
            dump_folder = argv[i + 1];
        }
    }

    // Every scenario against its recorded hash.
    std::map<std::string, uint64_t> expected;
    for (const ScenarioHash& hash : kScenarioHashes)
    {
        expected[hash.name] = hash.hash;
    }

    for (const Scenario& scenario : Scenarios())
    {
        Runner runner;
        const std::vector<std::string> lines = runner.Run(scenario.text);

        if (!dump_folder.empty())
        {
            std::ofstream out(fs::path(dump_folder) / (scenario.name + ".lines"));
            for (const std::string& line : lines)
            {
                out << line << "\n";
            }
        }

        const uint64_t hash = Hash(lines);
        char text[32];
        std::snprintf(text, sizeof(text), "0x%016llxull", static_cast<unsigned long long>(hash));
        Check(expected.count(scenario.name) && expected[scenario.name] == hash,
              scenario.name + " gives its recorded samples (its hash is {\"" + scenario.name + "\", " + text + "})");
    }

    TestAdpcmHistory();
    TestPcm16();
    TestFourTap();
    TestLpf();
    TestDevices();
    TestAux();
    TestUpsampler();
    TestLoopCount();

    return xsflib::testing::Summary("ax");
}
