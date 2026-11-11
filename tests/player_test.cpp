// SPDX-License-Identifier: MIT

// Tests the player and its playback (src/csf/player.* and xsflib's Playback) in archive mode, on sets made from a small
// archive. The output must be the model's output. A tagged length and fade must be kept. A set without a length must
// end half a second after its sound. A seek must land where a straight render does. The volume tag must apply. A group
// file outside the archive must be found. The effects that the descriptor lists for a sound must play. The variables
// that the csf_var tag sets must reach the sequence. Track volumes in a mini's descriptor must reach the sound's
// tracks. A set that can't play must report why. Game mode requires a game's code.

#include "csf/player.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/engine.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "csf/format.h"
#include "csf/ntdlib_cafe.h"
#include "csf/sar.h"
#include "rip/rip.h"

namespace fsar_files = ntdlib::testing::sar_files;

using xsflib::testing::Check;
using xsflib::testing::Peak;

namespace
{

// The archive's sounds: a note of 24 ticks that ends, and the same note played over and over.
constexpr uint32_t kNote = csf::fsar::ItemId(csf::fsar::kSoundItem, 0);
constexpr uint32_t kLoop = csf::fsar::ItemId(csf::fsar::kSoundItem, 1);

// The archive, and a group file holding its wave archive (file 2) when `grouped` leaves that file to the group.
struct TestArchive
{
    std::vector<uint8_t> archive;
    std::vector<uint8_t> group;
};

// A wave archive of one constant wave.
std::vector<uint8_t> Waves()
{
    return fsar_files::WaveArchive(true, {fsar_files::Wave(true, fsar_files::ConstantWave(true))});
}

// The spec of an archive whose sequence file, file 0, holds `bytecode`. File 1 is a bank of one region, and file 2 is
// the wave archive. With `grouped`, the archive leaves file 2 to a group outside it. The caller adds the sounds.
fsar_files::ArchiveSpec OneSequenceSpec(const std::vector<uint8_t>& bytecode, bool grouped)
{
    fsar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};
    spec.files = {{fsar_files::Sequence(true, bytecode, {{"Start", 0}}), "", false},
                  {fsar_files::Bank(true, fsar_files::OneRegionBank()), "", false},
                  {grouped ? std::vector<uint8_t>{} : Waves(), "", grouped}};
    if (grouped)
    {
        spec.groups = {{"GROUP_WAVES", 0xFFFFFFFF}};
    }

    return spec;
}

// With `sends`, each sound sets its track's send to aux A first (command 0xD9).
TestArchive MakeArchive(bool grouped, bool sends = false)
{
    // Timebase 48 and tempo 120: the note at offset 5 lasts 250 ms.
    const std::vector<uint8_t> kBytecode = {0xB0, 48,   0xE1, 0x00, 120, 60, 127,  24,   0xFF, 0xB0, 48,
                                            0xE1, 0x00, 120,  60,   127, 24, 0x89, 0x00, 0x00, 0x0E, 0xFF};
    const std::vector<uint8_t> kSendingBytecode = {0xD9, 127,  0xB0, 48,   0xE1, 0x00, 120,  60,   127,
                                                   24,   0xFF, 0xD9, 127,  0xB0, 48,   0xE1, 0x00, 120,
                                                   60,   127,  24,   0x89, 0x00, 0x00, 0x12, 0xFF};

    fsar_files::ArchiveSpec spec = OneSequenceSpec(sends ? kSendingBytecode : kBytecode, grouped);
    for (const uint32_t start : {0u, sends ? 11u : 9u})
    {
        fsar_files::SoundSpec sound;
        sound.name = start == 0 ? "SEQ_NOTE" : "SEQ_LOOP";
        sound.volume = 127;
        sound.banks = {0};
        sound.start_offset = start;
        spec.sounds.push_back(sound);
    }

    return {fsar_files::Archive(true, spec),
            grouped ? fsar_files::Group(true, {{2, Waves()}}) : std::vector<uint8_t>{}};
}

// An archive whose one sound plays the note only while variable 1, the player's, holds 4 and variable 17, a global
// one, holds 2. It plays nothing otherwise, as an effect does that waits for a value that the game sets. With
// `on_track`, the sound waits for track 0's variable 47 to hold 1 instead.
TestArchive MakeWaitingArchive(bool on_track = false)
{
    ntdlib::testing::length_cases::Code c(true);
    if (on_track)
    {
        c.CompareEqual(47, 1).If().Jump(16).End();
    }
    else
    {
        c.CompareEqual(1, 4).If().Jump(16).End();
        c.CompareEqual(17, 2).If().Jump(27).End();
    }

    c.Note(60, 127, 24).End();

    fsar_files::ArchiveSpec spec = OneSequenceSpec(c.Bytes(), false);
    fsar_files::SoundSpec sound;
    sound.name = "SEQ_WAITING";
    sound.volume = 127;
    sound.banks = {0};
    spec.sounds = {sound};

    return {fsar_files::Archive(true, spec), {}};
}

// The files of the sets under test, and a reader of them.
std::map<std::string, std::vector<uint8_t>> files;
const csf::FileReader kReader = xsflib::testing::MapReader(files);

// Writes a library holding `archive` (and its group file, if it has one) with `descriptor`, as the ripper does, and
// returns the library's path.
std::string WriteLibrary(const std::string& name, const TestArchive& archive, const csf::ArchiveDescriptor& descriptor)
{
    std::vector<csf::FileChunk> groups;
    if (!archive.group.empty())
    {
        groups.push_back({"content/sound/GROUP_WAVES.bfgrp", archive.group});
    }

    const std::string path = "sets/" + name + ".csflib";
    files[path] = ntdlib::rip::BuildArchiveLibrary<csf::rip::RipFormat>({"content/sound/test.bfsar", archive.archive},
                                                                        groups, descriptor, {});

    return path;
}

// Writes a .minicsf selecting `sound` from the library at `library`, with `tags`, and returns its path.
std::string WriteMini(const std::string& name, const std::string& library, uint32_t sound, csf::Tags tags)
{
    csf::ProgramBuilder b;
    b.AddSound(sound);
    tags["_lib"] = std::filesystem::path(library).filename().string();
    const std::string path = "sets/" + name + ".minicsf";
    files[path] = csf::WritePsf(csf::kPsfVersionCsf, {}, b.Data(), tags);

    return path;
}

// Writes a .minicsf like WriteMini's, with `descriptor` in its reserved area, and returns its path. The descriptor
// names the archive of a library that WriteLibrary wrote without a group file. The set loads it last. It therefore
// takes the place of the library's.
std::string WriteMiniWithDescriptor(const std::string& name, const std::string& library, uint32_t sound,
                                    csf::ArchiveDescriptor descriptor)
{
    csf::ProgramBuilder b;
    b.AddSound(sound);
    descriptor.archive_path = "content/sound/test.bfsar";
    const std::string path = "sets/" + name + ".minicsf";
    const csf::Tags tags = {{"_lib", std::filesystem::path(library).filename().string()}};
    files[path] = csf::WritePsf(csf::kPsfVersionCsf, descriptor.Serialize(), b.Data(), tags);

    return path;
}

// The whole of a playback, rendered in blocks of `block` frames, or nothing if it doesn't start.
std::vector<int16_t> RenderAll(const std::string& path, std::size_t block = 1000)
{
    return xsflib::testing::RenderAll<csf::Playback>(path, kReader, block);
}

// The model's samples for a sound, and the frame it went idle in (or 0 if it didn't). The model is idle once the sound
// has ended and its effects have stopped ringing.
struct ModelOutput
{
    std::vector<int16_t> samples;
    int idle_frame = 0;
};

// The model's output for `sound` with `options`, `frames` frames of it.
ModelOutput RunModel(const std::vector<uint8_t>& archive_bytes, uint32_t sound, int frames,
                     const csf::nwsnd::EngineOptions& options = {})
{
    const csf::fsar::SoundArchive archive = csf::fsar::SoundArchive::Load(archive_bytes, csf::kSarOptions);
    csf::nwsnd::ArchiveModel model(archive, options);
    model.engine->StartSequence(csf::fsar::ItemNumber(sound));

    ModelOutput out;
    for (int frame = 1; frame <= frames; ++frame)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = model.system.Output();
        out.samples.insert(out.samples.end(), output.begin(), output.end());
        if (out.idle_frame == 0 && !model.engine->IsBusy() && !model.engine->EffectsRinging())
        {
            out.idle_frame = frame;
        }
    }

    return out;
}

// Whether `samples` hold a render whose every sample is 0.
bool Silent(const std::vector<int16_t>& samples)
{
    return !samples.empty() && std::ranges::all_of(samples, [](int16_t sample) { return sample == 0; });
}

// What the cases share: the plain archive and its library, the model's output of its two sounds, the set that plays
// the loop with a tagged length and fade, rendered in blocks of 1,001 frames, and the set that plays the note without
// a length.
struct Fixture
{
    TestArchive plain;
    std::string library;
    ModelOutput loop;
    ModelOutput note;
    std::string tagged_path;
    std::vector<int16_t> tagged;
    std::vector<int16_t> untagged;
};

Fixture MakeFixture()
{
    Fixture fixture;
    fixture.plain = MakeArchive(false);
    fixture.library = WriteLibrary("plain", fixture.plain, {});
    fixture.loop = RunModel(fixture.plain.archive, kLoop, 600);
    fixture.note = RunModel(fixture.plain.archive, kNote, 400);
    fixture.tagged_path = WriteMini("tagged", fixture.library, kLoop, {{"length", "0:01"}, {"fade", "0.5"}});
    fixture.tagged = RenderAll(fixture.tagged_path, 1001);
    fixture.untagged = RenderAll(WriteMini("untagged", fixture.library, kNote, {}));

    return fixture;
}

void TestLengths(const Fixture& fixture)
{
    // A tagged length and fade at 48 kHz: 1 s, then a fade of half a second, rendered in blocks that don't divide a
    // frame.
    xsflib::testing::CheckLengthAndFade(fixture.tagged, fixture.loop.samples, 48000, 24000);

    // Without a length, the sound ends half a second after the model goes idle, in its frames of 144 samples.
    xsflib::testing::CheckEnd(fixture.untagged, fixture.note.samples,
                              static_cast<std::size_t>(fixture.note.idle_frame) * 144, 24000);
}

void TestSeeksAndVolume(const Fixture& fixture)
{
    // A seek forward and a seek back land where the straight render does.
    xsflib::testing::CheckSeeks<csf::Playback>(fixture.tagged_path, kReader, fixture.tagged, {30000, 10000});

    // The volume tag scales every sample.
    xsflib::testing::CheckVolume(RenderAll(WriteMini("quiet", fixture.library, kNote, {{"volume", "0.5"}})),
                                 fixture.untagged, 0.5);
}

void TestGroupFile(const Fixture& fixture)
{
    // A wave archive that only a group file outside the archive holds is found there.
    const TestArchive grouped = MakeArchive(true);
    const std::string grouped_library = WriteLibrary("grouped", grouped, {});

    Check(RenderAll(WriteMini("grouped", grouped_library, kNote, {})) == fixture.untagged,
          "a set whose waves are in a group file plays the same");
}

void TestEffects()
{
    // A delay on the TV's aux A, listed for the note. Both sounds send to aux A. The note plays with the delay's echo.
    // Without a length, the set ends half a second after the effects stop ringing. The loop plays without the delay.
    const TestArchive sending = MakeArchive(false, true);
    csf::ArchiveDescriptor delayed;
    const auto bits = [](float value)
    {
        return std::bit_cast<uint32_t>(value);
    };
    delayed.effects = {{csf::nwsnd::kEffectDeviceTv,
                        0,
                        csf::nwsnd::EffectType::kDelay,
                        {bits(30.0f), bits(0.5f), bits(1.0f), bits(50.0f), bits(1.0f)},
                        {csf::fsar::ItemNumber(kNote)}}};
    const std::string delayed_library = WriteLibrary("delayed", sending, delayed);
    const std::string sending_library = WriteLibrary("sending", sending, {});

    csf::nwsnd::EngineOptions options;
    options.effects = delayed.effects;
    const ModelOutput echoed = RunModel(sending.archive, kNote, 1000, options);
    const std::vector<int16_t> echoed_set = RenderAll(WriteMini("delayed", delayed_library, kNote, {}));

    xsflib::testing::CheckEnd(echoed_set, echoed.samples, static_cast<std::size_t>(echoed.idle_frame) * 144, 24000);
    Check(echoed_set != RenderAll(WriteMini("sending", sending_library, kNote, {})), "the delay's echo plays");
    Check(RenderAll(WriteMini("delayed loop", delayed_library, kLoop, {{"length", "0:01"}})) ==
              RenderAll(WriteMini("sending loop", sending_library, kLoop, {{"length", "0:01"}})),
          "a sound that no effect lists plays without effects");
}

void TestVariables(const Fixture& fixture)
{
    // The csf_var tag sets the sequence's variables before its first tick. The waiting sound plays the note only with
    // both of them. A tag with any part out of range sets none.
    const std::string waiting_library = WriteLibrary("waiting", MakeWaitingArchive(), {});

    Check(RenderAll(WriteMini("both", waiting_library, kNote, {{csf::kVariablesTag, "1=4, 17=2"}})) == fixture.untagged,
          "the variables that the tag sets let the sound play");
    Check(Silent(RenderAll(WriteMini("untagged waiting", waiting_library, kNote, {}))) &&
              Silent(RenderAll(WriteMini("one", waiting_library, kNote, {{csf::kVariablesTag, "1=4"}}))) &&
              Silent(RenderAll(WriteMini("out", waiting_library, kNote, {{csf::kVariablesTag, "1=4, 17=2, 32=0"}}))),
          "the sound waits without both variables");

    // "T:N=V" sets a track's variable.
    const std::string track_library = WriteLibrary("waiting on track", MakeWaitingArchive(true), {});

    Check(RenderAll(WriteMini("track", track_library, kNote, {{csf::kVariablesTag, "0:47=1"}})) == fixture.untagged,
          "track 0's variable that the tag sets lets the sound play");
    Check(Silent(RenderAll(WriteMini("untagged track", track_library, kNote, {}))) &&
              Silent(RenderAll(WriteMini("other track", track_library, kNote, {{csf::kVariablesTag, "1:47=1"}}))),
          "the sound waits without track 0's variable");
}

void TestTrackVolumes(const Fixture& fixture)
{
    // Track volumes in a mini's descriptor (docs/csf.md, "Track volumes"). The note plays on track 0. A volume of 0 for
    // track 0 silences it, and 0.5 halves its level, within 1% for the rounding of the voice's volume and of the
    // samples. The volumes of another sound leave the sound as it is.
    const auto with_volume = [&fixture](const std::string& name, uint32_t listed, float volume)
    {
        csf::SoundTrackVolumes entry;
        entry.sound = listed;
        entry.volumes[0] = volume;
        csf::ArchiveDescriptor descriptor;
        descriptor.flags = csf::ArchiveDescriptor::kFlagTrackVolumes;
        descriptor.track_volumes = {entry};

        return RenderAll(WriteMiniWithDescriptor(name, fixture.library, kNote, descriptor));
    };
    const std::vector<int16_t> half = with_volume("half", kNote, 0.5f);
    const int peak = Peak(fixture.untagged);

    Check(peak > 1000 && Silent(with_volume("silenced", kNote, 0.0f)), "a track volume of 0 silences the track");
    Check(half.size() == fixture.untagged.size() && std::abs(2 * Peak(half) - peak) * 100 <= peak,
          "a track volume of 0.5 halves the track's level: a peak of " + std::to_string(Peak(half)) + " for " +
              std::to_string(peak));
    Check(with_volume("other", kLoop, 0.0f) == fixture.untagged,
          "the track volumes of another sound leave the sound as it is");
}

void TestRefusals(const Fixture& fixture)
{
    // Sets that can't play: a group file the set doesn't hold, a sound the archive doesn't have, an item that isn't a
    // sound, a descriptor's voice count beyond AX's maximum, and a flag that version 1 doesn't have.
    csf::ArchiveDescriptor missing_group;
    missing_group.archive_path = "content/sound/test.bfsar";
    missing_group.group_paths = {"content/sound/GROUP_ELSEWHERE.bfgrp"};
    csf::ProgramBuilder b;
    b.AddFile(missing_group.archive_path, MakeArchive(true).archive);
    files["sets/missing.csflib"] = csf::WritePsf(csf::kPsfVersionCsf, missing_group.Serialize(), b.Data(), {});

    csf::ArchiveDescriptor many;
    many.voice_count = 97;
    const std::string many_library = WriteLibrary("many", fixture.plain, many);
    csf::ArchiveDescriptor flagged;
    flagged.flags = 2;
    const uint32_t absent = csf::fsar::ItemId(csf::fsar::kSoundItem, 5);
    const uint32_t wave_archive = csf::fsar::ItemId(csf::fsar::kWaveArchiveItem, 0);

    xsflib::testing::CheckRefusals<csf::Playback>(
        {{WriteMini("missing", "sets/missing.csflib", kNote, {}), "the set has no file content/sound/GROUP_ELSEWHERE"},
         {WriteMini("absent", fixture.library, absent, {}), "isn't a sequence"},
         {WriteMini("wave archive", fixture.library, wave_archive, {}), "isn't a sequence"},
         {WriteMini("many", many_library, kNote, {}), "voice count is 97"},
         {WriteMiniWithDescriptor("flagged", fixture.library, kNote, flagged), "version 1 has only bit 0"}},
        kReader);
}

} // namespace

int main()
{
    const auto tests = []
    {
        const Fixture fixture = MakeFixture();
        TestLengths(fixture);
        TestSeeksAndVolume(fixture);
        TestGroupFile(fixture);
        TestEffects();
        TestVariables(fixture);
        TestTrackVolumes(fixture);
        TestRefusals(fixture);
    };
    return xsflib::testing::RunTest("player", tests);
}
