// SPDX-License-Identifier: MIT

// The CSF file format code (src/csf/format.*): the PSF container, tags, times, program chunks, the process and archive
// descriptors, and _lib loading.

#include "csf/format.h"

#include <zlib.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "common/bytes.h"

namespace
{

using csf::ArchiveDescriptor;
using csf::FileReader;
using csf::LoadedSet;
using csf::ProcessDescriptor;
using csf::ProgramBuilder;
using csf::ProgramChunks;
using csf::PsfFile;
using csf::Tags;
using csf::WritePsf;

constexpr uint8_t kVersion = csf::kPsfVersionCsf;

// A reader of the files in `fs`, by their paths made normal.
FileReader MapReader(const std::map<std::string, std::vector<uint8_t>>& fs)
{
    return [&fs](const std::string& path, std::vector<uint8_t>& out)
    {
        const auto it = fs.find(std::filesystem::path(path).lexically_normal().generic_string());
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };
}

// The error LoadSet gives for `path`, or an empty string if the set loads.
std::string LoadError(const std::string& path, const FileReader& reader)
{
    LoadedSet set;
    const auto err = csf::LoadSet(path, reader, set);

    return err ? *err : std::string();
}

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

void TestTimes()
{
    CSF_CHECK(csf::ParseTime("") == -1);
    CSF_CHECK(csf::ParseTime("abc") == -1);
    CSF_CHECK(csf::ParseTime("5") == 5000);
    CSF_CHECK(csf::ParseTime("1.5") == 1500);
    CSF_CHECK(csf::ParseTime("1,25") == 1250);
    CSF_CHECK(csf::ParseTime("2:03") == 123000);
    CSF_CHECK(csf::ParseTime("2:03.5") == 123500);
    CSF_CHECK(csf::ParseTime("1:00:00") == 3600000);
    CSF_CHECK(csf::ParseTime(" 0:10.000 ") == 10000);
    CSF_CHECK(csf::ParseTime("1:2:3:4") == -1);
    CSF_CHECK(csf::ParseTime("-1") == -1);
    CSF_CHECK(csf::ParseTime("nan") == -1);
    CSF_CHECK(csf::ParseTime("inf") == -1);
    CSF_CHECK(csf::ParseTime("1e300") == -1);
    CSF_CHECK(csf::ParseTime("99999999999999999999:00") == -1);
    CSF_CHECK(csf::ParseTime("0x10") == -1);

    CSF_CHECK(csf::FormatTime(0) == "0:00.000");
    CSF_CHECK(csf::FormatTime(65998) == "1:05.998");
    CSF_CHECK(csf::FormatTime(600000) == "10:00.000");

    for (const long long ms : {0LL, 1LL, 999LL, 59999LL, 61001LL, 3599999LL})
    {
        CSF_CHECK(csf::ParseTime(csf::FormatTime(ms)) == ms);
    }
}

// Numbers in tags: an optional sign, digits with '.' or ',' before decimals, and an optional exponent, with the values
// that std::strtod gives in the C locale. Text after the number is left unread.
void TestDecimals()
{
    for (const char* text : {"0", "0.5", "1,25", "-1.5", "+2", ".5", "5.", "123.456", "0.000123", "2.718281828459045",
                             "1e3", "1.5E-3", "1e22", "1e-22", "9007199254740992", "0.1", "0.3", "62.226"})
    {
        std::string c_text = text;
        std::replace(c_text.begin(), c_text.end(), ',', '.');

        const auto number = csf::ReadDecimal(text);

        Check(number && number->length == std::strlen(text) && number->value == std::strtod(c_text.c_str(), nullptr),
              std::string(text) + " reads as std::strtod reads it");
    }

    const auto with_unit = csf::ReadDecimal("0.5dB");
    const auto bare_exponent = csf::ReadDecimal("1e+x");
    const auto hex = csf::ReadDecimal("0x10");
    const auto huge = csf::ReadDecimal("1e999");
    const auto tiny = csf::ReadDecimal("1e-999");

    CSF_CHECK(with_unit && with_unit->value == 0.5 && with_unit->length == 3);
    CSF_CHECK(bare_exponent && bare_exponent->value == 1 && bare_exponent->length == 1);
    CSF_CHECK(hex && hex->value == 0 && hex->length == 1);
    CSF_CHECK(huge && std::isinf(huge->value));
    CSF_CHECK(tiny && tiny->value == 0);
    for (const char* text : {"", ".", "-", "+.", "e5", " 1", "nan", "inf", "x1"})
    {
        Check(!csf::ReadDecimal(text), std::string("\"") + text + "\" isn't a number");
    }
}

// Under a locale that uses a comma for decimals, std::strtod stops at '.'. CSF still reads tags the same way. The check
// runs only where such a locale is installed.
void TestDecimalsUnderCommaLocale()
{
    const char* comma_locale = nullptr;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "de-DE", "German_Germany.1252"})
    {
        if (std::setlocale(LC_NUMERIC, name) && std::strtod("0.5", nullptr) == 0)
        {
            comma_locale = name;
            break;
        }
    }

    if (!comma_locale)
    {
        std::setlocale(LC_NUMERIC, "C");
        std::printf("no locale with decimal commas is installed: tags weren't read under one\n");
        return;
    }

    const auto volume = csf::ReadDecimal("0.5");
    const long long length = csf::ParseTime("1:02.226");
    std::setlocale(LC_NUMERIC, "C");

    Check(volume && volume->value == 0.5, std::string("a volume of 0.5 reads as 0.5 under ") + comma_locale);
    Check(length == 62226, std::string("a length of 1:02.226 reads as 62,226 ms under ") + comma_locale);
}

void TestPsfRoundTrip()
{
    const std::vector<uint8_t> reserved = {1, 2, 3, 4, 5};
    const Tags tags = {{"title", "Test"}, {"comment", "line one\nline two"}, {"_lib", "x.csflib"}};
    std::vector<uint8_t> program(100000);
    for (std::size_t i = 0; i < program.size(); i++)
    {
        program[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    }

    const auto file = WritePsf(kVersion, reserved, program, tags);
    PsfFile psf;
    const auto err = csf::ParsePsf(file, psf);

    CSF_CHECK(file.size() > 16 && std::memcmp(file.data(), "PSF", 3) == 0 && file[3] == 0x28);
    CSF_CHECK(!err);
    CSF_CHECK(psf.version == kVersion);
    CSF_CHECK(psf.reserved == reserved);
    CSF_CHECK(psf.program == program);
    CSF_CHECK(psf.tags == tags);
    CSF_CHECK(csf::TagAreaOffset(file) == 16 + reserved.size() + csf::LoadLe32(&file[8]));

    // Corruption of the compressed program is detected by the CRC.
    auto bad = file;
    bad[16 + reserved.size() + 10] ^= 0x55;

    CSF_CHECK(csf::ParsePsf(bad, psf) == "program CRC mismatch");

    // Truncation.
    auto cut = file;
    cut.resize(20);

    CSF_CHECK(csf::ParsePsf(cut, psf) == "truncated PSF file");
    CSF_CHECK(csf::ParsePsf(std::vector<uint8_t>{'P', 'S'}, psf) == "not a PSF file");

    // A program whose zlib stream stops short, with a size and CRC that match what's left of it.
    const uint32_t half = csf::LoadLe32(&file[8]) / 2;
    std::vector<uint8_t> short_stream(file.begin(),
                                      file.begin() + 16 + static_cast<std::ptrdiff_t>(reserved.size()) + half);
    csf::StoreLe32(&short_stream[8], half);
    csf::StoreLe32(&short_stream[12], static_cast<uint32_t>(crc32(0, &short_stream[16 + reserved.size()], half)));

    CSF_CHECK(csf::ParsePsf(short_stream, psf) == "truncated program data");
}

void TestTagParsing()
{
    const std::string header = std::string("PSF") + static_cast<char>(kVersion) + std::string(12, '\0');
    const std::string tagged = header +
                               "[TAG]Title = Hello \nTITLE2=x\ncomment=a\ncomment=b\nartist=me\ncomment=c\n"
                               "no equals sign\n=empty name\nlength=1:00\r\n";
    std::vector<uint8_t> data(tagged.begin(), tagged.end());
    PsfFile psf;

    CSF_CHECK(!csf::ParsePsf(data, psf));
    CSF_CHECK(psf.tags["title"] == "Hello");
    CSF_CHECK(psf.tags["title2"] == "x");
    CSF_CHECK(psf.tags["comment"] == "c"); // a and b join, and the later c replaces them
    CSF_CHECK(psf.tags["artist"] == "me");
    CSF_CHECK(psf.tags["length"] == "1:00");
    CSF_CHECK(psf.tags.count("") == 0);

    // Consecutive lines with the same name join.
    const std::string repeated = header + "[TAG]comment=a\ncomment=b\n";
    std::vector<uint8_t> data2(repeated.begin(), repeated.end());

    CSF_CHECK(!csf::ParsePsf(data2, psf));
    CSF_CHECK(psf.tags["comment"] == "a\nb");
}

void TestProcessDescriptor()
{
    ProcessDescriptor d = GameDescriptor();
    d.version = 2;
    d.flags = 3;
    d.output_mode = 2;

    const auto bytes = d.Serialize();
    const auto back = ProcessDescriptor::Parse(bytes);

    CSF_CHECK(bytes.size() % 4 == 0);
    CSF_CHECK(std::memcmp(bytes.data(), "CSFP", 4) == 0);
    CSF_CHECK(back.has_value());
    CSF_CHECK(back->version == 2 && back->flags == 3 && back->output_mode == 2 &&
              back->executable_path == d.executable_path && back->archive_path == d.archive_path &&
              back->profile == d.profile);

    // Data without the magic, cut short, or with more profile words than it holds, isn't a descriptor.
    CSF_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(64, 0)).has_value());
    CSF_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(bytes.begin(), bytes.end() - 4)).has_value());
    CSF_CHECK(!ProcessDescriptor::Parse(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 20)).has_value());
    CSF_CHECK(!ArchiveDescriptor::Parse(bytes).has_value());

    // A player only accepts version 1, with no flags, in stereo, with a profile of 32 words.
    CSF_CHECK(!GameDescriptor().Check());
    CSF_CHECK(d.Check().has_value());

    ProcessDescriptor flags = GameDescriptor();
    flags.flags = 1;
    ProcessDescriptor mono = GameDescriptor();
    mono.output_mode = 0;
    ProcessDescriptor short_profile = GameDescriptor();
    short_profile.profile.pop_back();

    CSF_CHECK(flags.Check().has_value());
    CSF_CHECK(mono.Check().has_value());
    CSF_CHECK(short_profile.Check().has_value());
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

    CSF_CHECK(raw.size() % 4 == 0);
    CSF_CHECK(std::memcmp(raw.data(), "CSFA", 4) == 0);
    CSF_CHECK(back && back->version == d.version && back->flags == d.flags && back->output_mode == d.output_mode &&
              back->voice_count == d.voice_count && back->ax_settings == d.ax_settings &&
              back->sdk_version == d.sdk_version && back->archive_path == d.archive_path &&
              back->group_paths == d.group_paths);
    CSF_CHECK(!ArchiveDescriptor::Parse(std::vector<uint8_t>(raw.begin(), raw.begin() + 40)).has_value());
    CSF_CHECK(!ProcessDescriptor::Parse(raw).has_value());

    // A player only accepts version 1, with no flags, in stereo, with 1 to 96 voices and AX settings that it supports.
    CSF_CHECK(d.Check().has_value());

    ArchiveDescriptor v1 = d;
    v1.version = 1;
    v1.flags = 0;
    v1.output_mode = csf::kOutputStereo;

    CSF_CHECK(!v1.Check());
    CSF_CHECK(!ArchiveDescriptor{}.Check());

    for (const uint32_t voices : {0u, 97u})
    {
        ArchiveDescriptor odd = v1;
        odd.voice_count = voices;
        CSF_CHECK(odd.Check().has_value());
    }

    ArchiveDescriptor settings = v1;
    settings.ax_settings = 4;

    CSF_CHECK(settings.Check().has_value());
}

void TestChunks()
{
    ProgramBuilder b;
    b.AddFile("content/sound/a.bfsar", std::vector<uint8_t>{9, 8, 7, 6, 5});
    b.AddSound(0x01000005);
    b.AddFile("code/game.rpx", std::vector<uint8_t>{1});

    ProgramChunks chunks;
    const auto err = csf::ParseProgram(b.Data(), chunks);

    CSF_CHECK(b.Data().size() % 4 == 0);
    CSF_CHECK(!err);
    CSF_CHECK(chunks.files.size() == 2 && chunks.sound == 0x01000005u);
    CSF_CHECK(chunks.files[0].path == "content/sound/a.bfsar" &&
              chunks.files[0].data == (std::vector<uint8_t>{9, 8, 7, 6, 5}));
    CSF_CHECK(chunks.files[1].path == "code/game.rpx" && chunks.files[1].data == std::vector<uint8_t>{1});

    // Unknown chunks are skipped.
    std::vector<uint8_t> prog = {'X', 'X', 'X', 'X', 4, 0, 0, 0, 1, 2, 3, 4};
    const std::vector<uint8_t> data = b.Data();
    prog.insert(prog.end(), data.begin(), data.end());
    ProgramChunks skipped;

    CSF_CHECK(!csf::ParseProgram(prog, skipped));
    CSF_CHECK(skipped.files.size() == 2 && skipped.sound.has_value());

    // A truncated chunk is an error, even when its size is so large that adding it to the position wraps around on a
    // 32-bit host. A FILE chunk whose path runs past its end is an error too, and so is an SND chunk without its word.
    const std::vector<uint8_t> trunc = {'F', 'I', 'L', 'E', 100, 0, 0, 0, 1, 2};
    const std::vector<uint8_t> huge = {'F', 'I', 'L', 'E', 0xf8, 0xff, 0xff, 0xff, 1, 2, 3, 4};
    const std::vector<uint8_t> long_path = {'F', 'I', 'L', 'E', 4, 0, 0, 0, 9, 0, 'a', 'b'};
    const std::vector<uint8_t> short_sound = {'S', 'N', 'D', ' ', 2, 0, 0, 0, 1, 2, 0, 0};
    ProgramChunks bad;

    CSF_CHECK(csf::ParseProgram(trunc, bad).has_value());
    CSF_CHECK(csf::ParseProgram(huge, bad).has_value());
    CSF_CHECK(csf::ParseProgram(long_path, bad).has_value());
    CSF_CHECK(csf::ParseProgram(short_sound, bad).has_value());
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
    CSF_CHECK(set.sources[0] == "dir/sub/../lib.csflib");
    CSF_CHECK(set.sources[1] == "dir/sub/x.minicsf");
    CSF_CHECK(set.sources[2] == "dir/sub/extra.minicsf");
    CSF_CHECK(set.files.size() == 2 && set.File("f") && *set.File("f") == std::vector<uint8_t>{2});
    CSF_CHECK(!set.File("g"));
    CSF_CHECK(set.archive.has_value() && !set.process.has_value());
    CSF_CHECK(set.sound == 0x01000003u);
    CSF_CHECK(set.tags["title"] == "X" && set.tags["length"] == "1:00" && set.tags.count("game") == 0);

    // An empty _lib2 names no library, like an empty _lib, and _lib3 still loads.
    fs["dir/sub/v.minicsf"] =
        WritePsf(kVersion, {}, {}, {{"_lib", "../lib.csflib"}, {"_lib2", ""}, {"_lib3", "extra.minicsf"}});
    LoadedSet gap;

    CSF_CHECK(!csf::LoadSet("dir/sub/v.minicsf", reader, gap));
    CSF_CHECK(gap.sources.size() == 3 && gap.sources[2] == "dir/sub/extra.minicsf");

    // A later descriptor of the other kind determines the mode.
    fs["dir/game.minicsf"] = WritePsf(kVersion, GameDescriptor().Serialize(), {}, {{"_lib", "lib.csflib"}});
    LoadedSet game;

    CSF_CHECK(!csf::LoadSet("dir/game.minicsf", reader, game));
    CSF_CHECK(game.process.has_value() && !game.archive.has_value());

    // A missing library, a wrong PSF version, a set without a descriptor and one without a sound are errors.
    fs["dir/y.minicsf"] = WritePsf(kVersion, {}, {}, {{"_lib", "missing.csflib"}});
    fs["dir/z.miniwsf"] = WritePsf(0x27, {}, {}, {{"_lib", "lib.csflib"}});
    fs["dir/w.minicsf"] = WritePsf(kVersion, {}, {}, {{"title", "w"}});
    fs["dir/nosound.csflib"] = WritePsf(kVersion, archive.Serialize(), {}, {});

    CSF_CHECK(LoadError("dir/y.minicsf", reader) == "can't read dir/missing.csflib");
    CSF_CHECK(LoadError("dir/z.miniwsf", reader).find("not a CSF file") != std::string::npos);
    CSF_CHECK(LoadError("dir/w.minicsf", reader).starts_with("no process or archive descriptor"));
    CSF_CHECK(LoadError("dir/nosound.csflib", reader).starts_with("no sound selected"));

    // So is a descriptor a version 1 player can't play.
    ProcessDescriptor v2 = GameDescriptor();
    v2.version = 2;
    fs["dir/v2.csflib"] = WritePsf(kVersion, v2.Serialize(), {}, {});

    CSF_CHECK(LoadError("dir/v2.csflib", reader).find("version 2") != std::string::npos);

    // A _lib cycle ends with an error instead of recursing forever.
    fs["dir/c1.minicsf"] = WritePsf(kVersion, {}, {}, {{"_lib", "c2.minicsf"}});
    fs["dir/c2.minicsf"] = WritePsf(kVersion, {}, {}, {{"_lib", "c1.minicsf"}});

    CSF_CHECK(LoadError("dir/c1.minicsf", reader) == "_lib nesting too deep");

    // So does a set that names more files than a set can have. Each of these names the next one ten times. Loading them
    // all would take 10^10 loads.
    for (int level = 0; level < 10; level++)
    {
        Tags tags;
        for (int n = 2; n <= 11; n++)
        {
            tags["_lib" + std::to_string(n)] = "f" + std::to_string(level + 1) + ".minicsf";
        }
        fs["dir/f" + std::to_string(level) + ".minicsf"] = WritePsf(kVersion, {}, {}, tags);
    }
    fs["dir/f10.minicsf"] = WritePsf(kVersion, {}, {}, {});

    CSF_CHECK(LoadError("dir/f0.minicsf", reader) == "more than 64 files in the set");
}

void TestLoadSetInArchive()
{
    // foobar2000's path for a file in an archive: unpack://zip|<length>|<the archive's path>|<its path in the archive>.
    const std::string archive = "unpack://zip|23|file://C:\\music\\set.zip|";
    std::map<std::string, std::vector<uint8_t>> fs;
    ProgramBuilder b;
    b.AddSound(0x01000000);
    fs[archive + "set.csflib"] = WritePsf(kVersion, ArchiveDescriptor{}.Serialize(), b.Data(), {});
    fs[archive + "x.minicsf"] = WritePsf(kVersion, {}, {}, {{"_lib", "set.csflib"}});
    const FileReader reader = [&fs](const std::string& path, std::vector<uint8_t>& out)
    {
        const auto it = fs.find(path);
        if (it == fs.end())
        {
            return false;
        }

        out = it->second;

        return true;
    };

    LoadedSet set;
    const auto err = csf::LoadSet(archive + "x.minicsf", reader, set, "/\\|");
    const std::string without = LoadError(archive + "x.minicsf", reader);

    // With | as a separator, the library is found in the archive, and without it, beside the archive.
    CSF_CHECK(!err && set.sources.size() == 2 && set.sources[0] == archive + "set.csflib");
    CSF_CHECK(without == "can't read unpack://zip|23|file://C:\\music\\set.csflib");
}

// A PSF file as one call of zlib's compress2 makes it. WritePsf compresses a program in pieces. The test checks that
// WritePsf gives the same bytes.
std::vector<uint8_t> OneCallPsf(const std::vector<uint8_t>& reserved, const std::vector<uint8_t>& program,
                                const Tags& tags)
{
    std::vector<uint8_t> compressed;
    uLongf size = 0;
    if (!program.empty())
    {
        size = compressBound(static_cast<uLong>(program.size()));
        compressed.resize(size);
        compress2(compressed.data(), &size, program.data(), static_cast<uLong>(program.size()), Z_BEST_COMPRESSION);
        compressed.resize(size);
    }

    std::vector<uint8_t> out(16);
    std::memcpy(out.data(), "PSF", 3);
    out[3] = kVersion;
    csf::StoreLe32(&out[4], static_cast<uint32_t>(reserved.size()));
    csf::StoreLe32(&out[8], static_cast<uint32_t>(compressed.size()));
    csf::StoreLe32(
        &out[12], compressed.empty() ? 0 : static_cast<uint32_t>(crc32(0, compressed.data(), static_cast<uInt>(size))));
    out.insert(out.end(), reserved.begin(), reserved.end());
    out.insert(out.end(), compressed.begin(), compressed.end());
    const std::string text = csf::FormatTagArea(tags);
    out.insert(out.end(), text.begin(), text.end());

    return out;
}

void TestProgramPieces()
{
    // Files of every length modulo four, copied and referred to, and large ones that compress well and badly.
    std::vector<std::vector<uint8_t>> files;
    std::mt19937 rng(5);
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
                             std::size_t{5}, std::size_t{3} << 20})
    {
        std::vector<uint8_t> file(size);
        for (uint8_t& byte : file)
        {
            byte = static_cast<uint8_t>(rng() % 4);
        }
        files.push_back(std::move(file));
    }
    std::vector<uint8_t> noise(1 << 20);
    for (uint8_t& byte : noise)
    {
        byte = static_cast<uint8_t>(rng());
    }
    files.push_back(std::move(noise));

    ProgramBuilder copied;
    ProgramBuilder referred;
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        const std::string path = "content/" + std::string(i, 'f');
        copied.AddFile(path, files[i]);
        referred.AddFileRef(path, files[i]);
        if (i == 3)
        {
            copied.AddSound(0x01000003);
            referred.AddSound(0x01000003);
        }
    }

    const std::vector<uint8_t> program = copied.Data();
    CSF_CHECK(referred.Data() == program);

    // A referred file is a piece of the program, not a copy.
    bool referred_in_place = false;
    for (const std::span<const uint8_t> piece : referred.Pieces())
    {
        referred_in_place = referred_in_place || piece.data() == files.back().data();
    }
    CSF_CHECK(referred_in_place);

    const std::vector<uint8_t> reserved = {7, 7};
    const Tags tags = {{"title", "pieces"}};
    const std::vector<uint8_t> file = WritePsf(kVersion, reserved, program, tags);
    CSF_CHECK(file == OneCallPsf(reserved, program, tags));
    CSF_CHECK(copied.WritePsf(kVersion, reserved, tags) == file);
    CSF_CHECK(referred.WritePsf(kVersion, reserved, tags) == file);
    CSF_CHECK(WritePsf(kVersion, {}, {}, {}) == OneCallPsf({}, {}, {}));
    CSF_CHECK(ProgramBuilder{}.WritePsf(kVersion, {}, tags) == OneCallPsf({}, {}, tags));

    ProgramChunks chunks;
    CSF_CHECK(!csf::ParsePsfChunks(file, chunks));
    CSF_CHECK(chunks.files.size() == files.size() && chunks.sound == 0x01000003u);
    for (std::size_t i = 0; i < files.size() && i < chunks.files.size(); ++i)
    {
        CSF_CHECK(chunks.files[i].path == "content/" + std::string(i, 'f') && chunks.files[i].data == files[i]);
    }
}

// The outcome of reading a program: its chunks, or the error.
struct ChunksRead
{
    std::string error;
    ProgramChunks chunks;
};

bool SameChunks(const ChunksRead& a, const ChunksRead& b)
{
    if (a.error != b.error || a.chunks.sound != b.chunks.sound || a.chunks.files.size() != b.chunks.files.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.chunks.files.size(); ++i)
    {
        if (a.chunks.files[i].path != b.chunks.files[i].path || a.chunks.files[i].data != b.chunks.files[i].data)
        {
            return false;
        }
    }

    return true;
}

// A PSF file's chunks read both ways: ParsePsf and then ParseProgram, and ParsePsfChunks. ParsePsfChunks reads the
// chunks straight from the zlib stream. The two ways must agree on every file, error included.
void CheckStreamedChunks(const std::vector<uint8_t>& file, const std::string& what)
{
    ChunksRead whole;
    PsfFile psf;
    if (const auto err = csf::ParsePsf(file, psf))
    {
        whole.error = *err;
    }
    else if (const auto err2 = csf::ParseProgram(psf.program, whole.chunks))
    {
        whole.error = *err2;
    }

    ChunksRead streamed;
    if (const auto err = csf::ParsePsfChunks(file, streamed.chunks))
    {
        streamed.error = *err;
    }

    Check(SameChunks(whole, streamed),
          what + ": the streamed chunks match (\"" + whole.error + "\", \"" + streamed.error + "\")");
}

// A random program: FILE, SND and unknown chunks of random sizes, sometimes cut short or followed by stray bytes.
std::vector<uint8_t> RandomProgram(std::mt19937& rng)
{
    ProgramBuilder b;
    const int chunks = static_cast<int>(rng() % 6);
    for (int i = 0; i < chunks; ++i)
    {
        std::vector<uint8_t> data(rng() % 3 == 0 ? rng() % 200000 : rng() % 40);
        for (uint8_t& byte : data)
        {
            byte = static_cast<uint8_t>(rng() % 7);
        }

        if (rng() % 4 == 0)
        {
            b.AddSound(static_cast<uint32_t>(rng()));
        }
        else
        {
            b.AddFile(std::string(rng() % 12, 'p'), data);
        }
    }

    std::vector<uint8_t> program = b.Data();
    if (rng() % 3 == 0)
    {
        program.insert(program.begin(), {'X', 'Y', 'Z', 'W', 5, 0, 0, 0, 1, 2, 3, 4, 5, 0, 0, 0});
    }
    if (rng() % 3 == 0 && !program.empty())
    {
        program.resize(rng() % program.size());
    }
    if (rng() % 4 == 0)
    {
        program.insert(program.end(), rng() % 10, 0xEE);
    }

    return program;
}

void TestStreamedChunks()
{
    ProgramBuilder b;
    b.AddFile("content/sound/a.bfsar", std::vector<uint8_t>(3 << 20, 0x5A));
    b.AddSound(0x01000005);
    b.AddFile("code/game.rpx", std::vector<uint8_t>{1, 2, 3});
    const std::vector<uint8_t> program = b.Data();
    CheckStreamedChunks(WritePsf(kVersion, {9}, program, {}), "a set's chunks");
    CheckStreamedChunks(WritePsf(kVersion, {}, {}, {}), "no program");

    // The chunk errors, a program that stops in a chunk's padding, and one with stray bytes after its last chunk.
    const std::vector<std::vector<uint8_t>> programs = {
        {'F', 'I', 'L', 'E', 100, 0, 0, 0, 1, 2},
        {'F', 'I', 'L', 'E', 0xf8, 0xff, 0xff, 0xff, 1, 2, 3, 4},
        {'F', 'I', 'L', 'E', 4, 0, 0, 0, 9, 0, 'a', 'b'},
        {'F', 'I', 'L', 'E', 1, 0, 0, 0, 9, 0, 0, 0},
        {'S', 'N', 'D', ' ', 2, 0, 0, 0, 1, 2, 0, 0},
        {'S', 'N', 'D', ' ', 5, 0, 0, 0, 1, 2, 3, 4, 5},
        {'S', 'N', 'D', ' ', 4, 0, 0, 0, 1, 2, 3, 4, 7, 7, 7},
    };
    for (std::size_t i = 0; i < programs.size(); ++i)
    {
        CheckStreamedChunks(WritePsf(kVersion, {}, programs[i], {}), "program " + std::to_string(i));
    }

    // The container's errors.
    const std::vector<uint8_t> file = WritePsf(kVersion, {}, program, {});
    std::vector<uint8_t> bad = file;
    bad[40] ^= 0x55;
    CheckStreamedChunks(bad, "a damaged program");
    CheckStreamedChunks({file.begin(), file.begin() + 20}, "a cut file");
    CheckStreamedChunks({'P', 'S'}, "not a PSF file");
    const uint32_t half = csf::LoadLe32(&file[8]) / 2;
    std::vector<uint8_t> short_stream(file.begin(), file.begin() + 16 + half);
    csf::StoreLe32(&short_stream[8], half);
    csf::StoreLe32(&short_stream[12], static_cast<uint32_t>(crc32(0, &short_stream[16], half)));
    CheckStreamedChunks(short_stream, "a stream cut short");

    std::mt19937 rng(28);
    for (int round = 0; round < 300 && failures == 0; ++round)
    {
        CheckStreamedChunks(WritePsf(kVersion, {}, RandomProgram(rng), {}), "random program " + std::to_string(round));
    }
}

} // namespace

int main()
{
    TestTimes();
    TestDecimals();
    TestDecimalsUnderCommaLocale();
    TestPsfRoundTrip();
    TestTagParsing();
    TestProcessDescriptor();
    TestArchiveDescriptor();
    TestChunks();
    TestStreamedChunks();
    TestProgramPieces();
    TestLoadSet();
    TestLoadSetInArchive();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    return 0;
}
