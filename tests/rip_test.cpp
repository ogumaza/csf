// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on the length cases (ntdlib/testing/length_cases.h): sequences that each take one
// of the analysis's paths, in an archive in the Wii U's byte order. The test also checks that the ripper recognises
// other consoles' sound archives by their first bytes, reads the names of a JAudio1 game's sounds, and plays a
// sequence with the variables of its variant. It measures a sequence whose counted loops run past the time limit. The
// analysis plays a sound's tracks at the volumes that the game gives them. The jobs of a layered piece's states follow
// its table (rip/track_states.h).

#include "rip/rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"

#include "csf/format.h"
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
    Check(!csf::rip::ForeignArchive("FSA").has_value(), "three bytes aren't an archive");
}

// A JaiInfo.asn with 18 categories and `sounds`. A name of 28 bytes fills its field with no zero after it.
std::vector<uint8_t> Asn(const std::vector<csf::rip::AsnSound>& sounds)
{
    std::vector<uint8_t> asn(16 + 32 * (18 + sounds.size()));
    asn[15] = static_cast<uint8_t>(sounds.size());
    for (std::size_t i = 0; i < 18; ++i)
    {
        asn[16 + 32 * i] = 'C';
    }

    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        uint8_t* record = asn.data() + 16 + 32 * (18 + i);
        std::copy(sounds[i].name.begin(), sounds[i].name.end(), record);
        for (int byte = 0; byte < 4; ++byte)
        {
            record[28 + byte] = static_cast<uint8_t>(sounds[i].id >> (24 - 8 * byte));
        }
    }

    return asn;
}

// Checks the reader of JaiInfo.asn, and that a JAudio1 game's rip offers the sounds of its sound table but the effects
// sequence, with the names that JaiInfo.asn gives them: the sequences, then the effects.
void CheckAsnSounds()
{
    const std::string long_name(28, 'N');
    const std::vector<uint8_t> asn = Asn({{0x80000800, "JA_BGM_SE_SEQUENCE"},
                                          {0x80000001, "JA_BGM_FIRST"},
                                          {0x80000002, long_name},
                                          {0x00000000, "JA_SE_EFFECT"},
                                          {0xc0000000, "JA_STRM_STREAM"}});

    const std::vector<csf::rip::AsnSound> sounds = csf::rip::ReadAsnSounds(asn);

    Check(sounds.size() == 5, "the file names 5 sounds");
    Check(sounds.size() == 5 && sounds[1].id == 0x80000001 && sounds[1].name == "JA_BGM_FIRST",
          "a sound's record gives its ID and name");
    Check(sounds.size() == 5 && sounds[2].name == long_name, "a name can fill its 28 bytes");

    bool threw = false;
    try
    {
        csf::rip::ReadAsnSounds(std::span(asn).first(asn.size() - 1));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    Check(threw, "a file too short for its sounds is an error");

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
    Check(csf::rip::AsnNameKey(0x00007c12) == 0x00007012 && csf::rip::AsnNameKey(0x80000801) == 0x80000801,
          "an effect's name key drops bits 10 and 11, and a sequence's keeps them");
}

// The analysis plays a sequence with the variables that it gets set before the first tick. The sequence plays its note
// only while variable 1, the player's, holds 4 and variable 17, a global one, holds 2. A variable profile gives each
// sequence's variants by its label.
// An analyzer of an archive whose one sequence, SEQ_WAITING, is `code`.
csf::rip::LengthAnalyzer WaitingAnalyzer(const length_cases::Code& code)
{
    sar_files::ArchiveSpec spec;
    sar_files::SoundSpec sound;
    sound.name = "SEQ_WAITING";
    sound.volume = 127;
    sound.banks = {0};
    spec.sounds = {sound};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};
    spec.files = {{sar_files::Sequence(true, code.Bytes(), {{"Start", 0}}), "", false},
                  {sar_files::Bank(true, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(true, {sar_files::Wave(true, sar_files::ConstantWave(true))}), "", false}};
    return csf::rip::LengthAnalyzer(csf::fsar::SoundArchive::Load(sar_files::Archive(true, spec), csf::kSarOptions));
}

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

    const csf::rip::VariableProfile profile = {{"Test", 0, ""}, {{"SEQ_WAITING", {{{1, 4}, {17, 2}}, {{1, 5}}}}}};
    Check(csf::rip::VariantsOf(profile, "SEQ_WAITING") ==
                  std::vector<xsflib::psf::Variables>{{{1, 4}, {17, 2}}, {{1, 5}}} &&
              csf::rip::VariantsOf(profile, "SEQ_OTHER").empty(),
          "the profile gives a sequence's variants by its label");
}

// A measure that reaches the time limit inside counted loops runs again with each track's outermost counted loop
// endless, as ntdlib's length analysis does. The sequence repeats a part of 10.5 s in a loop of 127 passes: C4 for
// 0.5 s, then D4 for 5 s twice in an inner loop. The passes take 1,333.5 s, past the limit of 600 s, which comes inside
// the inner loop. With the outer loop endless, the fade starts after two passes, at 21 s. Were the inner loop endless
// too, the track would repeat D4 for ever, and the fade would start after 10.5 s.
void CheckLoopsAtTheLimit()
{
    length_cases::Code c(true);
    c.LoopStart(127).Note(60, 127, 48);
    c.LoopStart(2).Note(62, 127, 480).LoopEnd();
    c.LoopEnd().End();
    const std::optional<csf::rip::Timing> timing = WaitingAnalyzer(c).Analyze(0);
    Check(timing && timing->length_ms >= 20995 && timing->length_ms <= 21015 && timing->fade_ms == 10000,
          "only the outer of two counted loops at the time limit loops for ever, " +
              std::to_string(timing ? timing->length_ms : -1) + " ms");
}

// An analyzer of an archive with one sequence of two tracks. Track 0 plays C4 for 250 ms at the start. Track 1 waits
// 5 s, then plays C4 for 250 ms too. The wave of 1 s ends, as in the length cases.
csf::rip::LengthAnalyzer TwoTrackAnalyzer()
{
    // Track 1 starts after the timebase and the tempo (5 bytes), and track 0's open (5), note (3) and end (1).
    constexpr uint32_t kTrack1 = 14;
    length_cases::Code code(true);
    code.OpenTrack(1, kTrack1).Note(60, 127, 24).End();
    XSFLIB_CHECK(code.Bytes().size() == kTrack1);
    code.Wait(480).Note(60, 127, 24).End();

    sar_files::ArchiveSpec spec;
    sar_files::SoundSpec sound;
    sound.name = "SEQ_TWO_TRACKS";
    sound.volume = 127;
    sound.allocate_track_flags = 3;
    sound.banks = {0};
    spec.sounds = {sound};
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};
    spec.files = {{sar_files::Sequence(true, code.Bytes(), {{"Start", 0}}), "", false},
                  {sar_files::Bank(true, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(true, {sar_files::Wave(true, sar_files::ConstantWave(true))}), "", false}};
    return csf::rip::LengthAnalyzer(csf::fsar::SoundArchive::Load(sar_files::Archive(true, spec), csf::kSarOptions));
}

// The analysis plays the tracks at the volumes that the game gives them as it starts the sound. Track 1's late note
// sets the length: 5 s more than SEQ_END's. With track 1 at 0, the sequence ends after track 0's note, as SEQ_END does.
// With track 0 at 0, the length stays. With both at 0, the sequence never sounds.
void CheckTrackVolumes()
{
    const csf::rip::LengthAnalyzer analyzer = TwoTrackAnalyzer();
    const auto length = [&analyzer](uint16_t muted)
    {
        const std::optional<csf::rip::Timing> timing = analyzer.Analyze(0, {}, csf::rip::TrackVolumesOf(muted));
        return timing && !timing->silent ? timing->length_ms : -1;
    };

    const long long full = length(0);
    Check(full >= 5740 && full <= 5780, "two tracks with a note 5 s apart play for " + std::to_string(full) + " ms");
    Check(length(2) >= 740 && length(2) <= 780, "with track 1 at 0, the sequence ends after track 0's note");
    Check(length(1) == full, "with track 0 at 0, the length stays");
    const std::optional<csf::rip::Timing> muted = analyzer.Analyze(0, {}, csf::rip::TrackVolumesOf(3));
    Check(muted && muted->silent, "with both tracks at 0, the sequence never sounds");
}

// A layered piece's job takes its first state's volumes. The jobs of its later states follow it, with the state's name
// after the piece's label. The other jobs stay as they are, their variables too. An archive that no table holds has no
// layered pieces.
void CheckTrackStates()
{
    const std::vector<csf::rip::LayeredPiece> pieces = {{"SEQ_B", {{"", 0x0006}, {"later", 0x0001}, {"all", 0}}}};
    const std::vector<ntdlib::rip::Job> selected = {{.index = 0, .label = "SEQ_A", .variables = {{1, 4}}},
                                                    {.index = 1, .label = "SEQ_B"},
                                                    {.index = 2, .label = "SEQ_B2"}};
    const std::vector<ntdlib::rip::Job> jobs = csf::rip::WithTrackStates(pieces, selected);

    ntdlib::rip::TrackVolumes start = ntdlib::rip::kFullTrackVolumes;
    start[1] = 0.0f;
    start[2] = 0.0f;
    ntdlib::rip::TrackVolumes later = ntdlib::rip::kFullTrackVolumes;
    later[0] = 0.0f;
    XSFLIB_CHECK(csf::rip::TrackVolumesOf(0x0006) == start &&
                 csf::rip::TrackVolumesOf(0) == ntdlib::rip::kFullTrackVolumes);
    Check(jobs.size() == 5 && jobs[0].label == "SEQ_A" && jobs[0].track_volumes == ntdlib::rip::kFullTrackVolumes &&
              jobs[0].variables == xsflib::psf::Variables{{1, 4}} && jobs[1].index == 1 && jobs[1].label == "SEQ_B" &&
              jobs[1].track_volumes == start && jobs[2].index == 1 && jobs[2].label == "SEQ_B later" &&
              jobs[2].track_volumes == later && jobs[3].index == 1 && jobs[3].label == "SEQ_B all" &&
              jobs[3].track_volumes == ntdlib::rip::kFullTrackVolumes && jobs[4].label == "SEQ_B2",
          "a layered piece's states follow it, and the other jobs stay as they are");
    Check(csf::rip::LayeredPieces(length_cases::Archive(true)).empty(),
          "an archive that no table holds has no layered pieces");
}

// The players read version 3's profiles and the shorter profiles of version 2. The later words are then 0.
void CheckProfileWords()
{
    using csf::game::JAudio1Profile;
    using csf::game::NwSndProfile;
    const std::vector<uint32_t> jaudio1 = csf::game::ProfileWords(csf::game::WindWakerHd());
    const std::vector<uint32_t> nwsnd = csf::game::ProfileWords(csf::game::AmiiboFestival());
    Check(jaudio1.size() == 41 && nwsnd.size() == 35, "version 3's profiles have 41 and 35 words");
    Check(csf::game::ProfileWords(csf::game::MarioParty10()).size() == 35, "Mario Party 10's profile has 35 words");

    const std::span<const uint32_t> jaudio1_words(jaudio1);
    const std::optional<JAudio1Profile> full = csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words, 24);
    const std::optional<JAudio1Profile> earlier =
        csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(24), 24);
    Check(full && full->set_port_data == 0x0280c6d8 && full->ms_basic == 0x104b4f14,
          "a JAudio1 profile of 41 words holds setPortData and msBasic");
    Check(full && full->bgm_start == 0x0202204c && full->sub_bgm[0] == 0x16000000 && full->sub_bgm[2] == 0x82 &&
              full->battle_bgm == 0x80000004 && full->bgm_handle == 0x78 && full->scene_started == 0x29d &&
              full->bgm_set_sword_using == 0x02028fe0 && full->change_bgm_status == 0x02027fd4 &&
              full->change_sub_bgm_status == 0x02027ff0,
          "a JAudio1 profile of 41 words holds the starters of music, the state's setters and the status's writers in "
          "their order");
    Check(earlier && earlier->start_sound == full->start_sound && earlier->set_port_data == 0 &&
              earlier->ms_basic == 0 && earlier->bgm_start == 0 && earlier->bgm_set_sword_using == 0,
          "a JAudio1 profile of 24 words leaves the later words 0");
    Check(!csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(26), 24) &&
              !csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(25), 24) &&
              !csf::game::ProfileFromWords<JAudio1Profile>(jaudio1_words.first(23), 24),
          "a JAudio1 profile of 26, 25 or 23 words is refused");

    const std::span<const uint32_t> nwsnd_words(nwsnd);
    const std::optional<NwSndProfile> writers = csf::game::ProfileFromWords<NwSndProfile>(nwsnd_words, 32);
    const std::optional<NwSndProfile> none = csf::game::ProfileFromWords<NwSndProfile>(nwsnd_words.first(32), 32);
    Check(writers && writers->write_track_variable == 0x022C121C && none && none->write_variable == 0 &&
              none->write_global_variable == 0 && none->write_track_variable == 0,
          "an nw::snd profile of 32 words leaves the writers 0");
    Check(!csf::game::ProfileFromWords<NwSndProfile>(nwsnd_words.first(33), 32),
          "an nw::snd profile of 33 words is refused");
}

} // namespace

int main()
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

    return xsflib::testing::Summary("rip");
}
