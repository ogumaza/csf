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
#include "xsflib/psf/psf.h"

#include "cafe/rpl.h"
#include "csf/format.h"
#include "rip/rip.h"
#include "title/title.h"

namespace csf::rip
{

// A JAudio1 effect that reads a port that the game sets when it starts the effect, and the values of the port that a
// rip plays the effect with. Each value plays a variant of the effect.
struct PortEffect
{
    uint32_t id; // the effect's JAudio ID
    int port;
    std::vector<int16_t> values;
};

// A JAudio game that game mode can drive: the release, its driver and profile, where its sound files are, and where its
// sequence parser and wave lookups are, for the analysis.
struct JAudioGame
{
    GameRelease release;
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

    // The effects that read a port that the game sets when it starts them. A rip holds a variant of such an effect for
    // each value, after the effect, with the value in the csf_var tag. The JAudio1 driver sets only port 6.
    std::vector<PortEffect> port_effects = {};
};

const std::vector<JAudioGame>& KnownJAudioGames();

// The JAudio game that `game` is, by its title ID and the SHA-1 of its executable, or nullptr. A game whose title isn't
// known is found by the SHA-1 alone.
const JAudioGame* FindJAudioGame(const title::GameFiles& game);

// The files that a rip of a JAudio game holds, by their paths in the title: the executable, and the sound folder's
// files but the left-out ones.
struct JAudioFiles
{
    FileChunk executable;
    std::vector<FileChunk> sound;
};

// Reads the rip's files from `game`. Throws std::runtime_error if one can't be read. The game opens the sound files by
// their paths under the content folder (csf::ContentFiles).
JAudioFiles ReadJAudioFiles(const JAudioGame& jaudio, const title::GameFiles& game);

// A sound that The Wind Waker HD's JaiInfo.asn names: its JAudio ID and its name.
struct AsnSound
{
    uint32_t id;
    std::string name;
};

// The key under which a JaiInfo.asn names the sound with the JAudio ID `id`. Bits 10 and 11 of an effect's ID set how
// long the sound library plays the effect (ntdlib::jaudio::kAafToEnd). The file gives each effect's ID as the game
// starts the effect. A rip plays every effect to its end, with bit 11. An effect's key is its ID without bits 10 and
// 11. Any other sound's key is its ID.
uint32_t AsnNameKey(uint32_t id);

// The sounds of a JaiInfo.asn, in the file's order. The file is a 16-byte header, then a list of 32-byte records. The
// header's last word gives the number of sounds. The first 18 records are the sound table's categories: 16 of effects,
// then one of sequences and one of streams. A record for each sound follows. A record holds a name of up to 28 bytes,
// padded with zeros, then a word. In a sound's record, the word is the sound's ID. Throws std::runtime_error if the
// file is too short for its sounds.
std::vector<AsnSound> ReadAsnSounds(std::span<const uint8_t> asn);

// The sounds that a rip of the game offers. A JAudio2 game's are its start-up archive's sounds, in the sound table's
// order. A JAudio1 game's are the sounds of its start-up archive's sound table but the effects sequence and the
// streams, with the names that its name file gives them (AsnNameKey). The sequences come first, then the effects, each
// in the sound table's order. An effect's ID sets bit 11 (ntdlib::jaudio::kAafToEnd). Throws std::runtime_error if
// `content` lacks one of those files, or if one is damaged.
std::vector<ntdlib::jaudio::SoundEntry> JAudioSounds(const JAudioGame& game,
                                                     const std::map<std::string, std::vector<uint8_t>>& content);

// What the ripper found of a sound by playing it: the wave groups to load, each the wave bank in the high half and the
// group in the low half, and its timing, if the length analysis ran and could work it out.
struct JAudioSound
{
    std::vector<uint32_t> waves;
    std::optional<ntdlib::rip::Timing> timing;
};

// Plays `sound` with `ports` (game::GameDriver::SetPorts) on the game's code to find its wave groups and, with
// `lengths`, its timing. `files` are the title's files that the game opens, by their paths under content/. Throws
// std::runtime_error if the game's code fails.
JAudioSound AnalyzeJAudioSound(const JAudioGame& game, const cafe::Rpl& executable,
                               const std::map<std::string, std::vector<uint8_t>>& files, uint32_t sound, bool lengths,
                               const xsflib::psf::Variables& ports = {});

// The .csflib: a version 2 process descriptor that names the game's driver and holds its profile, with the executable
// and the sound files.
std::vector<uint8_t> BuildJAudioLibrary(const JAudioGame& game, const FileChunk& executable,
                                        const std::vector<FileChunk>& sound_files, const Tags& tags);

// A .minicsf that plays `sound` from the library `lib_name`, with its wave groups. `ports`, if not empty, go in the
// csf_var tag.
std::vector<uint8_t> BuildJAudioMini(uint32_t sound, const std::vector<uint32_t>& waves, const std::string& lib_name,
                                     Tags tags, const xsflib::psf::Variables& ports = {});

} // namespace csf::rip
