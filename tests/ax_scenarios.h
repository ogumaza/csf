// SPDX-License-Identifier: MIT

// The scenarios that check CSF's model of AX, and the runner that plays them. A scenario is a text of directives: the
// voices of every format set up through the calls a game makes, with rate conversion, envelopes, filters, mixes to the
// TV and the GamePad, several priorities, changes made between frames and inside the frame callback, protection, the
// final-mix callback, the upsample stage and a remix matrix, at both renderer rates. The runner gives each frame's
// samples at each stage as lines, and a hash of the lines stands for them. Its hooks let another check follow a
// scenario as it runs.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ax/memory.h"
#include "ax/system.h"

namespace ax_scenarios
{

using csf::ax::Memory;
using csf::ax::System;
using csf::ax::Voice;
using xsflib::testing::Check;

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
inline std::vector<uint8_t> Pcm16Data(Random& random, int samples, int amplitude)
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

inline std::vector<uint8_t> Pcm8Data(Random& random, int samples)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < samples; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(random.Next()));
    }

    return bytes;
}

inline std::vector<uint8_t> AdpcmData(Random& random, int frames, int max_scale)
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

inline std::string MemLines(uint32_t address, const std::vector<uint8_t>& bytes)
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
inline Scenario RandomScenario(int number)
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
        const char* const kBuses[] = {"main", "a", "b", "c"};
        std::string line = "mix " + name + " tv 0";
        for (int i = 1 + random.Below(4); i > 0; --i)
        {
            const int channel = random.Below(6);
            const char* const bus = kBuses[random.Below(4)];
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
        const char* const kTypes[] = {"linear", "4tap8k", "4tap12k", "4tap16k"};
        text += "srctype " + name + " " + (src ? kTypes[random.Below(4)] : "none") + "\n";
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
inline std::vector<Scenario> Scenarios()
{
    std::vector<Scenario> scenarios;
    Random random(0x43534620);
    const std::string kHead = "dump all off\ndump vbus main out out48 dma on\n";

    // ADPCM without rate conversion and with each kind, envelopes and mix ramps.
    {
        std::string text = "init sndcore2\n" + kHead;
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
        std::string text = "init snd_core\n" + kHead;
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
        std::string text = "init sndcore2\n" + kHead;
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
        std::string text = "init sndcore2\n" + kHead;
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
        std::string text = "init snd_core\n" + kHead;
        text += MemLines(0x10000000, AdpcmData(random, 60, 12));
        text += "acquire f 10\noffsets f adpcm 0x10000000 0 2 950 2\n";
        text += std::string("adpcm f ") + kCoefs + " 0 0x2B 0 0\n";
        text += "srctype f none\nve f 0xF000 -40\nmix f tv 0 ch0.main=0x8000\nstate f play\nrender 4\n";
        scenarios.push_back({"envelope_snd_core", text});
    }

    // When changes take effect: between frames, in the frame callback, and with protection.
    {
        std::string text = "init sndcore2\n" + kHead;
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
        std::string text = "init sndcore2\n" + kHead;
        text += MemLines(0x10000000, Pcm16Data(random, 2000, 25000));
        const char* const kNames[] = {"v1", "v2", "v3", "v4", "v5", "v6"};
        const int kPriorities[] = {1, 5, 5, 20, 1, 31};
        for (int i = 0; i < 6; ++i)
        {
            char line[160];
            std::snprintf(line, sizeof(line),
                          "acquire %s %d\noffsets %s pcm16 0x10000000 1 %d 1999 %d\nsrctype %s none\n"
                          "ve %s 0x%x 0\nmix %s tv 0 ch0.main=0x%x ch1.main=0x%x\nstate %s play\n",
                          kNames[i], kPriorities[i], kNames[i], 100 * i, 37 * i, kNames[i], kNames[i],
                          0x3000 + 0x700 * i, kNames[i], 0x2000 + 0x800 * i, 0x6000 - 0x500 * i, kNames[i]);
            text += line;
        }
        text += "render 3\nfree v3\nrender 2\n";
        scenarios.push_back({"priorities", text});
    }

    // The final mix stage: a final-mix callback, the upsampler after it, and a remix matrix folding six channels into
    // two.
    {
        std::string text = "init sndcore2\n" + kHead;
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
        std::string text = "init sndcore2 48k\n" + kHead;
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
// the form that the hashes are made from: frame, stage, device, bus, channel, count, then the values in decimal.
class Runner
{
public:
    // The scenarios use the linear upsampler. Without `linear_upsampler`, every device keeps the FIR filter.
    explicit Runner(bool linear_upsampler = true) : linear_upsampler_(linear_upsampler)
    {
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    // Called with each directive's words before it's carried out, in the frame callback for those that wait for it.
    std::function<void(const std::vector<std::string>& words)> on_directive;

    // Called with the sample data that each mem directive maps, at its address.
    std::function<void(uint32_t address, std::span<const uint8_t> bytes)> on_mem;

    // Called after each frame, once the frame's lines are recorded.
    std::function<void()> after_frame;

    System& Ax()
    {
        return system_;
    }

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
        return word == "tv" ? System::Device::kTv : System::Device::kDrc;
    }

    void Execute(const std::vector<std::string>& w)
    {
        if (on_directive)
        {
            on_directive(w);
        }

        const std::string& op = w[0];
        if (op == "init")
        {
            // Every device takes the runner's upsampler (see the constructor).
            system_.Init(w[1] == "sndcore2" ? System::Library::kSndCore2 : System::Library::kSndCore,
                         w.size() > 2 && w[2] == "48k");
            system_.RegisterFrameCallback([this] { RunCallbackDirectives(); });
            system_.SetDeviceLinearUpsampler(System::Device::kTv, 0, linear_upsampler_);
            for (uint32_t drc = 0; drc < csf::ax::kDrcCount; ++drc)
            {
                system_.SetDeviceLinearUpsampler(System::Device::kDrc, drc, linear_upsampler_);
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
            if (on_mem)
            {
                on_mem(static_cast<uint32_t>(Number(w[1])), block);
            }
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
            const std::map<std::string, uint32_t> kTypes = {
                {"none", 0}, {"linear", 1}, {"4tap8k", 2}, {"4tap12k", 3}, {"4tap16k", 4}};
            system_.SetVoiceSrcType(VoiceNamed(w[1]), kTypes.at(w[2]));
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
        else if (op == "lpf")
        {
            system_.SetVoiceLpf(VoiceNamed(w[1]),
                                {static_cast<uint16_t>(Number(w[2])), static_cast<int16_t>(Number(w[3])),
                                 static_cast<int16_t>(Number(w[4])), static_cast<int16_t>(Number(w[5]))});
        }
        else if (op == "lpfcoefs")
        {
            system_.SetVoiceLpfCoefs(VoiceNamed(w[1]), static_cast<int16_t>(Number(w[2])),
                                     static_cast<int16_t>(Number(w[3])));
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
                if (after_frame)
                {
                    after_frame();
                }
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
        const bool scale = w[2] == "scale";
        const int64_t num = scale ? Number(w[3]) : 1;
        const int shift = scale ? static_cast<int>(Number(w[4])) : 0;
        const auto callback = [=](System::FinalMix& mix)
        {
            for (int32_t* channel : mix.channels)
            {
                for (int i = 0; i < mix.samples; ++i)
                {
                    channel[i] = static_cast<int32_t>((channel[i] * num) >> shift);
                }
            }
        };
        system_.RegisterDeviceFinalMixCallback(device, callback);
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
        const char* const kBuses[] = {"main", "a", "b", "c"};
        const csf::ax::Buses& voice_buses = system_.VoiceBuses();
        for (int bus = 0; bus < csf::ax::kBusCount; ++bus)
        {
            for (int c = 0; c < csf::ax::kTvChannels; ++c)
            {
                Add("vbus tv " + std::string(kBuses[bus]) + " " + std::to_string(c),
                    std::span<const int32_t>(voice_buses.tv[bus][c].data(), n));
            }
        }
        for (int bus = 0; bus < csf::ax::kBusCount; ++bus)
        {
            for (int c = 0; c < csf::ax::kDrcChannels; ++c)
            {
                Add("vbus drc0 " + std::string(kBuses[bus]) + " " + std::to_string(c),
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

    bool linear_upsampler_;
    Memory memory_;
    std::vector<std::vector<uint8_t>> blocks_;
    System system_{memory_};
    std::map<std::string, Voice*> voices_;
    std::vector<std::vector<std::string>> callback_directives_;
    int frame_ = 0;
    std::vector<std::string> lines_;
};

inline uint64_t Hash(const std::vector<std::string>& lines)
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

} // namespace ax_scenarios
