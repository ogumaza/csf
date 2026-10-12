// SPDX-License-Identifier: MIT

// The ripper for games whose sound runs on JAudio, which game mode drives with the game's code (game/jaudio1_driver.h
// and game/jaudio2_driver.h). A rip holds one .csflib with the game's executable and its sound files, and a .minicsf
// for each sound: the sound's JAudio ID, and the wave groups that the sound requires, in the set's waves file. The
// ripper plays each sound twice on the game's code (ntdlib/jaudio/game_lengths.h): once with only the game's common
// wave groups loaded, to find the waves that its notes look up, and once with their groups loaded, to work out its
// length.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "ntdlib/rip/archive.h"

#include "cafe/rpl.h"
#include "csf/format.h"
#include "title/title.h"

namespace csf::rip
{

// A JAudio game that game mode can drive: the title, its executable, its driver and profile, where its sound files are,
// and where its sequence parser and wave lookups are, for the analysis.
struct JAudioGame
{
    const char* name;
    uint64_t title_id;
    const char* executable_sha1;
    GameDriverKind driver;
    std::vector<uint32_t> (*profile_words)();
    const char* startup_archive;       // the start-up archive, by its path in the title
    const char* sound_folder;          // the folder that the game's sound reads, by its path in the title
    std::vector<const char*> left_out; // folders in it that no sequence reads: the streams
    const char* music_prefix;          // the music's names start with it
    ntdlib::jaudio::GameWatch watch;

    // A JAudio1 game's start-up archive names no sound. The file that names them (JaiInfo.asn), by its path in the
    // title, in the sound folder.
    const char* name_file = nullptr;
};

const std::vector<JAudioGame>& KnownJAudioGames();

// The JAudio game that `game` is, by its title ID and the SHA-1 of its executable, or nullptr.
const JAudioGame* FindJAudioGame(const title::GameFiles& game);

// The files that a rip of a JAudio game holds, by their paths in the title: the executable, and the sound folder's
// files but the left-out ones.
struct JAudioFiles
{
    FileChunk executable;
    std::vector<FileChunk> sound;
};

// Reads the rip's files from `game`. Nothing, with `error` set, if one can't be read.
std::optional<JAudioFiles> ReadJAudioFiles(const JAudioGame& jaudio, const title::GameFiles& game, std::string& error);

// The sound files by their paths under content/, as the game opens them.
std::map<std::string, std::vector<uint8_t>> ContentFiles(const std::vector<FileChunk>& sound);

// A sound that The Wind Waker HD's JaiInfo.asn names: its JAudio ID and its name.
struct AsnSound
{
    uint32_t id;
    std::string name;
};

// The sounds of a JaiInfo.asn, in the file's order. The file is a 16-byte header, then a list of 32-byte records. The
// header's last word gives the number of sounds. The first 18 records are the sound table's categories: 16 of effects,
// then one of sequences and one of streams. A record for each sound follows. A record holds a name of up to 28 bytes,
// padded with zeros, then a word. In a sound's record, the word is the sound's ID. Throws std::runtime_error if the
// file is too short for its sounds.
std::vector<AsnSound> ReadAsnSounds(std::span<const uint8_t> asn);

// The sounds that a rip of the game offers, in the sound table's order. A JAudio2 game's are its start-up archive's
// sounds. A JAudio1 game's are the sequences of its start-up archive's sound table but the effects sequence, with the
// names that its name file gives their IDs. Throws std::runtime_error if `content` lacks one of those files, or if one
// is damaged.
std::vector<ntdlib::jaudio::SoundEntry> JAudioSounds(const JAudioGame& game,
                                                     const std::map<std::string, std::vector<uint8_t>>& content);

// What the ripper found of a sound by playing it: the wave groups to load, each the wave bank in the high half and the
// group in the low half, and its timing, if the length analysis ran and could work it out.
struct JAudioSound
{
    std::vector<uint32_t> waves;
    std::optional<ntdlib::rip::Timing> timing;
};

// Plays `sound` on the game's code to find its wave groups and, with `lengths`, its timing. `files` are the title's
// files that the game opens, by their paths under content/. Throws std::runtime_error if the game's code fails.
JAudioSound AnalyzeJAudioSound(const JAudioGame& game, const cafe::Rpl& executable,
                               const std::map<std::string, std::vector<uint8_t>>& files, uint32_t sound, bool lengths);

// The .csflib: a version 2 process descriptor that names the game's driver and holds its profile, with the executable
// and the sound files.
std::vector<uint8_t> BuildJAudioLibrary(const JAudioGame& game, const FileChunk& executable,
                                        const std::vector<FileChunk>& sound_files, const Tags& tags);

// A .minicsf that plays `sound` from the library `lib_name`, with its wave groups.
std::vector<uint8_t> BuildJAudioMini(uint32_t sound, const std::vector<uint32_t>& waves, const std::string& lib_name,
                                     Tags tags);

} // namespace csf::rip
