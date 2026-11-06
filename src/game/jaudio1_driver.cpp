// SPDX-License-Identifier: MIT

#include "game/jaudio1_driver.h"

#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/cafe/ax/system.h"

#include "cafe/machine.h"
#include "cafe/memory.h"
#include "cafe/rpl.h"
#include "csf/format.h"
#include "csf/ntdlib_cafe.h"
#include "game/jaudio_driver.h"

namespace csf::game
{
namespace
{

// A wave system's status once its group has loaded, the state that the effects sequence's sound reaches once the
// start-up has loaded the stay waves, and the wave systems' load types that the start-up loads.
constexpr uint32_t kWaveLoaded = 2;
constexpr uint8_t kSoundPlaying = 4;
constexpr uint32_t kStayWaves = 1;

// The port that holds the low 16 bits of an effect's map information. The game passes the information when it starts
// the effect, and the sound library writes it to the port when the effect's sequence starts.
constexpr int kMapInfoPort = 6;

// The port that holds the status that a Zelda game's actors give a piece of music while it plays.
constexpr int kMusicStatusPort = 9;

// JAudio1's sound IDs: the top two bits give the sound's type. Music is a sequence, 0x80000000 | n. The low byte of a
// piece's number picks it in the Zelda games' tables, and IDs from 0x100 on start a piece in another state.
constexpr uint32_t kSoundTypeMask = 0xc0000000;
constexpr uint32_t kMusic = 0x80000000;
constexpr uint32_t kMusicPieces = 128;

// The distance that bgmNowBattle takes for an enemy that is far away. An enemy within 30 units brings in more of the
// battle music's tracks (JAIZelParam's ENEMY_NEARBY_DIST: 0x1018dc78 in The Wind Waker HD's executable).
constexpr double kFarAway = 10000.0;

// The functions of other libraries that the game's start-up reaches and that its sound doesn't use. The constructors of
// Miiverse's (nn_olv's) parameter objects return their objects. SAVEInit, for the save data, succeeds. Each library is
// registered under the name of the executable's import section for it: .fimport_nn_save and .fimport_nn_olv.rpl. The
// second name keeps the extension.
void RegisterStartupStubs(cafe::Machine& machine)
{
    machine.Register("nn_save", "SAVEInit", [](cafe::Machine& m) { m.Return(0); });
    for (const char* name :
         {"__ct__Q3_2nn3olv15InitializeParamFv", "__ct__Q3_2nn3olv18DownloadedPostDataFv",
          "__ct__Q3_2nn3olv19DownloadedTopicDataFv", "__ct__Q3_2nn3olv19StartPortalAppParamFv",
          "__ct__Q3_2nn3olv25DownloadPostDataListParamFv", "__ct__Q3_2nn3olv28UploadEmpathyToPostDataParamFv",
          "__ct__Q3_2nn3olv28UploadPostDataByPostAppParamFv"})
    {
        machine.Register("nn_olv.rpl", name, [](cafe::Machine& m) { m.Return(m.Arg(0)); });
    }
}

} // namespace

// The Wind Waker HD's executable (version 0) names no function. The addresses come from its strings, its RTTI and
// vtables, its calls of the system's functions, and the GameCube game's JAudio1, whose code the port keeps.
const JAudio1Profile& WindWakerHd()
{
    static const JAudio1Profile kProfile{
        .machine_heap = 224 << 20,
        .default_heap = 208 << 20,
        .static_init = 0x028ea09c,
        .sead_argument = 0x027478c4,
        .sead_heap_size = 192 << 20,
        .sead_init = 0x0274b1f0,
        .create_root_heaps = 0x0203e584,
        .create_more_heaps = 0x0203e6a4,
        .sound_root_heap = 0x10461268,
        .set_current_heap = 0x02756170,
        .create_sound_layer = 0x0202be64,
        .basic = 0x1048abd8,
        .basic_init = 0x02026044,
        .aram_size = 0x3000000,
        .frame_process = 0x0202636c,
        .se_handle = 0x104b5018,
        .wave_systems = 0x101f9b38,
        .wave_status = 0x104b4ee8,
        .load_group_wave = 0x02802278,
        .set_camera = 0x0280307c,
        .camera_position = 0x104b4f68,
        .camera_last_position = 0x104b4f6c,
        .camera_matrix = 0x104b4f38,
        .start_sound = 0x02802848,
        .set_port_data = 0x0280c6d8,
        .ms_basic = 0x104b4f14,
        .bgm_start = 0x0202204c,
        .sub_bgm_start = 0x0201de00,
        .battle_bgm_start = 0x0202879c,
        // JA_BGM_MBOSS, JA_BGM_MBOSS_S and JA_BGM_SEA_ENEMY (0x19, 0x1a and 0x1c), and JA_BGM_P_GANON_1 and
        // JA_BGM_P_GANON_2 (0x41 and 0x47). subBgmStart gives the first three tracks from bgmMute's table (0x1018dcb8).
        .sub_bgm = {0x16000000, 0, 0x00000082, 0},
        .battle_bgm = 0x80000004, // JA_BGM_BATTLE_NORM
        .bgm_handle = 0x78,
        .scene_started = 0x29d,
        .set_ship_sail_state = 0x020299d0,
        .set_link_on_board = 0x02027cf4,
        .bgm_set_sword_using = 0x02028fe0,
        .change_bgm_status = 0x02027fd4,
        .change_sub_bgm_status = 0x02027ff0,
    };
    return kProfile;
}

// The Japanese release's executable (version 0) has the USA release's code in another layout. Where the masked words of
// the two executables' code line up, the sound layer stays at the USA release's addresses up to 0x0202ead4 and sits 4
// bytes on after it. sead's code sits 2,796 bytes on, and JAudio1's 2,784 bytes on. JAudio1's variables sit 0x148 bytes
// on. The only other words that differ in the sound layer are two reads of the game's global state (0x5b30 for 0x5b34,
// at 0x02028814 and 0x0202c8e4). They read what the driver doesn't set, and read it the same way.
const JAudio1Profile& WindWakerHdJapan()
{
    static const JAudio1Profile kProfile = []
    {
        JAudio1Profile profile = WindWakerHd();
        profile.static_init = 0x028eab7c;
        profile.sead_argument = 0x027483b0;
        profile.sead_init = 0x0274bcdc;
        profile.create_root_heaps = 0x0203e588;
        profile.create_more_heaps = 0x0203e6a8;
        profile.set_current_heap = 0x02756c5c;
        profile.basic = 0x1048abd0;
        profile.se_handle = 0x104b5160;
        profile.wave_systems = 0x101f9b80;
        profile.wave_status = 0x104b5030;
        profile.load_group_wave = 0x02802d58;
        profile.set_camera = 0x02803b5c;
        profile.camera_position = 0x104b50b0;
        profile.camera_last_position = 0x104b50b4;
        profile.camera_matrix = 0x104b5080;
        profile.start_sound = 0x02803328;
        profile.set_port_data = 0x0280d1b8;
        profile.ms_basic = 0x104b505c;
        return profile;
    }();
    return kProfile;
}

JAudio1Driver::JAudio1Driver(const JAudio1Profile& profile, const cafe::Rpl& executable,
                             std::map<std::string, std::vector<uint8_t>> files)
    : JAudioDriver(profile.machine_heap), profile_(profile)
{
    RegisterStartupStubs(machine_);
    Load(executable, profile_.default_heap, std::move(files));
    StartJAudio(MakeHeaps());

    // The start-up's sounds and stay waves load a frame at a time.
    StartFrameProcess();
    WaitUntil([this] { return SetupDone(); }, "the game's sound start-up didn't finish");
}

void JAudio1Driver::Start(const std::string&, uint32_t sound, std::span<const uint32_t> waves)
{
    const JAudio1Profile& p = profile_;
    cafe::Memory& memory = machine_.memory;

    // The sound's wave groups, one group of a wave system each. A wave system that JAudio has loaded keeps its group.
    // The driver skips a wave system past the end of the game's list.
    const auto systems = static_cast<uint32_t>(WaveSystemLoadTypes().size());
    const auto loaded = [this, systems](uint32_t wave)
    {
        const uint32_t system = wave >> 16;
        return system >= systems || WaveSystemLoaded(system);
    };
    const auto load = [this, &p](uint32_t wave)
    {
        machine_.Call(p.load_group_wave, {wave >> 16, wave & 0xffff});
    };
    LoadWaves(waves, loaded, load);

    // JAIZelBasic's init cleared the default camera, and the driver sets it again. The sound starts at no position.
    // Port 6's value is its map information.
    machine_.Call(p.set_camera, {basic_, memory.Read32(p.camera_position), memory.Read32(p.camera_last_position),
                                 p.camera_matrix, 0});
    const bool music = (sound & kSoundTypeMask) == kMusic;
    if (music && p.bgm_start != 0)
    {
        handle_ = StartMusic(sound);
    }
    else
    {
        const auto map_info = tag_values_.find(kMapInfoPort);
        handle_ = coreinit_.Alloc(4);
        machine_.Call(p.start_sound, {basic_, sound, handle_, 0, 0,
                                      map_info == tag_values_.end() ? 0u : static_cast<uint16_t>(map_info->second), 4});
    }
    const uint32_t started = memory.Read32(handle_);
    if (started == 0)
    {
        throw std::runtime_error("the game didn't start the sound");
    }

    // The other ports. seStart gives an effect port 9 the same way after startSoundVec (the call at 0x0201bbe0). The
    // game writes a port of its music while the music plays: changeBgmStatus (0x02027fd4) and changeSubBgmStatus
    // (0x02027ff0) write port 9. The frame process gives music its ports (see FrameProcess).
    for (const auto& [port, value] : tag_values_)
    {
        if (port == kMapInfoPort || port >= kMaxPorts || p.set_port_data == 0)
        {
            continue;
        }
        if (music)
        {
            music_ports_[port] = value;
            continue;
        }

        machine_.Call(p.set_port_data,
                      {started, static_cast<uint32_t>(port), static_cast<uint32_t>(static_cast<uint16_t>(value))});
    }
}

uint32_t JAudio1Driver::StartMusic(uint32_t sound)
{
    const JAudio1Profile& p = profile_;
    const auto state = [this](int key)
    {
        const auto value = tag_values_.find(key);
        return value == tag_values_.end() ? 0u : static_cast<uint32_t>(value->second);
    };

    // The game's sound layer starts with the boat's sail down, Link off the boat, his sword sheathed and no enemy near.
    // The tag's values change that before the start. The setters only store the state while no music plays.
    if (state(kSailState) != 0)
    {
        machine_.Call(p.set_ship_sail_state, {basic_, state(kSailState)});
    }
    if (state(kOnBoardState) != 0)
    {
        machine_.Call(p.set_link_on_board, {basic_, state(kOnBoardState)});
    }
    if (state(kSwordState) != 0)
    {
        machine_.Call(p.bgm_set_sword_using, {basic_, state(kSwordState)});
    }
    enemy_distance_ = state(kNearState) != 0 ? 0.0 : kFarAway;

    // The battle music and the music that subBgmStart starts play over the main music, on the sub music's handle. The
    // driver calls bgmNowBattle for them at the start, and the frame process from the second frame on. With an enemy
    // near, bgmNowBattle also brings in layers of the music that subBgmStart started, such as the minibosses'
    // (mbossBgmNearByProcess, 0x02025940). Its call also sets the countdown of the sound layer's frame work
    // (bgmBattleGFrame). With the countdown at 0, the frame work stops JA_BGM_SEA_ENEMY in the first frame.
    // bgmNowBattle starts nothing before a scene has started. The driver marks it started, as
    // JAIZelBasic::sceneBgmStart does.
    const uint32_t piece = sound & ~kMusic;
    const bool sub = piece < kMusicPieces && (p.sub_bgm[piece >> 5] >> (piece & 31) & 1) != 0;
    music_over_ = sound == p.battle_bgm || sub;
    uint32_t handle = basic_ + p.bgm_handle;
    if (music_over_)
    {
        handle += 4;
        machine_.memory.Write8(basic_ + p.scene_started, 1);
    }
    if (sound == p.battle_bgm)
    {
        machine_.Call(p.battle_bgm_start, {basic_}, {enemy_distance_});
    }
    else if (sub)
    {
        machine_.Call(p.sub_bgm_start, {basic_, sound});
        machine_.Call(p.battle_bgm_start, {basic_}, {enemy_distance_});
    }
    else
    {
        machine_.Call(p.bgm_start, {basic_, sound, 0, 0});
    }

    return handle;
}

uint32_t JAudio1Driver::MakeHeaps()
{
    const JAudio1Profile& p = profile_;
    cafe::Memory& memory = machine_.memory;

    // The static constructors, then sead's framework with the heap that the machine can give.
    machine_.Call(p.static_init);
    const uint32_t argument = machine_.Call(p.sead_argument, {coreinit_.Alloc(8)}).r3;
    memory.Write32(argument, p.sead_heap_size);
    machine_.Call(p.sead_init, {argument});

    // The game's root heaps. One of them is the sound's.
    machine_.Call(p.create_root_heaps);
    machine_.Call(p.create_more_heaps);
    const uint32_t root = memory.Read32(p.sound_root_heap);
    if (root == 0)
    {
        throw std::runtime_error("the game's sound heap couldn't be made");
    }

    return root;
}

void JAudio1Driver::StartJAudio(uint32_t root)
{
    // AX, as the game starts it: snd_core's 32 kHz renderer. Then, from the sound's root heap, the game's sound layer
    // (which a task of the game's framework creates) and JAudio. The sound layer constructs the JAIZelBasic that the
    // game drives. JAudio's init returns once the audio thread has started. The thread starts after the first final-mix
    // callback, which the frames run while the game sleeps.
    const JAudio1Profile& p = profile_;
    sndcore2_.Init(ax::System::Library::kSndCore, false);
    machine_.Call(p.set_current_heap, {0, root});
    machine_.Call(p.create_sound_layer, {0});
    basic_ = p.ms_basic != 0 ? machine_.memory.Read32(p.ms_basic) : p.basic;
    if (basic_ == 0)
    {
        throw std::runtime_error("the game's sound layer made no JAIZelBasic");
    }

    machine_.Call(p.basic_init, {basic_, root, p.aram_size});
}

void JAudio1Driver::FrameProcess()
{
    const JAudio1Profile& p = profile_;

    // bgmNowBattle, from the second frame on, while the music over the main music plays. The start called it in the
    // first frame.
    if (music_over_ && ++frames_ > 1 && handle_ != 0 && machine_.memory.Read32(handle_) != 0)
    {
        machine_.Call(p.battle_bgm_start, {basic_}, {enemy_distance_});
    }

    // The music's ports, once the music has played for a frame. The writers of a piece's status give it port 9, as
    // the game's actors do.
    if (!music_ports_.empty() && handle_ != 0)
    {
        const uint32_t sound = machine_.memory.Read32(handle_);
        if (sound != 0 && machine_.memory.Read8(sound + 1) >= kSoundPlaying && ++music_frames_ > 1)
        {
            for (const auto& [port, value] : music_ports_)
            {
                const uint32_t status = music_over_ ? p.change_sub_bgm_status : p.change_bgm_status;
                const auto word = static_cast<uint32_t>(static_cast<uint16_t>(value));
                if (port == kMusicStatusPort && status != 0)
                {
                    machine_.Call(status, {basic_, word});
                }
                else
                {
                    machine_.Call(p.set_port_data, {sound, static_cast<uint32_t>(port), word});
                }
            }
            music_ports_.clear();
        }
    }

    machine_.Call(p.frame_process, {basic_});
}

bool JAudio1Driver::SetupDone()
{
    cafe::Memory& memory = machine_.memory;
    const uint32_t sound = memory.Read32(profile_.se_handle);
    if (sound == 0 || memory.Read8(sound + 1) < kSoundPlaying)
    {
        return false;
    }

    const std::vector<uint32_t> load_types = WaveSystemLoadTypes();
    for (uint32_t system = 0; system < load_types.size(); ++system)
    {
        if (load_types[system] <= kStayWaves && !WaveSystemLoaded(system))
        {
            return false;
        }
    }

    return true;
}

std::vector<uint32_t> JAudio1Driver::WaveSystemLoadTypes()
{
    cafe::Memory& memory = machine_.memory;
    std::vector<uint32_t> load_types;
    for (uint32_t entry = memory.Read32(profile_.wave_systems); memory.Read32(entry) != 0; entry += 12)
    {
        load_types.push_back(memory.Read32(entry + 8));
    }

    return load_types;
}

bool JAudio1Driver::WaveSystemLoaded(uint32_t system)
{
    cafe::Memory& memory = machine_.memory;
    return memory.Read32(memory.Read32(profile_.wave_status) + 4 * system) == kWaveLoaded;
}

} // namespace csf::game
