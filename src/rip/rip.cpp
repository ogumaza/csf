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
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/rip/archive.h"
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
        ++frames_;

        return true;
    }

    // The frames that the analysis has run.
    uint64_t Frames() const
    {
        return frames_;
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
    uint64_t frames_ = 0;
    std::array<int, kTrackCount> passes_{};  // each track's passes that changed the sound
    std::array<bool, kTrackCount> opened_{}; // each track that another track has opened
};

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
    SetTrackVolumes(*player, track_volumes);

    LengthModel length_model(model->system, *model->engine, *player);
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
        {{"Wii Party U", 0x0005000010137D00, "9d56d615edcafcd0e610a2426e0c66e79718cdcd"},
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
        {{"Nintendo Land", 0x0005000010102000, "bad9b52f7d4314a17144c353b0fa5ed84dbeffa8"},
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
             // Pikmin Adventure (PKM, 0x028CDAF8): as Octopus Dance, with early reflection 3 and set 5. Twelve
             // sounds, from SE_PKM_PLAYER_FALL_GATE to SE_PKM_TONOSAMA_BREAK_EYE.
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

const EffectProfile* FindEffectProfile(const title::GameFiles& game)
{
    return FindKnownGame(KnownEffectProfiles(), game);
}

const std::vector<VariableProfile>& KnownVariableProfiles()
{
    // Each sequence's variants. Their values come from the sequence's comparisons: each constant, the numbers on either
    // side of it, 0 and 1. A sequence that computes from its value got a range of values. Values that play the same
    // samples share a variant, which takes the fewest variables, then the lowest values. Of a range, the table keeps
    // the least value that sounds, the value at which the sequence stops following the value, and one or two between.
    // Each game's comment cites the code that sets the values, where a reading of the game's code found it (R-639).
    // Wii Party U's footsteps: the scene's footstep in variable 11, from 0 at the start to each of the 12 surfaces.
    static const std::vector<xsflib::psf::Variables> kWiiPartyUFootsteps = {
        {{11, 0}}, {{11, 1}}, {{11, 2}}, {{11, 3}},  {{11, 4}},  {{11, 5}},  {{11, 6}},
        {{11, 7}}, {{11, 8}}, {{11, 9}}, {{11, 10}}, {{11, 11}}, {{11, 12}},
    };
    // Wii Party U's sounds that map variable 15 or 10 from 0 to 127 onto their volume, pitch bend or tempo: the least
    // value that sounds, the middle and the top.
    static const std::vector<xsflib::psf::Variables> kWiiPartyU15From0 = {{{15, 0}}, {{15, 64}}, {{15, 127}}};
    static const std::vector<xsflib::psf::Variables> kWiiPartyU15From1 = {{{15, 1}}, {{15, 64}}, {{15, 127}}};
    static const std::vector<xsflib::psf::Variables> kWiiPartyU15From2 = {{{15, 2}}, {{15, 64}}, {{15, 127}}};
    static const std::vector<xsflib::psf::Variables> kWiiPartyU10From0 = {{{10, 0}}, {{10, 64}}, {{10, 127}}};
    static const std::vector<xsflib::psf::Variables> kWiiPartyU10From1 = {{{10, 1}}, {{10, 64}}, {{10, 127}}};
    static const std::vector<VariableProfile> kProfiles = {
        // Animal Crossing: amiibo Festival. resetScene sets every global to -1 (0x020F1574). A scene's entry in
        // settingpreset.bin then sets some, through the game's only call of writeGlobalVariable (0x0210D320), and
        // adds the tracks' variables (0x0210D37C). The scenes escape and card_hab set global 2 (18) to 0. Four scenes
        // give SQ_SE_ACT_HUMMING's track 0 its variable 15 (47) at 1. UIRoulette::playSE writes 15 = 0 after the
        // spin starts (0x02419044), and stop writes 15 = 1 (0x02419A84). audioSetCharacter writes 11 from a table of
        // 0 and 1 (0x023468D4). Villager 0x3E8 gets 1 (0x02346944). No code sets globals 3 (19) or 8 (24): only
        // sequences do. The code doesn't name SQ_SE_SYS_RLT_ROT, SQ_SE_ESCAPE_RLT_MAN_RLT or SQ_SE_INSECT_HORNET.
        {kAmiiboFestival,
         {
             {"SQ_SE_SYS_RLT_ROT", {{{15, 0}}}},
             {"SQ_SE_BOARD_RLT_MAN_RLT", {{{15, 0}}}},
             {"SQ_SE_BOARD_RLT_AUT_RLT", {{{15, 0}}}},
             {"SQ_SE_ESCAPE_RLT_MAN_RLT", {{{15, 0}}}},
             {"SQ_SE_ESCAPE_ENV_SHAPER", {{{18, 0}}}},
             {"SQ_SE_HAB_ENV", {{{18, 0}}}},
             {"SQ_SE_INSECT_HORNET", {{{15, 0}}}},
             {"SQ_SE_PLY_FSTEP_BELL", {{{11, 1}}}},
             {"SQ_SE_PLY_FSTEP_BELL_SUB", {{{11, 1}}}},
             {"SQ_SE_BOARD_LND_ONE_H_01_4", {{{19, 3}}}},
             {"SQ_SE_BOARD_LND_ONE_H_01_5", {{{19, 3}}}},
             {"SQ_SE_BOARD_LND_ONE_M_01_4", {{{19, 3}}}},
             {"SQ_SE_BOARD_LND_ONE_M_01_5", {{{19, 3}}}},
             {"SQ_SE_BOARD_LND_ONE_R_01_4", {{{19, 3}}}},
             {"SQ_SE_BOARD_LND_ONE_R_01_5", {{{19, 3}}}},
             {"SQ_SE_ESCAPE_MFA_FLY_WING_LP", {{{24, 1}}, {{24, 2}}}},
             // Track 0's variable 47.
             {"SQ_SE_ACT_HUMMING", {{{TrackVariableKey(0, 47), 1}}}},
         }},
        // Mario Party 10 (USA). NdSoundHandle::setFootWear, setWeight, setMaterial and setPiece (0x020ECBD8 to
        // 0x020ECCA4) write the sound's variables 8 to 11. resetScene sets every global to -1 (0x020E4A0C). Scenes
        // BD001 to BD005 of settingpreset.bin set global 13 (29) to 0, and scene MG0B05 sets global 8 (24) to 0,
        // through the reader at 0x020FEFC4. The sound's variables 0, 8 and 15 come from the boards' and minigames'
        // modules. tenst.rpx exports mp::CharaSound::setVariable (0x02E3698C) and calls it nowhere. mg0307.rpl writes
        // SQ_SE_MG0307_TRP's variable 15 after the start: 0 on one path (0x0200ED2C) and 1 on another (0x0200EE18).
        // mg0206.rpl sets the player's variable 15 to 0, 1 or 2 (0x0200A7E4 to 0x0200BD8C), and mp::CharaSound::setup
        // writes it to each sound that the character plays (0x02E36590). The game computes the other values from its
        // state, in mg0428.rpl, mg0b05.rpl, mg0k04.rpl, mg0k05.rpl, mg0k06.rpl, mg0306.rpl, sbd001.rpl and bd001.rpl.
        // No module names SQ_SE_PLY_GEN_DMG_SEL_DMG_FIR or MG0410's steps.
        {{"Mario Party 10", 0x0005000010162D00, "778ec07148c29eb0861208358f92ba68f917b95c"},
         {
             {"SQ_SE_PLY_GEN_DMG_SEL_DMG_FIR", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_BD000_KOOPA_DICE_BOUND", {{{8, 0}}}},
             {"SQ_SE_BD000_KOOPA_DICE_DRC_BOUND_DRC", {{{8, 0}}}},
             {"SQ_SE_BD005_KOOPA_DICE_DRC_BOUND_MTF", {{{8, 0}}}},
             {"SQ_SE_BD005_KOOPA_DICE_DRC_BOUND_MTF_DRC", {{{8, 0}}}},
             {"SQ_SE_BD001_VCL_LOP", {{{29, 0}}}},
             {"SQ_SE_BD001_VCL_LOP_WOOD", {{{29, 0}}}},
             {"SQ_SE_BD002_VCL_LOP", {{{29, 0}}}},
             {"SQ_SE_BD003_VCL_LOP", {{{29, 0}}}},
             {"SQ_SE_BD004_VCL_LOP", {{{29, 0}}}},
             {"SQ_SE_BD004_VCL_LOP_JET", {{{29, 0}}}},
             {"SQ_SE_BD005_VCL_LOP", {{{29, 0}}}},
             {"SQ_SE_SBD001_COIN_GET_APPEND", {{{0, 5}}}},
             {"SQ_SE_SBD001_COIN_GET_APPEND_DRC", {{{0, 5}}}},
             {"SQ_SE_PLY_AMI_BOUND_HAVOK", {{{15, 21}}}},
             {"SQ_SE_MG0206_JUMP", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SQ_SE_MG0206_ONP_LND", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0306_KNK_HIT_A_RM", {{{22, 0}}, {{22, 1}}}},
             {"SQ_SE_MG0307_TRP", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_PLY_FSTEP_RUN_MG0410", {{{8, 1}}, {{8, 2}}}},
             {"SQ_SE_PLY_FSTEP_LAND_MG0410", {{{8, 1}}, {{8, 2}}}},
             {"SQ_SE_MG0428_BLK_TUR_1P", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SQ_SE_MG0428_BLK_TUR_2P", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SQ_SE_MG0428_BLK_TUR_3P", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SQ_SE_MG0428_BLK_TUR_4P", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SQ_SE_MG0B05_TANK_S_IDLING_1P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_IDLING_2P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_IDLING_3P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_IDLING_4P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_AXEL_1P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_AXEL_2P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_AXEL_3P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_AXEL_4P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_CATERPILLAR_1P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_CATERPILLAR_2P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_CATERPILLAR_3P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_CATERPILLAR_4P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_GEAR_1P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_GEAR_2P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_GEAR_3P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_S_GEAR_4P", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_L_AXEL", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_L_CATERPILLAR", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_MG0B05_TANK_L_AXEL_OFF", {{{15, 0}}, {{15, 1}}}},
             {"SQ_SE_NPC000_TANK_READY00_2", {{{24, 0}}}},
             {"SQ_SE_MG0K04_KOP_TCH_DRC", {{{24, 0}}, {{24, 1}}, {{25, 1}}, {{24, 0}, {25, 0}}, {{24, 1}, {25, 1}}}},
             {"SQ_SE_MG0K05_DIC_LND", {{{15, 0}}}},
             {"SQ_SE_MG0K06_BAL_ROL_DRC", {{{22, 0}}, {{22, 1}}, {{15, 0}, {22, 0}}, {{15, 1}, {22, 1}}}},
         }},
        // Nintendo Land (USA). Four wrappers start the game's sounds (0x02024F0C, 0x02024FD8, 0x02025058 and
        // 0x02025124). After the start, each writes the sound's variable 15 from a halfword of the object that plays
        // it, unless the halfword holds 0xFFFF. The object's constructor writes 0xFFFF (0x02024DB4). The code doesn't
        // name SE_PKM_PLAYER_FOOTSTEP, and nobody has traced the value that its object holds. Three sounds that play at
        // -1 take the game's values here (R-639). The first variant of each holds the value at the start.
        // SE_LIG_OBAKE_LIFE_DOWN rises from the key that variable 4 gives to the top of its range. The game writes 4
        // from a halfword of the object (0x028C1BE4). The constructor (0x028C1470) and a reset (0x028C1C5C) set the
        // halfword to 0. After each start, the game reads variable 5 into it (0x028C1BFC). The sound thread hasn't set
        // the new sound up by then: BasicSound's flag at +0xB9 stays 0 until the player's set-up command (0x02ACF5A4)
        // has run. ReadVariable (0x02AD6EF8) therefore gives -1. The first play after a reset starts at 0, and each
        // later play at -1. The train's two sounds take variable 0 as a level from -10 to -6. At -1 they sound as at
        // -6. The game gives trunc(5 * y - 10) (0x028D984C to 0x028D98BC). y is a step curve of the train's speed,
        // capped at 1 (0x028D9118 to 0x028D915C): 0, 0.05, 0.35 and 0.6 from 0, 0.2, 0.6 and 0.85 (0x101D9D88, read by
        // 0x029434BC). Each of the train's states sets the speed to 0.5 (0x02741BE4, 0x027422F0, 0x02742AF0 and
        // 0x02742DA0), and the speed then moves toward 0.5, 1.4 or 1.5 (0x02742730). The game therefore gives -9 at the
        // start, then -8 and -7, and never -10 or -6.
        {{"Nintendo Land", 0x0005000010102000, "bad9b52f7d4314a17144c353b0fa5ed84dbeffa8"},
         {
             {"SE_PKM_PLAYER_FOOTSTEP", {{{15, -2}}, {{15, 0}}}},
             {"SE_LIG_OBAKE_LIFE_DOWN", {{{4, 0}}, {{4, -1}}}},
             {"SE_TTL_TRAIN_MOVE", {{{0, -9}}, {{0, -8}}, {{0, -7}}}},
             {"SE_TTL_TRAIN_MOVE_STEAM", {{{0, -9}}, {{0, -8}}, {{0, -7}}}},
         }},
        // Super Mario 3D World (USA). SeEvRunnerSqueak transposes its note by variable 0. Its instrument plays keys up
        // to 32, so it sounds from -32 down. The lantern gives variable 0 as arg / [0x104E4350] * 120 (0x021E8CFC to
        // 0x021E8D94). The code doesn't name the other effects. The game's SoundData/SeDataBase.szs plays the spin
        // attacks (actors Rosetta and GhostPlayer), Captain Toad's three effects (KinopioBrigade) and the squeak
        // (Runner) from actions or from code, and gives none of them a variable (LocalVarNo). The sound layer writes a
        // sound's variables only from its list of 12 parameter commands (0x024D4980): command 4 writes one of the
        // sound's (0x024D4AF0 to 0x024D4B18), and command 5 a global one (0x024D4B2C to 0x024D4B50), each from two
        // floats. The code that fills the lists hasn't been traced.
        {{"Super Mario 3D World", 0x0005000010145C00, "a40fe64f9d11f1c4e2c7ed490193a139f4aa9edb"},
         {
             {"SeOjLanternSwing", {{{0, 41}}}},
             {"SePvSpinAttack", {{{0, 4}}, {{2, -2}}}},
             {"SePvSpinAttackAir", {{{0, 4}}}},
             {"SePvKinotanJumpFail", {{{0, 5}}}},
             {"SePvKinotanAppearStage", {{{0, 5}}}},
             {"SePvKinotanAppearStageHaunt", {{{0, 5}}}},
             {"SeGvSpinAttack", {{{0, 4}}, {{2, -2}}}},
             {"SeGvSpinAttackAir", {{{0, 4}}}},
             {"SeEvRunnerSqueak", {{{0, -64}}, {{0, -48}}, {{0, -32}}}},
         }},
        // New Super Mario Bros. U (USA). The lifts, the water wheel and the map's scroll take a speed in variable 0.
        // Each sounds above a least speed, and follows the speed up to a limit. Each frame, the line lift gives its
        // speed * 100 (0x02799CD0 to 0x02799D34) to a held sound (0x02972CD8), the rising lift too (0x02793D4C), and
        // the water wheel its step of rotation (0x027B7D10). The map gives its touch scroll * 100 (0x021B57C8). The
        // speeds come from each level's data. The executable holds neither SE_OBJ_FREE_FALL's nor
        // SE_OBJ_RC_FENCE_ROLL's label, and loads neither archive index (0x5BD and 0x5C1) as an immediate.
        {{"New Super Mario Bros. U", 0x0005000010101D00, "7695146cfe9b87e47b0d650071621ee9a8aa5e94"},
         {
             {"SE_OBJ_RC_LIFTLINE_MOVE", {{{0, 20}}, {{0, 100}}, {{0, 200}}}},
             {"SE_OBJ_RC_UPLIFT_MOVE", {{{0, 20}}, {{0, 100}}, {{0, 200}}}},
             {"SE_OBJ_SUISHA_ROLL", {{{0, 5}}, {{0, 50}}, {{0, 300}}}},
             {"SE_OBJ_FREE_FALL", {{{0, 1}}}},
             {"SE_OBJ_RC_FENCE_ROLL", {{{0, 1}}, {{0, 97}}, {{0, 193}}, {{0, 289}}}},
             {"SE_SYS_MAP_TOUCH_SCROLL_LV", {{{0, 200}}, {{0, 1000}}, {{0, 4800}}, {{0, 10000}}}},
         }},
        // Wii Party U (USA, Rev 1). The animations play the Miis' sounds through np::CharaSound::playSE (0x031AEC64,
        // called from 0x031E380C and 0x031F68C4). Each start writes 16 halfwords of the CharaSound to the sound's
        // variables 0 to 15, all but 11 (0x031AE85C). The constructor sets them to 0 (0x031AEAFC). Only three minigames
        // change one, variable 15: nmg411.rpl to 1 in the opening and to 0 at the touchdown (0x02008F84 and
        // 0x02009B3C), nmg418.rpl to 0 or 1 (0x0200FAC4 and 0x0200FAE0), and nmg426.rpl to 0, 1, 2 or a halfword
        // (0x0200D504 to 0x0200D5C8). Variable 11 is the footstep. CharaSound writes the one that it holds, or else the
        // scene's (0x02293138). NdSysAudio::play writes the scene's to the four sounds that addFootStepSound lists
        // (0x022903D0, with the list from 0x031BDFDC). The scene's footstep starts at 0 (0x0228E59C). Each scene's
        // entry in SETTING_PACK.bin can set it by name (0x02296AD8, with the names at 0x1065A29C): 1 earth, 2 sand, 3
        // stone, 4 iron, 5 wood, 6 lawn, 7 grass, 8 water, 9 ice, 10 carpet, 11 barefoot and 12 barefoot on wood.
        // npg009.rpl gives the ball's hits a speed from 1 to 127 in variable 15 (0x02000588 and 0x02000310). npg001.rpl
        // writes the trace's variable 15 as the finger moves (0x0204369C). Each frame, npg001.rpl gives the balloon's
        // SEQ_SE_TVP_SDICE_03_AIR_B a value from 0 to 127 in variable 10 (0x02009A04 to 0x02009A40, through
        // 0x02009674). The sequence bends its pitch by the value. npg006.rpl raises SEQ_BGM_NPG006_THEMA's variable 0
        // from -1 to 1, 2 and 3 as the player's score passes each of three thresholds (0x02002F58, through 0x02000960).
        // The theme fades in its third track at 1 and its fourth at 2, and plays its ending at 3. In the archive,
        // SEQ_SE_NMG431_TARU_HIT_TARU draws a transpose from 0 to 2 that differs from the last one and keeps it in
        // global variable 28. SEQ_SE_NMG431_TARU_HIT_TARU_RM plays at that transpose and sets no variable. The hits on
        // wood do the same with globals 25 and 27. A rip plays the copies at the lowest transpose. 64 sequences check
        // that the variable lies in a range, 0 to 127 for all but one, and map it onto their volume, pitch bend or
        // tempo. Outside the range, as at -1, each plays its defaults. No value in the range plays those. A rip plays
        // them at the least value that sounds, the middle and the top. The game's code for those values hasn't been
        // read, apart from the two balls of npg001.rpl's dice (0 to 127, 0x02017720 to 0x020177A0, started at
        // 0x020174B8). Every other sequence that reads a variable that it doesn't set sounds at -1 as at a value in its
        // range.
        {{"Wii Party U", 0x0005000010137D00, "9d56d615edcafcd0e610a2426e0c66e79718cdcd"},
         {
             {"SEQ_SE_PLY_FUTTOBI_DAMAGE", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SEQ_SE_PLY_FUTTOBI_DAMAGE_DRC", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SEQ_SE_PLY_LAND_REAR_1", {{{15, 0}}, {{15, 1}}}},
             {"SEQ_SE_PLY_LAND_REAR_1_DRC", {{{15, 0}}, {{15, 1}}}},
             {"SEQ_SE_PLY_LAND_REAR_2", {{{15, 0}}}},
             {"SEQ_SE_PLY_LAND_REAR_2_DRC", {{{15, 0}}}},
             {"SEQ_SE_PLY_DAMAGE_BOUND", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SEQ_SE_PLY_DAMAGE_BOUND_DRC", {{{15, 0}}, {{15, 1}}, {{15, 2}}}},
             {"SEQ_SE_PLY_DAMAGE06_1", {{{15, 0}}, {{15, 1}}}},
             {"SEQ_SE_PLY_DAMAGE06_1_DRC", {{{15, 0}}, {{15, 1}}}},
             {"SEQ_SE_PLY_DAMAGE06_2", {{{15, 0}}}},
             {"SEQ_SE_PLY_DAMAGE06_2_DRC", {{{15, 0}}}},
             {"SEQ_SE_PLY_GROGGY02A", {{{15, 0}}, {{15, 1}}}},
             {"SEQ_SE_PLY_GROGGY02A_DRC", {{{15, 0}}, {{15, 1}}}},
             {"SEQ_SE_PLY_NMG411_NIN_JUMP00", {{{15, 0}}, {{15, 1}}}},
             {"SEQ_SE_NPG009_BALL_HIT_BALL", {{{15, 1}}, {{15, 64}}, {{15, 127}}}},
             {"SEQ_SE_NPG009_BALL_HIT_WALL_FLOOR", {{{15, 2}}, {{15, 64}}, {{15, 127}}}},
             {"SEQ_SE_NPG001_EVE_CHALL_3JYUMARU_NAZORI_DRC", {{{15, 2}}, {{15, 64}}, {{15, 127}}}},
             {"SEQ_SE_TVP_SDICE_03_AIR_B", {{{10, 0}}, {{10, 64}}, {{10, 127}}}},
             {"SEQ_BGM_NPG006_THEMA", {{{0, -1}}, {{0, 1}}, {{0, 2}}, {{0, 3}}}},
             {"SEQ_SE_NMG431_TARU_HIT_TARU_RM", {{{28, 0}}}},
             {"SEQ_SE_NMG431_TARU_HIT_KI_RM", {{{25, 0}, {27, 0}}}},
             {"SEQ_SE_NMG805_CAR_DRIVE_1P_DRC", kWiiPartyU10From0},
             {"SEQ_SE_NMG805_CAR_DRIVE_2P_DRC", kWiiPartyU10From0},
             {"SEQ_SE_TVP_SDICE_06_BALL", kWiiPartyU10From1},
             {"SEQ_SE_TVP_SDICE_06_BALL_OUTSIDE", kWiiPartyU10From1},
             {"SEQ_SE_DICE020_BOW_STRETCH", kWiiPartyU15From0},
             {"SEQ_SE_DICE023_AIR_DRC", kWiiPartyU15From0},
             {"SEQ_SE_DICE023_FUE_DRC", kWiiPartyU15From2},
             {"SEQ_SE_DICE029_BANE_NOBI_CHIDIMI", kWiiPartyU15From0},
             {"SEQ_SE_NMG308_WALL_HIT", kWiiPartyU15From1},
             {"SEQ_SE_NMG308_WALL_HIT_DRC", kWiiPartyU15From1},
             {"SEQ_SE_NMG313_TANK_L_AXEL", kWiiPartyU15From0},
             {"SEQ_SE_NMG313_TANK_L_AXEL_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NMG313_TANK_L_HIT_WALL", kWiiPartyU15From0},
             {"SEQ_SE_NMG313_TANK_L_HIT_WALL_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NMG408_BALL_HIT_WALL", kWiiPartyU15From0},
             {"SEQ_SE_NMG412_HIT_POLE", kWiiPartyU15From0},
             {"SEQ_SE_NMG412_HIT_RC", kWiiPartyU15From1},
             {"SEQ_SE_NMG412_HIT_WALL", kWiiPartyU15From1},
             {"SEQ_SE_NMG413_SLIDE_CONCRETE_1P", kWiiPartyU15From0},
             {"SEQ_SE_NMG413_SLIDE_CONCRETE_2P", kWiiPartyU15From0},
             {"SEQ_SE_NMG413_SLIDE_CONCRETE_3P", kWiiPartyU15From0},
             {"SEQ_SE_NMG413_SLIDE_CONCRETE_4P", kWiiPartyU15From0},
             {"SEQ_SE_NMG425_MII_SLIDE_1P", kWiiPartyU15From0},
             {"SEQ_SE_NMG425_MII_SLIDE_2P", kWiiPartyU15From0},
             {"SEQ_SE_NMG425_MII_SLIDE_3P", kWiiPartyU15From0},
             {"SEQ_SE_NMG425_MII_SLIDE_4P", kWiiPartyU15From0},
             {"SEQ_SE_NMG425_TUBE_SLIDE_1P", kWiiPartyU15From2},
             {"SEQ_SE_NMG425_TUBE_SLIDE_2P", kWiiPartyU15From2},
             {"SEQ_SE_NMG425_TUBE_SLIDE_3P", kWiiPartyU15From2},
             {"SEQ_SE_NMG425_TUBE_SLIDE_4P", kWiiPartyU15From2},
             {"SEQ_SE_NMG426_HIT_WALL", kWiiPartyU15From2},
             {"SEQ_SE_NMG429_CENTER_SLOT_VAR", kWiiPartyU15From0},
             {"SEQ_SE_NMG431_TARU_ROLL_1P", kWiiPartyU15From2},
             {"SEQ_SE_NMG431_TARU_ROLL_2P", kWiiPartyU15From2},
             {"SEQ_SE_NMG431_TARU_ROLL_3P", kWiiPartyU15From2},
             {"SEQ_SE_NMG431_TARU_ROLL_4P", kWiiPartyU15From2},
             {"SEQ_SE_NMG442_AMU", kWiiPartyU15From0},
             {"SEQ_SE_NMG601_KEZURU_DRC", kWiiPartyU15From2},
             {"SEQ_SE_NMG803_BALL_L_ROLL_LINE_1_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NMG803_BALL_L_ROLL_LINE_2_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NMG803_BALL_L_ROLL_LINE_3_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NMG803_BALL_M_ROLL_LINE_1_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NMG803_BALL_M_ROLL_LINE_2_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NMG803_BALL_M_ROLL_LINE_3_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NMG804_SCRATCH_DRC", kWiiPartyU15From2},
             {"SEQ_SE_NMG851_CONVEYOR", kWiiPartyU15From2},
             {"SEQ_SE_NMG854_CRANK_1P_DRC", kWiiPartyU15From2},
             {"SEQ_SE_NMG854_CRANK_2P_DRC", kWiiPartyU15From2},
             {"SEQ_SE_NPG002_DICE_BOUND", kWiiPartyU15From2},
             {"SEQ_SE_NPG002_DICE_FURU", kWiiPartyU15From0},
             {"SEQ_SE_NPG002_DICE_FURU_RM", kWiiPartyU15From0},
             {"SEQ_SE_NPG002_DICE_ROTA", kWiiPartyU15From0},
             {"SEQ_SE_NPG010_DRAW_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NPG012_BALL_HIT_PENDULUM_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NPG012_BALL_HIT_PENDULUM_SHINKOU_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NPG012_BALL_HIT_WALL_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NPG012_BALL_HIT_WALL_MAZE_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NPG012_BALL_HIT_WALL_PLASTIC_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NPG012_BALL_SHOT_DRC", kWiiPartyU15From0},
             {"SEQ_SE_NPG027_DICE_BOUND", kWiiPartyU15From2},
             {"SEQ_SE_NPG027_DICE_FURU", kWiiPartyU15From0},
             {"SEQ_SE_NPG027_DICE_FURU_RM", kWiiPartyU15From0},
             {"SEQ_SE_NPG027_DICE_ROTA", kWiiPartyU15From0},
             {"SEQ_SE_NPG001_DROUL_ROTA", {{{0, 2}}, {{0, 50}}, {{0, 100}}}},
             {"SEQ_SE_PLY_FOOT_WALK", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_FOOT_WALK_DRC", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_FOOT_WALK_V1", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_FOOT_WALK_V1_DRC", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_FOOT_RUN", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_FOOT_RUN_DRC", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_FOOT_RUN_V1", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_FOOT_RUN_V1_DRC", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_JUMP", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_JUMP_DRC", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_LANDING", kWiiPartyUFootsteps},
             {"SEQ_SE_LANDING", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_LANDING_DRC", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_LANDING_V1", kWiiPartyUFootsteps},
             {"SEQ_SE_PLY_LANDING_V1_DRC", kWiiPartyUFootsteps},
         }},
    };
    return kProfiles;
}

const VariableProfile* FindVariableProfile(const title::GameFiles& game)
{
    return FindKnownGame(KnownVariableProfiles(), game);
}

std::vector<xsflib::psf::Variables> VariantsOf(const VariableProfile& profile, const std::string& label)
{
    for (const VariableSound& sound : profile.sounds)
    {
        if (label == sound.label)
        {
            return sound.variants;
        }
    }

    return {};
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
    : impl_(new Impl{std::move(archive), std::move(settings)})
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
