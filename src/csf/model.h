// SPDX-License-Identifier: MIT

// What an archive descriptor sets up on archive mode's model of nw::snd and AX (ntdlib's cafe module). The player and
// the ripper's length analysis both set the model up with EngineOptionsFor. A rip's length then comes from the sound
// that the player plays.

#pragma once

#include <cstdint>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/cafe/snd/sequence.h"
#include "xsflib/psf/psf.h"

#include "csf/format.h"
#include "csf/ntdlib_cafe.h"

namespace csf
{

// The model's options for sound `sound_index` of a set with `descriptor`: nw::snd's voice count, AX's start-up (the
// renderer's rate, and where each device's output reaches 48 kHz), and the effects that the game sets up for the sound
// (nwsnd::EffectsFor).
inline nwsnd::EngineOptions EngineOptionsFor(const ArchiveDescriptor& descriptor, uint32_t sound_index)
{
    nwsnd::EngineOptions options;
    options.voice_count = static_cast<int>(descriptor.voice_count);
    options.renderer_48k = (descriptor.ax_settings & kAxRenderer48k) != 0;
    options.upsample_after_final_mix = (descriptor.ax_settings & kAxUpsampleAfterFinalMix) != 0;
    options.effects = nwsnd::EffectsFor(descriptor.effects, sound_index);

    return options;
}

// Sets the variables `variables` of the sequence that `player` plays, by TagVariables' keys: 0 to 15 the sequence's, 16
// to 31 the global ones, and the tracks' from kMaxVariables up. A game sets them through the sound's handle once it has
// started the sound, before the sequence's first tick. Call it after StartSequence. StartSequence returns the player,
// and Start applies the track variables written before it.
inline void SetVariables(nwsnd::SequenceSoundPlayer& player, nwsnd::Engine& engine,
                         const xsflib::psf::Variables& variables)
{
    for (const auto& [key, value] : variables)
    {
        if (key < kMaxVariables)
        {
            player.SetVariable(engine, key, value);
        }
        else
        {
            player.SetTrackVariable((key - kMaxVariables) / kTrackVariables, (key - kMaxVariables) % kTrackVariables,
                                    value);
        }
    }
}

} // namespace csf
