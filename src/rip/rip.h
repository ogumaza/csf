// SPDX-License-Identifier: MIT

// CSF ripper: builds a .csflib for one of a game's sound archives and one .minicsf per sequence, in game mode (the
// game's code, for games with a driver profile) or archive mode (a sound archive played by CSF's model of the SDK's
// sound library), and works out how long each sequence plays.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/rip/archive.h"

#include "csf/format.h"
#include "csf/ntdlib_cafe.h"
#include "csf/sar.h"
#include "game/nwsnd_driver.h"
#include "title/title.h"

namespace csf::rip
{

// The SHA-1 of the game's executable, in lowercase hexadecimal, or nothing if it can't be read.
std::optional<std::string> ExecutableSha1(const title::GameFiles& game);

// One release of one game, as the ripper's tables name it: the game's name, its title ID, and the SHA-1 of its
// executable in lowercase hexadecimal. The SHA-1 identifies the release.
struct GameRelease
{
    const char* name;
    uint64_t title_id;
    const char* executable_sha1;
};

// The entry of `known` for the game's title and executable, or nullptr. Each entry gives its GameRelease as `release`.
// A game whose folder holds another title's XML files has no known title (title::GameFiles::foreign_title_id). Such a
// game is found by its executable alone.
template <typename Known>
const Known* FindKnownGame(const std::vector<Known>& known, const title::GameFiles& game)
{
    const std::optional<std::string> sha1 = ExecutableSha1(game);
    for (const Known& entry : known)
    {
        const bool same_title = game.title_id == entry.release.title_id || game.foreign_title_id != 0;
        if (sha1 && same_title && *sha1 == entry.release.executable_sha1)
        {
            return &entry;
        }
    }

    return nullptr;
}

// A game that game mode can drive: the release, the driver's profile, and the voice count that the game gives nw::snd.
// Archive-mode rips of the game's archives take that voice count.
struct GameProfile
{
    GameRelease release;
    const game::NwSndProfile& (*profile)();
    uint32_t voice_count;
};

const std::vector<GameProfile>& KnownProfiles();

// The profile for a game, by its title ID and the SHA-1 of its executable, or nullptr if it has none. A game whose
// title isn't known is found by the SHA-1 alone.
const GameProfile* FindProfile(const title::GameFiles& game);

// The archive descriptor's settings for the rips of `game`'s sound archives: the game's SDK version and, with a driver
// profile, the voice count and AX's settings that the game gives nw::snd. An archive-mode rip adds the effects of the
// game's effect profile (FindEffectProfile).
ArchiveDescriptor GameSettings(const title::GameFiles& game, const GameProfile* profile);

// The effects that a game puts on its aux buses, for archive-mode rips of its sound archives (docs/csf.md, "Effects"):
// the release, and the effects in the order that the game appends them.
struct EffectProfile
{
    GameRelease release;
    std::vector<nwsnd::EffectSetup> effects;
};

const std::vector<EffectProfile>& KnownEffectProfiles();

// The effect profile for a game, by its title ID and the SHA-1 of its executable, or nullptr if it has none. A game
// whose title isn't known is found by the SHA-1 alone.
const EffectProfile* FindEffectProfile(const title::GameFiles& game);

// What another console's sound archive is, from the file's first bytes: "a 3DS sound archive (CSAR)", "a Wii sound
// archive (RSAR)" or "a Switch sound archive (little-endian FSAR)". Nothing for a Wii U sound archive or any other
// file. The Switch's archives are FSAR files too. Every number in them is little-endian. So are the arguments in their
// sequences' bytecode. The model reads the Wii U's big-endian archives only.
std::optional<std::string> ForeignArchive(std::string_view head);

// Game mode: a .csflib holding the game's executable and the sound archive, by their paths in the title, for the
// sequences at `sounds`, by their indices in the archive. In the library's copy of the archive, only the files that
// those sequences play keep their bytes. The rest of the FILE block is zeros. The game reads the same bytes in the same
// places (see LoadedArchive in rip.cpp). The .csflib has a process descriptor containing the driver's profile.
std::vector<uint8_t> BuildLibrary(const FileChunk& executable, const FileChunk& archive,
                                  std::span<const uint32_t> sounds, const game::NwSndProfile& profile,
                                  const Tags& tags);

// CSF's types for the libraries and minis that a rip writes in archive mode or either mode (ntdlib::rip's
// BuildArchiveLibrary and BuildMini). An archive-mode library holds the sound archive and the group files that its
// sequences require, with an archive descriptor.
struct RipFormat
{
    using ProgramBuilder = csf::ProgramBuilder;
    using ArchiveDescriptor = csf::ArchiveDescriptor;
    using FileChunk = csf::FileChunk;

    static constexpr uint8_t kPsfVersion = kPsfVersionCsf;
};

// A sequence's length and fade in milliseconds, and whether it never makes a sound (xsflib::rip).
using Timing = ntdlib::rip::Timing;

// Works out how long sequences play by running them on archive mode's model of nw::snd and AX (ntdlib's cafe module),
// set up as the player sets it up for a rip's descriptor (csf/model.h). Analyze can be called from several threads at
// once.
class LengthAnalyzer
{
public:
    // `archive` is the sound archive, with the group files that it requires added. The model plays it with the voice
    // count, AX's settings and the effects of `settings`, the descriptor that the rip gets. A sequence with effects
    // plays on while they ring.
    explicit LengthAnalyzer(fsar::SoundArchive archive, ArchiveDescriptor settings = {});

    ~LengthAnalyzer();

    // A looping sequence gets two loops and a 10 s fade. Any other sequence runs to its end, or to where it falls
    // silent for good. No sequence runs for more than 600 s. A sequence that never makes a sound is marked silent, and
    // gets the 0.5 s that follows a sequence's last sound. Returns nullopt when the length can't be worked out (and
    // then no length tag is written). Throws std::bad_alloc when memory runs out.
    std::optional<Timing> Analyze(uint32_t sound_index) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace csf::rip
