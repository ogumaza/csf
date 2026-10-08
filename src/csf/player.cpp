// SPDX-License-Identifier: MIT

// CSF player core (see player.h).

#include "csf/player.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cafe/rpl.h"
#include "fsar/formats.h"
#include "game/driver.h"
#include "nwsnd/engine.h"

namespace csf
{
namespace
{

// The title's content folder: game mode gives the game the set's files under it, at /vol/content/.
constexpr std::string_view kContent = "content/";

// A sound's nw::snd item id is its index in the archive's sound table, with the sound type in its top byte.
constexpr uint32_t kSoundItem = 0x01;

std::string Hex(uint32_t value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(value));

    return buf;
}

std::string Missing(const std::string& path)
{
    return "the set has no file " + path;
}

} // namespace

class Player::Source
{
public:
    // Archive mode: the model playing the sound from `archive`. `archive` has the group files that the descriptor
    // names.
    std::optional<std::string> StartArchive(const LoadedSet& set, const fsar::SoundArchive& archive);

    // Game mode: the game's executable running its sound code, given the set's files under content/.
    std::optional<std::string> StartGame(const LoadedSet& set);

    // Runs a frame of 3 ms and returns the TV's output: 144 interleaved stereo pairs.
    std::span<const int16_t> Frame();

    bool Finished();

private:
    std::unique_ptr<nwsnd::ArchiveModel> model_;

    // Declared before the driver. The driver holds them.
    game::Profile profile_;
    std::optional<cafe::Rpl> executable_;
    std::unique_ptr<game::Driver> driver_;
};

std::optional<std::string> Player::Source::StartArchive(const LoadedSet& set, const fsar::SoundArchive& archive)
{
    const ArchiveDescriptor& d = *set.archive;
    nwsnd::EngineOptions options;
    options.voice_count = static_cast<int>(d.voice_count);
    options.renderer_48k = (d.ax_settings & kAxRenderer48k) != 0;
    options.upsample_after_final_mix = (d.ax_settings & kAxUpsampleAfterFinalMix) != 0;
    model_ = std::make_unique<nwsnd::ArchiveModel>(archive, options);
    if ((set.sound >> 24) != kSoundItem || !model_->engine->StartSequence(set.sound & 0xFFFFFF))
    {
        return "sound " + Hex(set.sound) + " isn't a sequence whose files the set holds";
    }

    return std::nullopt;
}

std::optional<std::string> Player::Source::StartGame(const LoadedSet& set)
{
    const ProcessDescriptor& d = *set.process;
    const std::vector<uint8_t>* executable = set.File(d.executable_path);
    if (!executable)
    {
        return Missing(d.executable_path);
    }
    if (!d.archive_path.starts_with(kContent) || !set.File(d.archive_path))
    {
        return Missing(d.archive_path);
    }

    const std::optional<game::Profile> profile = game::ProfileFromWords(d.profile);
    if (!profile)
    {
        return "the game profile has " + std::to_string(d.profile.size()) + " words";
    }
    profile_ = *profile;
    executable_ = cafe::Rpl::Read(*executable);

    // The title's files under content/. A later FILE chunk replaces an earlier one with the same path.
    std::map<std::string, std::vector<uint8_t>> files;
    for (const FileChunk& file : set.files)
    {
        if (file.path.starts_with(kContent))
        {
            files[file.path.substr(kContent.size())] = file.data;
        }
    }

    driver_ = std::make_unique<game::Driver>(profile_, *executable_, std::move(files));
    driver_->Start(d.archive_path.substr(kContent.size()), set.sound);

    return std::nullopt;
}

std::span<const int16_t> Player::Source::Frame()
{
    if (driver_)
    {
        return driver_->Frame();
    }

    model_->engine->RunFrame();

    return model_->system.Output();
}

bool Player::Source::Finished()
{
    return driver_ ? driver_->IsFinished() : !model_->engine->IsBusy();
}

Player::Player() = default;

Player::~Player() = default;

bool Player::Fail(const std::string& message)
{
    error_ = message;
    state_ = State::kError;

    return false;
}

bool Player::Load(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    if (auto err = LoadSet(path, reader, set))
    {
        return Fail(*err);
    }

    return Load(std::move(set));
}

bool Player::Load(LoadedSet set)
{
    source_.reset();
    archive_.reset();
    set_ = std::move(set);
    state_ = State::kIdle;
    error_.clear();
    if (!set_.process && !set_.archive)
    {
        return Fail("no process or archive descriptor");
    }

    return true;
}

bool Player::Start()
{
    // The model's and the emulation's failures arrive as exceptions.
    try
    {
        return StartImpl();
    }
    catch (const std::exception& e)
    {
        source_.reset();
        return Fail(std::string("the sound couldn't start: ") + e.what());
    }
}

bool Player::StartImpl()
{
    output_.clear();
    consumed_ = 0;
    dropped_ = 0;
    finished_frame_ = 0;
    error_.clear();
    if (!set_.process && !set_.archive)
    {
        return Fail("nothing loaded");
    }

    if (set_.archive)
    {
        if (auto err = ReadArchive())
        {
            return Fail(*err);
        }
    }

    source_ = std::make_unique<Source>();
    const std::optional<std::string> err =
        set_.archive ? source_->StartArchive(set_, *archive_) : source_->StartGame(set_);
    if (err)
    {
        source_.reset();
        return Fail(*err);
    }

    state_ = State::kPlaying;

    return true;
}

std::optional<std::string> Player::ReadArchive()
{
    if (archive_)
    {
        return std::nullopt;
    }

    // The set's bytes move into the archive. A restart reuses the archive.
    const ArchiveDescriptor& d = *set_.archive;
    std::vector<uint8_t>* bytes = set_.File(d.archive_path);
    if (!bytes)
    {
        return Missing(d.archive_path);
    }

    auto archive = std::make_unique<fsar::SoundArchive>(fsar::SoundArchive::Load(std::move(*bytes)));
    for (const std::string& path : d.group_paths)
    {
        std::vector<uint8_t>* group = set_.File(path);
        if (!group)
        {
            return Missing(path);
        }

        archive->AddGroupFile(std::move(*group));
    }

    archive_ = std::move(archive);

    return std::nullopt;
}

bool Player::Step()
{
    try
    {
        const std::span<const int16_t> frame = source_->Frame();
        output_.insert(output_.end(), frame.begin(), frame.end());
        if (state_ == State::kPlaying && source_->Finished())
        {
            state_ = State::kFinished;
            finished_frame_ = (dropped_ + output_.size()) / 2;
        }
    }
    catch (const std::exception& e)
    {
        return Fail(std::string("emulation failed: ") + e.what());
    }

    return true;
}

std::size_t Player::Render(int16_t* out, std::size_t frames)
{
    if (state_ != State::kPlaying && state_ != State::kFinished)
    {
        std::fill(out, out + frames * 2, int16_t{0});
        return 0;
    }

    while ((output_.size() - consumed_) / 2 < frames)
    {
        if (!Step())
        {
            std::fill(out, out + frames * 2, int16_t{0});
            return 0;
        }
    }

    std::copy(output_.begin() + static_cast<std::ptrdiff_t>(consumed_),
              output_.begin() + static_cast<std::ptrdiff_t>(consumed_ + frames * 2), out);
    consumed_ += frames * 2;

    // Drop consumed samples now and then to bound memory.
    if (consumed_ > (1u << 20))
    {
        output_.erase(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        dropped_ += consumed_;
        consumed_ = 0;
    }

    return frames;
}

long long Player::LengthMs() const
{
    const auto it = set_.tags.find("length");
    return it == set_.tags.end() ? -1 : ParseTime(it->second);
}

long long Player::FadeMs() const
{
    const auto it = set_.tags.find("fade");
    return it == set_.tags.end() ? -1 : ParseTime(it->second);
}

double Player::Volume() const
{
    const auto it = set_.tags.find("volume");
    if (it == set_.tags.end())
    {
        return 1.0;
    }

    // A value that isn't a finite number, such as nan, is ignored: it would turn every sample into nonsense.
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end != it->second.c_str() && std::isfinite(v) ? v : 1.0;
}

} // namespace csf
