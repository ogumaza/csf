// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on the length cases (ntdlib/testing/length_cases.h): sequences that each take one
// of the analysis's paths, in an archive in the Wii U's byte order. The test also checks that the ripper recognises
// other consoles' sound archives by their first bytes, reads the names of a JAudio1 game's sounds, and plays a
// sequence with the variables of its variant. It measures the counted loops that run past the time limit. The
// analysis plays a sound's tracks at the volumes that the game gives them. The jobs of a layered piece's states follow
// ntdlib's lists of sequence starts (rip/track_states.h). A JAudio game's rip picks its sounds, starts, states and
// variants in the order that rip/jaudio.h gives (SelectJAudioSounds).

#include "rip/rip.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/z2_starts.h"
#include "ntdlib/jaudio/z2_states.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/game_rules.h"
#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"

#include "csf/format.h"
#include "csf/jaudio2_files.h"
#include "csf/sar.h"
#include "game/game_driver.h"
#include "game/jaudio1_driver.h"
#include "game/nwsnd_driver.h"
#include "rip/jaudio.h"
#include "rip/track_states.h"

namespace length_cases = ntdlib::testing::length_cases;
namespace jaudio_files = ntdlib::testing::jaudio_files;
namespace sar_files = ntdlib::testing::sar_files;

using xsflib::testing::Check;

namespace
{

// The Wii U's sound archives.
constexpr ntdlib::sar::Flavor kWiiU = ntdlib::sar::Flavor::kCafe;

// Checks which files ForeignArchive takes for other consoles' sound archives. The Wii U's archive and the Switch's are
// built in their byte orders.
void CheckForeignArchives()
{
    const auto head = [](const std::vector<uint8_t>& file)
    {
        return std::string(file.begin(), file.begin() + 8);
    };

    const std::string wii_u = head(sar_files::Archive(true, {}));
    const std::string switch_archive = head(sar_files::Archive(false, {}));

    Check(!csf::rip::ForeignArchive(wii_u).has_value(), "a Wii U archive isn't another console's");
    Check(csf::rip::ForeignArchive(switch_archive) == "a Switch sound archive (little-endian FSAR)",
          "a Switch archive is the Switch's");
    Check(csf::rip::ForeignArchive("CSAR\xFF\xFE") == "a 3DS sound archive (CSAR)", "a 3DS archive is the 3DS's");
    Check(csf::rip::ForeignArchive("RSAR\xFE\xFF") == "a Wii sound archive (RSAR)", "a Wii archive is the Wii's");
    Check(csf::rip::ForeignArchive("SDAT\xFF\xFE") == "a DS sound archive (SDAT)", "a DS archive is the DS's");
    Check(!csf::rip::ForeignArchive("FSA").has_value(), "three bytes aren't an archive");
}

// Checks that a JAudio1 game's rip offers the sounds of its sound table but the effects sequence, with the names that
// JaiInfo.asn gives them: the sequences, then the effects. ntdlib's aaf_test checks the reader of JaiInfo.asn and the
// keys of the names.
void CheckAsnSounds()
{
    const std::string long_name(28, 'N');
    const std::vector<uint8_t> asn = jaudio_files::NameFile({{0x80000800, "JA_BGM_SE_SEQUENCE"},
                                                             {0x80000001, "JA_BGM_FIRST"},
                                                             {0x80000002, long_name},
                                                             {0x00000000, "JA_SE_EFFECT"},
                                                             {0xc0000000, "JA_STRM_STREAM"}});

    // The sound table: the effects sequence in file 7, two pieces of music and an effect.
    csf::rip::JAudioGame game{};
    game.release.name = "a JAudio1 game";
    game.driver = csf::GameDriverKind::kJAudio1;
    game.startup_archive = "content/Sound/JaiInit.aaf";
    game.name_file = "content/Sound/JaiInfo.asn";
    const std::map<std::string, std::vector<uint8_t>> content = {
        {"Sound/JaiInit.aaf", jaudio_files::AafFile({{16, 127, 7}, {16, 100, 3}, {16, 90, 9}, {0, 80}}, {})},
        {"Sound/JaiInfo.asn", asn}};

    const std::vector<ntdlib::jaudio::SoundEntry> offered = csf::rip::JAudioSounds(game, content);

    Check(offered.size() == 3 && offered[0].Id() == 0x80000001 && offered[0].name == "JA_BGM_FIRST" &&
              offered[1].Id() == 0x80000002 && offered[1].name == long_name,
          "a JAudio1 rip offers the sequences but the effects sequence first");
    Check(offered.size() == 3 && offered[2].Id() == 0x00000800 && offered[2].name == "JA_SE_EFFECT",
          "then the effect, with bit 11 set, and the name that the file gives its ID without that bit");
}

// An analyzer of the archive `bytes`.
csf::rip::LengthAnalyzer AnalyzerOf(const std::vector<uint8_t>& bytes)
{
    return csf::rip::LengthAnalyzer(csf::fsar::SoundArchive::Load(bytes, csf::kSarOptions));
}

// An analyzer of an archive whose one sequence, SEQ_WAITING, is `code`.
csf::rip::LengthAnalyzer WaitingAnalyzer(const length_cases::Code& code)
{
    return AnalyzerOf(length_cases::SequenceArchive(kWiiU, "SEQ_WAITING", code));
}

// The SHA-1 of Wii Party U's sound archive, and one that no list holds.
constexpr std::string_view kWiiPartyU = "528580bb03f3fbdea2815d7fa7b355233d90d28d";
constexpr std::string_view kUnlistedArchive = "0000000000000000000000000000000000000000";

// The analysis plays a sequence with the variables that it gets set before the first tick. The sequence plays its note
// only while variable 1, the player's, holds 4 and variable 17, a global one, holds 2. The lists of sequence starts
// give the variants by the archive's SHA-1 and the label.
void CheckVariables()
{
    length_cases::Code c(true);
    c.CompareEqual(1, 4).If().Jump(16).End();
    c.CompareEqual(17, 2).If().Jump(27).End();
    c.Note(60, 127, 24).End();
    const csf::rip::LengthAnalyzer analyzer = WaitingAnalyzer(c);

    const auto silent = [&analyzer](const xsflib::psf::Variables& variables)
    {
        const std::optional<csf::rip::Timing> timing = analyzer.Analyze(0, variables);
        return timing && timing->silent;
    };
    const std::optional<csf::rip::Timing> both = analyzer.Analyze(0, {{1, 4}, {17, 2}});

    Check(silent({}) && silent({{1, 4}}) && silent({{17, 2}}), "the sequence waits without both variables");
    Check(both && !both->silent && both->length_ms > 0, "the sequence sounds with both variables");

    // Track 0's variable 47, as amiibo Festival's SQ_SE_ACT_HUMMING reads it.
    length_cases::Code track(true);
    track.CompareEqual(47, 1).If().Jump(16).End();
    track.Note(60, 127, 24).End();
    const csf::rip::LengthAnalyzer track_analyzer = WaitingAnalyzer(track);

    const std::optional<csf::rip::Timing> waiting = track_analyzer.Analyze(0);
    const std::optional<csf::rip::Timing> set = track_analyzer.Analyze(0, {{csf::TrackVariableKey(0, 47), 1}});
    const std::optional<csf::rip::Timing> other = track_analyzer.Analyze(0, {{csf::TrackVariableKey(1, 47), 1}});

    Check(waiting && waiting->silent && other && other->silent && set && !set->silent,
          "the sequence sounds with track 0's variable 47 only");

    // The variants come from ntdlib's lists of sequence starts, by the archive's SHA-1 and the label. The start comes
    // first when the game sets variables in it. Wii Party U's score theme starts at -1 as a variant of its own. A
    // track's variable has the key that csf::TrackVariableKey gives it.
    constexpr std::string_view kMarioParty10 = "00bdc6c4610830576a44b7198d25894fe87f06b9";
    constexpr std::string_view kAmiiboFestivalArc1 = "724d9942fa0ea684839a3de269878e11b206a135";
    using Variants = std::vector<xsflib::psf::Variables>;

    Check(csf::rip::VariantsOf(kWiiPartyU, "SEQ_BGM_NPG006_THEMA") == Variants{{{0, -1}}, {{0, 1}}, {{0, 2}}, {{0, 3}}},
          "the score theme's start and its three levels");
    Check(csf::rip::VariantsOf(kWiiPartyU, "SEQ_SE_PLY_LANDING_V1").size() == 13 &&
              csf::rip::VariantsOf(kWiiPartyU, "SEQ_SE_PLY_LANDING_V1")[0] == xsflib::psf::Variables{{11, 0}},
          "a start that the game sets comes first");
    Check(csf::rip::VariantsOf(kMarioParty10, "SQ_SE_MG0K04_KOP_TCH_DRC") ==
              Variants{{{24, 0}}, {{24, 1}}, {{25, 1}}, {{24, 0}, {25, 0}}, {{24, 1}, {25, 1}}},
          "states of one and two variables");
    Check(
        csf::rip::VariantsOf(kAmiiboFestivalArc1, "SQ_SE_ACT_HUMMING") == Variants{{{csf::TrackVariableKey(0, 47), 1}}},
        "a track's variable");
    Check(csf::rip::VariantsOf(kMarioParty10, "SEQ_SE_PLY_LANDING_V1").empty() &&
              csf::rip::VariantsOf(kWiiPartyU, "SEQ_OTHER").empty() &&
              csf::rip::VariantsOf(kUnlistedArchive, "SEQ_BGM_NPG006_THEMA").empty(),
          "no variants for a sequence or an archive that the lists don't hold");
}

// A measure that reaches the time limit inside counted loops runs again with each track's outermost counted loop
// endless, as ntdlib's length analysis does (length_cases::CheckLongLoops).
void CheckLoopsAtTheLimit()
{
    const auto measure = [](const std::vector<uint8_t>& bytes)
    {
        return AnalyzerOf(bytes).Analyze(0);
    };
    length_cases::CheckLongLoops(kWiiU, measure, "the Wii U");
}

// The analysis plays the tracks at the volumes that the game gives them as it starts the sound
// (length_cases::CheckTrackVolumes).
void CheckTrackVolumes()
{
    const csf::rip::LengthAnalyzer analyzer = AnalyzerOf(length_cases::TwoTracksArchive(kWiiU));
    const auto analyze = [&analyzer](uint16_t muted)
    {
        return analyzer.Analyze(0, {}, ntdlib::rip::TrackVolumesOf(muted));
    };
    length_cases::CheckTrackVolumes(analyze);
}

// A layered piece's job takes its first state's volumes. The jobs of its later states follow it, with the state's name
// after the piece's label. The other jobs stay as they are, their variables too. An archive that the lists don't hold
// has no layered pieces.
void CheckTrackStates()
{
    const std::vector<ntdlib::sar::LayeredSound> pieces = {{"SEQ_B", {{"", 0x0006}, {"later", 0x0001}, {"all", 0}}}};
    const std::vector<ntdlib::rip::Job> selected = {{.index = 0, .label = "SEQ_A", .variables = {{1, 4}}},
                                                    {.index = 1, .label = "SEQ_B"},
                                                    {.index = 2, .label = "SEQ_B2"}};
    ntdlib::rip::TrackVolumes start = ntdlib::rip::kFullTrackVolumes;
    start[1] = 0.0f;
    start[2] = 0.0f;
    ntdlib::rip::TrackVolumes later = ntdlib::rip::kFullTrackVolumes;
    later[0] = 0.0f;

    const std::vector<ntdlib::rip::Job> jobs = csf::rip::WithTrackStates(pieces, selected);

    XSFLIB_CHECK(ntdlib::rip::TrackVolumesOf(0x0006) == start &&
                 ntdlib::rip::TrackVolumesOf(0) == ntdlib::rip::kFullTrackVolumes);
    Check(jobs.size() == 5 && jobs[0].label == "SEQ_A" && jobs[0].track_volumes == ntdlib::rip::kFullTrackVolumes &&
              jobs[0].variables == xsflib::psf::Variables{{1, 4}} && jobs[1].index == 1 && jobs[1].label == "SEQ_B" &&
              jobs[1].track_volumes == start && jobs[2].index == 1 && jobs[2].label == "SEQ_B later" &&
              jobs[2].track_volumes == later && jobs[3].index == 1 && jobs[3].label == "SEQ_B all" &&
              jobs[3].track_volumes == ntdlib::rip::kFullTrackVolumes && jobs[4].label == "SEQ_B2",
          "a layered piece's states follow it, and the other jobs stay as they are");
    Check(csf::rip::LayeredPieces(kUnlistedArchive).empty() && csf::rip::LayeredPieces(kWiiPartyU).empty(),
          "an archive whose pieces play every track has no layered pieces");

    // Kirby and the Rainbow Curse's stage select, from ntdlib's lists: the piece starts with track 7 alone, and each
    // world and boss stage plays other tracks.
    const std::vector<ntdlib::sar::LayeredSound> rainbow =
        csf::rip::LayeredPieces("813a15124d13b1978e7e3cb49f6648b9cb50d705");
    Check(rainbow.size() == 1 && rainbow[0].label == "BGM_SEQ_MENU_INTERACTIVE" && rainbow[0].states.size() == 15 &&
              rainbow[0].states[0].name.empty() && rainbow[0].states[0].muted == 0xff7f &&
              rainbow[0].states[1].name == "world 1" && rainbow[0].states[1].muted == 0xfbfe &&
              rainbow[0].states[8].name == "world 1 boss" && rainbow[0].states[8].muted == 0xfafe &&
              rainbow[0].states[14].name == "world 7 boss" && rainbow[0].states[14].muted == 0xfebf,
          "the stage select's states");
}

// The players read version 3's profiles and the shorter profiles of earlier forms. The later words are then 0.
void CheckProfileWords()
{
    using csf::game::JAudio1Profile;
    using csf::game::NwSndProfile;
    const std::vector<uint32_t> jaudio1 = csf::game::ProfileWords(csf::game::WindWakerHd());
    const std::vector<uint32_t> nwsnd = csf::game::ProfileWords(csf::game::AmiiboFestival());
    constexpr std::array<std::size_t, 2> kJAudio1Shorter = {24, 41};
    constexpr std::array<std::size_t, 1> kNwSndShorter = {32};

    Check(jaudio1.size() == 43 && nwsnd.size() == 35, "version 3's profiles have 43 and 35 words");
    Check(csf::game::ProfileWords(csf::game::MarioParty10()).size() == 35, "Mario Party 10's profile has 35 words");

    const std::span<const uint32_t> jaudio1_words(jaudio1);
    const std::optional<JAudio1Profile> full =
        csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words, kJAudio1Shorter);
    const std::optional<JAudio1Profile> starters =
        csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(41), kJAudio1Shorter);
    const std::optional<JAudio1Profile> earlier =
        csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(24), kJAudio1Shorter);
    Check(full && full->set_port_data == 0x0280c6d8 && full->ms_basic == 0x104b4f14,
          "a JAudio1 profile of 43 words holds setPortData and msBasic");
    Check(
        full && full->bgm_start == 0x0202204c && full->sub_bgm[0] == 0x16000000 && full->sub_bgm[2] == 0x82 &&
            full->battle_bgm == 0x80000004 && full->bgm_handle == 0x78 && full->scene_started == 0x29d &&
            full->bgm_set_sword_using == 0x02028fe0 && full->change_bgm_status == 0x02027fd4 &&
            full->change_sub_bgm_status == 0x02027ff0 && full->register_sub_frame_callback == 0x028155fc &&
            full->write_port_app_direct == 0x02818b50,
        "a JAudio1 profile of 43 words holds the starters of music, the state's setters, the status's writers and the "
        "writers after the first tick in their order");
    Check(starters && starters->change_sub_bgm_status == 0x02027ff0 && starters->register_sub_frame_callback == 0 &&
              starters->write_port_app_direct == 0,
          "a JAudio1 profile of 41 words leaves the writers after the first tick 0");
    Check(earlier && earlier->start_sound == full->start_sound && earlier->set_port_data == 0 &&
              earlier->ms_basic == 0 && earlier->bgm_start == 0 && earlier->bgm_set_sword_using == 0,
          "a JAudio1 profile of 24 words leaves the later words 0");
    Check(!csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(42), kJAudio1Shorter) &&
              !csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(26), kJAudio1Shorter) &&
              !csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(25), kJAudio1Shorter) &&
              !csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(23), kJAudio1Shorter),
          "a JAudio1 profile of 42, 26, 25 or 23 words is refused");

    const std::span<const uint32_t> nwsnd_words(nwsnd);
    const std::optional<NwSndProfile> writers = csf::game::ProfileFromWords<NwSndProfile>(nwsnd_words, kNwSndShorter);
    const std::optional<NwSndProfile> none =
        csf::game::ProfileFromWords<NwSndProfile>(nwsnd_words.first(32), kNwSndShorter);
    Check(writers && writers->write_track_variable == 0x022C121C && none && none->write_variable == 0 &&
              none->write_global_variable == 0 && none->write_track_variable == 0,
          "an nw::snd profile of 32 words leaves the writers 0");
    Check(!csf::game::ProfileFromWords<NwSndProfile>(nwsnd_words.first(33), kNwSndShorter),
          "an nw::snd profile of 33 words is refused");
}

// The selection of a JAudio game's sounds: each sound in the order of the sound table. A piece's start with a suffix
// follows the piece, then its states with a suffix and its other states. An effect's variants by port follow the
// effect. A start without a suffix mutes the piece's children. A state without a suffix replaces the piece's start. A
// state that the game writes once the piece plays is left out. A sound without a name is labelled with its ID.
void CheckJAudioSelection()
{
    ntdlib::jaudio::SoundEntry piece;
    piece.name = "BGM_FIELD";
    piece.index = 1;
    ntdlib::jaudio::SoundEntry effect;
    effect.name = "SE_STEP";
    effect.section = 1;
    effect.index = 2;
    ntdlib::jaudio::SoundEntry unnamed;
    unnamed.section = 2;
    unnamed.index = 3;
    ntdlib::jaudio::SoundEntry village;
    village.name = "BGM_VILLAGE";
    village.index = 4;
    const std::vector<ntdlib::jaudio::SoundEntry> sounds = {piece, effect, unnamed, village};

    const ntdlib::jaudio::MutedStart starts[] = {{.sound = 1, .muted_children = 0x0003, .suffix = ""},
                                                 {.sound = 1, .muted_children = 0x0004, .suffix = "_riding"},
                                                 {.sound = 4, .muted_children = 0x0008, .suffix = ""}};
    ntdlib::jaudio::MusicState state;
    state.sound = 1;
    state.suffix = "_status1";
    state.groups[0] = {.children = 0x0010, .volume = 0.0f};
    state.groups[1] = {.children = 0x0020, .volume = 0.5f};
    state.status_port = 3;
    state.section_statuses[0] = {.section = 1, .status = 12};
    ntdlib::jaudio::MusicState later = state;
    later.suffix = "_later";
    later.status_later = true;
    ntdlib::jaudio::MusicState village_start;
    village_start.sound = 4;
    village_start.groups[0] = {.children = 0x0040, .volume = 0.25f};
    village_start.status_port = 5;
    const ntdlib::jaudio::MusicState states[] = {state, later, village_start};

    csf::rip::JAudioGame game{};
    game.music_prefix = "BGM_";
    game.port_effects = {{.id = effect.Id(), .port = 6, .values = {1, 2}}};
    game.music_variants = {{.piece = 1, .id = 0x80000105, .suffix = "night", .values = {{16, 1}}}};
    game.muted_starts = starts;
    game.music_states = states;

    const std::vector<csf::rip::JAudioJob> jobs = csf::rip::SelectJAudioSounds(sounds, game, false, std::regex(".*"));
    std::vector<std::string> labels;
    for (const csf::rip::JAudioJob& job : jobs)
    {
        labels.push_back(job.label);
    }

    Check(labels == std::vector<std::string>{"BGM_FIELD", "BGM_FIELD_riding", "BGM_FIELD_status1", "BGM_FIELD_night",
                                             "SE_STEP", "SE_STEP 6=1", "SE_STEP 6=2", "0x02000003", "BGM_VILLAGE"},
          "a JAudio game's sounds, starts, states and variants in order");
    if (labels.size() != 9)
    {
        return;
    }

    Check(jobs[0].muted_children == 0x0003 && jobs[0].ports.empty() && jobs[1].muted_children == 0x0004,
          "a start mutes the piece's children");
    Check(jobs[2].muted_children == 0x0010 &&
              jobs[2].volumes == std::vector<csf::ChildVolume>{{.children = 0x0020, .volume = 0.5f}} &&
              jobs[2].section_statuses == std::vector<csf::SectionStatus>{{.section = 1, .status = 12}} &&
              jobs[2].ports == xsflib::psf::Variables{{csf::kStatusPort, 3}},
          "a state gives the piece's children, sections and status");
    Check(jobs[3].id == 0x80000105 && jobs[3].index == 0 && jobs[3].ports == xsflib::psf::Variables{{16, 1}},
          "another state starts the piece by its ID with its values");
    Check(jobs[5].index == 1 && jobs[5].ports == xsflib::psf::Variables{{6, 1}}, "an effect's variant sets its port");
    Check(jobs[8].muted_children == 0 &&
              jobs[8].volumes == std::vector<csf::ChildVolume>{{.children = 0x0040, .volume = 0.25f}} &&
              jobs[8].ports == xsflib::psf::Variables{{csf::kStatusPort, 5}},
          "a state without a suffix replaces the piece's start");

    const std::vector<csf::rip::JAudioJob> music =
        csf::rip::SelectJAudioSounds(sounds, game, true, std::regex("_r|STEP"));
    Check(music.size() == 1 && music[0].label == "BGM_FIELD_riding", "--bgm and --only pick by the label");
}

} // namespace

int main()
{
    const auto tests = []
    {
        // CSF's analysis waits for the loops of the tracks other than the main track (its model's OthersLooped).
        const csf::rip::LengthAnalyzer analyzer(
            csf::fsar::SoundArchive::Load(length_cases::Archive(true), csf::kSarOptions));
        length_cases::CheckCases([&analyzer](uint32_t index) { return analyzer.Analyze(index); }, true);

        CheckForeignArchives();
        CheckAsnSounds();
        CheckVariables();
        CheckLoopsAtTheLimit();
        CheckProfileWords();
        CheckTrackVolumes();
        CheckTrackStates();
        CheckJAudioSelection();
    };
    return xsflib::testing::RunTest("rip", tests);
}
