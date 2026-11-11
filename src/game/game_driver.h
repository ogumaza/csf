// SPDX-License-Identifier: MIT

// Game mode's drivers. A driver runs a game's sound code on the emulated system, starts one of its sounds and renders
// it. The process descriptor names the driver (docs/csf.md, "Process descriptor"), and the descriptor's profile gives
// the addresses and settings in the game's code that the driver requires.
//
// The emulated system is the PowerPC machine, with Cafe OS's coreinit and sndcore2 at a high level. Time passes a frame
// of AX at a time. In each frame, outside the game's waits, the game's threads run. Then the driver runs what the
// game's main loop runs at that time.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/cafe/ax/system.h"
#include "xsflib/psf/psf.h"

#include "cafe/coreinit.h"
#include "cafe/machine.h"
#include "cafe/rpl.h"
#include "cafe/sndcore2.h"
#include "csf/jaudio2_files.h"
#include "csf/ntdlib_cafe.h"

namespace csf::game
{

class GameDriver
{
public:
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;
    virtual ~GameDriver() = default;

    // Starts the sound `sound`, the ID in the set's SND chunk, with the driver's main sound file at `path` (under
    // /vol/content). A JAudio driver loads the wave groups `waves` first, each the wave bank in the high half and the
    // group in the low half. Throws std::runtime_error if the game's code refuses.
    virtual void Start(const std::string& path, uint32_t sound, std::span<const uint32_t> waves) = 0;

    // The values that the set's csf_var tag gives the sound, by number, for Start. For the JAudio1 driver they are the
    // sound's ports (csf::TagPorts). It passes port 6 to the game as the sound's map information: the value that a game
    // passes with an effect, such as the material of the ground under a footstep. It sets the other ports after the
    // start. The JAudio2 driver writes each port to the sound's root track once the sound has started, as Twilight
    // Princess HD's code writes a piece's status to port 9. For the nw::snd driver they are the sequence's variables
    // (csf::TagVariables).
    void SetTagValues(xsflib::psf::Variables values)
    {
        tag_values_ = std::move(values);
    }

    // The children of track 0 that Start mutes after it starts the sound, bit n for child n (csf::MutedChildren). The
    // JAudio2 driver mutes them as Twilight Princess HD's bgmStart does. The other drivers mute none.
    void SetMutedChildren(uint32_t children)
    {
        muted_children_ = children;
    }

    // Children of track 0 that Start sets to other volumes after it mutes the muted ones (csf::ChildVolumes). The
    // JAudio2 driver sets them as Twilight Princess HD's code sets a piece's tracks for a state of the game. The other
    // drivers set none.
    void SetChildVolumes(std::vector<ChildVolume> groups)
    {
        child_volumes_ = std::move(groups);
    }

    // The statuses that the sound gets on port 9 of its root track in each of the game's frames, by the section on port
    // 10 (csf::SectionStatuses). The JAudio2 driver writes them as Twilight Princess HD's fieldBgmFramework does, from
    // before the sound's first tick on. The other drivers write none.
    void SetSectionStatuses(std::vector<SectionStatus> statuses)
    {
        section_statuses_ = std::move(statuses);
    }

    // Runs one frame of AX, 3 ms, and returns the TV's output: 144 stereo pairs at 48 kHz.
    std::span<const int16_t> Frame();

    // Whether the sound has ended: the game has cleared the first word of the sound's handle (HandleAddress).
    bool IsFinished();

    // The machine that runs the game, for the JAudio games' length analysis (ntdlib/jaudio/game_lengths.h), and for
    // tests that read the game's state or call its code.
    cafe::Machine& Machine()
    {
        return machine_;
    }

    // The guest's address of the handle that Start started the sound with, or 0 before Start. The handle's first word
    // holds the sound until the game lets go of it.
    uint32_t HandleAddress() const
    {
        return handle_;
    }

    // The files that the game has read from so far, by their paths under content/ as Load was given them
    // (cafe::Coreinit::FilesRead).
    std::set<std::string> FilesRead() const;

    // AX, for switching on Cemu's departures (ntdlib/cafe/ax/cemu_departures.h).
    ax::System& AxSystem()
    {
        return sndcore2_.System();
    }

protected:
    // A machine with `machine_heap` bytes of heap. A driver may register handlers of its own before Load.
    explicit GameDriver(uint32_t machine_heap);

    // Loads the game's executable, starts coreinit with a default heap of `default_heap` bytes, and starts sndcore2.
    // The game opens `files` by their paths under /vol/content. From then on, time passes a frame at a time while the
    // game waits for its threads.
    void Load(const cafe::Rpl& executable, uint32_t default_heap, std::map<std::string, std::vector<uint8_t>> files);

    // Runs what the game's main loop runs in a frame, after the game's threads. Frame calls it from Load on. The frames
    // of the driver's start-up call it too.
    virtual void RunMainLoop() = 0;

    // Whether the time has reached `count` / `per_second` seconds: whether AX's frames so far, 3 ms each, add up to at
    // least count * 1000 / per_second ms. A step of the main loop that comes `per_second` times a second, the first at
    // time 0, is then due for the `count`-th time, counting from 0.
    bool TimeReaches(uint64_t count, uint64_t per_second) const;

    // The drivers call the game's code through these members on almost every line. Google's style makes data members
    // private. These are protected instead.
    cafe::Machine machine_;
    cafe::Coreinit coreinit_{machine_};
    cafe::Sndcore2 sndcore2_{machine_};
    uint32_t handle_ = 0;                         // the guest's address of the sound's handle, once Start has made it
    xsflib::psf::Variables tag_values_;           // the values that SetTagValues gave
    uint32_t muted_children_ = 0;                 // the children that SetMutedChildren gave
    std::vector<ChildVolume> child_volumes_;      // the groups that SetChildVolumes gave
    std::vector<SectionStatus> section_statuses_; // the statuses that SetSectionStatuses gave

private:
    uint64_t quarter_ticks_ = 0;     // the time since the driver started, in quarters of a tick
    uint64_t frames_ = 0;            // AX's frames so far
    std::vector<std::string> paths_; // the paths of the files that Load was given, under content/
};

// The number of words in the driver profile `Profile`. A driver's profile holds nothing but 32-bit words. The process
// descriptor's profile holds them in the order of the profile's fields (docs/csf.md, "Driver").
template <typename Profile>
inline constexpr uint32_t kProfileWordCount = sizeof(Profile) / 4;

// A driver profile's words, for the process descriptor.
template <typename Profile>
std::vector<uint32_t> ProfileWords(const Profile& profile)
{
    const auto words = std::bit_cast<std::array<uint32_t, kProfileWordCount<Profile>>>(profile);
    return {words.begin(), words.end()};
}

// A driver profile from the process descriptor's words, or nothing if their number is none of kProfileWordCount,
// `earlier` and `earliest`: the numbers of words of the profile in earlier versions of the format. A shorter profile
// leaves the later fields 0.
template <typename Profile>
std::optional<Profile> ProfileFromWords(std::span<const uint32_t> words,
                                        std::size_t earlier = kProfileWordCount<Profile>,
                                        std::size_t earliest = kProfileWordCount<Profile>)
{
    std::array<uint32_t, kProfileWordCount<Profile>> fields{};
    if (words.size() != earliest && words.size() != earlier && words.size() != fields.size())
    {
        return std::nullopt;
    }

    std::copy(words.begin(), words.end(), fields.begin());

    return std::bit_cast<Profile>(fields);
}

// The driver `driver` (a GameDriverKind) with the profile `words`, for the game whose executable is `executable`.
// `files` are the title's files that the game opens, by their paths under /vol/content. The driver copies the
// executable into the game's memory. `executable` may then be freed. Returns nullptr and sets `error` if this player
// has no such driver, or if the profile's words don't fit it. Throws std::runtime_error if the game's code fails.
std::unique_ptr<GameDriver> MakeGameDriver(uint32_t driver, const std::vector<uint32_t>& words,
                                           const cafe::Rpl& executable,
                                           std::map<std::string, std::vector<uint8_t>> files, std::string& error);

} // namespace csf::game
