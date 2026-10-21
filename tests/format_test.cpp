// SPDX-License-Identifier: MIT

// The CSF file format code (src/csf/format.*): the PSF version byte, the process and archive descriptors, and how a
// set's files give them, its sound and its tags. xsflib's tests check the PSF container, the tags, the times, the
// streaming programs and the loading of a set's _lib chain.

#include "csf/format.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "csf/ntdlib_cafe.h"

using xsflib::testing::Check;
using xsflib::testing::LoadError;
using xsflib::testing::MapReader;

namespace
{

using csf::ArchiveDescriptor;
using csf::FileReader;
using csf::LoadedSet;
using csf::ProcessDescriptor;
using csf::ProgramBuilder;
using csf::SetFormat;
using csf::WritePsf;
using csf::nwsnd::EffectType;

constexpr uint8_t kVersion = csf::kPsfVersionCsf;

// A process descriptor that a version 1 player accepts.
ProcessDescriptor GameDescriptor()
{
    ProcessDescriptor d;
    d.executable_path = "code/game.rpx";
    d.archive_path = "content/sound/a.bfsar";
    for (uint32_t i = 0; i < csf::kProfileWords; ++i)
    {
        d.profile.push_back(0x02000000 + 4 * i);
    }

    return d;
}

// A CSF file is a PSF file with the version byte 0x28 (docs/csf.md, "Version byte").
void TestVersionByte()
{
    ProgramBuilder b;
    b.AddSound(0x01000001);
    const std::vector<uint8_t> file = WritePsf(kVersion, ArchiveDescriptor().Serialize(), b.Data(), {});

    XSFLIB_CHECK(file.size() > 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x28);
}

// A version 2 descriptor of driver 1, with flags 3 and output mode 2. A player refuses the flags and the output mode.
ProcessDescriptor DriverDescriptor()
{
    ProcessDescriptor d = GameDescriptor();
    d.version = ProcessDescriptor::kVersion2;
    d.flags = 3;
    d.output_mode = 2;
    d.driver = 1;

    return d;
}

// Each version reads back as it was written. Version 2 has a driver word after the output mode.
void TestProcessDescriptorRoundTrip()
{
    const ProcessDescriptor d = DriverDescriptor();

    const auto bytes = d.Serialize();
    const auto back = ProcessDescriptor::Parse(bytes);

    XSFLIB_CHECK(bytes.size() % 4 == 0);
    XSFLIB_CHECK(std::memcmp(bytes.data(), "CSFP", 4) == 0);
    XSFLIB_CHECK(back.has_value());
    XSFLIB_CHECK(back->version == 2 && back->flags == 3 && back->output_mode == 2 && back->driver == 1 &&
                 back->executable_path == d.executable_path && back->archive_path == d.archive_path &&
                 back->profile == d.profile);

    const ProcessDescriptor v1 = GameDescriptor();

    const auto v1_bytes = v1.Serialize();
    const auto v1_back = ProcessDescriptor::Parse(v1_bytes);

    XSFLIB_CHECK(v1_bytes.size() + 4 == bytes.size());
    XSFLIB_CHECK(v1_back && v1_back->version == 1 && v1_back->driver == 0 && v1_back->profile == v1.profile);
}

// Data without the magic, cut short, or with more profile words than it holds, isn't a descriptor. Another version
// gives only its header. Check refuses that header.
void TestProcessDescriptorParse()
{
    const auto bytes = DriverDescriptor().Serialize();

    XSFLIB_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(64, 0)).has_value());
    XSFLIB_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(bytes.begin(), bytes.end() - 4)).has_value());
    XSFLIB_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 20)).has_value());
    XSFLIB_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 18)).has_value());
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(bytes).has_value());

    ProcessDescriptor v3 = GameDescriptor();
    v3.version = 3;

    const auto v3_back = ProcessDescriptor::Parse(v3.Serialize());

    XSFLIB_CHECK(v3_back && v3_back->version == 3 && v3_back->profile.empty() && v3_back->Check().has_value());
}

// A player accepts versions 1 and 2, with no flags, in stereo. Version 1 takes a profile of 32 words, and version 2 a
// driver that the player has. The driver checks the profile.
void TestProcessDescriptorCheck()
{
    XSFLIB_CHECK(!GameDescriptor().Check());
    XSFLIB_CHECK(DriverDescriptor().Check().has_value());

    ProcessDescriptor flags = GameDescriptor();
    flags.flags = 1;
    ProcessDescriptor mono = GameDescriptor();
    mono.output_mode = 0;
    ProcessDescriptor short_profile = GameDescriptor();
    short_profile.profile.pop_back();

    XSFLIB_CHECK(flags.Check().has_value());
    XSFLIB_CHECK(mono.Check().has_value());
    XSFLIB_CHECK(short_profile.Check().has_value());

    ProcessDescriptor v2 = GameDescriptor();
    v2.version = ProcessDescriptor::kVersion2;
    v2.driver = 2;
    v2.profile.pop_back();
    ProcessDescriptor v2_flags = v2;
    v2_flags.flags = 1;
    ProcessDescriptor v2_mono = v2;
    v2_mono.output_mode = 0;
    ProcessDescriptor v2_driver = v2;
    v2_driver.driver = csf::kGameDriverKinds;

    XSFLIB_CHECK(!v2.Check());
    XSFLIB_CHECK(v2_flags.Check().has_value());
    XSFLIB_CHECK(v2_mono.Check().has_value());
    XSFLIB_CHECK(v2_driver.Check().has_value());
}

void TestArchiveDescriptor()
{
    ArchiveDescriptor d;
    d.version = 2;
    d.flags = 1;
    d.output_mode = 2;
    d.voice_count = 78;
    d.ax_settings = csf::kAxRenderer48k;
    d.sdk_version = 21213;
    d.archive_path = "content/sound/b.bfsar";
    d.group_paths = {"content/sound/groups/GROUP_A.bfgrp", "content/sound/groups/GROUP_BB.bfgrp"};

    const auto raw = d.Serialize();
    const auto back = ArchiveDescriptor::Parse(raw);

    XSFLIB_CHECK(raw.size() % 4 == 0);
    XSFLIB_CHECK(std::memcmp(raw.data(), "CSFA", 4) == 0);
    XSFLIB_CHECK(back && back->version == d.version && back->flags == d.flags && back->output_mode == d.output_mode &&
                 back->voice_count == d.voice_count && back->ax_settings == d.ax_settings &&
                 back->sdk_version == d.sdk_version && back->archive_path == d.archive_path &&
                 back->group_paths == d.group_paths);
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 40)).has_value());
    XSFLIB_CHECK(!ProcessDescriptor::Parse(raw).has_value());

    // A player only accepts version 1, with no flags, in stereo, with 1 to 96 voices and AX settings that it supports.
    XSFLIB_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = 0;
    v1.output_mode = csf::kOutputStereo;

    XSFLIB_CHECK(!v1.Check());
    XSFLIB_CHECK(!ArchiveDescriptor{}.Check());

    for (const uint32_t voices : {0u, 97u})
    {
        ArchiveDescriptor odd = v1;
        odd.voice_count = voices;
        XSFLIB_CHECK(odd.Check().has_value());
    }

    ArchiveDescriptor settings = v1;
    settings.ax_settings = 4;

    XSFLIB_CHECK(settings.Check().has_value());
}

// The effects after the archive descriptor's paths (docs/csf.md, "Archive descriptor"). A descriptor without effects
// ends after the paths. Parse leaves out an effect of a type that it doesn't know, and one with fewer words than its
// type has.
void TestArchiveDescriptorEffects()
{
    ArchiveDescriptor d;
    d.archive_path = "content/sound/c.bfsar";
    const auto plain = d.Serialize();

    d.effects = {
        {csf::nwsnd::kEffectDeviceTv, 0, EffectType::kDelay, {1, 2, 3, 4, 5}, {}},
        {csf::nwsnd::kEffectDeviceDrc, 2, EffectType::kReverbStd, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, {7, 9}},
    };
    const auto raw = d.Serialize();

    // The count, then each effect: device, bus, type and word count, a byte each, the sound count, the words and the
    // sounds.
    XSFLIB_CHECK(raw.size() == plain.size() + 4 + (8 + 5 * 4) + (8 + 10 * 4 + 2 * 4));
    XSFLIB_CHECK(std::equal(plain.begin(), plain.end(), raw.begin()));
    XSFLIB_CHECK(raw[plain.size()] == 2 && raw[plain.size() + 4] == 0 && raw[plain.size() + 6] == 4 &&
                 raw[plain.size() + 7] == 5);

    const auto back = ArchiveDescriptor::Parse(raw);
    XSFLIB_CHECK(back && back->effects == d.effects && back->archive_path == d.archive_path && !back->Check());

    const auto none = ArchiveDescriptor::Parse(plain);
    XSFLIB_CHECK(none && none->effects.empty());
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.end() - 4)).has_value());
    XSFLIB_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + plain.size() + 2)));

    ArchiveDescriptor odd = d;
    odd.effects = {
        {0, 0, static_cast<EffectType>(9), {1, 2}, {3}},
        {0, 1, EffectType::kDelay, {1, 2, 3, 4}, {}},
        {0, 1, EffectType::kChorus, {1, 2, 3, 4, 5, 6}, {}},
    };
    const auto kept = ArchiveDescriptor::Parse(odd.Serialize());
    XSFLIB_CHECK(kept && kept->effects.size() == 1 && kept->effects[0] == odd.effects[2]);

    // A player refuses an effect on a device or a bus that AX doesn't have.
    ArchiveDescriptor bus = d;
    bus.effects[0].bus = 3;
    XSFLIB_CHECK(bus.Check().has_value());

    ArchiveDescriptor device = d;
    device.effects[1].device = 2;
    XSFLIB_CHECK(device.Check().has_value());
}

void TestLoadSet()
{
    // dir/lib.csflib holds the descriptor and the base files. dir/sub/x.minicsf loads it with _lib=../lib.csflib, and
    // loads extra.minicsf from the same directory with _lib2.
    std::map<std::string, std::vector<uint8_t>> fs;
    const ArchiveDescriptor archive;
    {
        ProgramBuilder b;
        b.AddFile("f", std::vector<uint8_t>{1});
        b.AddSound(0x01000001);
        fs["dir/lib.csflib"] = WritePsf(kVersion, archive.Serialize(), b.Data(), {{"game", "G"}});
    }
    {
        ProgramBuilder b;
        b.AddFile("f", std::vector<uint8_t>{2});
        b.AddSound(0x01000002);
        fs["dir/sub/x.minicsf"] =
            WritePsf(kVersion, {}, b.Data(),
                     {{"_lib", "../lib.csflib"}, {"_lib2", "extra.minicsf"}, {"title", "X"}, {"length", "1:00"}});
    }
    {
        ProgramBuilder b;
        b.AddSound(0x01000003);
        fs["dir/sub/extra.minicsf"] = WritePsf(kVersion, {}, b.Data(), {{"title", "extra"}});
    }
    const FileReader reader = MapReader(fs);

    LoadedSet set;
    const auto err = csf::LoadSet("dir/sub/x.minicsf", reader, set);

    if (!Check(!err && set.sources.size() == 3, "x.minicsf loads: " + (err ? *err : std::string("wrong result"))))
    {
        return;
    }

    XSFLIB_CHECK(set.sources[0] == "dir/sub/../lib.csflib");
    XSFLIB_CHECK(set.sources[1] == "dir/sub/x.minicsf");
    XSFLIB_CHECK(set.sources[2] == "dir/sub/extra.minicsf");
    XSFLIB_CHECK(set.files.size() == 2 && set.File("f") && *set.File("f") == std::vector<uint8_t>{2});
    XSFLIB_CHECK(!set.File("g"));
    XSFLIB_CHECK(set.archive.has_value() && !set.descriptor.has_value());
    XSFLIB_CHECK(set.sound == 0x01000003u);
    XSFLIB_CHECK(set.tags["title"] == "X" && set.tags["length"] == "1:00" && set.tags.count("game") == 0);

    // A later descriptor of the other kind determines the mode.
    fs["dir/game.minicsf"] = WritePsf(kVersion, GameDescriptor().Serialize(), {}, {{"_lib", "lib.csflib"}});
    LoadedSet game;

    XSFLIB_CHECK(!csf::LoadSet("dir/game.minicsf", reader, game));
    XSFLIB_CHECK(game.descriptor.has_value() && !game.archive.has_value());

    // A wrong PSF version, a set without a descriptor and one without a sound are errors.
    fs["dir/z.miniwsf"] = WritePsf(0x27, {}, {}, {{"_lib", "lib.csflib"}});
    fs["dir/w.minicsf"] = WritePsf(kVersion, {}, {}, {{"title", "w"}});
    fs["dir/nosound.csflib"] = WritePsf(kVersion, archive.Serialize(), {}, {});

    XSFLIB_CHECK(LoadError<SetFormat>("dir/z.miniwsf", reader).find("not a CSF file") != std::string::npos);
    XSFLIB_CHECK(LoadError<SetFormat>("dir/w.minicsf", reader).starts_with("no process or archive descriptor"));
    XSFLIB_CHECK(LoadError<SetFormat>("dir/nosound.csflib", reader).starts_with("no sound selected"));

    // So is a descriptor that this player can't play.
    ProcessDescriptor v3 = GameDescriptor();
    v3.version = 3;
    fs["dir/v3.csflib"] = WritePsf(kVersion, v3.Serialize(), {}, {});

    XSFLIB_CHECK(LoadError<SetFormat>("dir/v3.csflib", reader).find("version 3") != std::string::npos);
}

// The waves file: none without it, a group per little-endian word, and an error for a size that isn't a multiple of 4.
void TestWaveGroups()
{
    LoadedSet set;
    const auto none = csf::WaveGroups(set);
    XSFLIB_CHECK(none && none->empty());

    set.files.push_back({csf::kWavesPath, {0x05, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00}});
    const auto two = csf::WaveGroups(set);
    XSFLIB_CHECK(two && *two == std::vector<uint32_t>({0x00010005, 0x00000002}));

    set.files.push_back({csf::kWavesPath, {0x05, 0x00, 0x01, 0x00, 0x02, 0x00}});
    XSFLIB_CHECK(!csf::WaveGroups(set));
}

// The files that game mode gives the game: those under content/, by their paths under it, a later one replacing an
// earlier one.
void TestContentFiles()
{
    const std::vector<csf::FileChunk> files = {{"content/sound/a.bfsar", {1}},
                                               {"code/game.rpx", {2}},
                                               {csf::kWavesPath, {3}},
                                               {"content/sound/a.bfsar", {4}},
                                               {"content/b.bin", {5}}};

    const std::map<std::string, std::vector<uint8_t>> content = csf::ContentFiles(files);

    XSFLIB_CHECK(content.size() == 2 && content.at("sound/a.bfsar") == std::vector<uint8_t>{4} &&
                 content.at("b.bin") == std::vector<uint8_t>{5});
    XSFLIB_CHECK(csf::ContentPath("content/sound/a.bfsar") == "sound/a.bfsar");
    XSFLIB_CHECK(!csf::ContentPath("code/game.rpx") && !csf::ContentPath("contents/x") && !csf::ContentPath("content"));
}

} // namespace

int main()
{
    TestVersionByte();
    TestProcessDescriptorRoundTrip();
    TestProcessDescriptorParse();
    TestProcessDescriptorCheck();
    TestArchiveDescriptor();
    TestArchiveDescriptorEffects();
    TestLoadSet();
    TestWaveGroups();
    TestContentFiles();

    return xsflib::testing::Summary("format");
}
