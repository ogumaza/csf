// SPDX-License-Identifier: MIT

// Tests the player and its playback (src/csf/player.* and xsflib's Playback) in archive mode, on sets made from a small
// archive. The output must be the model's output. A tagged length and fade must be kept. A set without a length must
// end half a second after its sound. A seek must land where a straight render does. The volume tag must apply. A group
// file outside the archive must be found. A set that can't play must report why. Game mode requires a game's code.

#include "csf/player.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "csf/format.h"
#include "csf/sar.h"
#include "nwsnd/engine.h"
#include "rip/rip.h"

namespace fsar_files = ntdlib::testing::sar_files;

using xsflib::testing::Check;

namespace
{

// The archive's sounds: a note of 24 ticks that ends, and the same note played over and over.
constexpr uint32_t kNote = 0x01000000;
constexpr uint32_t kLoop = 0x01000001;

// The archive, and a group file holding its wave archive (file 2) when `grouped` leaves that file to the group.
struct TestArchive
{
    std::vector<uint8_t> archive;
    std::vector<uint8_t> group;
};

TestArchive MakeArchive(bool grouped)
{
    std::vector<uint8_t> samples(2 * 32000);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        samples[i] = 0x10; // 0x1000, big-endian
    }
    fsar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 32000;
    wave.channels = {samples};

    fsar_files::Region region;
    region.original_key = 60;
    region.adshr = {127, 127, 127, 0, 127};
    fsar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    fsar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    fsar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    // Timebase 48 and tempo 120: the note at offset 5 lasts 250 ms.
    const std::vector<uint8_t> kBytecode = {0xB0, 48,   0xE1, 0x00, 120, 60, 127,  24,   0xFF, 0xB0, 48,
                                            0xE1, 0x00, 120,  60,   127, 24, 0x89, 0x00, 0x00, 0x0E, 0xFF};

    fsar_files::ArchiveSpec spec;
    for (const uint32_t start : {0u, 9u})
    {
        fsar_files::SoundSpec sound;
        sound.name = start == 0 ? "SEQ_NOTE" : "SEQ_LOOP";
        sound.volume = 127;
        sound.banks = {0};
        sound.start_offset = start;
        spec.sounds.push_back(sound);
    }

    spec.banks = {{"BANK", 1}};
    spec.wave_archives = {{"WARC", 2}};
    spec.players = {{"PLAYER", 2}};
    const std::vector<uint8_t> waves = fsar_files::WaveArchive(true, {fsar_files::Wave(true, wave)});
    spec.files = {{fsar_files::Sequence(true, kBytecode, {{"Start", 0}}), "", false},
                  {fsar_files::Bank(true, bank), "", false},
                  {grouped ? std::vector<uint8_t>{} : waves, "", grouped}};
    if (grouped)
    {
        spec.groups = {{"GROUP_WAVES", 0xFFFFFFFF}};
    }

    return {fsar_files::Archive(true, spec), grouped ? fsar_files::Group(true, {{2, waves}}) : std::vector<uint8_t>{}};
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

// The whole of a playback, rendered in blocks of `block` frames, or nothing if it doesn't start.
std::vector<int16_t> RenderAll(const std::string& path, std::size_t block = 1000)
{
    return xsflib::testing::RenderAll<csf::Playback>(path, kReader, block);
}

// The model's output for `sound`, `frames` frames of it, and the frame it went idle in (or 0 if it didn't).
std::vector<int16_t> ModelOutput(const std::vector<uint8_t>& archive_bytes, uint32_t sound, int frames, int& idle)
{
    const csf::fsar::SoundArchive archive = csf::fsar::SoundArchive::Load(archive_bytes, csf::kSarOptions);
    csf::nwsnd::ArchiveModel model(archive);
    model.engine->StartSequence(sound & 0xFFFFFF);

    std::vector<int16_t> out;
    idle = 0;
    for (int frame = 1; frame <= frames; ++frame)
    {
        model.engine->RunFrame();
        const std::span<const int16_t> output = model.system.Output();
        out.insert(out.end(), output.begin(), output.end());
        if (idle == 0 && !model.engine->IsBusy())
        {
            idle = frame;
        }
    }

    return out;
}

} // namespace

int main()
{
    const TestArchive plain = MakeArchive(false);
    const std::string library = WriteLibrary("plain", plain, {});
    int idle = 0;
    const std::vector<int16_t> loop = ModelOutput(plain.archive, kLoop, 600, idle);
    const std::vector<int16_t> note = ModelOutput(plain.archive, kNote, 400, idle);

    // A tagged length and fade at 48 kHz: 1 s, then a fade of half a second, rendered in blocks that don't divide a
    // frame.
    const std::string tagged_path = WriteMini("tagged", library, kLoop, {{"length", "0:01"}, {"fade", "0.5"}});
    const std::vector<int16_t> tagged = RenderAll(tagged_path, 1001);
    xsflib::testing::CheckLengthAndFade(tagged, loop, 48000, 24000);

    // Without a length, the sound ends half a second after the model goes idle, in its frames of 144 samples.
    const std::vector<int16_t> untagged = RenderAll(WriteMini("untagged", library, kNote, {}));
    xsflib::testing::CheckEnd(untagged, note, static_cast<std::size_t>(idle) * 144, 24000);

    // A seek forward and a seek back land where the straight render does.
    xsflib::testing::CheckSeeks<csf::Playback>(tagged_path, kReader, tagged, {30000, 10000});

    // The volume tag scales every sample.
    xsflib::testing::CheckVolume(RenderAll(WriteMini("quiet", library, kNote, {{"volume", "0.5"}})), untagged, 0.5);

    // A wave archive that only a group file outside the archive holds is found there.
    const TestArchive grouped = MakeArchive(true);
    const std::string grouped_library = WriteLibrary("grouped", grouped, {});

    Check(RenderAll(WriteMini("grouped", grouped_library, kNote, {})) == untagged,
          "a set whose waves are in a group file plays the same");

    // Sets that can't play: a group file the set doesn't hold, a sound the archive doesn't have, an item that isn't a
    // sound, and a descriptor's voice count beyond AX's maximum.
    csf::ArchiveDescriptor missing_group;
    missing_group.archive_path = "content/sound/test.bfsar";
    missing_group.group_paths = {"content/sound/GROUP_ELSEWHERE.bfgrp"};
    csf::ProgramBuilder b;
    b.AddFile(missing_group.archive_path, grouped.archive);
    files["sets/missing.csflib"] = csf::WritePsf(csf::kPsfVersionCsf, missing_group.Serialize(), b.Data(), {});

    csf::ArchiveDescriptor many;
    many.voice_count = 97;
    const std::string many_library = WriteLibrary("many", plain, many);

    xsflib::testing::CheckRefusals<csf::Playback>(
        {{WriteMini("missing", "sets/missing.csflib", kNote, {}), "the set has no file content/sound/GROUP_ELSEWHERE"},
         {WriteMini("absent", library, 0x01000005, {}), "isn't a sequence"},
         {WriteMini("bank", library, 0x05000000, {}), "isn't a sequence"},
         {WriteMini("many", many_library, kNote, {}), "voice count is 97"}},
        kReader);

    return xsflib::testing::Summary("player");
}
