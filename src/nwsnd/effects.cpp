// SPDX-License-Identifier: MIT

#include "nwsnd/effects.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include "ntdlib/nwsnd/console.h"

#include "ax/pb.h"

namespace csf::nwsnd
{
namespace
{

using ntdlib::nwsnd::CafePolicy;

float Float(uint32_t word)
{
    return std::bit_cast<float>(word);
}

// The classes' clamp of a gain or a share: 1 above 1, and below 0 (or for NaN) 0.
float UpToOne(float x)
{
    return x > 1.0f ? 1.0f : CafePolicy::AtLeastZero(x);
}

// The reverbs' pre-delay: the maximum's raw value when the time is longer, else at least 0.
float PreDelay(float time, float max)
{
    return time > max ? max : CafePolicy::AtLeastZero(time);
}

} // namespace

axfx::ReverbHiExp::Params ReverbHiParams(std::span<const uint32_t> words)
{
    axfx::ReverbHiExp::Params p;
    p.early_mode = words[6];
    p.pre_delay_time_max = CafePolicy::AtLeastZero(Float(words[7]));
    p.pre_delay_time = PreDelay(Float(words[0]), Float(words[7]));
    p.fused_mode = words[8];
    p.fused_time = CafePolicy::AtLeastZero(Float(words[1]));
    p.coloration = UpToOne(Float(words[2]));
    p.damping = UpToOne(Float(words[3]));
    p.crosstalk = UpToOne(Float(words[4]));
    p.early_gain = UpToOne(Float(words[9]));
    p.fused_gain = UpToOne(Float(words[10]));
    p.out_gain = UpToOne(Float(words[5]));
    p.send_gain = 0.0f;

    return p;
}

axfx::ReverbStdExp::Params ReverbStdParams(std::span<const uint32_t> words)
{
    axfx::ReverbStdExp::Params p;
    p.early_mode = words[5];
    p.pre_delay_time_max = CafePolicy::AtLeastZero(Float(words[6]));
    p.pre_delay_time = PreDelay(Float(words[0]), Float(words[6]));
    p.fused_mode = words[7];
    p.fused_time = CafePolicy::AtLeastZero(Float(words[1]));
    p.coloration = UpToOne(Float(words[2]));
    p.damping = UpToOne(Float(words[3]));
    p.early_gain = UpToOne(Float(words[8]));
    p.fused_gain = UpToOne(Float(words[9]));
    p.out_gain = UpToOne(Float(words[4]));
    p.send_gain = 0.0f;

    return p;
}

axfx::ChorusExp::Params ChorusParams(std::span<const uint32_t> words)
{
    // The delay: at most 50 ms, and at least 0.1 ms. The rate: at most 2 Hz, and at least 0.1 Hz.
    const float delay = Float(words[0]);
    const float rate = Float(words[2]);
    const float feedback = Float(words[3]);

    axfx::ChorusExp::Params p;
    p.delay_time = delay > 50.0f ? 50.0f : CafePolicy::Max(delay, 0.1f);
    p.depth = UpToOne(Float(words[1]));
    p.rate = rate > 2.0f ? 2.0f : CafePolicy::Max(rate, 0.1f);
    p.feedback = feedback > 0.99f ? 0.99f : CafePolicy::AtLeastZero(feedback);
    p.out_gain = UpToOne(Float(words[4]));
    p.send_gain = 0.0f;

    return p;
}

axfx::DelayExp::Params DelayParams(std::span<const uint32_t> words)
{
    // The longest delay is at least 0.04 ms. A delay longer than the raw maximum takes the raw maximum. Any other delay
    // is at least 0.04 ms.
    const float delay = Float(words[0]);
    const float feedback = Float(words[1]);
    const float max_delay = Float(words[3]);

    axfx::DelayExp::Params p;
    p.max_delay = CafePolicy::Max(max_delay, 0.04f);
    p.delay = delay > max_delay ? max_delay : CafePolicy::Max(delay, 0.04f);
    p.feedback = feedback > 0.99f ? 0.99f : CafePolicy::AtLeastZero(feedback);
    p.lpf = UpToOne(Float(words[4]));
    p.out_gain = UpToOne(Float(words[2]));
    p.send_gain = 0.0f;

    return p;
}

Effects::Effects(ax::System& system, const std::vector<EffectSetup>& setups, const std::array<OutputMode, 2>& modes)
    : system_(&system)
{
    // AXGetInputSamplesPerSec: 32,000 or 48,000, a frame's samples in 3 ms.
    const auto rate = static_cast<uint32_t>(system.SamplesPerFrame() * 1000 / 3);
    for (const EffectSetup& setup : setups)
    {
        if (setup.device > kEffectDeviceDrc || setup.bus > 2 || setup.params.size() < EffectParamWords(setup.type))
        {
            continue;
        }

        // The class's Initialize: the library's Init with the parameters that SetParam converted. AppendEffect skips
        // an effect whose Init fails.
        Model model;
        bool initialized = false;
        switch (setup.type)
        {
        case EffectType::kReverbHi:
            initialized = model.emplace<axfx::ReverbHiExp>().Init(ReverbHiParams(setup.params), rate);
            break;
        case EffectType::kReverbStd:
            initialized = model.emplace<axfx::ReverbStdExp>().Init(ReverbStdParams(setup.params), rate);
            break;
        case EffectType::kChorus:
            initialized = model.emplace<axfx::ChorusExp>().Init(ChorusParams(setup.params), rate);
            break;
        case EffectType::kDelay:
            initialized = model.emplace<axfx::DelayExp>().Init(DelayParams(setup.params), rate);
            break;
        }
        if (!initialized)
        {
            continue;
        }

        const auto device = setup.device == kEffectDeviceTv ? ax::System::Device::kTv : ax::System::Device::kDrc;
        Bus* found = nullptr;
        for (Bus& bus : buses_)
        {
            if (bus.device == device && bus.bus == setup.bus)
            {
                found = &bus;
            }
        }
        if (found == nullptr)
        {
            found = &buses_.emplace_back();
            found->device = device;
            found->bus = setup.bus;
            found->skip = modes[setup.device] == OutputMode::kDpl2;
        }
        found->models.push_back(std::move(model));
    }

    // The buses' callbacks, once the list of buses no longer moves.
    for (std::size_t i = 0; i < buses_.size(); i++)
    {
        const Bus& bus = buses_[i];
        system.SetAuxReturnVolume(bus.device, 0, bus.bus, ax::kUnityVolume);
        system.RegisterAuxCallback(bus.device, 0, bus.bus,
                                   [this, i](std::span<int32_t* const> channels, uint32_t samples)
                                   { Run(buses_[i], channels, samples); });
    }
}

Effects::~Effects()
{
    for (const Bus& bus : buses_)
    {
        system_->RegisterAuxCallback(bus.device, 0, bus.bus, {});
    }
}

void Effects::Run(Bus& bus, std::span<int32_t* const> channels, uint32_t samples)
{
    if (bus.skip || channels.size() < axfx::kChannelCount)
    {
        return;
    }

    const axfx::Buffers frame{channels[0], channels[1], channels[2]};
    for (Model& model : bus.models)
    {
        std::visit([&frame, samples](auto& effect) { effect.Process(frame, samples); }, model);
    }
}

} // namespace csf::nwsnd
