// SPDX-License-Identifier: MIT

// Game mode's nw::snd driver. The driver sets a game's NintendoWare sound library (nw::snd) up with the game's code,
// the way the game does. The driver opens a sound archive, loads a sound's data and starts the sound. Then the driver
// runs AX's frames with the game's threads, and calls the archive player's update every 60th of a second. The game's
// main loop does the same. An NwSndProfile gives the addresses of the functions that the driver calls in the game's
// executable, and the settings that the game uses.

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "cafe/rpl.h"
#include "game/game_driver.h"

namespace csf::game
{

// The addresses and settings in a game's executable that the driver requires. The process descriptor's profile holds
// them as words, in this order (docs/csf.md, "Driver").
struct NwSndProfile
{
    // The function that the executable's start-up code calls to run the static constructors.
    uint32_t static_init = 0;

    // A word in nw::snd that the game sets before starting the sound system, and the value that it sets (if any).
    uint32_t setting_address = 0;
    uint32_t setting_value = 0;

    // AX's start-up, in the bits of the archive descriptor's AX settings (kAxRenderer48k and kAxUpsampleAfterFinalMix
    // in csf/format.h): the 48 kHz renderer or the 32 kHz one, and each device's upsampling after the final mix.
    uint32_t ax_settings = 0;

    // nw::snd's functions. The sound system: SoundSystemParam's constructor, GetRequiredMemSize and Initialize.
    uint32_t system_param = 0;
    uint32_t system_size = 0;
    uint32_t system_init = 0;

    // The sound heap: SoundHeap's constructor and Create, and the size the game gives it.
    uint32_t heap_construct = 0;
    uint32_t heap_create = 0;
    uint32_t heap_size = 0;

    // The archive: FsSoundArchive's constructor, the size of its buffer, Open (with the game's two settings for it),
    // the file header's place in it, the sizes of the info and string blocks and their loaders.
    uint32_t archive_construct = 0;
    uint32_t archive_buffer_size = 0;
    uint32_t archive_open = 0;
    uint32_t open_priority = 0;
    uint32_t open_cache = 0;
    uint32_t file_header = 0;
    uint32_t info_size = 0;
    uint32_t load_info = 0;
    uint32_t strings_size = 0;
    uint32_t load_strings = 0;

    // The data manager: its constructor, GetRequiredMemSize, Initialize, and where its loader is in it.
    uint32_t manager_construct = 0;
    uint32_t manager_size = 0;
    uint32_t manager_init = 0;
    uint32_t manager_loader = 0;

    // The archive player: its constructor, GetRequiredMemSize, GetRequiredStreamBufferSize (which the game multiplies
    // by `stream_buffers`), Initialize and Update.
    uint32_t player_construct = 0;
    uint32_t player_size = 0;
    uint32_t stream_buffer_size = 0;
    uint32_t stream_buffers = 1;
    uint32_t player_init = 0;
    uint32_t player_update = 0;

    // SoundArchiveLoader::LoadData and SoundStartable::StartSound, by the sound's ID.
    uint32_t load_data = 0;
    uint32_t start_sound = 0;
};

// Animal Crossing: amiibo Festival's profile: its executable's addresses, and the settings of NdSysAudio, its wrapper
// round nw::snd (NdSysAudio's constructor at 0x020EE63C, initializeLocal at 0x020EEF48, SoundArchive::load at
// 0x020F6560 and updatePlayer at 0x020EFAAC).
const NwSndProfile& AmiiboFestival();

// The voices that amiibo Festival gives nw::snd. NdSysAudio's constructor sets nw::snd's channel count to 78
// (0x020EE8CC). Game mode sets the count through the profile's setting. Archive mode's rips of the game's archives take
// it too.
constexpr uint32_t kAmiiboFestivalVoices = 78;

class NwSndDriver final : public GameDriver
{
public:
    // Loads the game's executable and starts nw::snd. `files` are the title's files the game opens, by their paths
    // under /vol/content. Throws std::runtime_error if the game's code fails.
    NwSndDriver(const NwSndProfile& profile, const cafe::Rpl& executable,
                std::map<std::string, std::vector<uint8_t>> files);

    NwSndDriver(const NwSndDriver&) = delete;
    NwSndDriver& operator=(const NwSndDriver&) = delete;

    // Opens the archive at `path` (under /vol/content), loads the data of the sound whose ID is `sound` (nw::snd's ID:
    // 0x01000000 and the sound's index) and starts it. nw::snd loads the sound's waves itself. Start ignores `waves`.
    // Throws std::runtime_error if nw::snd refuses. The handle is a SoundHandle. Its first word is its sound, and
    // SoundHandle::DetachSound (0x022B9D94) clears that word once the sound has ended.
    void Start(const std::string& path, uint32_t sound, std::span<const uint32_t> waves) override;

private:
    // The main loop updates the archive player every 60th of a second, once Start has made the player.
    void RunMainLoop() override;

    // Start's steps before it loads the sound's data. OpenArchive opens the archive at `path` (under /vol/content),
    // reads its info and its labels, and returns it. SetUpDataManager returns the archive's data manager, and
    // SetUpPlayer sets up the archive player in player_. Each throws std::runtime_error if nw::snd refuses.
    uint32_t OpenArchive(const std::string& path);
    uint32_t SetUpDataManager(uint32_t archive);
    void SetUpPlayer(uint32_t archive, uint32_t manager);

    uint32_t Allocate(uint32_t size, uint32_t align = 64);
    uint32_t String(const std::string& text);

    NwSndProfile profile_;
    uint32_t heap_ = 0;
    uint32_t player_ = 0;
    uint64_t updates_ = 0; // the archive player's updates so far
};

} // namespace csf::game
