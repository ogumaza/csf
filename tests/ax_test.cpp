// SPDX-License-Identifier: MIT

// Checks CSF's model of AX two ways. Scenarios (voices of every format set up through the calls a game makes, with rate
// conversion, envelopes, filters, mixes to the TV and the GamePad, several priorities, changes made between frames and
// inside the frame callback, protection, the final-mix callback, the upsample stage and a remix matrix, at both
// renderer rates) must give the samples recorded for them at every stage. A hash of each stage's samples is compared
// with the hash in ax_test_hashes.h. The rest of the test checks particular behaviours: the ADPCM history at a voice's
// start and at its loop, a one-shot voice's last sample, the interpolation, the 4-tap filters, the low-pass filter, the
// second GamePad, the aux buses' timing, the FIR upsampler and AXGetVoiceLoopCount.
//
// usage: ax_test [--dump <folder>]
//   --dump writes each scenario's stage lines.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ax/memory.h"
#include "ax/resample.h"
#include "ax/system.h"
#include "ax_test_hashes.h"
#include "check.h"

namespace
{

using csf::ax::Memory;
using csf::ax::System;
using csf::ax::Voice;

namespace fs = std::filesystem;

// A small deterministic generator for test data.
class Random
{
public:
    explicit Random(uint32_t seed) : state_(seed)
    {
    }

    uint32_t Next()
    {
        state_ = state_ * 1664525u + 1013904223u;

        return state_ >> 8;
    }

    int Below(int limit)
    {
        return static_cast<int>(Next() % static_cast<uint32_t>(limit));
    }

private:
    uint32_t state_;
};

// Sample data in a scenario's words: PCM16 noise, PCM8 noise, or DSP-ADPCM frames with random headers and nibbles.
std::vector<uint8_t> Pcm16Data(Random& random, int samples, int amplitude)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < samples; ++i)
    {
        const auto value = static_cast<uint16_t>(random.Below(2 * amplitude + 1) - amplitude);
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value));
    }

    return bytes;
}

std::vector<uint8_t> Pcm8Data(Random& random, int samples)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < samples; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(random.Next()));
    }

    return bytes;
}

std::vector<uint8_t> AdpcmData(Random& random, int frames, int max_scale)
{
    std::vector<uint8_t> bytes;
    for (int frame = 0; frame < frames; ++frame)
    {
        const int coefficients = random.Below(8);
        const int scale = random.Below(max_scale + 1);
        bytes.push_back(static_cast<uint8_t>(coefficients << 4 | scale));
        for (int i = 0; i < 7; ++i)
        {
            bytes.push_back(static_cast<uint8_t>(random.Next()));
        }
    }

    return bytes;
}

// Eight coefficient pairs of the kind encoders choose.
const char* const kCoefs = "0 0 2048 0 0 2048 1024 1024 4096 -2048 3600 -1700 1800 -900 3072 -1024";

std::string MemLines(uint32_t address, const std::vector<uint8_t>& bytes)
{
    std::string text;
    for (std::size_t at = 0; at < bytes.size(); at += 1024)
    {
        char line[32];
        std::snprintf(line, sizeof(line), "mem 0x%08x hex ", static_cast<unsigned>(address + at));
        text += line;
        for (std::size_t i = at; i < std::min(bytes.size(), at + 1024); ++i)
        {
            char hex[3];
            std::snprintf(hex, sizeof(hex), "%02x", bytes[i]);
            text += hex;
        }
        text += "\n";
    }

    return text;
}

struct Scenario
{
    std::string name;
    std::string text;
};

// A random scenario within the same limits as the others. It has up to six voices at random priorities. Each voice is a
// one-shot ADPCM voice too long to end, a looping ADPCM stream, PCM8 with rate conversion, or looping PCM16 without it.
// The voices have random rates, envelopes, mixes and biquad filters. They change between frames and in the frame
// callback, with and without protection. Some scenarios also have a final-mix callback, the upsampler after it and a
// remix matrix.
//
// Each random number is drawn in a statement of its own, in the order of the scenario's text. C++ leaves the order in
// which an expression's operands are evaluated to the compiler. MSVC and clang-cl evaluate them in another order than
// GCC and Clang. The scenarios, and so their hashes, would differ between them.
Scenario RandomScenario(int number)
{
    Random random(0x9E3779B9u * static_cast<uint32_t>(number + 1));
    const bool sndcore2 = random.Below(3) != 0;
    const bool renderer_48k = sndcore2 && random.Below(4) == 0;
    std::string text = std::string("init ") + (sndcore2 ? "sndcore2" : "snd_core") + (renderer_48k ? " 48k" : "") +
                       "\ndump all off\ndump vbus main out out48 dma on\n";

    // Ratios and weights are multiples of 1/64 and 1/16. A float holds those exactly.
    const auto ratio = [&random]
    {
        return std::to_string((8 + random.Below(293)) / 64.0);
    };
    const auto delta = [&random](int max)
    {
        return random.Below(4) == 0 ? 0 : random.Below(2 * max + 1) - max;
    };
    const auto envelope = [&]
    {
        const int volume = random.Below(0x10000);
        return std::to_string(volume) + " " + std::to_string(delta(64));
    };
    const auto mix = [&](const std::string& name)
    {
        const char* const buses[] = {"main", "a", "b", "c"};
        std::string line = "mix " + name + " tv 0";
        for (int i = 1 + random.Below(4); i > 0; --i)
        {
            const int channel = random.Below(6);
            const char* const bus = buses[random.Below(4)];
            const int volume = random.Below(0x10000);
            line += " ch" + std::to_string(channel) + "." + bus + "=" + std::to_string(volume) + "/" +
                    std::to_string(delta(40));
        }
        line += "\n";
        if (random.Below(2))
        {
            const int channel = random.Below(2);
            const int volume = random.Below(0x10000);
            line += "mix " + name + " drc 0 ch" + std::to_string(channel) + ".main=" + std::to_string(volume) + "/" +
                    std::to_string(delta(40)) + "\n";
        }

        return line;
    };
    const auto coefs = [&random]
    {
        std::string words;
        for (int i = 0; i < 5; ++i)
        {
            words += " " + std::to_string(random.Below(32768) - 16384);
        }

        return words;
    };

    struct Made
    {
        std::string name;
        bool src;
    };
    std::vector<Made> voices;
    for (int v = 0, count = 1 + random.Below(6); v < count; ++v)
    {
        const std::string name = "v" + std::to_string(v);
        const uint32_t address = 0x10000000 + 0x10000 * static_cast<uint32_t>(v);
        const std::string base = std::to_string(address);
        const int kind = random.Below(4);
        text += "acquire " + name + " " + std::to_string(1 + random.Below(31)) + "\n";
        if (kind == 0)
        {
            text += MemLines(address, AdpcmData(random, 700, 11));
            text += "offsets " + name + " adpcm " + base + " 0 2 11199 " + std::to_string(2 + random.Below(14)) + "\n";
        }
        else if (kind == 1)
        {
            const int frames = 3 + random.Below(10);
            text += MemLines(address, AdpcmData(random, frames, 11));
            text += "type " + name + " stream\noffsets " + name + " adpcm " + base + " 1 " +
                    std::to_string(2 + random.Below(14)) + " " + std::to_string(16 * frames - 1) + " 2\n";
            const int coefficients = random.Below(8);
            const int scale = random.Below(12);
            text += "adpcmloop " + name + " " + std::to_string(coefficients << 4 | scale) + " 0 0\n";
        }
        else if (kind == 2)
        {
            const int samples = 50 + random.Below(400);
            text += MemLines(address, Pcm8Data(random, samples));
            const char* const looped = random.Below(2) ? " 1 " : " 0 ";
            const int loop_start = random.Below(samples / 2);
            text += "offsets " + name + " pcm8 " + base + looped + std::to_string(loop_start) + " " +
                    std::to_string(samples - 1) + " 0\n";
        }
        else
        {
            const int samples = 20 + random.Below(400);
            text += MemLines(address, Pcm16Data(random, samples, 32767));
            text += "offsets " + name + " pcm16 " + base + " 1 " + std::to_string(random.Below(samples / 2)) + " " +
                    std::to_string(samples - 1) + " 0\n";
        }
        if (kind < 2)
        {
            const int coefficients = random.Below(8);
            const int scale = random.Below(12);
            text += "adpcm " + name + " " + kCoefs + " 0 " + std::to_string(coefficients << 4 | scale) + " 0 0\n";
        }

        const bool src = kind != 3;
        const char* const types[] = {"linear", "4tap8k", "4tap12k", "4tap16k"};
        text += "srctype " + name + " " + (src ? types[random.Below(4)] : "none") + "\n";
        if (src)
        {
            text += "srcratio " + name + " " + ratio() + "\n";
        }
        text += "ve " + name + " " + envelope() + "\n";
        text += mix(name);
        if (random.Below(4) == 0)
        {
            text += "biquad " + name + " 2 0 0 0 0" + coefs() + "\n";
        }
        text += "state " + name + " play\n";
        voices.push_back({name, src});
    }

    for (int segment = 1 + random.Below(4); segment > 0; --segment)
    {
        text += "render " + std::to_string(1 + random.Below(3)) + "\n";
        const int protect = random.Below(8); // 0: AXUserBegin, 1: AXVoiceBegin, else none
        if (protect < 2)
        {
            text += protect == 0 ? "userbegin\n" : "voicebegin " + voices[0].name + "\n";
        }
        for (int change = random.Below(4); change > 0; --change)
        {
            const Made& voice = voices[static_cast<std::size_t>(random.Below(static_cast<int>(voices.size())))];
            const int kind = random.Below(5);
            if (kind == 2)
            {
                // A mix change is two directives at most. The change is made between frames.
                text += mix(voice.name);
                continue;
            }

            std::string line;
            if (kind == 0)
            {
                line = "ve " + voice.name + " " + envelope();
            }
            else if (kind == 1 && voice.src)
            {
                line = "srcratio " + voice.name + " " + ratio();
            }
            else if (kind == 3)
            {
                line = "state " + voice.name + (random.Below(2) ? " play" : " stop");
            }
            else
            {
                line = "biquadcoefs " + voice.name + coefs();
            }
            text += (random.Below(3) == 0 ? "@cb " : "") + line + "\n";
        }
        if (protect < 2)
        {
            text += "render 1\n" + (protect == 0 ? std::string("userend\n") : "voiceend " + voices[0].name + "\n");
        }

        if (random.Below(5) == 0)
        {
            if (random.Below(2))
            {
                text += "finalmix tv pass\n";
            }
            else
            {
                const int scale = 1 + random.Below(7);
                text += "finalmix tv scale " + std::to_string(scale) + " " + std::to_string(random.Below(4)) + "\n";
            }
        }
        if (!renderer_48k && random.Below(6) == 0)
        {
            const std::string stage = random.Below(2) ? "after" : "before";
            text += "upsample tv " + stage + "\nupsample drc " + stage + "\n";
        }
        if (random.Below(6) == 0)
        {
            text += "remix tv 6 2";
            for (int i = 0; i < 12; ++i)
            {
                text += " " + std::to_string(random.Below(17) / 16.0);
            }
            text += "\n";
        }
    }

    char name[16];
    std::snprintf(name, sizeof(name), "random_%03d", number);

    return {name, text};
}

// The scenarios. They were written to compare CSF with Cemu's AX. That was before CSF's renderer followed AX's
// microcode. So they keep to what the two did alike: ADPCM voices that are never reused and whose one-shot ends aren't
// reached, ADPCM loops only on streams, PCM16 only without rate conversion and looping, no low-pass filter, no aux
// callbacks and no mix to the second GamePad.
std::vector<Scenario> Scenarios()
{
    std::vector<Scenario> scenarios;
    Random random(0x43534620);
    const std::string head = "dump all off\ndump vbus main out out48 dma on\n";

    // ADPCM without rate conversion and with each kind, envelopes and mix ramps.
    {
        std::string text = "init sndcore2\n" + head;
        text += MemLines(0x10000000, AdpcmData(random, 120, 11));
        text += MemLines(0x10010000, AdpcmData(random, 120, 9));
        text += MemLines(0x10020000, AdpcmData(random, 120, 12));
        text += "acquire a 10\nacquire b 10\nacquire c 12\n";
        text += "offsets a adpcm 0x10000000 0 2 1900 2\n";
        text += std::string("adpcm a ") + kCoefs + " 0 0x23 0 0\n";
        text += "srctype a none\nve a 0x8000 0\nmix a tv 0 ch0.main=0x8000 ch1.main=0x4000\n";
        text += "offsets b adpcm 0x10010000 0 2 1900 2\n";
        text += std::string("adpcm b ") + kCoefs + " 0 0x45 0 0\n";
        text += "srctype b linear\nsrcratio b 1.375\nve b 0x6000 3\n";
        text += "mix b tv 0 ch0.main=0x7000/-5 ch1.main=0x3000/7 ch4.main=0x2000 ch0.a=0x1000\n";
        text += "offsets c adpcm 0x10020000 0 2 1900 2\n";
        text += std::string("adpcm c ") + kCoefs + " 0 0x12 0 0\n";
        text += "srctype c 4tap12k\nsrcratio c 0.6875\nve c 0x9000 -4\n";
        text += "mix c tv 0 ch0.main=0x5000 ch1.main=0x5000/-2\nmix c drc 0 ch0.main=0x8000 ch1.main=0x2000/9\n";
        text += "state a play\nstate b play\nstate c play\nrender 6\n";
        text += "srcratio b 0.8125\nve a 0x4000 0\n@cb ve c 0x2000 7\nrender 4\nstate a stop\nrender 3\n";
        scenarios.push_back({"adpcm", text});
    }

    // A looping ADPCM stream. Its history continues through the loop.
    {
        std::string text = "init snd_core\n" + head;
        text += MemLines(0x10000000, AdpcmData(random, 20, 10));
        text += "acquire s 20\ntype s stream\n";
        text += "offsets s adpcm 0x10000000 1 37 250 2\n";
        text += std::string("adpcm s ") + kCoefs + " 0 0x34 0 0\n";
        text += "adpcmloop s 0x51 0 0\nsrctype s linear\nsrcratio s 1.5\nve s 0x8000 0\n";
        text += "mix s tv 0 ch0.main=0x8000 ch1.main=0x8000\nstate s play\nrender 12\n";
        scenarios.push_back({"adpcm_stream_loop", text});
    }

    // PCM8 with linear rate conversion, looping and one-shot, and PCM16 looping without rate conversion.
    {
        std::string text = "init sndcore2\n" + head;
        text += MemLines(0x10000000, Pcm8Data(random, 700));
        text += MemLines(0x10010000, Pcm16Data(random, 300, 20000));
        text += "acquire p 5\nacquire q 5\nacquire r 7\n";
        text += "offsets p pcm8 0x10000000 1 100 400 0\nsrctype p linear\nsrcratio p 1.25\nve p 0x8000 0\n";
        text += "mix p tv 0 ch0.main=0x8000\n";
        text += "offsets q pcm8 0x10000000 0 0 450 30\nsrctype q 4tap16k\nsrcratio q 2.75\nve q 0x7000 0\n";
        text += "mix q tv 0 ch1.main=0x8000 ch2.main=0x1000\n";
        text += "offsets r pcm16 0x10010000 1 50 299 0\nsrctype r none\nve r 0xA000 -9\n";
        text += "mix r tv 0 ch0.main=0x4000 ch1.main=0x4000/1\n";
        text += "state p play\nstate q play\nstate r play\nrender 9\n";
        scenarios.push_back({"pcm", text});
    }

    // The biquad filter, and an envelope above unity. sndcore2 clamps that envelope.
    {
        std::string text = "init sndcore2\n" + head;
        text += MemLines(0x10000000, AdpcmData(random, 100, 12));
        text += "acquire f 10\n";
        text += "offsets f adpcm 0x10000000 0 2 1590 2\n";
        text += std::string("adpcm f ") + kCoefs + " 0 0x2B 0 0\n";
        text += "srctype f none\nve f 0xF000 -40\nmix f tv 0 ch0.main=0x8000 ch1.main=0x8000\n";
        text += "biquad f 2 0 0 0 0 4000 8000 4000 15000 -6000\nstate f play\nrender 5\n";
        text += "biquadcoefs f 16384 -20000 8000 -12000 -3000\nrender 4\n";
        scenarios.push_back({"biquad", text});
    }

    // Under snd_core the envelope isn't clamped.
    {
        std::string text = "init snd_core\n" + head;
        text += MemLines(0x10000000, AdpcmData(random, 60, 12));
        text += "acquire f 10\noffsets f adpcm 0x10000000 0 2 950 2\n";
        text += std::string("adpcm f ") + kCoefs + " 0 0x2B 0 0\n";
        text += "srctype f none\nve f 0xF000 -40\nmix f tv 0 ch0.main=0x8000\nstate f play\nrender 4\n";
        scenarios.push_back({"envelope_snd_core", text});
    }

    // When changes take effect: between frames, in the frame callback, and with protection.
    {
        std::string text = "init sndcore2\n" + head;
        text += MemLines(0x10000000, Pcm16Data(random, 400, 30000));
        text += "acquire v 9\noffsets v pcm16 0x10000000 1 0 399 0\nsrctype v none\nve v 0x8000 0\n";
        text += "mix v tv 0 ch0.main=0x8000\nstate v play\nrender 2\n";
        text += "ve v 0x4000 0\nrender 1\n@cb ve v 0x2000 0\nrender 2\n";
        text += "voicebegin v\nve v 0x6000 0\nrender 2\nvoiceend v\nrender 1\n";
        text += "userbegin\nve v 0x7000 0\nrender 1\nuserend\nrender 1\n";
        scenarios.push_back({"timing", text});
    }

    // Voices at several priorities, and a voice stolen for a higher priority.
    {
        std::string text = "init sndcore2\n" + head;
        text += MemLines(0x10000000, Pcm16Data(random, 2000, 25000));
        const char* const names[] = {"v1", "v2", "v3", "v4", "v5", "v6"};
        const int priorities[] = {1, 5, 5, 20, 1, 31};
        for (int i = 0; i < 6; ++i)
        {
            const std::string name = names[i];
            char line[160];
            std::snprintf(line, sizeof(line),
                          "acquire %s %d\noffsets %s pcm16 0x10000000 1 %d 1999 %d\nsrctype %s none\n"
                          "ve %s 0x%x 0\nmix %s tv 0 ch0.main=0x%x ch1.main=0x%x\nstate %s play\n",
                          names[i], priorities[i], names[i], 100 * i, 37 * i, names[i], names[i], 0x3000 + 0x700 * i,
                          names[i], 0x2000 + 0x800 * i, 0x6000 - 0x500 * i, names[i]);
            text += line;
        }
        text += "render 3\nfree v3\nrender 2\n";
        scenarios.push_back({"priorities", text});
    }

    // The final mix stage: a final-mix callback, the upsampler after it, and a remix matrix folding six channels into
    // two.
    {
        std::string text = "init sndcore2\n" + head;
        text += MemLines(0x10000000, Pcm16Data(random, 1000, 12000));
        text += "acquire v 10\noffsets v pcm16 0x10000000 1 0 999 0\nsrctype v none\nve v 0x8000 0\n";
        text += "mix v tv 0 ch0.main=0x8000 ch1.main=0x6000 ch4.main=0x5000 ch2.main=0x2000\n";
        text += "mix v drc 0 ch0.main=0x7000 ch1.main=0x3000\nstate v play\n";
        text +=
            "finalmix tv pass\nrender 2\nfinalmix tv scale 3 2\nrender 2\nupsample tv after\nupsample drc "
            "after\nrender 2\n";
        text += "remix tv 6 2 0.75 0 0.5 0 0.5 0.25 0 0.75 0 0.5 0.5 0.25\nrender 2\n";
        scenarios.push_back({"final_mix", text});
    }

    // The 48 kHz renderer.
    {
        std::string text = "init sndcore2 48k\n" + head;
        text += MemLines(0x10000000, AdpcmData(random, 100, 11));
        text += "acquire v 10\noffsets v adpcm 0x10000000 0 2 1590 2\n";
        text += std::string("adpcm v ") + kCoefs + " 0 0x16 0 0\n";
        text += "srctype v linear\nsrcratio v 0.6875\nve v 0x8000 0\n";
        text += "mix v tv 0 ch0.main=0x8000 ch1.main=0x4000\nmix v drc 0 ch0.main=0x6000\nstate v play\nrender 5\n";
        scenarios.push_back({"renderer_48k", text});
    }

    for (int i = 0; i < 200; ++i)
    {
        scenarios.push_back(RandomScenario(i));
    }

    return scenarios;
}

// Runs a scenario on CSF's model and returns each frame's samples at each stage, one line per channel. The lines are in
// the form that the hashes are made from: frame, stage, device, bus, channel, count, then the values. Floats are
// written as their bits in hex, and zero as 0.
class Runner
{
public:
    std::vector<std::string> Run(const std::string& text)
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream words_in(line);
            std::vector<std::string> words;
            for (std::string word; words_in >> word;)
            {
                if (word[0] == '#')
                {
                    break;
                }
                words.push_back(word);
            }
            if (words.empty())
            {
                continue;
            }

            if (words[0] == "@cb")
            {
                callback_directives_.emplace_back(words.begin() + 1, words.end());
                continue;
            }
            Execute(words);
        }

        return lines_;
    }

private:
    static long Number(const std::string& word)
    {
        return std::stol(word, nullptr, 0);
    }

    Voice& VoiceNamed(const std::string& name)
    {
        return *voices_.at(name);
    }

    static System::Device DeviceNamed(const std::string& word)
    {
        return word == "tv" ? System::Device::kTv : word == "drc" ? System::Device::kDrc : System::Device::kRemote;
    }

    void Execute(const std::vector<std::string>& w)
    {
        const std::string& op = w[0];
        if (op == "init")
        {
            // The scenarios use the linear upsampler. The FIR filter, every device's default, is checked on its own.
            system_.Init(w[1] == "sndcore2" ? System::Library::kSndCore2 : System::Library::kSndCore,
                         w.size() > 2 && w[2] == "48k");
            system_.RegisterFrameCallback([this] { RunCallbackDirectives(); });
            system_.SetDeviceLinearUpsampler(System::Device::kTv, 0, true);
            for (uint32_t drc = 0; drc < csf::ax::kDrcCount; ++drc)
            {
                system_.SetDeviceLinearUpsampler(System::Device::kDrc, drc, true);
            }
        }
        else if (op == "mem")
        {
            std::vector<uint8_t>& block = blocks_.emplace_back();
            for (std::size_t i = 0; i + 1 < w[3].size(); i += 2)
            {
                block.push_back(static_cast<uint8_t>(std::stoul(w[3].substr(i, 2), nullptr, 16)));
            }
            memory_.Map(static_cast<uint32_t>(Number(w[1])), block);
        }
        else if (op == "acquire")
        {
            voices_[w[1]] = system_.AcquireVoice(static_cast<uint32_t>(Number(w[2])));
        }
        else if (op == "free")
        {
            system_.FreeVoice(VoiceNamed(w[1]));
        }
        else if (op == "offsets")
        {
            csf::ax::Offsets offsets;
            offsets.format = w[2] == "adpcm"   ? csf::ax::SampleFormat::kAdpcm
                             : w[2] == "pcm16" ? csf::ax::SampleFormat::kPcm16
                                               : csf::ax::SampleFormat::kPcm8;
            offsets.samples = static_cast<uint32_t>(Number(w[3]));
            offsets.loop_flag = static_cast<uint16_t>(Number(w[4]));
            offsets.loop = static_cast<uint32_t>(Number(w[5]));
            offsets.end = static_cast<uint32_t>(Number(w[6]));
            offsets.current = static_cast<uint32_t>(Number(w[7]));
            system_.SetVoiceOffsets(VoiceNamed(w[1]), offsets);
        }
        else if (op == "adpcm")
        {
            csf::ax::Adpcm adpcm;
            for (int i = 0; i < 16; ++i)
            {
                adpcm.coefs[i] = static_cast<int16_t>(Number(w[2 + i]));
            }
            adpcm.gain = static_cast<uint16_t>(Number(w[18]));
            adpcm.predictor_scale = static_cast<uint16_t>(Number(w[19]));
            adpcm.yn1 = static_cast<int16_t>(Number(w[20]));
            adpcm.yn2 = static_cast<int16_t>(Number(w[21]));
            system_.SetVoiceAdpcm(VoiceNamed(w[1]), adpcm);
        }
        else if (op == "adpcmloop")
        {
            system_.SetVoiceAdpcmLoop(VoiceNamed(w[1]),
                                      {static_cast<uint16_t>(Number(w[2])), static_cast<int16_t>(Number(w[3])),
                                       static_cast<int16_t>(Number(w[4]))});
        }
        else if (op == "type")
        {
            system_.SetVoiceType(VoiceNamed(w[1]),
                                 w[2] == "stream" ? csf::ax::VoiceType::kStream : csf::ax::VoiceType::kNormal);
        }
        else if (op == "srctype")
        {
            const std::map<std::string, uint32_t> types = {
                {"none", 0}, {"linear", 1}, {"4tap8k", 2}, {"4tap12k", 3}, {"4tap16k", 4}};
            system_.SetVoiceSrcType(VoiceNamed(w[1]), types.at(w[2]));
        }
        else if (op == "srcratio")
        {
            system_.SetVoiceSrcRatio(VoiceNamed(w[1]), std::stof(w[2]));
        }
        else if (op == "ve")
        {
            system_.SetVoiceVe(VoiceNamed(w[1]),
                               {static_cast<uint16_t>(Number(w[2])), static_cast<int16_t>(Number(w[3]))});
        }
        else if (op == "mix")
        {
            const System::Device device = DeviceNamed(w[2]);
            std::vector<System::ChannelMix> mix(device == System::Device::kTv ? csf::ax::kTvChannels
                                                                              : csf::ax::kDrcChannels);
            for (std::size_t i = 4; i < w.size(); ++i)
            {
                // ch<c>.<bus>=<volume>[/<delta>]
                const std::size_t dot = w[i].find('.');
                const std::size_t equals = w[i].find('=');
                const std::size_t slash = w[i].find('/');
                const int channel = std::stoi(w[i].substr(2, dot - 2));
                const std::string bus_name = w[i].substr(dot + 1, equals - dot - 1);
                const int bus = bus_name == "main" ? 0 : bus_name == "a" ? 1 : bus_name == "b" ? 2 : 3;
                mix[channel][bus].volume = static_cast<uint16_t>(Number(w[i].substr(equals + 1, slash - equals - 1)));
                if (slash != std::string::npos)
                {
                    mix[channel][bus].delta = static_cast<int16_t>(Number(w[i].substr(slash + 1)));
                }
            }
            system_.SetVoiceDeviceMix(VoiceNamed(w[1]), device, static_cast<uint32_t>(Number(w[3])), mix);
        }
        else if (op == "biquad")
        {
            csf::ax::Biquad biquad;
            biquad.on = static_cast<uint16_t>(Number(w[2]));
            int16_t* const fields[] = {&biquad.xn1, &biquad.xn2, &biquad.yn1, &biquad.yn2, &biquad.b0,
                                       &biquad.b1,  &biquad.b2,  &biquad.a1,  &biquad.a2};
            for (int i = 0; i < 9; ++i)
            {
                *fields[i] = static_cast<int16_t>(Number(w[3 + i]));
            }
            system_.SetVoiceBiquad(VoiceNamed(w[1]), biquad);
        }
        else if (op == "biquadcoefs")
        {
            system_.SetVoiceBiquadCoefs(VoiceNamed(w[1]), static_cast<int16_t>(Number(w[2])),
                                        static_cast<int16_t>(Number(w[3])), static_cast<int16_t>(Number(w[4])),
                                        static_cast<int16_t>(Number(w[5])), static_cast<int16_t>(Number(w[6])));
        }
        else if (op == "state")
        {
            system_.SetVoiceState(VoiceNamed(w[1]),
                                  w[2] == "play" ? csf::ax::VoiceState::kRun : csf::ax::VoiceState::kStop);
        }
        else if (op == "voicebegin")
        {
            system_.VoiceBegin(VoiceNamed(w[1]));
        }
        else if (op == "voiceend")
        {
            system_.VoiceEnd(VoiceNamed(w[1]));
        }
        else if (op == "userbegin")
        {
            system_.UserBegin();
        }
        else if (op == "userend")
        {
            system_.UserEnd();
        }
        else if (op == "finalmix")
        {
            FinalMix(DeviceNamed(w[1]), w);
        }
        else if (op == "upsample")
        {
            system_.SetDeviceUpsampleStage(DeviceNamed(w[1]), w[2] == "after");
        }
        else if (op == "remix")
        {
            std::vector<float> weights;
            for (std::size_t i = 4; i < w.size(); ++i)
            {
                weights.push_back(std::stof(w[i]));
            }
            system_.SetDeviceRemixMatrix(DeviceNamed(w[1]), static_cast<uint32_t>(Number(w[2])),
                                         static_cast<uint32_t>(Number(w[3])), weights);
        }
        else if (op == "render")
        {
            for (long i = Number(w[1]); i > 0; --i)
            {
                system_.RenderFrame();
                Record();
                ++frame_;
            }
        }
        else if (op != "dump")
        {
            Check(false, "the scenario's directive " + op + " is one that the runner supports");
        }
    }

    void FinalMix(System::Device device, const std::vector<std::string>& w)
    {
        if (w[2] == "none")
        {
            system_.RegisterDeviceFinalMixCallback(device, {});

            return;
        }

        const bool zero = w[2] == "zero";
        const bool scale = w[2] == "scale";
        const int64_t num = scale ? Number(w[3]) : 1;
        const int shift = scale ? static_cast<int>(Number(w[4])) : 0;
        system_.RegisterDeviceFinalMixCallback(
            device,
            [=](System::FinalMix& mix)
            {
                for (int32_t* channel : mix.channels)
                {
                    for (int i = 0; i < mix.samples; ++i)
                    {
                        channel[i] = zero ? 0 : static_cast<int32_t>((channel[i] * num) >> shift);
                    }
                }
            });
    }

    void RunCallbackDirectives()
    {
        std::vector<std::vector<std::string>> directives;
        directives.swap(callback_directives_);
        for (const std::vector<std::string>& words : directives)
        {
            Execute(words);
        }
    }

    template <typename T>
    void Add(const std::string& head, std::span<const T> values)
    {
        std::string line = std::to_string(frame_) + " " + head + " " + std::to_string(values.size());
        for (const T value : values)
        {
            line += " " + std::to_string(value);
        }
        lines_.push_back(line);
    }

    void Record()
    {
        const std::size_t n = static_cast<std::size_t>(system_.SamplesPerFrame());
        const char* const buses[] = {"main", "a", "b", "c"};
        const csf::ax::Buses& voice_buses = system_.VoiceBuses();
        for (int bus = 0; bus < csf::ax::kBusCount; ++bus)
        {
            for (int c = 0; c < csf::ax::kTvChannels; ++c)
            {
                Add("vbus tv " + std::string(buses[bus]) + " " + std::to_string(c),
                    std::span<const int32_t>(voice_buses.tv[bus][c].data(), n));
            }
        }
        for (int bus = 0; bus < csf::ax::kBusCount; ++bus)
        {
            for (int c = 0; c < csf::ax::kDrcChannels; ++c)
            {
                Add("vbus drc0 " + std::string(buses[bus]) + " " + std::to_string(c),
                    std::span<const int32_t>(voice_buses.drc[0][bus][c].data(), n));
            }
        }
        for (int c = 0; c < csf::ax::kTvChannels; ++c)
        {
            Add("main tv main " + std::to_string(c), std::span<const int32_t>(system_.MainBuses().tv[0][c].data(), n));
        }
        for (int c = 0; c < csf::ax::kTvChannels; ++c)
        {
            Add("out tv - " + std::to_string(c), system_.TvOutput(c));
        }
        for (int c = 0; c < csf::ax::kDrcChannels; ++c)
        {
            Add("out drc0 - " + std::to_string(c), system_.DrcOutput(0, c));
        }
        for (int c = 0; c < csf::ax::kTvChannels; ++c)
        {
            Add("out48 tv - " + std::to_string(c), system_.Tv48(c));
        }
        for (int c = 0; c < csf::ax::kDrcChannels; ++c)
        {
            Add("out48 drc0 - " + std::to_string(c), system_.Drc48(0, c));
        }
        Add("dma tv - interleaved2", system_.Output());
        std::vector<int16_t> drc;
        for (int i = 0; i < csf::ax::kOutputSamplesPerFrame; ++i)
        {
            drc.push_back(static_cast<int16_t>(std::clamp(system_.Drc48(0, 0)[i], -32768, 32767)));
            drc.push_back(static_cast<int16_t>(std::clamp(system_.Drc48(0, 1)[i], -32768, 32767)));
        }
        Add("dma drc - interleaved2", std::span<const int16_t>(drc));
    }

    Memory memory_;
    std::vector<std::vector<uint8_t>> blocks_;
    System system_{memory_};
    std::map<std::string, Voice*> voices_;
    std::vector<std::vector<std::string>> callback_directives_;
    int frame_ = 0;
    std::vector<std::string> lines_;
};

uint64_t Hash(const std::vector<std::string>& lines)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const std::string& line : lines)
    {
        for (const char c : line + "\n")
        {
            hash = (hash ^ static_cast<uint8_t>(c)) * 0x100000001b3ull;
        }
    }

    return hash;
}

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
    const std::vector<uint8_t> bytes = {0, 100, 0, 200, 1, 44, 1, 144};

    // Without rate conversion, a one-shot voice plays its last sample.
    Bench once(bytes, csf::ax::SampleFormat::kPcm16, 0, 0, 2, 0);
    once.system.SetVoiceState(*once.voice, csf::ax::VoiceState::kRun);
    const std::vector<int32_t> frame = once.Frame();
    Check(frame[0] == 100 && frame[1] == 200 && frame[2] == 300 && frame[3] == 0,
          "a one-shot PCM16 voice plays its last sample");

    // Linear interpolation goes from the oldest of the last four samples. So at a ratio of 1 it gives the sample three
    // before.
    Bench linear(bytes, csf::ax::SampleFormat::kPcm16, 0, 0, 3, 0);
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
    const std::array<int16_t, 4> second = {0, 10000, 0, 0};
    Check(Resample(SrcMode::kFourTap, 2, second, 0x01FF) == 9960 &&
              Resample(SrcMode::kFourTap, 2, second, 0x0200) == 9957,
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
    bench.system.RegisterAuxCallback(System::Device::kTv, 0, 0, [&seen](std::span<int32_t* const> channels, uint32_t)
                                     { seen.push_back(channels[0][10]); });
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

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
