// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/snd/engine.h"
#include "xsflib/rip/length.h"
#include "xsflib/sha1.h"

#include "csf/ntdlib_cafe.h"
#include "csf/sar.h"

namespace csf::rip
{
namespace
{

// What the length analysis (xsflib::rip::MeasureLength) reads of archive mode's model: AX's output and voices, the
// engine and the sequence player. The model also counts each track's passes. A looping sequence's fade then waits for
// the tracks that loop at other lengths than the main track (OthersLooped).
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

    // Effects ring on after the notes have ended. The analysis waits for a second without sound before a sequence with
    // effects ends.
    bool Ringing() const
    {
        return engine_.EffectsRinging();
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

    // Sets the hooks for the analysis's loops and for every track's passes. Only the passes that changed the sound
    // count.
    template <typename F>
    void OnLoop(F on_loop)
    {
        engine_.on_loop = std::move(on_loop);
        engine_.on_track_loop = [this](int track, uint32_t)
        {
            const auto* looping = player_.Track(track);
            if (looping && !looping->IsIdle())
            {
                ++passes_[static_cast<std::size_t>(track)];
            }
        };
    }

    // Whether every open track other than `track` has made kLengthLoops passes that changed the sound. A track whose
    // last pass changed nothing, or that waits for ever, doesn't hold the fade back.
    bool OthersLooped(int track) const
    {
        for (int i = 0; i < static_cast<int>(passes_.size()); ++i)
        {
            const auto* other = player_.Track(i);
            if (i != track && other && other->IsOpen() && !other->IsIdle() && !other->WaitsForEver(engine_) &&
                passes_[static_cast<std::size_t>(i)] < xsflib::rip::kLengthLoops)
            {
                return false;
            }
        }

        return true;
    }

private:
    const ax::System& system_;
    nwsnd::Engine& engine_;
    const nwsnd::SequenceSoundPlayer& player_;
    std::array<int, 16> passes_{}; // each track's passes that changed the sound
};

// Measures a sequence on the model. The default rules fit it: frames of 3 ms, and a frame's commands sound from the
// next frame on.
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, nwsnd::EngineOptions options,
                                      const std::vector<nwsnd::EffectSetup>& effects, uint32_t sound_index)
{
    options.effects = nwsnd::EffectsFor(effects, sound_index);
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

namespace
{

// The SHA-1 of the game's executable, or nothing if it can't be read.
std::optional<std::string> ExecutableSha1(const title::GameFiles& game)
{
    std::vector<uint8_t> executable;
    if (!game.read_file || !game.read_file("code/" + game.executable, executable))
    {
        return std::nullopt;
    }

    return xsflib::Sha1::Hex(xsflib::Sha1::Of(executable));
}

// The profile in `profiles` for the game's title and executable, or nullptr.
template <typename Profile>
const Profile* FindIn(const std::vector<Profile>& profiles, const title::GameFiles& game)
{
    const std::optional<std::string> sha1 = ExecutableSha1(game);
    for (const Profile& profile : profiles)
    {
        if (sha1 && game.title_id == profile.title_id && *sha1 == profile.executable_sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

} // namespace

const GameProfile* FindProfile(const title::GameFiles& game)
{
    return FindIn(KnownProfiles(), game);
}

const std::vector<EffectProfile>& KnownEffectProfiles()
{
    // The parameters' floats, as their bits.
    const auto f = [](float value)
    {
        return std::bit_cast<uint32_t>(value);
    };

    static const std::vector<EffectProfile> kProfiles = {
        // Animal Crossing: amiibo Festival (USA). bbq::settingAudio (0x02345D84) puts an FxDelay on the TV's aux A at
        // boot: a delay of 300 ms, a feedback of 0.4, an output gain of 1, a longest delay of 500 ms and the low-pass
        // filter open. The game's scenes set no other effect.
        {"Animal Crossing: amiibo Festival",
         0x00050000101C6400,
         "4686cf8a1253f8d58996cf665731729c8c83e368",
         {{nwsnd::kEffectDeviceTv,
           0,
           nwsnd::EffectType::kDelay,
           {f(300.0f), f(0.4f), f(1.0f), f(500.0f), f(1.0f)},
           {}}}},
        // Mario Party 10 (USA). At boot, mp::audio::setting (0x02E1F280) sets up the preset EFFECT_A_COMMON.
        // NdSysAudio::appendFxReverbStd (0x020EF394) then puts an FxReverbStd on the TV's aux A: a pre-delay of 10 ms,
        // a fused time of 2.2 s, a coloration of 0.6, a damping of 0.3, an output gain of 1, early mode 4, a longest
        // pre-delay of 50 ms, fused mode 4, an early gain of 0.3 and a fused gain of 0.55. The GamePad gets the same
        // reverb. CSF plays only the TV's output.
        {"Mario Party 10",
         0x0005000010162D00,
         "778ec07148c29eb0861208358f92ba68f917b95c",
         {{nwsnd::kEffectDeviceTv,
           0,
           nwsnd::EffectType::kReverbStd,
           {f(0.01f), f(2.2f), f(0.6f), f(0.3f), f(1.0f), 4, f(0.05f), 4, f(0.3f), f(0.55f)},
           {}}}},
    };
    return kProfiles;
}

const EffectProfile* FindEffectProfile(const title::GameFiles& game)
{
    return FindIn(KnownEffectProfiles(), game);
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
    std::vector<nwsnd::EffectSetup> effects;
};

LengthAnalyzer::LengthAnalyzer(fsar::SoundArchive archive, nwsnd::EngineOptions options,
                               std::vector<nwsnd::EffectSetup> effects)
    : impl_(new Impl{std::move(archive), std::move(options), std::move(effects)})
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    const auto measure = [this, sound_index]
    {
        return MeasureSequence(impl_->archive, impl_->options, impl_->effects, sound_index);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace csf::rip
