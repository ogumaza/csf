// SPDX-License-Identifier: MIT

#include "rip/rip.h"

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

#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/cafe/snd/length_model.h"
#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/cafe/snd/sequence_starts.h"
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/binary.h"
#include "xsflib/psf/psf.h"
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

// Measures a sequence on the model, set up for `settings`, with `variables` and `track_volumes` set before its first
// tick. The default rules fit it: frames of 3 ms, and a frame's commands sound from the next frame on. The counted
// loops in `endless_loops` loop for ever. When the measure reaches the time limit inside counted loops, it runs again
// with each open track's outermost counted loop endless, as ntdlib's MeasureSequence does
// (ntdlib::nwsnd::OutermostCountedLoops).
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, const ArchiveDescriptor& settings,
                                      uint32_t sound_index, const xsflib::psf::Variables& variables,
                                      const ntdlib::rip::TrackVolumes& track_volumes,
                                      const ntdlib::nwsnd::CountedLoopSet& endless_loops = {})
{
    auto model = std::make_unique<nwsnd::ArchiveModel>(archive, EngineOptionsFor(settings, sound_index));
    model->engine->SetEndlessLoops(endless_loops);
    nwsnd::SequenceSoundPlayer* player = model->engine->StartSequence(sound_index);
    if (!player)
    {
        return std::nullopt;
    }

    SetVariables(*player, *model->engine, variables);
    player->SetTrackVolumes(track_volumes);

    ntdlib::nwsnd::LengthModel<ntdlib::cafe::snd::LengthConsole> length_model(*model, *player);
    const xsflib::rip::LengthRules rules;
    const std::optional<Timing> timing = xsflib::rip::MeasureLength(length_model, rules);
    if (timing && endless_loops.empty() && length_model.Frames() >= rules.max_frames)
    {
        if (const ntdlib::nwsnd::CountedLoopSet running = ntdlib::nwsnd::OutermostCountedLoops(*player);
            !running.empty())
        {
            return MeasureSequence(archive, settings, sound_index, variables, track_volumes, running);
        }
    }

    return timing;
}

// Animal Crossing: amiibo Festival (USA), the reference game. It has a driver profile and an effect profile.
constexpr GameRelease kAmiiboFestival = {"Animal Crossing: amiibo Festival", 0x00050000101C6400,
                                         "4686cf8a1253f8d58996cf665731729c8c83e368"};

// Mario Party 10 (USA). It has a driver profile and an effect profile. It rips in archive mode by default: 4,802 of its
// 6,008 sequences and its music's SQ_BGM_MENU send to the reverb on aux A, and game mode plays no effects.
constexpr GameRelease kMarioParty10 = {"Mario Party 10", 0x0005000010162D00,
                                       "778ec07148c29eb0861208358f92ba68f917b95c"};

// Wii Party U (USA, Rev 1). It has an effect profile.
constexpr GameRelease kWiiPartyU = {"Wii Party U", 0x0005000010137D00, "9d56d615edcafcd0e610a2426e0c66e79718cdcd"};

// Nintendo Land (USA). It has an effect profile.
constexpr GameRelease kNintendoLand = {"Nintendo Land", 0x0005000010102000, "bad9b52f7d4314a17144c353b0fa5ed84dbeffa8"};

// amiibo Festival's nw::snd ends each read of a file in a sound archive on a multiple of 32 bytes from the archive's
// start, or at the archive's end.
constexpr std::size_t kReadAlignment = 32;

// The copy of the sound archive `archive` that a game-mode library holds for the sequences at `sounds`. When the driver
// opens the archive, amiibo Festival's nw::snd reads the archive's first 256 bytes, its INFO block and its STRG block.
// To load a sequence, LoadData reads the sequence's file, the files of its banks and the files of the wave archives
// that the banks name. Each read starts at the place that the file table gives. The copy keeps all those bytes in their
// places, with zeros in place of the FILE block's other bytes (ntdlib::rip::KeepLoadedFiles). The game therefore reads
// the same bytes as from the whole archive. LoadedArchive returns the whole archive when the archive can't be read or
// one of `sounds` isn't a sequence.
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
        // Mario Party 10.
        {kMarioParty10, &game::MarioParty10, game::kMarioParty10Voices, true},
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

GameIdentity IdentityOf(const title::GameFiles& game)
{
    return {game.title_id, game.foreign_title_id, ExecutableSha1(game)};
}

const GameProfile* FindProfile(const GameIdentity& game)
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

namespace
{

const std::vector<EffectProfile>& KnownEffectProfiles()
{
    // The parameters' floats, as their bits.
    const auto f = [](float value)
    {
        return std::bit_cast<uint32_t>(value);
    };

    // Nintendo Land's flag of the threaded path, the first byte of its word.
    constexpr uint32_t kThreaded = 0x01000000;

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
        {kMarioParty10,
         {{nwsnd::kEffectDeviceTv,
           0,
           nwsnd::EffectType::kReverbStd,
           {f(0.01f), f(2.2f), f(0.6f), f(0.3f), f(1.0f), 4, f(0.05f), 4, f(0.3f), f(0.55f)},
           {}}}},
        // Wii Party U (USA, Rev 1). At boot, np::snd::setting (0x031BDF34) loads sound/common/NEXPY_PRESETS.bin and
        // sets up its preset SND_EFFECT_AUXA_00 for the TV. NdSysAudio::setFxPreset (0x0229463C) then puts the preset's
        // one effect on the TV's aux A through appendFxReverbHi (0x0229430C): an FxReverbHi with a pre-delay of 10 ms,
        // a fused time of 2 s, a coloration of 0.6, a damping of 0.4, a crosstalk of 0.1, an output gain of 1, early
        // mode 4, a longest pre-delay of 50 ms, fused mode 4, an early gain of 0.35 and a fused gain of 0.45. The
        // GamePad gets the same reverb from SND_EFFECT_AUXA_00_DRC. Nothing changes aux A later. The scenes' settings
        // (0x022960A8) and the minigames' modules set bus B only, and the vocal archives (0x022978D4) set bus C.
        {kWiiPartyU,
         {{nwsnd::kEffectDeviceTv,
           0,
           nwsnd::EffectType::kReverbHi,
           {f(0.01f), f(2.0f), f(0.6f), f(0.4f), f(0.1f), f(1.0f), 4, f(0.05f), 4, f(0.35f), f(0.45f)},
           {}}}},
        // Nintendo Land (USA). The game's code defines two effect classes, a reverb (constructor 0x02018784) and a
        // delay (0x020179C8). As each attraction's scene starts, its sound manager's set-up (slot +0x54 of its vtable)
        // calls 0x0201F918 twice. Each call makes a reverb and gives it the scene's parameters. The set-up appends one
        // reverb to the TV's aux A and the other to the GamePad's (0x0201FB30). Every scene gives an all-pass
        // coefficient of 0.6, a damping of 0.4 and a gain of 1, the 32 kHz tables, and the threaded path with a ring of
        // 8 frames and a start of 4. The plaza's set-up (0x028D4D2C) appends the delay and then a reverb to the TV's
        // aux A, at the 48 kHz rate. The plaza's set-up also puts level meters on both devices' aux A. The meters leave
        // the sound as it is. Each effect here lists the attraction's sequences that send to aux A, by their indices in
        // lunch.bfsar. Their labels start with the attraction's prefix. The addresses below are the set-up's first
        // call. Animal Crossing: Sweet Day (DBT, 0x028B8468), Captain Falcon's Twister Race (FZR, 0x028BC1CC) and
        // Yoshi's Fruit Cart (YSI, 0x028DBB5C) set reverbs too. None of their sequences sends to aux A. CSF plays only
        // the TV's output.
        {kNintendoLand,
         {
             // The plaza (TTL): SE_TTL_STATUE_Gwr_Seaweed. A delay of 0.04 ms at 48 kHz, one sample, without feedback,
             // at a level of 0.5 and a damping of 0.3. Then a reverb without the pre-delay and the early reflection
             // (set 0, a level of 0), a decay of 3 s and a level of 1.
             {nwsnd::kEffectDeviceTv,
              0,
              nwsnd::EffectType::kNintendoLandDelay,
              {f(0.04f), f(0.0f), f(0.5f), f(0.3f), 1, kThreaded, 8, 4},
              {816}},
             {nwsnd::kEffectDeviceTv,
              0,
              nwsnd::EffectType::kNintendoLandReverb,
              {f(0.0f), f(3.0f), f(0.6f), f(0.4f), f(1.0f), 0, 0, f(0.0f), f(1.0f), 1, kThreaded, 8, 4},
              {816}},
             // Octopus Dance (GWR, 0x028BD6D8): a pre-delay of 20 ms, a decay of 1 s, early reflection 1, set 3, an
             // early level of 0.1 and a level of 0.9. SE_GWR_CORECT_L, SE_GWR_CORECT, SE_GWR_OCT_L_HIT, and the TV's
             // and GamePad's SUBMARINE_HIT, BOAT_HIT, SEAWEED_HIT and HEART_HIT.
             {nwsnd::kEffectDeviceTv,
              0,
              nwsnd::EffectType::kNintendoLandReverb,
              {f(0.02f), f(1.0f), f(0.6f), f(0.4f), f(1.0f), 1, 3, f(0.1f), f(0.9f), 0, kThreaded, 8, 4},
              {1159, 1160, 1200, 1201, 1202, 1203, 1204, 1205, 1206, 1207, 1208}},
             // Luigi's Ghost Mansion (LIG, 0x028C0E50): as Octopus Dance, with early reflection 3.
             // SE_LIG_S_TUTORIAL_START, SE_LIG_S_COUNT_DOWN and SE_LIG_S_TIMEUP.
             {nwsnd::kEffectDeviceTv,
              0,
              nwsnd::EffectType::kNintendoLandReverb,
              {f(0.02f), f(1.0f), f(0.6f), f(0.4f), f(1.0f), 3, 3, f(0.1f), f(0.9f), 0, kThreaded, 8, 4},
              {1216, 1217, 1218}},
             // Metroid Blast (MTR, 0x028C496C): as Octopus Dance, with early reflection 5 and set 4. The five
             // SE_MTR_COUNTDOWN sounds, SE_MTR_S_REMAIN_30, SE_MTR_PLAYER_SNIPE_ALERT and SE_MTR_SAMS_SEARCH.
             {nwsnd::kEffectDeviceTv,
              0,
              nwsnd::EffectType::kNintendoLandReverb,
              {f(0.02f), f(1.0f), f(0.6f), f(0.4f), f(1.0f), 5, 4, f(0.1f), f(0.9f), 0, kThreaded, 8, 4},
              {1311, 1312, 1313, 1314, 1315, 1320, 1348, 1378}},
             // Takamaru's Ninja Castle (NZM, 0x028CA158): as Luigi's Ghost Mansion. SE_NZM_S_GAME_START,
             // SE_NZM_NEBUTA_MOUTH_DAMAGE and SE_NZM_NEBUTA_DAMAGE_LAST.
             {nwsnd::kEffectDeviceTv,
              0,
              nwsnd::EffectType::kNintendoLandReverb,
              {f(0.02f), f(1.0f), f(0.6f), f(0.4f), f(1.0f), 3, 3, f(0.1f), f(0.9f), 0, kThreaded, 8, 4},
              {1594, 1648, 1650}},
             // Pikmin Adventure (PKM, 0x028CDAF8): as Octopus Dance, with early reflection 3 and set 5. Twelve sounds,
             // from SE_PKM_PLAYER_FALL_GATE to SE_PKM_TONOSAMA_BREAK_EYE.
             {nwsnd::kEffectDeviceTv,
              0,
              nwsnd::EffectType::kNintendoLandReverb,
              {f(0.02f), f(1.0f), f(0.6f), f(0.4f), f(1.0f), 3, 5, f(0.1f), f(0.9f), 0, kThreaded, 8, 4},
              {1767, 1802, 1828, 1864, 1885, 1886, 1914, 1967, 1968, 1999, 2000, 2018}},
             // Battle Quest (ZLD, 0x028DDD70): as Luigi's Ghost Mansion. SE_ZLD_MAIN_TITLE and SE_ZLD_GATE_OPEN.
             {nwsnd::kEffectDeviceTv,
              0,
              nwsnd::EffectType::kNintendoLandReverb,
              {f(0.02f), f(1.0f), f(0.6f), f(0.4f), f(1.0f), 3, 3, f(0.1f), f(0.9f), 0, kThreaded, 8, 4},
              {2078, 2155}},
         }},
    };
    return kProfiles;
}

} // namespace

const EffectProfile* FindEffectProfile(const GameIdentity& game)
{
    return FindKnownGame(KnownEffectProfiles(), game);
}

namespace
{

// Wii Party U's sound archive, and its theme of the score game. Variable 0 holds the level of the player's score. At
// -1, as nw::snd starts it, the theme plays its first layers. A rip holds that start as a variant too, before the three
// levels (docs/csf.md, "Archive mode").
constexpr std::string_view kWiiPartyUArchive = "528580bb03f3fbdea2815d7fa7b355233d90d28d";
constexpr std::string_view kScoreTheme = "SEQ_BGM_NPG006_THEMA";

// `values` by their numbers in the csf_var tag. A track's variable i is TrackVariableKey(track, 32 + i).
xsflib::psf::Variables VariablesOf(const ntdlib::cafe::snd::SequenceVariables& values)
{
    using ntdlib::cafe::snd::SequenceVariable;
    xsflib::psf::Variables variables;
    for (const SequenceVariable& value : values)
    {
        if (value.index == SequenceVariable::kNone)
        {
            continue;
        }

        variables[value.track == SequenceVariable::kNoTrack
                      ? value.index
                      : TrackVariableKey(value.track, kMaxVariables + value.index)] = value.value;
    }

    return variables;
}

} // namespace

std::vector<xsflib::psf::Variables> VariantsOf(std::string_view archive_sha1, std::string_view label)
{
    std::vector<xsflib::psf::Variables> variants;
    const xsflib::psf::Variables start = VariablesOf(ntdlib::cafe::snd::StartOfSequence(archive_sha1, label).variables);
    if (!start.empty())
    {
        variants.push_back(start);
    }
    else if (archive_sha1 == kWiiPartyUArchive && label == kScoreTheme)
    {
        variants.push_back({{0, -1}});
    }

    for (const ntdlib::cafe::snd::SequenceArrangement& state :
         ntdlib::cafe::snd::SequenceArrangements(archive_sha1, label))
    {
        if (xsflib::psf::Variables later = VariablesOf(state.variables); !later.empty())
        {
            variants.push_back(std::move(later));
        }
    }

    return variants;
}

std::optional<std::string> ForeignArchive(std::string_view head)
{
    switch (ntdlib::sar::SoundArchiveKindOf(head))
    {
    case ntdlib::sar::SoundArchiveKind::kCsar:
        return "a 3DS sound archive (CSAR)";
    case ntdlib::sar::SoundArchiveKind::kRsar:
        return "a Wii sound archive (RSAR)";
    case ntdlib::sar::SoundArchiveKind::kFsarLittle:
        return "a Switch sound archive (little-endian FSAR)";
    default:
        return std::nullopt;
    }
}

std::vector<uint8_t> BuildLibrary(const FileChunk& executable, const FileChunk& archive,
                                  std::span<const uint32_t> sounds, const game::NwSndProfile& profile, const Tags& tags)
{
    // The profile holds the variables' writers, which a version 1 descriptor's profile doesn't.
    ProcessDescriptor descriptor;
    descriptor.version = ProcessDescriptor::kVersion2;
    descriptor.driver = static_cast<uint32_t>(GameDriverKind::kNwSnd);
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
    : impl_(std::make_unique<Impl>(Impl{std::move(archive), std::move(settings)}))
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index, const xsflib::psf::Variables& variables,
                                              const ntdlib::rip::TrackVolumes& track_volumes) const
{
    const auto measure = [this, sound_index, &variables, &track_volumes]
    {
        return MeasureSequence(impl_->archive, impl_->settings, sound_index, variables, track_volumes);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace csf::rip
