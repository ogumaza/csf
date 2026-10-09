// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on sequences that each take one of its paths. They are: a sequence that ends; one
// that loops by jumping back, and one that loops for ever; one that never sounds; held notes on a looping wave and on
// one that ends; a jump back into code the track hasn't run (not a loop); notes at a track volume and at a sound volume
// of 0 (which never sound either); and a track that loops every tick before another track's note starts. In the rest,
// another track plays while a track polls or counts a variable once a tick, or marks beats for the game; a track that
// changes its volume but plays no notes comes before the track that plays, or is track 0; a tied note's wave ends
// during a pass; or a note runs out while its track polls, or would but for a held length or the damper. In four more,
// another track plays while track 0 waits for a note that never ends, waits out a held note, waits for notes that do
// end, or plays a note that never ends without waiting for it. In two more, track 0 plays notes in a loop inside a
// subroutine: a loop that never ends, and one that a conditional command leaves after three passes. In another, track
// 0 waits a tick and jumps back while track 1 holds a note that never ends and only changes its volume. In the last,
// track 1 plays a note and then counts ticks in a loop inside its loop. The test also checks that the ripper recognises
// other consoles' sound archives by their first bytes.

#include "rip/rip.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "check.h"
#include "fsar/formats.h"
#include "fsar_files.h"

namespace
{

// Timebase 48 and tempo 120: 48 ticks take 500 ms.
const std::vector<uint8_t> kStart = {0xB0, 48, 0xE1, 0x00, 120};

// Each sequence, after kStart, where the sound starts in it, the sound's volume and the tracks it allocates.
struct TestSequence
{
    const char* name;
    std::vector<uint8_t> bytecode;
    uint32_t start = 0;
    uint8_t volume = 127;
    uint32_t tracks = 1;
};

const std::vector<TestSequence> kSequences = {
    // A note of 24 ticks, then the end.
    {"SEQ_END", {60, 127, 24, 0xFF}},
    // A note of 24 ticks, played over and over by a jump back to it.
    {"SEQ_JUMP", {60, 127, 24, 0x89, 0x00, 0x00, 0x05, 0xFF}},
    // The same note in a loop with a count of 0. Such a loop repeats for ever.
    {"SEQ_FOREVER", {0xD4, 0, 60, 127, 24, 0xFC, 0xFF}},
    // A wait and the end, with no note.
    {"SEQ_SILENT", {0x80, 48, 0xFF}},
    // A held note of 9,600 ticks on program 1. Program 1's wave loops.
    {"SEQ_HELD_LOOP", {0x81, 1, 60, 127, 0xCB, 0x00, 0xFF}},
    // A held note on program 0. Program 0's wave ends.
    {"SEQ_HELD_ONCE", {60, 127, 0xCB, 0x00, 0xFF}},
    // Starts after a note and its end, and jumps back to the note. The track hasn't played that note. The jump isn't a
    // loop.
    {"SEQ_SHARED", {60, 127, 24, 0xFF, 0x89, 0x00, 0x00, 0x05}, 9},
    // A note at a track volume of 0.
    {"SEQ_TRACK_MUTED", {0xC1, 0, 60, 127, 24, 0xFF}},
    // A note in a sound whose volume is 0.
    {"SEQ_SOUND_MUTED", {60, 127, 24, 0xFF}, 0, 0},
    // Opens track 1 at 16 and waits a tick over and over. Meanwhile, track 1 waits 48 ticks and holds a note on program
    // 1.
    {"SEQ_LATE",
     {0x88, 1, 0x00, 0x00, 16, 0x80, 1, 0x89, 0x00, 0x00, 10, 0x80, 48, 0x81, 1, 60, 127, 0xCB, 0x00, 0xFF},
     0,
     127,
     3},
    // Track 0 opens track 1 at 27, then polls variable 0 once a tick: it compares the variable with 1 (F0 90), would
    // jump to its end if they were equal, and waits a tick. Track 1 plays a note of 24 ticks over and over.
    {"SEQ_POLL_MUSIC",
     {0x88, 1, 0x00, 0x00, 27,   0xF0, 0x90, 0x00, 0x00, 0x01, 0xA2, 0x89, 0x00, 0x00, 26,
      0x80, 1, 0x89, 0x00, 0x00, 10,   0xFF, 60,   127,  24,   0x89, 0x00, 0x00, 27,   0xFF},
     0,
     127,
     3},
    // Track 0 polls as in SEQ_POLL_MUSIC. Track 1 ties a note of a tick on program 1 to itself over and over. The note
    // sounds on, and nothing changes.
    {"SEQ_POLL_HOLD",
     {0x88, 1,    0x00, 0x00, 27,   0xF0, 0x90, 0x00, 0x00, 0x01, 0xA2, 0x89, 0x00, 0x00, 26,   0x80, 1,
      0x89, 0x00, 0x00, 10,   0xFF, 0x81, 1,    0xC8, 1,    60,   127,  1,    0x89, 0x00, 0x00, 31,   0xFF},
     0,
     127,
     3},
    // Track 0 polls as in SEQ_POLL_MUSIC. Track 1 plays a note of 12 ticks, rests for 48 ticks, plays the note again
    // and ends.
    {"SEQ_POLL_END",
     {0x88, 1,    0x00, 0x00, 27, 0xF0, 0x90, 0x00, 0x00, 0x01, 0xA2, 0x89, 0x00, 0x00, 26,  0x80,
      1,    0x89, 0x00, 0x00, 10, 0xFF, 60,   127,  12,   0x80, 48,   60,   127,  12,   0xFF},
     0,
     127,
     3},
    // Track 0 sets the track's volume to 100 at the start of each pass. Only the first pass changes the volume.
    // Otherwise track 0 polls as in SEQ_POLL_MUSIC. Track 1, at 29, plays a note of 24 ticks over and over.
    {"SEQ_POLL_MOVE",
     {0x88, 1,    0x00, 0x00, 29,   0xC1, 100, 0xF0, 0x90, 0x00, 0x00, 0x01, 0xA2, 0x89, 0x00, 0x00,
      28,   0x80, 1,    0x89, 0x00, 0x00, 10,  0xFF, 60,   127,  24,   0x89, 0x00, 0x00, 29,   0xFF},
     0,
     127,
     3},
    // Track 0 sets variable 1 to 1 and then to 2, 24 ticks apart, over and over. These are beats for the game. No
    // track reads them. Track 1, at 29, holds a note on program 1.
    {"SEQ_BEATS",
     {0x88, 1,    0x00, 0x00, 29,   0xF0, 0x80, 0x01, 0x00, 0x01, 0x80, 24,  0xF0, 0x80, 0x01, 0x00,
      0x02, 0x80, 24,   0x89, 0x00, 0x00, 10,   0xFF, 0x81, 1,    60,   127, 0xCB, 0x00, 0xFF},
     0,
     127,
     3},
    // Track 0 counts in variable 2, once a tick, to 30 (F0 81 adds, F0 91 compares). Then track 0 jumps to 31 and
    // holds a note on program 1. Track 1, at 38, holds another note on program 1 from the start.
    {"SEQ_POLL_COUNT",
     {0x88, 1,    0x00, 0x00, 38,   0xF0, 0x81, 0x02, 0x00, 0x01, 0xF0, 0x91, 0x02, 0x00,
      30,   0xA2, 0x89, 0x00, 0x00, 31,   0x80, 1,    0x89, 0x00, 0x00, 10,   0x81, 1,
      62,   127,  0xCB, 0x00, 0xFF, 0x81, 1,    60,   127,  0xCB, 0x00, 0xFF},
     0,
     127,
     3},
    // Track 0 opens tracks 1 and 2, then waits 48 ticks and jumps back, over and over. Track 1, at 22, sets its volume
    // to 100 and to 90, 12 ticks apart, and jumps back. Track 2, at 35, plays a note of 24 ticks over and over.
    {"SEQ_QUIET",
     {0x88, 1,  0x00, 0x00, 22,   0x88, 2,    0x00, 0x00, 35, 0x80, 48, 0x89, 0x00, 0x00, 15,   0xFF, 0xC1, 100,
      0x80, 12, 0xC1, 90,   0x80, 12,   0x89, 0x00, 0x00, 22, 0xFF, 60, 127,  24,   0x89, 0x00, 0x00, 35,   0xFF},
     0,
     127,
     7},
    // Ties notes of 36 ticks on program 0 to one another over and over (from 10). The wave ends after 1 s, during the
    // third pass. The next pass starts a new note.
    {"SEQ_RESTART", {0xC8, 1, 60, 127, 36, 60, 127, 36, 0x89, 0x00, 0x00, 10, 0xFF}},
    // Plays a note of 48 ticks on program 1 without waiting for it (C7 00), then waits a tick and jumps back, over and
    // over (from 12).
    {"SEQ_ENDING", {0x81, 1, 0xC7, 0x00, 60, 127, 48, 0x80, 1, 0x89, 0x00, 0x00, 12, 0xFF}},
    // Track 0 opens track 1 at 23, then sets its volume to 100 and to 90, 24 ticks apart, and jumps back (to 10), over
    // and over. Track 1 plays a note of 24 ticks over and over.
    {"SEQ_CONDUCTOR",
     {0x88, 1,    0x00, 0x00, 23,   0xC1, 100, 0x80, 24,   0xC1, 90,   0x80, 24,
      0x89, 0x00, 0x00, 10,   0xFF, 60,   127, 24,   0x89, 0x00, 0x00, 23,   0xFF},
     0,
     127,
     3},
    // Plays a held note of 9,600 ticks on program 1 without waiting for it, then waits a tick and jumps back, over and
    // over (from 13).
    {"SEQ_HELD_POLL", {0x81, 1, 0xC7, 0x00, 60, 127, 0xCB, 0x00, 0x80, 1, 0x89, 0x00, 0x00, 13, 0xFF}},
    // Turns the damper on (DF 7F), plays a note of 48 ticks on program 1 without waiting for it, then waits a tick and
    // jumps back, over and over (from 14).
    {"SEQ_DAMPER", {0x81, 1, 0xDF, 127, 0xC7, 0x00, 60, 127, 48, 0x80, 1, 0x89, 0x00, 0x00, 14, 0xFF}},
    // Track 0 opens track 1 at 16, plays a note of length 0 on program 1 and ends. With note wait on, it waits for the
    // note to end, and the note never does. Track 1 plays a note of 24 ticks over and over.
    {"SEQ_ENDLESS",
     {0x88, 1, 0x00, 0x00, 16, 0x81, 1, 60, 127, 0, 0xFF, 60, 127, 24, 0x89, 0x00, 0x00, 16, 0xFF},
     0,
     127,
     3},
    // Track 0 opens track 1 at 17, holds a note of 9,600 ticks on program 1 and ends. Track 1 plays as in SEQ_ENDLESS.
    {"SEQ_HELD_UNDER",
     {0x88, 1, 0x00, 0x00, 17, 0x81, 1, 60, 127, 0xCB, 0x00, 0xFF, 60, 127, 24, 0x89, 0x00, 0x00, 17, 0xFF},
     0,
     127,
     3},
    // Track 0 opens track 1 at 25. With note wait off, it plays a note of 96 ticks on program 1. With note wait on, it
    // plays a note of length 0 on program 0, whose wave ends after 1 s, and waits for both notes to end. Then it ends.
    // Track 1 plays as in SEQ_ENDLESS.
    {"SEQ_NOTES_END",
     {0x88, 1, 0x00, 0x00, 25, 0xC7, 0x00, 0x81, 1,  60,   127,  96,   0xC7, 0x01,
      0x81, 0, 60,   127,  0,  0xFF, 60,   127,  24, 0x89, 0x00, 0x00, 25,   0xFF},
     0,
     127,
     3},
    // As SEQ_CONDUCTOR, but before its loop (from 17), track 0 plays a note of length 0 on program 1 with note wait
    // off.
    // That note never ends, but track 0 doesn't wait for it. Track 1 is at 30.
    {"SEQ_ENDLESS_NO_WAIT",
     {0x88, 1,    0x00, 0x00, 30,   0xC7, 0x00, 0x81, 1,  60,  127, 0,    0xC1, 100,  0x80, 24,  0xC1,
      90,   0x80, 24,   0x89, 0x00, 0x00, 17,   0xFF, 60, 127, 24,  0x89, 0x00, 0x00, 30,   0xFF},
     0,
     127,
     3},
    // Track 0 calls a subroutine at 10. The subroutine plays a note of 24 ticks and jumps back to its start
    // unconditionally.
    {"SEQ_SUB_LOOP", {0x8A, 0x00, 0x00, 10, 0xFF, 60, 127, 24, 0x89, 0x00, 0x00, 10, 0xFF}},
    // Track 0 sets variable 0 to 0 and calls a subroutine at 15. The subroutine plays a note of 24 ticks and adds 1 to
    // the variable. It returns once the variable is 3, and otherwise jumps back to its start.
    {"SEQ_SUB_COUNT", {0xF0, 0x80, 0,    0x00, 0x00, 0x8A, 0x00, 0x00, 15,   0xFF, 60,   127,  24,   0xF0, 0x81,
                       0,    0x00, 0x01, 0xF0, 0x90, 0,    0x00, 0x03, 0xA2, 0xFD, 0x89, 0x00, 0x00, 15,   0xFF}},
    // Track 0 opens track 1 at 17, then waits a tick and jumps back, over and over. Track 1 plays a note of length 0 on
    // program 1 without waiting for it. Then it loops for ever (D4 00 to FC). Each pass sets its volume to 100 and to
    // 90, 12 ticks apart, and plays no note.
    {"SEQ_ENDLESS_SWELL",
     {0x88, 1,   0x00, 0x00, 17, 0x80, 1,   0x89, 0x00, 0x00, 10, 0xFF, 0xC7, 0x00, 0x81, 1,
      60,   127, 0,    0xD4, 0,  0xC1, 100, 0x80, 12,   0xC1, 90, 0x80, 12,   0xFC, 0xFF},
     0,
     127,
     3},
    // Track 0 opens track 1 at 11 and ends. Track 1 plays a note of 24 ticks, then counts 24 ticks in variable 0 with
    // an
    // inner loop that jumps back to 19. Then track 1 jumps back to the note, over and over.
    {"SEQ_INNER_LOOP",
     {0x88, 1,    0x00, 0x00, 11,   0xFF, 60,   127,  24,   0xF0, 0x80, 0x00, 0x00, 0x00,
      0x80, 1,    0xF0, 0x81, 0x00, 0x00, 0x01, 0xF0, 0x91, 0x00, 0x00, 24,   0xA2, 0x89,
      0x00, 0x00, 40,   0x89, 0x00, 0x00, 19,   0x89, 0x00, 0x00, 11,   0xFF},
     0,
     127,
     3},
};

std::vector<uint8_t> MakeArchive()
{
    // A wave of 1 s that ends, and one of 0.1 s that loops.
    std::vector<uint8_t> samples(2 * 32000);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        samples[i] = 0x10; // 0x1000, big-endian
    }
    fsar_files::WaveSpec once;
    once.encoding = 1;
    once.loop_end = 32000;
    once.channels = {samples};
    fsar_files::WaveSpec looping = once;
    looping.loop = true;
    looping.loop_end = 3200;
    looping.channels = {std::vector<uint8_t>(samples.begin(), samples.begin() + 2 * 3200)};

    // Program 0 plays the wave that ends, and program 1 the one that loops. Both have the fastest attack, decay and
    // release, a sustain at full volume and no hold. A note then settles at once.
    fsar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}, {0x05000000, 1}};
    for (uint32_t wave = 0; wave < 2; ++wave)
    {
        fsar_files::Region region;
        region.wave = wave;
        region.original_key = 60;
        region.adshr = {127, 127, 127, 0, 127};
        fsar_files::VelocityTable velocities;
        velocities.entries = {{0, region}};
        fsar_files::KeyTable keys;
        keys.entries = {{0, velocities}};
        bank.instruments.push_back(keys);
    }

    fsar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {
        {fsar_files::Bank(true, bank), "", false},
        {fsar_files::WaveArchive(true, {fsar_files::Wave(true, once), fsar_files::Wave(true, looping)}), "", false}};
    for (const TestSequence& sequence : kSequences)
    {
        std::vector<uint8_t> bytecode = kStart;
        bytecode.insert(bytecode.end(), sequence.bytecode.begin(), sequence.bytecode.end());
        fsar_files::SoundSpec sound;
        sound.name = sequence.name;
        sound.volume = sequence.volume;
        sound.allocate_track_flags = sequence.tracks;
        sound.banks = {0};
        sound.file = static_cast<uint32_t>(spec.files.size());
        sound.start_offset = sequence.start;
        spec.sounds.push_back(sound);
        spec.files.push_back({fsar_files::Sequence(true, bytecode, {{"Start", 0}}), "", false});
    }

    return fsar_files::Archive(true, spec);
}

// Checks that sound `index` measures `fade_ms` of fade and a length from `min_ms` to `max_ms`, and is silent or not.
void CheckTiming(const csf::rip::LengthAnalyzer& analyzer, uint32_t index, long long min_ms, long long max_ms,
                 long long fade_ms, bool silent = false)
{
    const std::optional<csf::rip::Timing> timing = analyzer.Analyze(index);
    const std::string name = kSequences[index].name;
    if (!Check(timing.has_value(), name + " has a length"))
    {
        return;
    }

    Check(timing->length_ms >= min_ms && timing->length_ms <= max_ms && timing->fade_ms == fade_ms,
          name + "'s length is " + std::to_string(timing->length_ms) + " ms with a fade of " +
              std::to_string(timing->fade_ms) + " ms");
    Check(timing->silent == silent, name + (silent ? " is silent" : " isn't silent"));
}

// Checks which files ForeignArchive takes for other consoles' sound archives. The Wii U's archive and the Switch's are
// built in their byte orders.
void CheckForeignArchives()
{
    const auto head = [](const std::vector<uint8_t>& file)
    {
        return std::string(file.begin(), file.begin() + 8);
    };
    const std::string wii_u = head(MakeArchive());
    const std::string switch_archive = head(fsar_files::Archive(false, {}));

    Check(!csf::rip::ForeignArchive(wii_u).has_value(), "a Wii U archive isn't another console's");
    Check(csf::rip::ForeignArchive(switch_archive) == "a Switch sound archive (little-endian FSAR)",
          "a Switch archive is the Switch's");
    Check(csf::rip::ForeignArchive("CSAR\xFF\xFE") == "a 3DS sound archive (CSAR)", "a 3DS archive is the 3DS's");
    Check(csf::rip::ForeignArchive("RSAR\xFE\xFF") == "a Wii sound archive (RSAR)", "a Wii archive is the Wii's");
    Check(!csf::rip::ForeignArchive("FSA").has_value(), "three bytes aren't an archive");
}

} // namespace

int main()
{
    const csf::rip::LengthAnalyzer analyzer(csf::fsar::SoundArchive::Load(MakeArchive()));

    // A note of 250 ms with the fastest release, from the second frame, and 500 ms of tail.
    CheckTiming(analyzer, 0, 740, 780, 0);

    // Two loops of 24 ticks, the first starting in the second frame, then the fade.
    CheckTiming(analyzer, 1, 495, 515, 10000);
    CheckTiming(analyzer, 2, 495, 515, 10000);

    // No sound: the tail alone.
    CheckTiming(analyzer, 3, 500, 500, 0, true);

    // The held note settles at once. Its wave of 0.1 s loops twice.
    CheckTiming(analyzer, 4, 200, 300, 10000);

    // The held note's wave ends after 1 s, and so does the sequence.
    CheckTiming(analyzer, 5, 1490, 1530, 0);

    // The note plays once.
    CheckTiming(analyzer, 6, 740, 780, 0);

    // Notes at a volume of 0 make no sound.
    CheckTiming(analyzer, 7, 500, 500, 0, true);
    CheckTiming(analyzer, 8, 500, 500, 0, true);

    // Track 0's loops of a tick change nothing, and don't count. The held note starts at 500 ms. Its wave of 0.1 s
    // loops twice, and then the fade starts.
    CheckTiming(analyzer, 9, 690, 730, 10000);

    // Track 0 only polls, and its passes don't count. Track 1's loops are the sequence's loops: two loops of 24 ticks.
    CheckTiming(analyzer, 10, 495, 515, 10000);

    // No track changes anything after the first ticks. The tied note's wave of 0.1 s loops twice.
    CheckTiming(analyzer, 11, 190, 260, 10000);

    // The second note ends at 750 ms, and 500 ms of tail follow. Track 0's polls during the rest aren't loops.
    CheckTiming(analyzer, 12, 1235, 1285, 0);

    // Track 0's first pass changes its volume, and counts as a loop. Then track 1's loops take over, and the count
    // starts again: two loops of 24 ticks.
    CheckTiming(analyzer, 13, 495, 515, 10000);

    // No track reads the beats. The sequence is steady once track 0 has made a pass, at 500 ms, and by then the held
    // note's wave has looped more than twice.
    CheckTiming(analyzer, 14, 495, 530, 10000);

    // Track 0 isn't still while it counts. The second held note starts at about 320 ms, two semitones up, and its wave
    // loops twice in 180 ms.
    CheckTiming(analyzer, 15, 495, 540, 10000);

    // Track 0's passes are idle. Track 1's passes change its volume but play no notes. Neither keeps track 2 from being
    // the main track. Track 2's loops at 48 and 72 ticks count. At 24 ticks, track 0 had made no pass yet.
    CheckTiming(analyzer, 16, 745, 765, 10000);

    // The ties change nothing. The wave ends at 1 s, during a pass, and the next pass starts a new note at 1,125 ms. A
    // track whose passes play notes isn't stuck. That pass is the first loop. The pass that starts the note again at
    // 2,250 ms is the second.
    CheckTiming(analyzer, 17, 2620, 2640, 10000);

    // The note runs out at 500 ms, and 500 ms of tail follow. Its track polls, but isn't settled while the note has
    // length left.
    CheckTiming(analyzer, 18, 990, 1015, 0);

    // Track 0's passes change its volume and play no notes. Track 0 keeps track 1 from being the main track all the
    // same. Otherwise the loops of both would count. Each would start the count again. Track 0's loops at 48 and 96
    // ticks are the sequence's loops.
    CheckTiming(analyzer, 19, 995, 1015, 10000);

    // The game stops a held note before its length runs out. Its track polls and is settled, and the note's wave of 0.1
    // s loops twice.
    CheckTiming(analyzer, 20, 200, 300, 10000);

    // The damper keeps the note on when its length runs out. Its track is settled, as in SEQ_HELD_POLL.
    CheckTiming(analyzer, 21, 200, 300, 10000);

    // Track 0 waits for ever. A track that waits for ever doesn't keep a later track from being the main track. Track
    // 1's loops at 24 and 48 ticks count.
    CheckTiming(analyzer, 22, 495, 515, 10000);
    CheckTiming(analyzer, 23, 495, 515, 10000);

    // Track 0's notes end at 1 s, and then track 0 ends. Until then it keeps track 1 from being the main track. Track
    // 1's loops at 120 and 144 ticks count.
    CheckTiming(analyzer, 24, 1495, 1515, 10000);

    // Track 0 doesn't wait for its endless note. So track 0 doesn't wait for ever. Its loops at 48 and 96 ticks are the
    // sequence's loops. SEQ_CONDUCTOR's loops work the same way.
    CheckTiming(analyzer, 25, 995, 1015, 10000);

    // No conditional command runs in the subroutine. So nothing can leave the subroutine's loop. The loop's jumps back
    // at 24 and 48 ticks count.
    CheckTiming(analyzer, 26, 495, 515, 10000);

    // The subroutine's passes run a conditional command, and don't count. The third note ends at 750 ms, and 500 ms of
    // tail follow.
    CheckTiming(analyzer, 27, 1235, 1285, 0);

    // Track 0's passes are idle. Track 1's first pass played its note and ends at 24 ticks. Its second pass plays no
    // note and only changes the volume of the note of length 0. Two loops of 24 ticks, then the fade.
    CheckTiming(analyzer, 28, 495, 515, 10000);

    // Track 1's passes back to the note play the note, and the inner loop's passes play none. Track 1's loops at 48 and
    // 96 ticks count.
    CheckTiming(analyzer, 29, 995, 1015, 10000);

    // A sound that isn't a sequence has no length.
    Check(!analyzer.Analyze(static_cast<uint32_t>(kSequences.size())).has_value(), "a sound past the last has none");

    CheckForeignArchives();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    return 0;
}
