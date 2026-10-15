// SPDX-License-Identifier: MIT

// The effects that a game puts on AX's aux buses through nw::snd (docs/csf.md, "Effects"): each effect's device and
// bus, its class and the parameters that the game gives the class's SetParam. The archive descriptor holds them for
// archive mode.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace csf::nwsnd
{

// nw::snd's effect classes, by their type in the archive descriptor. The parameters are the words of the class's
// parameter structure, in order: floats as their bits, and the reverbs' modes as integers.
enum class EffectType : uint8_t
{
    // FxReverbHi: preDelayTime, fusedTime, coloration, damping, crossTalk, outGain, earlyMode, preDelayTimeMax,
    // fusedMode, earlyGain, fusedGain.
    kReverbHi = 1,
    // FxReverbStd: preDelayTime, fusedTime, coloration, damping, outGain, earlyMode, preDelayTimeMax, fusedMode,
    // earlyGain, fusedGain.
    kReverbStd = 2,
    // FxChorus: delayTime (ms), depth, rate (Hz), feedback, outGain.
    kChorus = 3,
    // FxDelay: delay (ms), feedback, outGain, maxDelay (ms), lpf.
    kDelay = 4,
};

// The number of words in the parameter structure of `type`, or 0 for a type that isn't one of these.
constexpr uint32_t EffectParamWords(EffectType type)
{
    switch (type)
    {
    case EffectType::kReverbHi:
        return 11;
    case EffectType::kReverbStd:
        return 10;
    case EffectType::kChorus:
    case EffectType::kDelay:
        return 5;
    }

    return 0;
}

// The devices whose aux buses take effects, as AX numbers them: the TV and the first GamePad.
constexpr uint8_t kEffectDeviceTv = 0;
constexpr uint8_t kEffectDeviceDrc = 1;

// One effect on an aux bus. The effect is set up for the sounds in `sounds`. An effect that lists no sounds is set up
// for every sound that no other effect on the same device's bus lists.
struct EffectSetup
{
    uint8_t device = kEffectDeviceTv;
    uint8_t bus = 0; // 0 A, 1 B, 2 C
    EffectType type = EffectType::kDelay;
    std::vector<uint32_t> params;
    std::vector<uint32_t> sounds; // indices in the archive's sound table

    bool operator==(const EffectSetup&) const = default;
};

// Whether `setup` lists `sound`.
inline bool ListsSound(const EffectSetup& setup, uint32_t sound)
{
    return std::find(setup.sounds.begin(), setup.sounds.end(), sound) != setup.sounds.end();
}

// Whether the game sets up `setups[index]` for `sound`. An effect is set up for the sounds that it lists. One that
// lists none is set up for every sound that no other effect on the same device's bus lists.
inline bool SetUpFor(const std::vector<EffectSetup>& setups, std::size_t index, uint32_t sound)
{
    const EffectSetup& setup = setups[index];
    if (!setup.sounds.empty())
    {
        return ListsSound(setup, sound);
    }

    const auto takes_sound = [&setup, sound](const EffectSetup& other)
    {
        return other.device == setup.device && other.bus == setup.bus && ListsSound(other, sound);
    };

    return std::none_of(setups.begin(), setups.end(), takes_sound);
}

// The effects of `setups` that the game sets up for `sound` (see SetUpFor), in order.
inline std::vector<EffectSetup> EffectsFor(const std::vector<EffectSetup>& setups, uint32_t sound)
{
    std::vector<EffectSetup> found;
    for (std::size_t i = 0; i < setups.size(); i++)
    {
        if (SetUpFor(setups, i, sound))
        {
            found.push_back(setups[i]);
        }
    }

    return found;
}

} // namespace csf::nwsnd
