// SPDX-License-Identifier: MIT

// Game mode's JAudio1 driver, for a game whose sound runs on JAudio1 under JSystem on sead, as The Wind Waker HD's
// does. JAudio1 mixes on the CPU in a virtual DSP thread, and hands its mix to AX in the devices' final-mix callbacks.
// The driver does what the game's start-up does for sound, without its graphics: sead's framework, the game's root
// heaps and JAIZelBasic's init. Then it loads the sound's wave groups and starts the sound. The game's frame process
// runs 30 times a second, as the game's frames do.

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "cafe/rpl.h"
#include "game/jaudio_driver.h"

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

// The Wind Waker HD's profile (USA, version 0).
const JAudio1Profile& WindWakerHd();

class JAudio1Driver final : public JAudioDriver
{
public:
    // Loads the game's executable and starts its sound system. `files` are the title's files the game opens, by their
    // paths under /vol/content. Throws std::runtime_error if the game's code fails.
    JAudio1Driver(const JAudio1Profile& profile, const cafe::Rpl& executable,
                  std::map<std::string, std::vector<uint8_t>> files);

    JAudio1Driver(const JAudio1Driver&) = delete;
    JAudio1Driver& operator=(const JAudio1Driver&) = delete;

    // The handle is a word that holds the sound's JAISound until JAudio clears it.
    void Start(const std::string& path, uint32_t sound, std::span<const uint32_t> waves) override;

private:
    // The game's start-up for sound, in two steps. MakeHeaps runs the static constructors, sead's framework and the
    // game's creators of its root heaps, and returns the sound's root heap. It throws std::runtime_error if there is
    // none. StartJAudio starts AX, then the game's sound layer and JAudio on that heap.
    uint32_t MakeHeaps();
    void StartJAudio(uint32_t root);

    // JAIZelBasic's frame process. The game's frames come 30 times a second, as the GameCube game's do. They run it
    // from JAIZelBasic's init on.
    void FrameProcess() override;

    // Whether the start-up's sounds and stay waves are in (see JAudio1Profile::se_handle).
    bool SetupDone();

    // The load type of each wave system in the start-up archive's list, in the list's order, and whether a wave
    // system's group is loaded.
    std::vector<uint32_t> WaveSystemLoadTypes();
    bool WaveSystemLoaded(uint32_t system);

    JAudio1Profile profile_;
};

} // namespace csf::game
