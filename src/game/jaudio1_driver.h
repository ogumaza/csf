// SPDX-License-Identifier: MIT

// Game mode's JAudio1 driver, for a game whose sound runs on JAudio1 under JSystem on sead, as The Wind Waker HD's
// does. JAudio1 mixes on the CPU in a virtual DSP thread, and hands its mix to AX in the devices' final-mix callbacks.
// The driver does what the game's start-up does for sound, without its graphics: sead's framework, the game's root
// heaps and JAIZelBasic's init. Then it loads the sound's wave groups and starts the sound. The game's frame process
// runs 30 times a second, as the game's frames do.

#pragma once

#include <array>
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

    // JAIZelBasic: an instance, its init(heap, ARAM size) with the size, and its frame process. The driver drives the
    // instance that ms_basic (below) points to once the game's sound layer exists. It drives the instance here only for
    // a profile without ms_basic.
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

    // JAIBasic::startSoundVec(basic, id, handle, position, 0, map information, 4).
    uint32_t start_sound = 0;

    // JAISound::setPortData(sound, port, value), and the variable JAIBasic::msBasic. JAIBasic's constructor sets
    // msBasic. The game's sound layer constructs the JAIZelBasic that the game drives after the instance in basic.
    // msBasic then points to it. JAudio reads the camera through msBasic. A profile from before version 3 of the format
    // stops before these two words. They are then 0. The driver then sets no port but 6.
    uint32_t set_port_data = 0;
    uint32_t ms_basic = 0;

    // The Zelda games' starters of music, which give a piece the tracks and tempo that the game's state picks (version
    // 3 of the format). JAIZelBasic::bgmStart(basic, id, fade, 0) starts the main music. Besides the sound table's IDs,
    // it takes IDs that start a piece in another state. subBgmStart(basic, id) starts music that plays over the main
    // music, and bgmNowBattle(basic, distance) the battle music, as an enemy comes within that distance. The pieces
    // that the game starts with subBgmStart have their bits set in sub_bgm: bit n & 31 of word n >> 5 for the music
    // 0x80000000 | n. battle_bgm is the piece that bgmNowBattle starts away from the sea. Without bgm_start, the driver
    // starts music with startSoundVec.
    uint32_t bgm_start = 0;
    uint32_t sub_bgm_start = 0;
    uint32_t battle_bgm_start = 0;
    std::array<uint32_t, 4> sub_bgm = {};
    uint32_t battle_bgm = 0;

    // The offsets in JAIZelBasic of the main music's handle, which the sub music's handle follows, and of the byte that
    // the game sets once a scene's music has started. bgmNowBattle starts nothing until then.
    uint32_t bgm_handle = 0;
    uint32_t scene_started = 0;

    // The setters of the game's state that the starters read (csf::kJAudio1States): setShipSailState(basic, state),
    // setLinkOnBoard(basic, on board) and bgmSetSwordUsing(basic, drawn).
    uint32_t set_ship_sail_state = 0;
    uint32_t set_link_on_board = 0;
    uint32_t bgm_set_sword_using = 0;

    // The writers of the status that the game's actors give a piece while it plays, on port 9: changeBgmStatus(basic,
    // status) for the main music and changeSubBgmStatus(basic, status) for the music over it.
    uint32_t change_bgm_status = 0;
    uint32_t change_sub_bgm_status = 0;
};

// The words of a JAudio1 profile in version 2 of the format (docs/csf.md, "JAudio1 driver").
inline constexpr uint32_t kJAudio1Version2Words = 24;

// The Wind Waker HD's profiles (USA and Japan, version 0).
const JAudio1Profile& WindWakerHd();
const JAudio1Profile& WindWakerHdJapan();

class JAudio1Driver final : public JAudioDriver
{
public:
    // Loads the game's executable and starts its sound system. `files` are the title's files the game opens, by their
    // paths under /vol/content. Throws std::runtime_error if the game's code fails.
    JAudio1Driver(const JAudio1Profile& profile, const cafe::Rpl& executable,
                  std::map<std::string, std::vector<uint8_t>> files);

    JAudio1Driver(const JAudio1Driver&) = delete;
    JAudio1Driver& operator=(const JAudio1Driver&) = delete;

    // The handle is a word that holds the sound's JAISound until JAudio clears it. Port 6 of the ports that
    // SetTagValues gave is the sound's map information, or 0 without it. The driver gives the sound each other port
    // through JAISound::setPortData. An effect gets them right after the start, as the game's seStart gives every
    // effect port 9. Music gets them once it plays (JAISound's state 4) and a frame has run, as a game writes ports
    // while its music plays. A piece's sequence may clear a port in its first commands. With the profile's starters of
    // music, music starts through them, on the game's handle of the main music or of the sub music. Port 9 of such
    // music goes through the profile's writers of a piece's status. The values from csf::kMaxPorts up set the game's
    // state first (csf::kJAudio1States).
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

    // Sets the game's state from the tag's values, then starts the music `sound` through the profile's starters of
    // music. Returns the handle.
    uint32_t StartMusic(uint32_t sound);

    JAudio1Profile profile_;

    // The JAIZelBasic that the driver drives (see JAudio1Profile::basic).
    uint32_t basic_ = 0;

    // The ports that the frame process gives the music once it plays, and the frames that it has played so far.
    std::map<int, int> music_ports_;
    int music_frames_ = 0;

    // Whether the music plays over the main music: a piece that subBgmStart or bgmNowBattle started. The game calls
    // bgmNowBattle once a frame while a fight goes on, with the distance to the nearest enemy. Without that call, the
    // sound layer stops the battle music 30 frames later. The frame process calls it for such music from its second
    // frame on (see FrameProcess).
    bool music_over_ = false;
    double enemy_distance_ = 0.0;
    int music_over_frames_ = 0;
};

} // namespace csf::game
