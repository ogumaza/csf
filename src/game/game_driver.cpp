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
#include "game/driver.h"
#include "game/jaudio1_driver.h"
#include "game/jaudio2_driver.h"

namespace csf::game
{
namespace
{

// The nw::snd driver with its profile. The driver holds the profile by reference.
class NwSndDriver final : public GameDriver
{
public:
    NwSndDriver(const Profile& profile, const cafe::Rpl& executable, std::map<std::string, std::vector<uint8_t>> files)
        : profile_(profile), driver_(profile_, executable, std::move(files))
    {
    }

    // nw::snd loads a sound's waves itself.
    void Start(const std::string& path, uint32_t sound, std::span<const uint32_t>) override
    {
        driver_.Start(path, sound);
    }

    std::span<const int16_t> Frame() override
    {
        return driver_.Frame();
    }

    bool IsFinished() override
    {
        return driver_.IsFinished();
    }

private:
    Profile profile_;
    Driver driver_;
};

} // namespace

std::unique_ptr<GameDriver> MakeGameDriver(uint32_t driver, const std::vector<uint32_t>& words,
                                           const cafe::Rpl& executable,
                                           std::map<std::string, std::vector<uint8_t>> files, std::string& error)
{
    if (driver == static_cast<uint32_t>(GameDriverKind::kJAudio1))
    {
        const std::optional<JAudio1Profile> profile = JAudio1ProfileFromWords(words);
        if (!profile)
        {
            error = "the JAudio1 profile has " + std::to_string(words.size()) + " words; the driver takes " +
                    std::to_string(kJAudio1ProfileWords);
            return nullptr;
        }

        return std::make_unique<JAudio1Driver>(*profile, executable, std::move(files));
    }

    if (driver == static_cast<uint32_t>(GameDriverKind::kJAudio2))
    {
        const std::optional<JAudio2Profile> profile = JAudio2ProfileFromWords(words);
        if (!profile)
        {
            error = "the JAudio2 profile has " + std::to_string(words.size()) + " words; the driver takes " +
                    std::to_string(kJAudio2ProfileWords);
            return nullptr;
        }

        return std::make_unique<JAudio2Driver>(*profile, executable, std::move(files));
    }

    if (driver != static_cast<uint32_t>(GameDriverKind::kNwSnd))
    {
        error = "this player has no game-mode driver " + std::to_string(driver) + " yet";
        return nullptr;
    }

    const std::optional<Profile> profile = ProfileFromWords(words);
    if (!profile)
    {
        error = "the game profile has " + std::to_string(words.size()) + " words";
        return nullptr;
    }

    return std::make_unique<NwSndDriver>(*profile, executable, std::move(files));
}

} // namespace csf::game
