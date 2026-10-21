// SPDX-License-Identifier: MIT

#include "game/jaudio2_driver.h"

#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cafe/memory.h"
#include "cafe/rpl.h"
#include "game/jaudio_driver.h"

namespace csf::game
{
namespace
{

// A wave group's status once it's loaded.
constexpr uint32_t kWaveLoaded = 2;

} // namespace

// Twilight Princess HD's executable (version 81) names no function. The addresses come from its relocations, its RTTI
// and vtables, and its calls of the system's functions.
const JAudio2Profile& TwilightPrincessHd()
{
    static const JAudio2Profile kProfile{
        .machine_heap = 224 << 20,
        .default_heap = 208 << 20,
        .static_init = 0x02c3ba44,
        .reset_data = 0x1012b5dc,
        .reset_size = 0x18,
        .reset_gate = 0x12,
        .first_init = 0x02b13568,
        .root_setting = 0x1012b888,
        .root_setting_value = 1,
        .system_heap_size_variable = 0x1012b88c,
        .system_heap_size = 0x24a55e0,
        .root_heap = 0x1012c058,
        .arena_root_heap = 0x1012c05c,
        .system_heap = 0x1012c060,
        .create_assert_heap = 0x02adcf34,
        .create_aram = 0x02b85638,
        .aram_audio_size = 0xd00000,
        .aram_graph_size = 0xffffffff,
        .aram_priorities = 0x00080706,
        .create_command_heap = 0x02add170,
        .command_heap_size = 0x1000,
        .create_archive_heap = 0x02add218,
        .archive_heap_size = 0x1720c00,
        .create_game_heap = 0x02adcfa0,
        .game_heap_size = 0x1b4e000,
        .create_zelda_heap = 0x02add0c8,
        .zelda_heap_margin = 0x850000,
        .get_free_size = 0x02b96230,
        .become_current_heap = 0x02b96060,
        .get_current_heap = 0x02b95d00,
        .set_trans_buffer = 0x02b88244,
        .trans_buffer_size = 0x2000,
        .create_thread_switch = 0x02b9820c,
        .thread_ctor = 0x02b97f60,
        .create_disc_thread = 0x02adc088,
        .create_solid_heap = 0x02b97414,
        .audio_heap = 0x1017fad0,
        .audio_heap_size = 0x20a000,
        .audio_setup = 0x02ad9c3c,
        .audio_ready = 0x1012afec,
        .audio_manager = 0x1017e710,
        .audio_frame = 0x02c06ccc,
        .start_sound = 0x02c06d4c,
        .scene_manager = 0x1017ebc8,
        .load_se_wave = 0x02c19be0,
        .load_bgm_wave = 0x02c19cc4,
        .load_wave = 0x02c1a320,
        .wave_status = 0x02c1a0b4,
    };
    return kProfile;
}

JAudio2Driver::JAudio2Driver(const JAudio2Profile& profile, const cafe::Rpl& executable,
                             std::map<std::string, std::vector<uint8_t>> files)
    : JAudioDriver(profile.machine_heap), profile_(profile)
{
    const JAudio2Profile& p = profile_;
    cafe::Memory& memory = machine_.memory;
    Load(executable, p.default_heap, std::move(files));

    // The static constructors, which make the game's arenas and its audio manager. Then the reset data, and the
    // machine's start-up: the file system, the heaps and the threads.
    machine_.Call(p.static_init);
    MakeResetData();
    const uint32_t system = MakeHeaps();
    MakeThreads(system);

    // The audio heap, then the audio set-up once a game frame until it's done. The set-up loads the start-up archive
    // and mounts the sequence archive through the disc thread, starts AX through the audio thread, and preloads the
    // game's common wave groups.
    const uint32_t current = machine_.Call(p.get_current_heap).r3;
    memory.Write32(p.audio_heap, machine_.Call(p.create_solid_heap, {p.audio_heap_size, current, 0}).r3);
    WaitUntil([&] { return memory.Read8(p.audio_ready) != 0; }, "the game's audio set-up didn't finish",
              [&] { machine_.Call(p.audio_setup); });
    StartFrameProcess();
}

void JAudio2Driver::Start(const std::string&, uint32_t sound, std::span<const uint32_t> waves)
{
    const JAudio2Profile& p = profile_;

    // The audio set-up preloads the game's common groups. A group that's loaded already stays as it is.
    const auto load = [this, &p](uint32_t wave)
    {
        const uint32_t bank = wave >> 16;
        const uint32_t group = wave & 0xffff;
        if (bank == 0)
        {
            machine_.Call(p.load_se_wave, {p.scene_manager, group});
        }
        else if (bank == 1)
        {
            machine_.Call(p.load_bgm_wave, {p.scene_manager, group});
        }
        else
        {
            machine_.Call(p.load_wave, {p.scene_manager, group, bank});
        }
    };
    LoadWaves(waves, [this](uint32_t wave) { return WaveLoaded(wave); }, load);

    // The ID goes by reference. The handle's word holds the sound while it plays.
    const uint32_t id = coreinit_.Alloc(4);
    machine_.memory.Write32(id, sound);
    handle_ = coreinit_.Alloc(4);
    if (machine_.Call(p.start_sound, {p.audio_manager, id, handle_, 0}).r3 == 0 || machine_.memory.Read32(handle_) == 0)
    {
        throw std::runtime_error("the game didn't start the sound");
    }
}

void JAudio2Driver::MakeResetData()
{
    // The game's main makes the reset data, and its first scene sets the gate.
    const JAudio2Profile& p = profile_;
    cafe::Memory& memory = machine_.memory;
    const uint32_t reset = coreinit_.Alloc(p.reset_size);
    memory.Write32(reset + 0xc, 0xffffffff);
    memory.Write8(reset + p.reset_gate, 1);
    memory.Write32(p.reset_data, reset);
}

uint32_t JAudio2Driver::MakeHeaps()
{
    const JAudio2Profile& p = profile_;
    cafe::Memory& memory = machine_.memory;

    // JFWSystem's first set-up, with the game's settings: the file system, the root heaps and the system heap.
    memory.Write32(p.root_setting, p.root_setting_value);
    memory.Write32(p.system_heap_size_variable, p.system_heap_size);
    machine_.Call(p.first_init);
    const uint32_t root = memory.Read32(p.root_heap);
    const uint32_t system = memory.Read32(p.system_heap);

    // The game's heaps and ARAM, in the order that the game makes them.
    machine_.Call(p.create_assert_heap, {root});
    machine_.Call(p.create_aram, {p.aram_audio_size, p.aram_graph_size, (p.aram_priorities >> 16) & 0xff,
                                  (p.aram_priorities >> 8) & 0xff, p.aram_priorities & 0xff});
    machine_.Call(p.create_command_heap, {p.command_heap_size, root});
    machine_.Call(p.create_archive_heap, {p.archive_heap_size, memory.Read32(p.arena_root_heap)});
    machine_.Call(p.create_game_heap, {p.game_heap_size, root});

    // The zelda heap, from what the system heap has left, becomes the current heap.
    const uint32_t free = machine_.Call(p.get_free_size, {system}).r3;
    const uint32_t zelda =
        free > p.zelda_heap_margin ? machine_.Call(p.create_zelda_heap, {free - p.zelda_heap_margin, system}).r3 : 0;
    if (zelda == 0)
    {
        throw std::runtime_error("the game's heaps couldn't be made");
    }

    machine_.Call(p.become_current_heap, {zelda});

    return system;
}

void JAudio2Driver::MakeThreads(uint32_t system)
{
    const JAudio2Profile& p = profile_;
    machine_.Call(p.set_trans_buffer, {0, p.trans_buffer_size, system});
    machine_.Call(p.create_thread_switch, {0});
    machine_.Call(p.thread_ctor, {0, machine_.MainThread().object, 0});
    machine_.Call(p.create_disc_thread, {static_cast<uint32_t>(machine_.MainThread().priority - 2)});
}

void JAudio2Driver::FrameProcess()
{
    machine_.Call(profile_.audio_frame, {profile_.audio_manager});
}

bool JAudio2Driver::WaveLoaded(uint32_t wave)
{
    return machine_.Call(profile_.wave_status, {profile_.scene_manager, wave & 0xffff, wave >> 16}).r3 == kWaveLoaded;
}

} // namespace csf::game
