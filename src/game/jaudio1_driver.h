// SPDX-License-Identifier: MIT

// Game mode's JAudio1 driver, for a game whose sound runs on JAudio1 under JSystem on sead, as The Wind Waker HD's
// does. JAudio1 mixes on the CPU in a virtual DSP thread, and hands its mix to AX in the devices' final-mix callbacks.
// The driver does what the game's start-up does for sound, without its graphics: sead's framework, the game's root
// heaps and JAIZelBasic's init. Then it loads the sound's wave groups and starts the sound. The game's frame process
// runs 30 times a second, as the game's frames do.

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

// The addresses and settings in a JAudio1 game's executable that the driver requires. The process descriptor's profile
// holds them as words, in this order (docs/csf.md, "JAudio1 driver").
struct JAudio1Profile
{
    // The machine's heap, and the default heap's share of it.
    uint32_t machine_heap = 0;
    uint32_t default_heap = 0;

    // The function that runs the executable's static constructors.
    uint32_t static_init = 0;

    // sead's framework: the builder of its start-up argument (given 8 bytes), the heap size that the argument's first
    // word gives, and the start-up (given the argument).
    uint32_t sead_argument = 0;
    uint32_t sead_heap_size = 0;
    uint32_t sead_init = 0;

    // The game's two creators of its root heaps, and the variable that then holds the sound's root heap.
    uint32_t create_root_heaps = 0;
    uint32_t create_more_heaps = 0;
    uint32_t sound_root_heap = 0;

    // sead's setCurrentHeap(manager, heap): the heap that allocations without a heap take, on the calling thread.
    uint32_t set_current_heap = 0;

    // The createInstance(heap) of the game's sound layer's singleton. Its constructor also sets the sound files' paths.
    uint32_t create_sound_layer = 0;

    // JAIZelBasic: the game's instance, its init(heap, ARAM size) with the size, and its frame process.
    uint32_t basic = 0;
    uint32_t basic_init = 0;
    uint32_t aram_size = 0;
    uint32_t frame_process = 0;

    // The start-up is done once the effects sequence's sound (the variable seHandle) has reached state 4, and each wave
    // system that stays loaded has loaded. The variables that hold the wave systems' list (12-byte entries {data, size,
    // load type}, ending with a null data word) and their load statuses (2 once loaded).
    uint32_t se_handle = 0;
    uint32_t wave_systems = 0;
    uint32_t wave_status = 0;

    // BankWave::loadGroupWave(wave system, group).
    uint32_t load_group_wave = 0;

    // JAIBasic::setCameraInfo(basic, position, last position, matrix, camera), and the default camera's two position
    // variables and its matrix.
    uint32_t set_camera = 0;
    uint32_t camera_position = 0;
    uint32_t camera_last_position = 0;
    uint32_t camera_matrix = 0;

    // JAIBasic::startSoundVec(basic, id, handle, position, 0, 0, 4).
    uint32_t start_sound = 0;
};

// The number of words in a JAudio1 profile.
inline constexpr uint32_t kJAudio1ProfileWords = sizeof(JAudio1Profile) / 4;

// A profile from the process descriptor's words, or nothing if their number isn't kJAudio1ProfileWords.
std::optional<JAudio1Profile> JAudio1ProfileFromWords(const std::vector<uint32_t>& words);

std::vector<uint32_t> JAudio1ProfileWords(const JAudio1Profile& profile);

// The Wind Waker HD's profile (USA, version 0).
const JAudio1Profile& WindWakerHd();

class JAudio1Driver final : public GameDriver
{
public:
    // Loads the game's executable and starts its sound system. `files` are the title's files the game opens, by their
    // paths under /vol/content. Throws std::runtime_error if the game's code fails.
    JAudio1Driver(const JAudio1Profile& profile, const cafe::Rpl& executable,
                  std::map<std::string, std::vector<uint8_t>> files);

    JAudio1Driver(const JAudio1Driver&) = delete;
    JAudio1Driver& operator=(const JAudio1Driver&) = delete;

    void Start(const std::string& path, uint32_t sound, std::span<const uint32_t> waves) override;
    std::span<const int16_t> Frame() override;

    // Whether the sound has ended: JAudio has cleared the handle that it started the sound with.
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
    // Whether the start-up's sounds and stay waves are in (see JAudio1Profile::se_handle).
    bool SetupDone();

    // The number of wave systems in the start-up archive's list, and whether a wave system's group is loaded.
    uint32_t WaveSystemCount();
    bool WaveSystemLoaded(uint32_t system);

    // Runs AX's frames up to the game's next frame, which runs the frame process once the sound system is up.
    void GameFrame();

    JAudio1Profile profile_;
    cafe::Machine machine_;
    cafe::Coreinit coreinit_{machine_};
    cafe::Sndcore2 sndcore2_{machine_};
    bool ready_ = false;         // JAIZelBasic's init is done, and the frame process runs
    uint32_t handle_ = 0;        // the guest's word for the sound's handle
    uint64_t quarter_ticks_ = 0; // the time since the driver started, in quarters of a tick
    uint64_t frames_ = 0;        // AX's frames so far
    uint64_t game_frames_ = 0;   // the game's frames so far
};

} // namespace csf::game
