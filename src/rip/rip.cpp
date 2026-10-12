// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/sha1.h"

#include "ax/system.h"
#include "csf/sar.h"
#include "nwsnd/engine.h"

namespace csf::rip
{
namespace
{

// What the length analysis (xsflib::rip::MeasureLength) reads of archive mode's model: AX's output and voices, the
// engine and the sequence player.
class LengthModel
{
public:
    LengthModel(const ax::System& system, nwsnd::Engine& engine, const nwsnd::SequenceSoundPlayer& player)
        : system_(system), engine_(engine), player_(player)
    {
    }

    bool RunFrame()
    {
        engine_.RunFrame();

        return true;
    }

    bool Sounded() const
    {
        return xsflib::rip::Audible(system_.Output());
    }

    // The model has no effects. Nothing rings once the voices stop.
    bool Ringing() const
    {
        return false;
    }

    std::size_t VoiceCount() const
    {
        return system_.Voices().size();
    }

    xsflib::rip::VoicePlace Voice(std::size_t index) const
    {
        const ax::Pb& pb = system_.Voices()[index].renderer;
        return {pb.state == ax::VoiceState::kRun, pb.loop_flag != 0, pb.current_address};
    }

    bool Active() const
    {
        return player_.IsActive();
    }

    bool Settled() const
    {
        return player_.IsSettled(engine_);
    }

    bool Stuck() const
    {
        return player_.IsStuck(engine_);
    }

    bool Moving() const
    {
        return player_.IsMoving();
    }

    uint64_t ChangedAt() const
    {
        return player_.ChangedAt();
    }

    bool Busy() const
    {
        return engine_.IsBusy();
    }

    bool ChannelsChanging() const
    {
        return engine_.Channels().AnyChanging();
    }

    int ActiveChannels() const
    {
        return engine_.Channels().ActiveCount();
    }

    template <typename F>
    void OnLoop(F on_loop)
    {
        engine_.on_loop = std::move(on_loop);
    }

private:
    const ax::System& system_;
    nwsnd::Engine& engine_;
    const nwsnd::SequenceSoundPlayer& player_;
};

// Measures a sequence on the model. The default rules fit it: frames of 3 ms, and a frame's commands sound from the
// next frame on.
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, const nwsnd::EngineOptions& options,
                                      uint32_t sound_index)
{
    auto model = std::make_unique<nwsnd::ArchiveModel>(archive, options);
    const nwsnd::SequenceSoundPlayer* player = model->engine->StartSequence(sound_index);
    if (!player)
    {
        return std::nullopt;
    }

    LengthModel length_model(model->system, *model->engine, *player);
    return xsflib::rip::MeasureLength(length_model, xsflib::rip::LengthRules{});
}

} // namespace

const std::vector<GameProfile>& KnownProfiles()
{
    static const std::vector<GameProfile> kProfiles = {
        // Animal Crossing: amiibo Festival (USA). Its NdSysAudio gives nw::snd 78 voices.
        {"Animal Crossing: amiibo Festival", 0x00050000101C6400, "4686cf8a1253f8d58996cf665731729c8c83e368",
         &game::AmiiboFestival, 78},
    };
    return kProfiles;
}

const GameProfile* FindProfile(const title::GameFiles& game)
{
    std::vector<uint8_t> executable;
    if (!game.read_file || !game.read_file("code/" + game.executable, executable))
    {
        return nullptr;
    }

    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(executable));
    for (const GameProfile& profile : KnownProfiles())
    {
        if (game.title_id == profile.title_id && sha1 == profile.executable_sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

std::optional<std::string> ForeignArchive(std::string_view head)
{
    if (head.starts_with("CSAR"))
    {
        return "a 3DS sound archive (CSAR)";
    }
    if (head.starts_with("RSAR"))
    {
        return "a Wii sound archive (RSAR)";
    }
    if (head.starts_with("FSAR\xFF\xFE"))
    {
        return "a Switch sound archive (little-endian FSAR)";
    }

    return std::nullopt;
}

std::vector<uint8_t> BuildLibrary(const FileChunk& executable, const FileChunk& archive, const game::Profile& profile,
                                  const Tags& tags)
{
    ProcessDescriptor descriptor;
    descriptor.executable_path = executable.path;
    descriptor.archive_path = archive.path;
    descriptor.profile = game::ProfileWords(profile);

    ProgramBuilder program;
    program.AddFileRef(executable.path, executable.data);
    program.AddFileRef(archive.path, archive.data);

    return program.WritePsf(kPsfVersionCsf, descriptor.Serialize(), tags);
}

struct LengthAnalyzer::Impl
{
    fsar::SoundArchive archive;
    nwsnd::EngineOptions options;
};

LengthAnalyzer::LengthAnalyzer(fsar::SoundArchive archive, nwsnd::EngineOptions options)
    : impl_(new Impl{std::move(archive), options})
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    const auto measure = [this, sound_index]
    {
        return MeasureSequence(impl_->archive, impl_->options, sound_index);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace csf::rip
