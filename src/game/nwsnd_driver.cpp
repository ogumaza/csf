// SPDX-License-Identifier: MIT

#include "game/nwsnd_driver.h"

#include <cstdint>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/cafe/ax/system.h"

#include "cafe/coreinit.h"
#include "cafe/rpl.h"
#include "csf/format.h"
#include "csf/ntdlib_cafe.h"
#include "game/game_driver.h"

namespace csf::game
{
namespace
{

// The machine's heap, for the system's structures, and the default heap's share of it, for the driver's objects and the
// game's objects.
constexpr uint32_t kMachineHeap = 64 << 20;
constexpr uint32_t kDefaultHeap = 48 << 20;

// The sizes of the objects that the driver makes, with room to spare. The game embeds these objects in larger ones.
constexpr uint32_t kObjectSize = 0x800;
constexpr uint32_t kClientSize = 0x1700;

// A version 1 process descriptor's profile is this driver's (docs/csf.md, "Driver").
static_assert(kProfileWordCount<NwSndProfile> == kProfileWords);

} // namespace

const NwSndProfile& AmiiboFestival()
{
    static const NwSndProfile kProfile{
        .static_init = 0x02001FF0,
        // nw::snd's channel count.
        .setting_address = 0x101AA478,
        .setting_value = kAmiiboFestivalVoices,
        // AXInitWithParams with the 32 kHz renderer, and both devices upsampling after the final mix (0x020EEF7C).
        .ax_settings = kAxUpsampleAfterFinalMix,
        .system_param = 0x0229F638,
        .system_size = 0x0229B2BC,
        .system_init = 0x0229AF14,
        .heap_construct = 0x0229FA84,
        .heap_create = 0x022A0264,
        .heap_size = 0x800000,
        .archive_construct = 0x02290148,
        .archive_buffer_size = 0x02290418,
        .archive_open = 0x022908BC,
        .open_priority = 0x10,
        .open_cache = 8,
        .file_header = 0x2D0,
        .info_size = 0x02290C38,
        .load_info = 0x02290E34,
        .strings_size = 0x02290F44,
        .load_strings = 0x02290F78,
        .manager_construct = 0x0228F290,
        .manager_size = 0x0228F330,
        .manager_init = 0x0228F994,
        .manager_loader = 0xC,
        .player_construct = 0x02291B1C,
        .player_size = 0x02291F84,
        .stream_buffer_size = 0x022920F8,
        .stream_buffers = 4,
        .player_init = 0x02293488,
        .player_update = 0x02293E0C,
        .load_data = 0x022A7430,
        .start_sound = 0x0228EE74,
    };
    return kProfile;
}

NwSndDriver::NwSndDriver(const NwSndProfile& profile, const cafe::Rpl& executable,
                         std::map<std::string, std::vector<uint8_t>> files)
    : GameDriver(kMachineHeap), profile_(profile)
{
    Load(executable, kDefaultHeap, std::move(files));

    // The game's start-up, then its sound system's start-up, in the order that the profile's game uses.
    machine_.Call(profile_.static_init);
    if (profile_.setting_address != 0)
    {
        machine_.memory.Write32(profile_.setting_address, profile_.setting_value);
    }

    const bool upsample_after_final_mix = (profile_.ax_settings & kAxUpsampleAfterFinalMix) != 0;
    sndcore2_.Init((profile_.ax_settings & kAxRenderer48k) != 0);
    sndcore2_.System().SetDeviceUpsampleStage(ax::System::Device::kTv, upsample_after_final_mix);
    sndcore2_.System().SetDeviceUpsampleStage(ax::System::Device::kDrc, upsample_after_final_mix);

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

void NwSndDriver::Start(const std::string& path, uint32_t sound, std::span<const uint32_t>)
{
    const uint32_t archive = OpenArchive(path);
    const uint32_t manager = SetUpDataManager(archive);
    SetUpPlayer(archive, manager);

    // The sound's data (everything it requires: its sequence, banks and waves), and the sound.
    if (machine_.Call(profile_.load_data, {manager + profile_.manager_loader, sound, heap_, 0xFFFFFFFF, 0}).r3 == 0)
    {
        throw std::runtime_error("the game couldn't load the sound's data");
    }

    handle_ = Allocate(0x20, 8);
    const uint32_t result = machine_.Call(profile_.start_sound, {player_, handle_, sound, 0}).r3;
    if (result != 0)
    {
        throw std::runtime_error("the game didn't start the sound (" + std::to_string(result) + ")");
    }
}

uint32_t NwSndDriver::OpenArchive(const std::string& path)
{
    const uint32_t archive = Allocate(kObjectSize, 8);
    machine_.Call(profile_.archive_construct, {archive});
    const uint32_t buffer_size = machine_.Call(profile_.archive_buffer_size, {archive}).r3;
    if (machine_
            .Call(profile_.archive_open,
                  {archive, Allocate(kClientSize), String(cafe::GuestContentPath(path)), Allocate(buffer_size),
                   buffer_size, profile_.open_priority, profile_.open_cache})
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

    return archive;
}

uint32_t NwSndDriver::SetUpDataManager(uint32_t archive)
{
    const uint32_t manager = Allocate(kObjectSize, 8);
    machine_.Call(profile_.manager_construct, {manager});
    const uint32_t manager_size = machine_.Call(profile_.manager_size, {manager, archive}).r3;
    if (machine_.Call(profile_.manager_init, {manager, archive, Allocate(manager_size), manager_size}).r3 == 0)
    {
        throw std::runtime_error("the game's data manager couldn't be set up");
    }

    return manager;
}

void NwSndDriver::SetUpPlayer(uint32_t archive, uint32_t manager)
{
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
}

void NwSndDriver::RunMainLoop()
{
    while (player_ != 0 && TimeReaches(updates_, 60))
    {
        machine_.Call(profile_.player_update, {player_});
        ++updates_;
    }
}

uint32_t NwSndDriver::Allocate(uint32_t size, uint32_t align)
{
    return coreinit_.Alloc(size, align);
}

uint32_t NwSndDriver::String(const std::string& text)
{
    const uint32_t address = Allocate(static_cast<uint32_t>(text.size() + 1), 8);
    machine_.memory.WriteString(address, text);

    return address;
}

} // namespace csf::game
