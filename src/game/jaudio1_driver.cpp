// SPDX-License-Identifier: MIT

#include "game/jaudio1_driver.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/cafe/ax/system.h"

#include "cafe/rpl.h"
#include "csf/ntdlib_cafe.h"
#include "game/game_driver.h"

namespace csf::game
{
namespace
{

using Words = std::array<uint32_t, kJAudio1ProfileWords>;

// The game's frames: 30 a second, as the GameCube game's are. Game frame k comes once n of AX's frames of 3 ms reach
// k * 1000 / 30 ms, that is once 9n >= 100k.
bool GameFrameDue(uint64_t ax_frames, uint64_t game_frame)
{
    return 9 * ax_frames >= 100 * game_frame;
}

// The game's frames that the start-up and a wave group's load may take before the driver gives up.
constexpr int kSetupFrames = 600;
constexpr int kWaveFrames = 600;

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

std::optional<JAudio1Profile> JAudio1ProfileFromWords(const std::vector<uint32_t>& words)
{
    if (words.size() != kJAudio1ProfileWords)
    {
        return std::nullopt;
    }

    Words array;
    std::memcpy(array.data(), words.data(), sizeof(array));

    return std::bit_cast<JAudio1Profile>(array);
}

std::vector<uint32_t> JAudio1ProfileWords(const JAudio1Profile& profile)
{
    const auto array = std::bit_cast<Words>(profile);
    return {array.begin(), array.end()};
}

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
    : profile_(profile), machine_(profile.machine_heap)
{
    const JAudio1Profile& p = profile_;
    cafe::Memory& memory = machine_.memory;
    RegisterStartupStubs(machine_);
    machine_.LoadExecutable(executable);
    coreinit_.Start(p.default_heap);
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

    // The static constructors, then sead's framework with the heap that the machine can give, then the game's root
    // heaps, which hold the sound's.
    machine_.Call(p.static_init);
    const uint32_t argument = machine_.Call(p.sead_argument, {coreinit_.Alloc(8)}).r3;
    memory.Write32(argument, p.sead_heap_size);
    machine_.Call(p.sead_init, {argument});
    machine_.Call(p.create_root_heaps);
    machine_.Call(p.create_more_heaps);
    const uint32_t root = memory.Read32(p.sound_root_heap);
    if (root == 0)
    {
        throw std::runtime_error("the game's sound heap couldn't be made");
    }

    // AX, as the game starts it: snd_core's 32 kHz renderer. Then, from the sound's root heap, the game's sound layer
    // (which a task of the game's framework creates) and JAudio. JAudio's init returns once the audio thread has
    // started. The thread starts after the first final-mix callback, which the frames run while the game sleeps.
    sndcore2_.Init(ax::System::Library::kSndCore, false);
    machine_.Call(p.set_current_heap, {0, root});
    machine_.Call(p.create_sound_layer, {0});
    machine_.Call(p.basic_init, {p.basic, root, p.aram_size});

    // The start-up's sounds and stay waves load a frame at a time.
    ready_ = true;
    for (int frame = 0; !SetupDone(); ++frame)
    {
        if (frame == kSetupFrames)
        {
            throw std::runtime_error("the game's sound start-up didn't finish");
        }

        GameFrame();
    }
}

void JAudio1Driver::Start(const std::string&, uint32_t sound, std::span<const uint32_t> waves)
{
    const JAudio1Profile& p = profile_;
    cafe::Memory& memory = machine_.memory;

    // The sound's wave groups, one group of a wave system each. A wave system that JAudio has loaded keeps its group.
    const uint32_t systems = WaveSystemCount();
    for (const uint32_t wave : waves)
    {
        const uint32_t system = wave >> 16;
        if (system < systems && !WaveSystemLoaded(system))
        {
            machine_.Call(p.load_group_wave, {system, wave & 0xffff});
        }
    }

    for (const uint32_t wave : waves)
    {
        const uint32_t system = wave >> 16;
        for (int frame = 0; system < systems && !WaveSystemLoaded(system); ++frame)
        {
            if (frame == kWaveFrames)
            {
                throw std::runtime_error("wave group " + std::to_string(system) + "/" + std::to_string(wave & 0xffff) +
                                         " didn't load");
            }

            GameFrame();
        }
    }

    // The default camera, which JAIZelBasic's init cleared, then the sound at no position.
    machine_.Call(p.set_camera, {p.basic, memory.Read32(p.camera_position), memory.Read32(p.camera_last_position),
                                 p.camera_matrix, 0});
    handle_ = coreinit_.Alloc(4);
    memory.Write32(handle_, 0);
    machine_.Call(p.start_sound, {p.basic, sound, handle_, 0, 0, 0, 4});
    if (memory.Read32(handle_) == 0)
    {
        throw std::runtime_error("the game didn't start the sound");
    }
}

std::span<const int16_t> JAudio1Driver::Frame()
{
    quarter_ticks_ += kFrameQuarterTicks;
    coreinit_.AdvanceTime(quarter_ticks_ / 4 - coreinit_.Time());
    sndcore2_.RenderFrame();
    ++frames_;

    // Outside the game's waits, the game's threads run, and its frames come 30 times a second.
    if (machine_.Current() == &machine_.MainThread())
    {
        machine_.RunThreads();
        while (ready_ && GameFrameDue(frames_, game_frames_))
        {
            machine_.Call(profile_.frame_process, {profile_.basic});
            ++game_frames_;
        }
    }

    return sndcore2_.System().Output();
}

bool JAudio1Driver::IsFinished()
{
    return handle_ != 0 && machine_.memory.Read32(handle_) == 0;
}

bool JAudio1Driver::SetupDone()
{
    cafe::Memory& memory = machine_.memory;
    const uint32_t sound = memory.Read32(profile_.se_handle);
    if (sound == 0 || memory.Read8(sound + 1) < kSoundPlaying)
    {
        return false;
    }

    uint32_t system = 0;
    for (uint32_t entry = memory.Read32(profile_.wave_systems); memory.Read32(entry) != 0; entry += 12, ++system)
    {
        if (memory.Read32(entry + 8) <= kStayWaves && !WaveSystemLoaded(system))
        {
            return false;
        }
    }

    return true;
}

uint32_t JAudio1Driver::WaveSystemCount()
{
    cafe::Memory& memory = machine_.memory;
    uint32_t count = 0;
    for (uint32_t entry = memory.Read32(profile_.wave_systems); memory.Read32(entry) != 0; entry += 12)
    {
        ++count;
    }

    return count;
}

bool JAudio1Driver::WaveSystemLoaded(uint32_t system)
{
    cafe::Memory& memory = machine_.memory;
    return memory.Read32(memory.Read32(profile_.wave_status) + 4 * system) == kWaveLoaded;
}

void JAudio1Driver::GameFrame()
{
    const uint64_t next = game_frames_ + 1;
    while (game_frames_ < next)
    {
        Frame();
    }
}

} // namespace csf::game
