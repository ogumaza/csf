// SPDX-License-Identifier: MIT

#include "game/driver.h"

#include <cstddef>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace csf::game
{
namespace
{

// The machine's heap, for the system's structures, and the default heap's share of it, for the driver's objects and the
// game's objects.
constexpr uint32_t kMachineHeap = 64 << 20;
constexpr uint32_t kDefaultHeap = 48 << 20;

// A frame of AX is 3 ms: 186,468.75 ticks of 62,156,250 a second. So frames are counted in quarters of a tick.
constexpr uint64_t kFrameQuarterTicks = 745'875;

// The sizes of the objects that the driver makes, with room to spare. The game embeds these objects in larger ones.
constexpr uint32_t kObjectSize = 0x800;
constexpr uint32_t kClientSize = 0x1700;

} // namespace

const Profile& AmiiboFestival()
{
    static const Profile profile = []
    {
        Profile p;
        p.name = "Animal Crossing: amiibo Festival";
        p.static_init = 0x02001FF0;
        // NdSysAudio's constructor sets nw::snd's channel count to 78 (0x020EE8CC).
        p.setting_address = 0x101AA478;
        p.setting_value = 78;
        // AXInitWithParams with the 32 kHz renderer, and both devices upsampling after the final mix (0x020EEF7C).
        p.renderer_48k = false;
        p.upsample_after_final_mix = true;
        p.system_param = 0x0229F638;
        p.system_size = 0x0229B2BC;
        p.system_init = 0x0229AF14;
        p.heap_construct = 0x0229FA84;
        p.heap_create = 0x022A0264;
        p.heap_size = 0x800000;
        p.archive_construct = 0x02290148;
        p.archive_buffer_size = 0x02290418;
        p.archive_open = 0x022908BC;
        p.open_priority = 0x10;
        p.open_cache = 8;
        p.file_header = 0x2D0;
        p.info_size = 0x02290C38;
        p.load_info = 0x02290E34;
        p.strings_size = 0x02290F44;
        p.load_strings = 0x02290F78;
        p.manager_construct = 0x0228F290;
        p.manager_size = 0x0228F330;
        p.manager_init = 0x0228F994;
        p.manager_loader = 0xC;
        p.player_construct = 0x02291B1C;
        p.player_size = 0x02291F84;
        p.stream_buffer_size = 0x022920F8;
        p.stream_buffers = 4;
        p.player_init = 0x02293488;
        p.player_update = 0x02293E0C;
        p.load_data = 0x022A7430;
        p.start_sound = 0x0228EE74;
        // ChannelManager::GetInstance (0x022B8B54) and UpdateAllChannel (0x022C97B4); Channel::Update (0x022C42C8),
        // CallChannelCallback (0x022B8BA0) and SequenceTrack::NoteOn (0x022C9A4C) for the channel's fields;
        // SequenceSoundPlayer::GetPlayerTrack (0x022B5588); MultiVoice::SetVolume and SetPitch (0x022AA3C8 and
        // 0x022AA3F4).
        p.channel_manager = 0x108D6910;
        p.channel_list = 0x10;
        p.channel_node = 0x2EC;
        p.channel_key = 0x224;
        p.channel_original_key = 0x225;
        p.channel_velocity = 0x22C;
        p.channel_envelope = 0x10;
        p.channel_track = 0x234;
        p.channel_voice = 0x238;
        p.track_player = 0x214;
        p.player_tracks = 0x1C8;
        p.voice_volume = 0x388;
        p.voice_pitch = 0x38C;
        // SequenceTrack::UpdateChannelParam (0x022B55B4) copies the TV's OutputParam to the channel at +0xCC (the
        // GamePads' OutputParams follow at +0x11C and +0x16C); Bank::NoteOn sets the note's pan; Util::CalcRandom
        // (0x0229EF64).
        p.channel_tv_param = 0xCC;
        p.param_pan = 0x38;
        p.param_span = 0x3C;
        p.channel_note_pan = 0x1FC;
        p.random_state = 0x101AA438;
        return p;
    }();

    return profile;
}

std::vector<uint32_t> ProfileWords(const Profile& p)
{
    const uint32_t ax = (p.renderer_48k ? 1u : 0u) | (p.upsample_after_final_mix ? 2u : 0u);

    return {p.static_init,       p.setting_address, p.setting_value,      ax,
            p.system_param,      p.system_size,     p.system_init,        p.heap_construct,
            p.heap_create,       p.heap_size,       p.archive_construct,  p.archive_buffer_size,
            p.archive_open,      p.open_priority,   p.open_cache,         p.file_header,
            p.info_size,         p.load_info,       p.strings_size,       p.load_strings,
            p.manager_construct, p.manager_size,    p.manager_init,       p.manager_loader,
            p.player_construct,  p.player_size,     p.stream_buffer_size, p.stream_buffers,
            p.player_init,       p.player_update,   p.load_data,          p.start_sound};
}

std::optional<Profile> ProfileFromWords(std::span<const uint32_t> words)
{
    if (words.size() != ProfileWords(Profile{}).size())
    {
        return std::nullopt;
    }

    Profile p;
    uint32_t ax = 0;
    uint32_t* const fields[] = {&p.static_init,       &p.setting_address, &p.setting_value,      &ax,
                                &p.system_param,      &p.system_size,     &p.system_init,        &p.heap_construct,
                                &p.heap_create,       &p.heap_size,       &p.archive_construct,  &p.archive_buffer_size,
                                &p.archive_open,      &p.open_priority,   &p.open_cache,         &p.file_header,
                                &p.info_size,         &p.load_info,       &p.strings_size,       &p.load_strings,
                                &p.manager_construct, &p.manager_size,    &p.manager_init,       &p.manager_loader,
                                &p.player_construct,  &p.player_size,     &p.stream_buffer_size, &p.stream_buffers,
                                &p.player_init,       &p.player_update,   &p.load_data,          &p.start_sound};
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        *fields[i] = words[i];
    }
    p.renderer_48k = (ax & 1) != 0;
    p.upsample_after_final_mix = (ax & 2) != 0;

    return p;
}

Driver::Driver(const Profile& profile, const cafe::Rpl& executable, std::map<std::string, std::vector<uint8_t>> files)
    : profile_(profile), machine_(kMachineHeap)
{
    machine_.LoadExecutable(executable);
    coreinit_.Start(kDefaultHeap);
    sndcore2_.Start();
    for (auto& [path, bytes] : files)
    {
        coreinit_.AddFile("/vol/content/" + path, std::move(bytes));
    }

    // While the game waits for its threads, time passes a frame at a time.
    machine_.idle = [this]
    {
        Frame();
        return true;
    };

    // The game's start-up, then its sound system's start-up, in the order that the profile's game uses.
    machine_.Call(profile_.static_init);
    if (profile_.setting_address != 0)
    {
        machine_.memory.Write32(profile_.setting_address, profile_.setting_value);
    }
    sndcore2_.Init(profile_.renderer_48k);
    sndcore2_.System().SetDeviceUpsampleStage(ax::System::Device::kTv, profile_.upsample_after_final_mix);
    sndcore2_.System().SetDeviceUpsampleStage(ax::System::Device::kDrc, profile_.upsample_after_final_mix);

    const uint32_t param = Allocate(kObjectSize);
    machine_.Call(profile_.system_param, {param});
    const uint32_t system_size = machine_.Call(profile_.system_size, {param}).r3;
    machine_.Call(profile_.system_init, {param, Allocate(system_size, 32), system_size});

    heap_ = Allocate(kObjectSize);
    machine_.Call(profile_.heap_construct, {heap_});
    if (machine_.Call(profile_.heap_create, {heap_, Allocate(profile_.heap_size, 32), profile_.heap_size}).r3 == 0)
    {
        throw std::runtime_error("the game's sound heap couldn't be made");
    }
}

void Driver::Start(const std::string& path, uint32_t id)
{
    // The archive, its info and its labels.
    const uint32_t archive = Allocate(kObjectSize, 8);
    machine_.Call(profile_.archive_construct, {archive});
    const uint32_t buffer_size = machine_.Call(profile_.archive_buffer_size, {archive}).r3;
    if (machine_
            .Call(profile_.archive_open,
                  {archive, Allocate(kClientSize), String("/vol/content/" + path), Allocate(buffer_size), buffer_size,
                   profile_.open_priority, profile_.open_cache})
            .r3 == 0)
    {
        throw std::runtime_error("the game couldn't open " + path);
    }
    const uint32_t header = archive + profile_.file_header;
    const uint32_t info_size = machine_.Call(profile_.info_size, {header}).r3;
    const uint32_t strings_size = machine_.Call(profile_.strings_size, {header}).r3;
    if (machine_.Call(profile_.load_info, {archive, Allocate(info_size), info_size}).r3 == 0 ||
        machine_.Call(profile_.load_strings, {archive, Allocate(strings_size), strings_size}).r3 == 0)
    {
        throw std::runtime_error("the game couldn't read " + path);
    }

    // The data manager and the archive player.
    const uint32_t manager = Allocate(kObjectSize, 8);
    machine_.Call(profile_.manager_construct, {manager});
    const uint32_t manager_size = machine_.Call(profile_.manager_size, {manager, archive}).r3;
    if (machine_.Call(profile_.manager_init, {manager, archive, Allocate(manager_size), manager_size}).r3 == 0)
    {
        throw std::runtime_error("the game's data manager couldn't be set up");
    }
    player_ = Allocate(kObjectSize, 8);
    machine_.Call(profile_.player_construct, {player_});
    const uint32_t setup_size = machine_.Call(profile_.player_size, {player_, archive, 0}).r3;
    const uint32_t stream_size =
        machine_.Call(profile_.stream_buffer_size, {player_, archive}).r3 * profile_.stream_buffers;
    // SoundArchivePlayer::InitializeParam: the archive, the data manager, the setup buffer and its size, the stream
    // buffer and its size, the stream cache and its size (none), and the user parameters' size (none).
    const uint32_t init = Allocate(0x40, 8);
    const uint32_t words[] = {
        archive, manager, Allocate(setup_size), setup_size, stream_size ? Allocate(stream_size) : 0, stream_size, 0,
        0,       0};
    for (uint32_t i = 0; i < std::size(words); ++i)
    {
        machine_.memory.Write32(init + 4 * i, words[i]);
    }
    if (machine_.Call(profile_.player_init, {player_, init}).r3 == 0)
    {
        throw std::runtime_error("the game's archive player couldn't be set up");
    }

    // The sound's data (everything it requires: its sequence, banks and waves), and the sound.
    if (machine_.Call(profile_.load_data, {manager + profile_.manager_loader, id, heap_, 0xFFFFFFFF, 0}).r3 == 0)
    {
        throw std::runtime_error("the game couldn't load the sound's data");
    }
    handle_ = Allocate(0x20, 8);
    const uint32_t result = machine_.Call(profile_.start_sound, {player_, handle_, id, 0}).r3;
    if (result != 0)
    {
        throw std::runtime_error("the game didn't start the sound (" + std::to_string(result) + ")");
    }
}

std::span<const int16_t> Driver::Frame()
{
    quarter_ticks_ += kFrameQuarterTicks;
    coreinit_.AdvanceTime(quarter_ticks_ / 4 - coreinit_.Time());
    sndcore2_.RenderFrame();
    ++frames_;

    // Outside the game's waits, the game's threads run. The main loop updates the player every 60th of a second: the
    // k-th update comes once 3n ms reach k * 1000 / 60 ms, that is once 9n >= 50k.
    if (machine_.Current() == &machine_.MainThread())
    {
        machine_.RunThreads();
        while (player_ != 0 && 9 * frames_ >= 50 * updates_)
        {
            machine_.Call(profile_.player_update, {player_});
            ++updates_;
        }
    }

    return sndcore2_.System().Output();
}

std::vector<ChannelState> Driver::Channels()
{
    if (profile_.channel_manager == 0)
    {
        return {};
    }

    cafe::Memory& memory = machine_.memory;
    std::vector<ChannelState> channels;
    const uint32_t list = profile_.channel_manager + profile_.channel_list;
    for (uint32_t node = memory.Read32(list); node != list && node != 0; node = memory.Read32(node))
    {
        const uint32_t channel = node - profile_.channel_node;
        ChannelState state;
        state.id = channel;
        state.key = memory.Read8(channel + profile_.channel_key);
        state.original_key = memory.Read8(channel + profile_.channel_original_key);
        state.velocity = memory.ReadFloat(channel + profile_.channel_velocity);
        state.envelope = memory.Read32(channel + profile_.channel_envelope);
        state.level = memory.ReadFloat(channel + profile_.channel_envelope + 4);
        state.pan = memory.ReadFloat(channel + profile_.channel_tv_param + profile_.param_pan);
        state.span = memory.ReadFloat(channel + profile_.channel_tv_param + profile_.param_span);
        state.note_pan = memory.ReadFloat(channel + profile_.channel_note_pan);
        if (const uint32_t voice = memory.Read32(channel + profile_.channel_voice))
        {
            state.volume = memory.ReadFloat(voice + profile_.voice_volume);
            state.pitch = memory.ReadFloat(voice + profile_.voice_pitch);
        }
        // The track: the callback's data, if it's one of its player's tracks.
        const uint32_t track = memory.Read32(channel + profile_.channel_track);
        if (track != 0 && !memory.RegionAt(track + profile_.track_player).empty())
        {
            const uint32_t player = memory.Read32(track + profile_.track_player);
            for (int i = 0; i < 16 && player != 0 && !memory.RegionAt(player).empty(); ++i)
            {
                if (memory.Read32(player + profile_.player_tracks + 4 * static_cast<uint32_t>(i)) == track)
                {
                    state.track = i;
                }
            }
        }
        channels.push_back(state);
    }

    return channels;
}

bool Driver::IsFinished()
{
    return handle_ != 0 && machine_.memory.Read32(handle_) == 0;
}

uint32_t Driver::RandomState()
{
    return machine_.memory.Read32(profile_.random_state);
}

uint32_t Driver::Allocate(uint32_t size, uint32_t align)
{
    return coreinit_.Alloc(size, align);
}

uint32_t Driver::String(const std::string& text)
{
    const auto size = static_cast<uint32_t>(text.size() + 1);
    const uint32_t address = Allocate(size, 8);
    std::memcpy(machine_.memory.Pointer(address, size), text.c_str(), size);

    return address;
}

} // namespace csf::game
