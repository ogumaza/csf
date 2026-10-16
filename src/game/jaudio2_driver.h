// SPDX-License-Identifier: MIT

// Game mode's JAudio2 driver, for a game whose sound runs on JAudio2 with the game's Z2 sound layer above it. The
// game's JSystem framework provides the heaps and the threads. JAudio2 mixes on the CPU, in the game's DSP thread, and
// hands the mix to AX through the devices' final-mix callbacks. The driver does what the game's start-up does for
// sound, without its graphics, in the order that the profile's game does it. Then it loads the sound's wave groups and
// starts the sound. The game's frame process runs 30 times a second, as the game's frames do.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cafe/coreinit.h"
#include "cafe/machine.h"
#include "cafe/rpl.h"
#include "cafe/sndcore2.h"
#include "game/game_driver.h"

namespace csf::game
{

// The addresses and settings in a JAudio2 game's executable that the driver requires. The process descriptor's profile
// holds them as words, in this order (docs/csf.md, "JAudio2 driver").
struct JAudio2Profile
{
    // The machine's heap, and the default heap's share of it.
    uint32_t machine_heap = 0;
    uint32_t default_heap = 0;

    // The function that runs the executable's static constructors.
    uint32_t static_init = 0;

    // The variable that points to the reset data, the data's size, and the byte in it that must be 1 for the audio
    // set-up to run.
    uint32_t reset_data = 0;
    uint32_t reset_size = 0;
    uint32_t reset_gate = 0;

    // JFWSystem's first set-up (the file system, the root heaps and the system heap). The word that it reads as the
    // root heaps' setting, and the setting. The word that it reads as the system heap's size, and the size.
    uint32_t first_init = 0;
    uint32_t root_setting = 0;
    uint32_t root_setting_value = 0;
    uint32_t system_heap_size_variable = 0;
    uint32_t system_heap_size = 0;

    // The variables that hold the root heap, the second arena's root heap and the system heap.
    uint32_t root_heap = 0;
    uint32_t arena_root_heap = 0;
    uint32_t system_heap = 0;

    // The assert heap's creator, which takes the root heap.
    uint32_t create_assert_heap = 0;

    // JKRAram::create, its audio and graphics sizes, and its threads' priorities: the stream's, the decompressor's and
    // the pieces', a byte each from bit 16 down.
    uint32_t create_aram = 0;
    uint32_t aram_audio_size = 0;
    uint32_t aram_graph_size = 0;
    uint32_t aram_priorities = 0;

    // The creators of the command heap, the archive heap (from the second arena's root heap) and the game heap, with
    // their sizes. The game heap holds the audio set-up's data.
    uint32_t create_command_heap = 0;
    uint32_t command_heap_size = 0;
    uint32_t create_archive_heap = 0;
    uint32_t archive_heap_size = 0;
    uint32_t create_game_heap = 0;
    uint32_t game_heap_size = 0;

    // The zelda heap's creator, and what the heap leaves free in the system heap.
    uint32_t create_zelda_heap = 0;
    uint32_t zelda_heap_margin = 0;

    // JKRHeap's getFreeSize, becomeCurrentHeap and getCurrentHeap.
    uint32_t get_free_size = 0;
    uint32_t become_current_heap = 0;
    uint32_t get_current_heap = 0;

    // JKRAramStream::setTransBuffer and its buffer's size, JKRThreadSwitch::createManager, the JKRThread constructor
    // (given no object, a thread and a message count) and the disc thread's creator (given a priority).
    uint32_t set_trans_buffer = 0;
    uint32_t trans_buffer_size = 0;
    uint32_t create_thread_switch = 0;
    uint32_t thread_ctor = 0;
    uint32_t create_disc_thread = 0;

    // JKRSolidHeap::create, the variable that holds the audio heap, and the heap's size.
    uint32_t create_solid_heap = 0;
    uint32_t audio_heap = 0;
    uint32_t audio_heap_size = 0;

    // The audio set-up, and the byte that it sets when it's done.
    uint32_t audio_setup = 0;
    uint32_t audio_ready = 0;

    // The audio manager, its frame process and its startSound(id by reference, handle, position).
    uint32_t audio_manager = 0;
    uint32_t audio_frame = 0;
    uint32_t start_sound = 0;

    // The scene manager, its loaders of wave groups from bank 0, from bank 1 and from any bank, and a group's status (2
    // once it's loaded).
    uint32_t scene_manager = 0;
    uint32_t load_se_wave = 0;
    uint32_t load_bgm_wave = 0;
    uint32_t load_wave = 0;
    uint32_t wave_status = 0;
};

// The number of words in a JAudio2 profile.
inline constexpr uint32_t kJAudio2ProfileWords = sizeof(JAudio2Profile) / 4;

// A profile from the process descriptor's words, or nothing if their number isn't kJAudio2ProfileWords.
std::optional<JAudio2Profile> JAudio2ProfileFromWords(const std::vector<uint32_t>& words);

std::vector<uint32_t> JAudio2ProfileWords(const JAudio2Profile& profile);

// Twilight Princess HD's profile (USA, version 81).
const JAudio2Profile& TwilightPrincessHd();

class JAudio2Driver final : public GameDriver
{
public:
    // Loads the game's executable and starts its sound system. `files` are the title's files the game opens, by their
    // paths under /vol/content. Throws std::runtime_error if the game's code fails.
    JAudio2Driver(const JAudio2Profile& profile, const cafe::Rpl& executable,
                  std::map<std::string, std::vector<uint8_t>> files);

    JAudio2Driver(const JAudio2Driver&) = delete;
    JAudio2Driver& operator=(const JAudio2Driver&) = delete;

    void Start(const std::string& path, uint32_t sound, std::span<const uint32_t> waves) override;
    std::span<const int16_t> Frame() override;

    // Whether the sound has ended: the game has cleared the handle that it started the sound with.
    bool IsFinished() override;

    // The machine that runs the game, and the guest's word that holds the sound's JAISound until the game releases it,
    // for the length analysis (ntdlib/jaudio/game_lengths.h).
    cafe::Machine& Machine()
    {
        return machine_;
    }

    uint32_t HandleAddress() const
    {
        return handle_;
    }

    // AX, for switching on Cemu's departures (ax/cemu_departures.h).
    ax::System& AxSystem()
    {
        return sndcore2_.System();
    }

private:
    // Runs AX's frames up to the game's next frame, which runs the frame process once the sound system is up.
    void GameFrame();

    JAudio2Profile profile_;
    cafe::Machine machine_;
    cafe::Coreinit coreinit_{machine_};
    cafe::Sndcore2 sndcore2_{machine_};
    bool ready_ = false;         // the audio set-up is done, and the frame process runs
    uint32_t handle_ = 0;        // the guest's word for the sound's handle
    uint64_t quarter_ticks_ = 0; // the time since the driver started, in quarters of a tick
    uint64_t frames_ = 0;        // AX's frames so far
    uint64_t game_frames_ = 0;   // the game's frames so far
};

} // namespace csf::game
