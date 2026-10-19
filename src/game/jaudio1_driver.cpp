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

#include "cafe/rpl.h"
#include "csf/ntdlib_cafe.h"

namespace csf::game
{
namespace
{

// A wave system's status once its group has loaded, the state that the effects sequence's sound reaches once the
// start-up has loaded the stay waves, and the wave systems' load types that the start-up loads.
constexpr uint32_t kWaveLoaded = 2;
constexpr uint8_t kSoundPlaying = 4;
constexpr uint32_t kStayWaves = 1;

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
    };
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

    // The default camera, which JAIZelBasic's init cleared, then the sound at no position.
    machine_.Call(p.set_camera, {p.basic, memory.Read32(p.camera_position), memory.Read32(p.camera_last_position),
                                 p.camera_matrix, 0});
    handle_ = coreinit_.Alloc(4);
    machine_.Call(p.start_sound, {p.basic, sound, handle_, 0, 0, 0, 4});
    if (memory.Read32(handle_) == 0)
    {
        throw std::runtime_error("the game didn't start the sound");
    }
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
    // (which a task of the game's framework creates) and JAudio. JAudio's init returns once the audio thread has
    // started. The thread starts after the first final-mix callback, which the frames run while the game sleeps.
    const JAudio1Profile& p = profile_;
    sndcore2_.Init(ax::System::Library::kSndCore, false);
    machine_.Call(p.set_current_heap, {0, root});
    machine_.Call(p.create_sound_layer, {0});
    machine_.Call(p.basic_init, {p.basic, root, p.aram_size});
}

void JAudio1Driver::FrameProcess()
{
    machine_.Call(profile_.frame_process, {profile_.basic});
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
