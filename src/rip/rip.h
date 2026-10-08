// SPDX-License-Identifier: MIT

// CSF ripper: builds a .csflib for one of a game's sound archives and one .minicsf per sequence, in game mode (the
// game's code, for games with a driver profile) or archive mode (a sound archive played by CSF's model of the SDK's
// sound library), and works out how long each sequence plays.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "csf/format.h"
#include "fsar/formats.h"
#include "game/driver.h"
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

// The names of the groups `archive` lists with no file: those that the game keeps outside the archive in group files.
std::vector<std::string> ExternalGroups(const fsar::SoundArchive& archive);

// Game mode: a .csflib holding the game's executable and the sound archive, by their paths in the title. The .csflib
// has a process descriptor containing the driver's profile.
std::vector<uint8_t> BuildLibrary(const FileChunk& executable, const FileChunk& archive, const game::Profile& profile,
                                  const Tags& tags);

// Archive mode: a .csflib holding the sound archive and the group files that its sequences require, by their paths in
// the title. The .csflib has an archive descriptor with `settings`' voice count, AX settings and SDK version.
std::vector<uint8_t> BuildArchiveLibrary(const FileChunk& archive, const std::vector<FileChunk>& groups,
                                         ArchiveDescriptor settings, const Tags& tags);

// Either mode: a .minicsf that plays sound `index` from the library `lib_name`.
std::vector<uint8_t> BuildMini(uint32_t index, const std::string& lib_name, Tags tags);

// Lists the data that sound `index` requires and the archive (with its added group files) doesn't hold: its sequence,
// its banks and their wave archives. Such data is kept in another file, or cut off by truncation. Empty when nothing is
// missing.
std::vector<std::string> MissingData(const fsar::SoundArchive& archive, uint32_t index);

// Whether `csfrip --bgm` picks the sound at `index` as music: a sequence whose label has BGM in it, or one that plays
// on a music player. Games give their music players names with BGM, MUSIC, SONG or FANFARE among the words that
// underscores separate (PLAYER_BGM, SONG_PLAYER), even where the music's labels don't say so. A player whose name also
// has the word SE plays sound effects.
bool IsMusic(const fsar::SoundArchive& archive, uint32_t index);

// Filenames for one rip, valid on Windows, macOS and Linux and unique even on case-insensitive filesystems.
class FileNames
{
public:
    // "<base>.csflib".
    std::string Library(const std::string& base);

    // "<index, four digits> <label>.minicsf".
    std::string Mini(uint32_t index, const std::string& label);

private:
    std::string Unique(const std::string& stem, const std::string& extension);

    std::set<std::string> taken_; // the names given out so far, in lowercase
};

// A sequence's length and fade in milliseconds, and whether it never makes a sound.
struct Timing
{
    long long length_ms;
    long long fade_ms;
    bool silent = false;
};

// Works out how long sequences play by running them on archive mode's model of nw::snd and AX (src/nwsnd and src/ax).
// Analyze can be called from several threads at once.
class LengthAnalyzer
{
public:
    // `archive` is the sound archive, with the group files that it requires added. The model plays it with `options`.
    explicit LengthAnalyzer(fsar::SoundArchive archive, nwsnd::EngineOptions options = {});
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
