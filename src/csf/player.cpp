// SPDX-License-Identifier: MIT

// CSF player core (see player.h).

#include "csf/player.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/sar/set_archive.h"
#include "xsflib/player/archive_source.h"
#include "xsflib/text.h"

#include "cafe/rpl.h"
#include "csf/sar.h"
#include "game/game_driver.h"
#include "nwsnd/engine.h"

namespace csf
{
namespace
{

// The title's content folder: game mode gives the game the set's files under it, at /vol/content/.
constexpr std::string_view kContent = "content/";

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

    // The model's options, from the archive descriptor: the effects are the ones that the game sets up for the sound.
    struct Prepared
    {
        nwsnd::EngineOptions options;
    };

    static constexpr bool kKeepArchive = true;

    static std::optional<std::string> Prepare(const LoadedSet& set, Prepared& prepared)
    {
        const ArchiveDescriptor& d = *set.archive;
        prepared.options.voice_count = static_cast<int>(d.voice_count);
        prepared.options.renderer_48k = (d.ax_settings & kAxRenderer48k) != 0;
        prepared.options.upsample_after_final_mix = (d.ax_settings & kAxUpsampleAfterFinalMix) != 0;
        if (set.sound)
        {
            prepared.options.effects = nwsnd::EffectsFor(d.effects, *set.sound & 0xFFFFFF);
        }

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

    static std::optional<std::string> StartSequence(Model& model, const LoadedSet& set, const Prepared&)
    {
        // A sound's nw::snd item id is its index in the archive's sound table, with the sound type in its top byte.
        if ((*set.sound >> 24) != fsar::kSoundItem || !model.engine->StartSequence(*set.sound & 0xFFFFFF))
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
    if (!d.archive_path.starts_with(kContent) || !set.File(d.archive_path))
    {
        return Missing(d.archive_path);
    }

    // The title's files under content/. A later FILE chunk replaces an earlier one with the same path.
    std::map<std::string, std::vector<uint8_t>> files;
    for (const FileChunk& file : set.files)
    {
        if (file.path.starts_with(kContent))
        {
            files[file.path.substr(kContent.size())] = file.data;
        }
    }

    const std::optional<std::vector<uint32_t>> waves = WaveGroups(set);
    if (!waves)
    {
        return "the waves file's size isn't a multiple of 4";
    }

    // The driver copies the executable into the game's memory. The parsed file is freed once the driver is made.
    std::string error;
    std::unique_ptr<game::GameDriver> driver =
        game::MakeGameDriver(d.driver, d.profile, cafe::Rpl::Read(*executable), std::move(files), error);
    if (!driver)
    {
        return error;
    }

    driver->Start(d.archive_path.substr(kContent.size()), *set.sound, *waves);
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
