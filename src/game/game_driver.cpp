// SPDX-License-Identifier: MIT

#include "game/game_driver.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"

#include "cafe/coreinit.h"
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

// The driver `Driver`, whose profile is `Profile`, with the profile `words`: all of the profile's words, or the
// `earlier` or `earliest` words of the profile in earlier versions of the format. `library` names the driver's sound
// library in the error for words that don't fit the profile.
template <typename Driver, typename Profile>
std::unique_ptr<GameDriver> MakeDriver(const char* library, const std::vector<uint32_t>& words, std::size_t earlier,
                                       std::size_t earliest, const cafe::Rpl& executable,
                                       std::map<std::string, std::vector<uint8_t>> files, std::string& error)
{
    const std::optional<Profile> profile = ProfileFromWords<Profile>(words, earlier, earliest);
    if (!profile)
    {
        constexpr std::size_t kAllWords = kProfileWordCount<Profile>;
        error = std::string("the ") + library + " profile has " + std::to_string(words.size()) +
                " words; the driver takes " + (earliest == kAllWords ? "" : std::to_string(earliest) + ", ") +
                (earlier == kAllWords ? "" : std::to_string(earlier) + " or ") + std::to_string(kAllWords);
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

std::set<std::string> GameDriver::FilesRead() const
{
    // Coreinit keeps the guest's paths in lowercase.
    std::set<std::string> read;
    for (const std::string& path : paths_)
    {
        if (coreinit_.FilesRead().contains(xsflib::AsciiLower(cafe::GuestContentPath(path))))
        {
            read.insert(path);
        }
    }

    return read;
}

void GameDriver::Load(const cafe::Rpl& executable, uint32_t default_heap,
                      std::map<std::string, std::vector<uint8_t>> files)
{
    machine_.LoadExecutable(executable);
    coreinit_.Start(default_heap);
    sndcore2_.Start();
    for (auto& [path, bytes] : files)
    {
        coreinit_.AddFile(cafe::GuestContentPath(path), std::move(bytes));
        paths_.push_back(path);
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
        // A profile from before version 3 of the format stops before setPortData, msBasic and the starters of music.
        return MakeDriver<JAudio1Driver, JAudio1Profile>("JAudio1", words, kJAudio1Version2Words,
                                                         kProfileWordCount<JAudio1Profile>, executable,
                                                         std::move(files), error);
    }
    if (driver == static_cast<uint32_t>(GameDriverKind::kJAudio2))
    {
        return MakeDriver<JAudio2Driver, JAudio2Profile>("JAudio2", words, kJAudio2ManagerProfileWords,
                                                         kJAudio2FirstProfileWords, executable, std::move(files),
                                                         error);
    }

    if (driver != static_cast<uint32_t>(GameDriverKind::kNwSnd))
    {
        error = "this player has no game-mode driver " + std::to_string(driver) + " yet";
        return nullptr;
    }

    // A profile from before version 3 of the format stops before the variables' writers.
    return MakeDriver<NwSndDriver, NwSndProfile>("nw::snd", words, kProfileWords, kProfileWordCount<NwSndProfile>,
                                                 executable, std::move(files), error);
}

} // namespace csf::game
