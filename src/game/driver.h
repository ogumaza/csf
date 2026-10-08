// SPDX-License-Identifier: MIT

// Game mode's driver. The driver sets a game's NintendoWare sound library (nw::snd) up with the game's code, the way
// the game does. The driver opens a sound archive, loads a sound's data and starts the sound. Then the driver runs AX's
// frames with the game's threads, and calls the archive player's update every 60th of a second. The game's main loop
// does the same. A Profile gives the addresses of the functions that the driver calls in the game's executable, and the
// settings that the game uses.

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

namespace csf::game
{

struct Profile
{
    std::string name;

    // The function that the executable's start-up code calls to run the static constructors.
    uint32_t static_init = 0;

    // A word in nw::snd that the game sets before starting the sound system, and the value that it sets (if any).
    uint32_t setting_address = 0;
    uint32_t setting_value = 0;

    // AX's start-up: the 48 kHz renderer or the 32 kHz one, and each device's upsampling after the final mix.
    bool renderer_48k = false;
    bool upsample_after_final_mix = false;

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

    // Where nw::snd keeps the channels that sound: the ChannelManager (a static object), its list of them, and a
    // channel's list node, within the channel.
    uint32_t channel_manager = 0;
    uint32_t channel_list = 0;
    uint32_t channel_node = 0;

    // A channel's fields: its key and original key (u8), its velocity's volume (f32), its envelope (a CurveAdshr,
    // whose stage and level in 0.1 dB come first), the data of its callback (the sequence track that played it) and
    // its MultiVoice. A track's player, and a player's 16 tracks. A MultiVoice's volume and pitch for the frame (f32).
    uint32_t channel_key = 0;
    uint32_t channel_original_key = 0;
    uint32_t channel_velocity = 0;
    uint32_t channel_envelope = 0;
    uint32_t channel_track = 0;
    uint32_t channel_voice = 0;
    uint32_t track_player = 0;
    uint32_t player_tracks = 0;
    uint32_t voice_volume = 0;
    uint32_t voice_pitch = 0;

    // A channel's TV output parameters (an OutputParam), and in them the pan and span the track gives; the note's pan,
    // from its region and the note; and the state of nw::snd's random numbers (Util::CalcRandom's seed).
    uint32_t channel_tv_param = 0;
    uint32_t param_pan = 0;
    uint32_t param_span = 0;
    uint32_t channel_note_pan = 0;
    uint32_t random_state = 0;
};

// The profile's words as a game-mode rip stores them (docs/csf.md, "Driver"): everything that a game-mode player
// requires. The words leave out the name and the fields that Driver::Channels reads.
std::vector<uint32_t> ProfileWords(const Profile& profile);

// A profile from a game-mode rip's words, or nothing if there aren't as many as ProfileWords gives. Its name is empty,
// and Driver::Channels finds nothing with it.
std::optional<Profile> ProfileFromWords(std::span<const uint32_t> words);

// A sounding channel in nw::snd. It's in the state that the game's code left it in after a frame.
struct ChannelState
{
    uint32_t id = 0;          // its address in the guest, the same while it sounds
    int track = -1;           // the sequence track that played it, 0 to 15, or -1
    uint8_t key = 0;          // the note's key
    uint8_t original_key = 0; // the key the wave sounds at its rate
    float velocity = 0.0f;    // the velocity's volume: velocity / 127
    uint32_t envelope = 0;    // the envelope's stage: 0 attack, 1 hold, 2 decay, 3 sustain, 4 release
    float level = 0.0f;       // its level, in 0.1 dB
    float volume = 0.0f;      // the volume the channel gave its voices this frame
    float pitch = 0.0f;       // and their pitch (a ratio)
    float pan = 0.0f;         // the TV's pan the track gives the channel, -1 left to 1 right
    float span = 0.0f;        // and its surround pan, 0 front to 2 back
    float note_pan = 0.0f;    // the note's pan, from its region and the note: (note + region - 64) / 63
};

// Animal Crossing: amiibo Festival's profile: its executable's addresses, and the settings of NdSysAudio, its wrapper
// round nw::snd (NdSysAudio's constructor at 0x020EE63C, initializeLocal at 0x020EEF48, SoundArchive::load at
// 0x020F6560 and updatePlayer at 0x020EFAAC).
const Profile& AmiiboFestival();

class Driver
{
public:
    // Loads the game's executable and starts nw::snd. `files` are the title's files the game opens, by their paths
    // under /vol/content. Throws std::runtime_error if the game's code fails.
    Driver(const Profile& profile, const cafe::Rpl& executable, std::map<std::string, std::vector<uint8_t>> files);

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    // Opens the archive at `path` (under /vol/content), loads the data of the sound `id` (nw::snd's ID: 0x01000000
    // and the sound's index) and starts it. Throws std::runtime_error if nw::snd refuses.
    void Start(const std::string& path, uint32_t id);

    // Runs one frame of AX, 3 ms, and returns the TV's output: 144 stereo pairs at 48 kHz.
    std::span<const int16_t> Frame();

    // Whether the sound has ended: nw::snd has let go of the handle that it started the sound with. A SoundHandle's
    // first word is its sound, and SoundHandle::DetachSound (0x022B9D94) clears that word.
    bool IsFinished();

    // The channels that sound, after the last frame, in the order nw::snd keeps them.
    std::vector<ChannelState> Channels();

    // The state of nw::snd's random numbers now.
    uint32_t RandomState();

    cafe::Machine& Machine()
    {
        return machine_;
    }

private:
    uint32_t Allocate(uint32_t size, uint32_t align = 64);
    uint32_t String(const std::string& text);

    const Profile& profile_;
    cafe::Machine machine_;
    cafe::Coreinit coreinit_{machine_};
    cafe::Sndcore2 sndcore2_{machine_};
    uint32_t heap_ = 0;
    uint32_t player_ = 0;
    uint32_t handle_ = 0;
    uint64_t quarter_ticks_ = 0; // the time since the driver started, in quarters of a tick
    uint64_t frames_ = 0;
    uint64_t updates_ = 0; // the archive player's updates so far
};

} // namespace csf::game
