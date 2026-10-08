// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on sequences that each take one of its paths. They are: a sequence that ends; one
// that loops by jumping back, and one that loops for ever; one that never sounds; held notes on a looping wave and on
// one that ends; a jump back into code the track hasn't run (not a loop); notes at a track volume and at a sound volume
// of 0 (which never sound either); and a track that loops every tick before another track's note starts. In the rest, a
// track polls or counts a variable once a tick, or marks beats for the game, while another track plays.

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

    // A sound that isn't a sequence has no length.
    Check(!analyzer.Analyze(static_cast<uint32_t>(kSequences.size())).has_value(), "a sound past the last has none");

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    return 0;
}
