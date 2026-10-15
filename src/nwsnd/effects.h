// SPDX-License-Identifier: MIT

// nw::snd's effect classes on AX's aux buses (FxReverbHi, FxReverbStd, FxChorus and FxDelay), run on the models of the
// system library's effects in src/axfx. Each function's comment gives the address of the routine it follows in Animal
// Crossing: amiibo Festival's executable.
//
// A game makes an effect, sets its parameters and appends it to a bus. The class's SetParam converts the parameters
// into the library's. SoundSystem::AppendEffect runs the class's Initialize. Initialize calls the library's Init.
// AppendEffect appends the effect only if Init succeeds. The first effect on a bus registers HardwareManager's aux
// callback (AuxCallbackFunc, 0x022980C8) with AX, and sets the bus's return volume to 0x8000 (AppendEffect,
// 0x022983CC). In each frame, the callback runs the bus's effects in the order that the game appended them. Each
// class's UpdateBuffer (for FxDelay, 0x022B0F2C) passes the library's callback the bus's first three channels.
// UpdateBuffer does nothing while the device mixes for Dolby Pro Logic II.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "ax/system.h"
#include "axfx/chorus.h"
#include "axfx/delay.h"
#include "axfx/reverb_hi.h"
#include "axfx/reverb_std.h"
#include "nwsnd/effect_setup.h"
#include "nwsnd/util.h"

namespace csf::nwsnd
{

// The conversions of the classes' SetParam: the parameter structure's words, at least EffectParamWords(type) of them,
// into the library's parameters. busIn and busOut are always null, and the send gain is 0.
axfx::ReverbHiExp::Params ReverbHiParams(std::span<const uint32_t> words);   // FxReverbHi, 0x022B1B3C
axfx::ReverbStdExp::Params ReverbStdParams(std::span<const uint32_t> words); // FxReverbStd, 0x022B151C
axfx::ChorusExp::Params ChorusParams(std::span<const uint32_t> words);       // FxChorus, 0x022B0FB0
axfx::DelayExp::Params DelayParams(std::span<const uint32_t> words);         // FxDelay, 0x022B09B0

// The effects of one sound, on the aux buses of `system`. The effects are set up and appended when the object is made,
// and taken off the buses when it's destroyed.
class Effects
{
public:
    // Sets up `setups` in order, at the rate of the renderer that `system` started with. An effect whose Init fails
    // isn't appended. The effects skip their frames on a device whose mode in `modes` (the TV's, then the GamePad's)
    // is Dolby Pro Logic II.
    Effects(ax::System& system, const std::vector<EffectSetup>& setups, const std::array<OutputMode, 2>& modes);

    Effects(const Effects&) = delete;
    Effects& operator=(const Effects&) = delete;

    ~Effects();

    // Whether any effect is on a bus.
    bool Any() const
    {
        return !buses_.empty();
    }

private:
    using Model = std::variant<axfx::ReverbHiExp, axfx::ReverbStdExp, axfx::ChorusExp, axfx::DelayExp>;

    // A bus with effects: the device, the bus, whether the effects skip their frames, and the effects in order.
    struct Bus
    {
        ax::System::Device device = ax::System::Device::kTv;
        int bus = 0;
        bool skip = false;
        std::vector<Model> models;
    };

    // The callback of `bus` (AuxCallbackFunc): each effect's UpdateBuffer in turn.
    void Run(Bus& bus, std::span<int32_t* const> channels, uint32_t samples);

    ax::System* system_;
    std::vector<Bus> buses_;
};

} // namespace csf::nwsnd
