// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/rip/archive.h"
#include "xsflib/binary.h"
#include "xsflib/rip/length.h"
#include "xsflib/sha1.h"

#include "csf/format.h"
#include "csf/model.h"
#include "csf/ntdlib_cafe.h"
#include "csf/sar.h"
#include "game/game_driver.h"
#include "game/nwsnd_driver.h"
#include "title/title.h"

namespace csf::rip
{
namespace
{

using ntdlib::nwsnd::kTrackCount;

// What the length analysis (xsflib::rip::MeasureLength) reads of archive mode's model: AX's output and voices, the
// engine and the sequence player. The model also counts each track's passes. A looping sequence's fade then waits for
// the tracks that loop at other lengths than the main track (OthersLooped). A track that another track opens again
// makes a pass too. In some sequences, the main track opens the other tracks again in each of its loops. Those tracks
// never loop.
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
    // count. A track that another track opens again starts its part again. That start counts as a pass that changed
    // the sound. The first open isn't a pass. On the Wii U, an open clears the track's wait. The pass therefore starts
    // at the open.
    template <typename F>
    void OnLoop(F on_loop)
    {
        const auto count_pass = [this](int track, uint32_t)
        {
            const auto* looping = player_.Track(track);
            if (looping && !looping->IsIdle())
            {
                ++passes_[static_cast<std::size_t>(track)];
            }
        };

        const auto count_open = [this](int, int opened, uint32_t)
        {
            const auto index = static_cast<std::size_t>(opened);
            if (opened_[index])
            {
                ++passes_[index];
            }

            opened_[index] = true;
        };

        engine_.SetLoopHook(std::move(on_loop));
        engine_.SetTrackLoopHook(count_pass);
        engine_.SetOpenTrackHook(count_open);
    }

    // Whether every open track other than `track` has made kLengthLoops passes that changed the sound. A track whose
    // last pass changed nothing, or that waits for ever, doesn't hold the fade back.
    bool OthersLooped(int track) const
    {
        for (int i = 0; i < kTrackCount; ++i)
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
    std::array<int, kTrackCount> passes_{};  // each track's passes that changed the sound
    std::array<bool, kTrackCount> opened_{}; // each track that another track has opened
};

// Measures a sequence on the model, set up for `settings`. The default rules fit it: frames of 3 ms, and a frame's
// commands sound from the next frame on.
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, const ArchiveDescriptor& settings,
                                      uint32_t sound_index)
{
    auto model = std::make_unique<nwsnd::ArchiveModel>(archive, EngineOptionsFor(settings, sound_index));
    const nwsnd::SequenceSoundPlayer* player = model->engine->StartSequence(sound_index);
    if (!player)
    {
        return std::nullopt;
    }

    LengthModel length_model(model->system, *model->engine, *player);
    return xsflib::rip::MeasureLength(length_model, xsflib::rip::LengthRules{});
}

// Animal Crossing: amiibo Festival (USA), the reference game. It has a driver profile and an effect profile.
constexpr GameRelease kAmiiboFestival = {"Animal Crossing: amiibo Festival", 0x00050000101C6400,
                                         "4686cf8a1253f8d58996cf665731729c8c83e368"};

// amiibo Festival's nw::snd ends each read of a file in a sound archive on a multiple of 32 bytes from the archive's
// start, or at the archive's end.
constexpr std::size_t kReadAlignment = 32;

// The copy of the sound archive `archive` that a game-mode library holds for the sequences at `sounds`. When the
// driver opens the archive, amiibo Festival's nw::snd reads the archive's first 256 bytes, its INFO block and its STRG
// block. To load a sequence, LoadData reads the sequence's file, the files of its banks and the files of the wave
// archives that the banks name. Each read starts at the place that the file table gives. The copy keeps all those bytes
// in their places, with zeros in place of the FILE block's other bytes (ntdlib::rip::KeepLoadedFiles). The game
// therefore reads the same bytes as from the whole archive. LoadedArchive returns the whole archive when the archive
// can't be read or one of `sounds` isn't a sequence.
std::vector<uint8_t> LoadedArchive(const std::vector<uint8_t>& archive, std::span<const uint32_t> sounds)
{
    try
    {
        const fsar::SoundArchive read = fsar::SoundArchive::Load(archive, kSarOptions);
        if (std::optional<std::vector<uint8_t>> copy = ntdlib::rip::KeepLoadedFiles(read, sounds, kReadAlignment))
        {
            return std::move(*copy);
        }
    }
    catch (const xsflib::FormatError&)
    {
        // A damaged archive goes into the library whole.
    }

    return archive;
}

} // namespace

const std::vector<GameProfile>& KnownProfiles()
{
    static const std::vector<GameProfile> kProfiles = {
        // Animal Crossing: amiibo Festival.
        {kAmiiboFestival, &game::AmiiboFestival, game::kAmiiboFestivalVoices},
    };
    return kProfiles;
}

std::optional<std::string> ExecutableSha1(const title::GameFiles& game)
{
    std::vector<uint8_t> executable;
    if (!game.read_file || !game.read_file(game.ExecutablePath(), executable))
    {
        return std::nullopt;
    }

    return xsflib::Sha1::Hex(xsflib::Sha1::Of(executable));
}

const GameProfile* FindProfile(const title::GameFiles& game)
{
    return FindKnownGame(KnownProfiles(), game);
}

ArchiveDescriptor GameSettings(const title::GameFiles& game, const GameProfile* profile)
{
    ArchiveDescriptor settings;
    settings.sdk_version = game.sdk_version;
    if (profile)
    {
        settings.voice_count = profile->voice_count;
        settings.ax_settings = profile->profile().ax_settings;
    }

    return settings;
}

const std::vector<EffectProfile>& KnownEffectProfiles()
{
    // The parameters' floats, as their bits.
    const auto f = [](float value)
    {
        return std::bit_cast<uint32_t>(value);
    };

    static const std::vector<EffectProfile> kProfiles = {
        // Animal Crossing: amiibo Festival. bbq::settingAudio (0x02345D84) puts an FxDelay on the TV's aux A at boot: a
        // delay of 300 ms, a feedback of 0.4, an output gain of 1, a longest delay of 500 ms and the low-pass filter
        // open. The game's scenes set no other effect.
        {kAmiiboFestival,
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
        {{"Mario Party 10", 0x0005000010162D00, "778ec07148c29eb0861208358f92ba68f917b95c"},
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
    return FindKnownGame(KnownEffectProfiles(), game);
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

std::vector<uint8_t> BuildLibrary(const FileChunk& executable, const FileChunk& archive,
                                  std::span<const uint32_t> sounds, const game::NwSndProfile& profile, const Tags& tags)
{
    ProcessDescriptor descriptor;
    descriptor.executable_path = executable.path;
    descriptor.archive_path = archive.path;
    descriptor.profile = game::ProfileWords(profile);

    // The archive holds only the files that the sounds play.
    const std::vector<uint8_t> loaded = LoadedArchive(archive.data, sounds);
    ProgramBuilder program;
    program.AddFileRef(executable.path, executable.data);
    program.AddFileRef(archive.path, loaded);

    return program.WritePsf(kPsfVersionCsf, descriptor.Serialize(), tags);
}

struct LengthAnalyzer::Impl
{
    fsar::SoundArchive archive;
    ArchiveDescriptor settings;
};

LengthAnalyzer::LengthAnalyzer(fsar::SoundArchive archive, ArchiveDescriptor settings)
    : impl_(new Impl{std::move(archive), std::move(settings)})
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    const auto measure = [this, sound_index]
    {
        return MeasureSequence(impl_->archive, impl_->settings, sound_index);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace csf::rip
