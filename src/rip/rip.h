// SPDX-License-Identifier: MIT

// CSF ripper: builds a .csflib for one of a game's sound archives and one .minicsf per sequence, in game mode (the
// game's code, for games with a driver profile) or archive mode (a sound archive played by CSF's model of the SDK's
// sound library), and works out how long each sequence plays.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/rip/archive.h"

#include "csf/format.h"
#include "csf/sar.h"
#include "game/driver.h"
#include "nwsnd/effect_setup.h"
#include "nwsnd/engine.h"
#include "title/title.h"

namespace csf::rip
{

// A game that game mode can drive: the title, its executable, the driver's profile, and the voice count that the game
// gives nw::snd. Archive-mode rips of the game's archives take that voice count.
struct GameProfile
{
    const char* name;
    uint64_t title_id;
    const char* executable_sha1;
    const game::Profile& (*profile)();
    uint32_t voice_count;
};

const std::vector<GameProfile>& KnownProfiles();

// The profile for a game, by its title ID and the SHA-1 of its executable, or nullptr if it has none.
const GameProfile* FindProfile(const title::GameFiles& game);

// The effects that a game puts on its aux buses, for archive-mode rips of its sound archives (docs/csf.md, "Effects"):
// the title, its executable, and the effects in the order that the game appends them. A profile is for one release of
// the game.
struct EffectProfile
{
    const char* name;
    uint64_t title_id;
    const char* executable_sha1;
    std::vector<nwsnd::EffectSetup> effects;
};

const std::vector<EffectProfile>& KnownEffectProfiles();

// The effect profile for a game, by its title ID and the SHA-1 of its executable, or nullptr if it has none.
const EffectProfile* FindEffectProfile(const title::GameFiles& game);

// What another console's sound archive is, from the file's first bytes: "a 3DS sound archive (CSAR)", "a Wii sound
// archive (RSAR)" or "a Switch sound archive (little-endian FSAR)". Nothing for a Wii U sound archive or any other
// file. The Switch's archives are FSAR files too. Every number in them is little-endian. So are the arguments in their
// sequences' bytecode. The model reads the Wii U's big-endian archives only.
std::optional<std::string> ForeignArchive(std::string_view head);

// Game mode: a .csflib holding the game's executable and the sound archive, by their paths in the title. The .csflib
// has a process descriptor containing the driver's profile.
std::vector<uint8_t> BuildLibrary(const FileChunk& executable, const FileChunk& archive, const game::Profile& profile,
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

// Works out how long sequences play by running them on archive mode's model of nw::snd and AX (src/nwsnd and src/ax).
// Analyze can be called from several threads at once.
class LengthAnalyzer
{
public:
    // `archive` is the sound archive, with the group files that it requires added. The model plays it with `options`,
    // and with the effects of `effects` that the game sets up for each sound (nwsnd::EffectsFor). A sequence then plays
    // on while its effects ring.
    explicit LengthAnalyzer(fsar::SoundArchive archive, nwsnd::EngineOptions options = {},
                            std::vector<nwsnd::EffectSetup> effects = {});

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
