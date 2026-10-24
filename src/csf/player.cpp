// SPDX-License-Identifier: MIT

// CSF player core (see player.h).

#include "csf/player.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/sar/set_archive.h"
#include "xsflib/player/archive_source.h"
#include "xsflib/psf/psf.h"
#include "xsflib/text.h"

#include "cafe/rpl.h"
#include "csf/format.h"
#include "csf/model.h"
#include "csf/ntdlib_cafe.h"
#include "csf/sar.h"
#include "game/game_driver.h"

namespace csf
{
namespace
{

std::string Missing(const std::string& path)
{
    return "the set has no file " + path;
}

// Archive mode's steps on xsflib's ArchiveSource: the sound archive with its group files, and the model playing the
// sequence on it. The first start moves the archive's bytes out of the set. Later starts reuse the archive.
struct ArchiveTraits
{
    using Set = LoadedSet;
    using Archive = fsar::SoundArchive;
    using Model = nwsnd::ArchiveModel;

    // The model's options, from the archive descriptor (csf/model.h).
    struct Prepared
    {
        nwsnd::EngineOptions options;
    };

    static constexpr bool kKeepArchive = true;

    // The player has checked the descriptor. LoadSet requires a sound.
    static std::optional<std::string> Prepare(const LoadedSet& set, Prepared& prepared)
    {
        prepared.options = EngineOptionsFor(*set.archive, fsar::ItemNumber(*set.sound));

        return std::nullopt;
    }

    // The set's bytes move into the archive. The archive is only given out once all its group files are in it.
    static std::optional<std::string> LoadArchive(LoadedSet& set, const Prepared&, std::optional<Archive>& archive)
    {
        const ArchiveDescriptor& d = *set.archive;
        return fsar::LoadSetArchive(set, d.archive_path, d.group_paths, kSarOptions, archive);
    }

    static std::unique_ptr<Model> MakeModel(const Prepared& prepared, const Archive& archive)
    {
        return std::make_unique<Model>(archive, prepared.options);
    }

    static std::optional<std::string> StartModel(Model&, const Archive&, const Prepared&)
    {
        return std::nullopt;
    }

    // A sound's nw::snd item ID is its index in the archive's sound table, with the sound type in its top byte.
    static std::optional<std::string> StartSequence(Model& model, const LoadedSet& set, const Prepared&)
    {
        if (fsar::ItemType(*set.sound) != fsar::kSoundItem ||
            !model.engine->StartSequence(fsar::ItemNumber(*set.sound)))
        {
            return "sound " + xsflib::Hex(*set.sound, 8) + " isn't a sequence whose files the set holds";
        }

        return std::nullopt;
    }

    // A frame of 3 ms: the TV's output, 144 interleaved stereo pairs.
    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> frame = model.system.Output();
        out.insert(out.end(), frame.begin(), frame.end());

        return true;
    }

    // The sound plays on while its effects ring.
    static bool Busy(const Model& model)
    {
        return model.engine->IsBusy() || model.engine->EffectsRinging();
    }
};

} // namespace

// The sound's source: in archive mode, xsflib's ArchiveSource, which keeps the archive for restarts; in game mode, the
// game's driver, made anew at each start.
class Player::Source
{
public:
    // Archive mode: the model playing the set's sequence. The first start moves the set's archive and group files into
    // the source.
    std::optional<std::string> StartArchive(LoadedSet& set, std::vector<int16_t>& out)
    {
        return archive_.Start(set, out);
    }

    // Game mode: the game's executable running its sound code, given the set's files under content/.
    std::optional<std::string> StartGame(const LoadedSet& set);

    // Runs a frame of 3 ms and adds the TV's output to `out`: 144 interleaved stereo pairs.
    void Frame(std::vector<int16_t>& out);

    bool Finished();

private:
    xsflib::player::ArchiveSource<ArchiveTraits> archive_;
    std::unique_ptr<game::GameDriver> game_; // game mode's driver
};

std::optional<std::string> Player::Source::StartGame(const LoadedSet& set)
{
    // A restart frees the last start's driver first.
    game_.reset();

    const ProcessDescriptor& d = *set.descriptor;
    const std::vector<uint8_t>* executable = set.File(d.executable_path);
    if (!executable)
    {
        return Missing(d.executable_path);
    }

    const std::optional<std::string> archive = ContentPath(d.archive_path);
    if (!archive || !set.File(d.archive_path))
    {
        return Missing(d.archive_path);
    }

    const std::optional<std::vector<uint32_t>> waves = WaveGroups(set);
    if (!waves)
    {
        return "the waves file's size isn't a multiple of 4";
    }

    // The driver copies the executable into the game's memory. The parsed file is freed once the driver is made.
    std::string error;
    std::unique_ptr<game::GameDriver> driver =
        game::MakeGameDriver(d.driver, d.profile, cafe::Rpl::Read(*executable), ContentFiles(set.files), error);
    if (!driver)
    {
        return error;
    }

    // The ports that the set's tag gives the sound, then the sound.
    if (std::optional<xsflib::psf::Variables> ports = TagPorts(set.tags))
    {
        driver->SetPorts(std::move(*ports));
    }

    driver->Start(*archive, *set.sound, *waves);
    game_ = std::move(driver);

    return std::nullopt;
}

void Player::Source::Frame(std::vector<int16_t>& out)
{
    if (game_)
    {
        const std::span<const int16_t> frame = game_->Frame();
        out.insert(out.end(), frame.begin(), frame.end());
        return;
    }

    archive_.RunFrame(out);
}

bool Player::Source::Finished()
{
    return game_ ? game_->IsFinished() : !archive_.Busy();
}

Player::Player() = default;

Player::~Player() = default;

void Player::Unload()
{
    source_.reset();
}

bool Player::StartSource()
{
    output_.clear();
    if (!source_)
    {
        source_ = std::make_unique<Source>();
    }

    const std::optional<std::string> err =
        set_.archive ? source_->StartArchive(set_, output_) : source_->StartGame(set_);
    if (err)
    {
        return Fail(*err);
    }

    return true;
}

bool Player::RunStep()
{
    source_->Frame(output_);

    return true;
}

bool Player::Finished()
{
    return source_->Finished();
}

} // namespace csf
