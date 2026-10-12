// SPDX-License-Identifier: MIT

// CSF player core: loads a .csflib/.minicsf set and produces its audio, in game mode (the game's sound code, on an
// emulated Espresso with Cafe OS modelled at a high level) or archive mode (CSF's model of the SDK's sound library).
// Both mix with CSF's model of AX. Portable C++ with no platform audio or UI. The CLI renderer and the foobar2000
// component use it.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/player/playback.h"
#include "xsflib/player/player.h"

#include "csf/format.h"

namespace csf
{

// Loading, starting, rendering and the tags' length, fade and volume come from xsflib's PlayerBase. Game mode's start
// sets the game's sound library up and starts the sound with the game's code. Archive mode's start starts the sound on
// the model. Start can be called again to restart from the beginning.
class Player : public xsflib::player::PlayerBase<Player, LoadedSet>
{
public:
    // The rate players report: 48,000 Hz.
    static constexpr int kSampleRate = kPlayerSampleRate;

    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

private:
    friend class xsflib::player::PlayerBase<Player, LoadedSet>;

    // A mode's engine: archive mode's model, or game mode's driver.
    class Source;

    static constexpr const char* kNotPlayable = "no process or archive descriptor";
    static constexpr const char* kStartFailure = "the sound couldn't start: ";
    static constexpr const char* kStepFailure = "emulation failed: ";

    static std::optional<std::string> LoadFile(const std::string& path, const FileReader& reader, LoadedSet& set)
    {
        return LoadSet(path, reader, set);
    }

    // Releases the previous set's source, with its archive.
    void Unload();

    bool Playable() const
    {
        return set_.descriptor || set_.archive;
    }

    // Starts the sound: game mode on the game's driver, archive mode on the model. In archive mode, the first start
    // moves the bytes of the sound archive and the group files out of the set and into the source's archive. A large
    // archive is then held once, and their chunks are empty after that. Later starts reuse the archive.
    bool StartSource();

    // Runs one frame of the sound, 3 ms.
    bool RunStep();

    // Whether the sound has finished: in game mode, the game's sound handle is free again; in archive mode, the
    // sequence has ended and no channel sounds.
    bool Finished();

    std::vector<int16_t>& Output()
    {
        return output_;
    }

    std::unique_ptr<Source> source_; // made at the first start, and kept for restarts
    std::vector<int16_t> output_;    // samples produced and not dropped yet
};

// Playback of a set with length, fade and volume, from xsflib: the CLI renderer and the foobar2000 component play a
// file identically. CSF's player takes no snapshots. A backward seek renders from the start.
using Playback = xsflib::player::Playback<Player>;
using PlaybackOptions = xsflib::player::PlaybackOptions<Player>;

} // namespace csf
