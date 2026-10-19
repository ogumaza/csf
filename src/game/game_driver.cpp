// SPDX-License-Identifier: MIT

#include "game/game_driver.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "cafe/rpl.h"
#include "csf/format.h"
#include "game/jaudio1_driver.h"
#include "game/jaudio2_driver.h"
#include "game/nwsnd_driver.h"

namespace csf::game
{
namespace
{

// A frame of AX is 3 ms: 186,468.75 ticks of 62,156,250 a second. So the time counts in quarters of a tick.
constexpr uint64_t kFrameQuarterTicks = 745'875;

// The JAudio driver `Driver`, whose profile is `Profile`, with the profile `words`. `library` names the driver's sound
// library in the error for words that don't fit the profile.
template <typename Driver, typename Profile>
std::unique_ptr<GameDriver> MakeJAudioDriver(const char* library, const std::vector<uint32_t>& words,
                                             const cafe::Rpl& executable,
                                             std::map<std::string, std::vector<uint8_t>> files, std::string& error)
{
    const std::optional<Profile> profile = ProfileFromWords<Profile>(words);
    if (!profile)
    {
        error = std::string("the ") + library + " profile has " + std::to_string(words.size()) +
                " words; the driver takes " + std::to_string(kProfileWordCount<Profile>);
        return nullptr;
    }

    return std::make_unique<Driver>(*profile, executable, std::move(files));
}

} // namespace

GameDriver::GameDriver(uint32_t machine_heap) : machine_(machine_heap)
{
}

std::span<const int16_t> GameDriver::Frame()
{
    quarter_ticks_ += kFrameQuarterTicks;
    coreinit_.AdvanceTime(quarter_ticks_ / 4 - coreinit_.Time());
    sndcore2_.RenderFrame();
    ++frames_;

    if (machine_.Current() == &machine_.MainThread())
    {
        machine_.RunThreads();
        RunMainLoop();
    }

    return sndcore2_.System().Output();
}

bool GameDriver::IsFinished()
{
    return handle_ != 0 && machine_.memory.Read32(handle_) == 0;
}

void GameDriver::Load(const cafe::Rpl& executable, uint32_t default_heap,
                      std::map<std::string, std::vector<uint8_t>> files)
{
    machine_.LoadExecutable(executable);
    coreinit_.Start(default_heap);
    sndcore2_.Start();
    for (auto& [path, bytes] : files)
    {
        coreinit_.AddFile("/vol/content/" + path, std::move(bytes));
    }

    machine_.idle = [this]
    {
        Frame();

        return true;
    };
}

bool GameDriver::TimeReaches(uint64_t count, uint64_t per_second) const
{
    return 3 * frames_ * per_second >= 1000 * count;
}

std::unique_ptr<GameDriver> MakeGameDriver(uint32_t driver, const std::vector<uint32_t>& words,
                                           const cafe::Rpl& executable,
                                           std::map<std::string, std::vector<uint8_t>> files, std::string& error)
{
    if (driver == static_cast<uint32_t>(GameDriverKind::kJAudio1))
    {
        return MakeJAudioDriver<JAudio1Driver, JAudio1Profile>("JAudio1", words, executable, std::move(files), error);
    }
    if (driver == static_cast<uint32_t>(GameDriverKind::kJAudio2))
    {
        return MakeJAudioDriver<JAudio2Driver, JAudio2Profile>("JAudio2", words, executable, std::move(files), error);
    }

    if (driver != static_cast<uint32_t>(GameDriverKind::kNwSnd))
    {
        error = "this player has no game-mode driver " + std::to_string(driver) + " yet";
        return nullptr;
    }

    const std::optional<NwSndProfile> profile = ProfileFromWords<NwSndProfile>(words);
    if (!profile)
    {
        error = "the game profile has " + std::to_string(words.size()) + " words";
        return nullptr;
    }

    return std::make_unique<NwSndDriver>(*profile, executable, std::move(files));
}

} // namespace csf::game
