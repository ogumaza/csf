// SPDX-License-Identifier: MIT

// Builds sound archives and their files in the Wii U's byte order and the Switch's byte order. The test checks what the
// FSAR readers read from them: sounds and their settings and names; files the archive holds, files only a group holds,
// whether the group is in the archive or in a group file the game loads itself, and files outside it; sequences and
// their labels; banks with every kind of region table; wave archives and waves. Damaged files and other formats must be
// refused. A bank whose tables point at one another over and over mustn't take memory without limit.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.h"
#include "fsar/formats.h"
#include "fsar_files.h"

namespace
{

using fsar_files::KeyTable;
using fsar_files::Region;
using fsar_files::VelocityTable;

std::vector<uint8_t> Bytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool StartsWith(std::span<const uint8_t> data, const std::vector<uint8_t>& start)
{
    return data.size() >= start.size() && std::equal(start.begin(), start.end(), data.begin());
}

// Checks that `parse` throws std::runtime_error with `words` in its message.
template <typename Parse>
void CheckThrows(const std::string& what, Parse parse, const std::string& words)
{
    try
    {
        parse();
        Check(false, what + " is refused");
    }
    catch (const std::runtime_error& e)
    {
        Check(std::string(e.what()).find(words) != std::string::npos,
              what + " is refused with \"" + words + "\": " + e.what());
    }
}

void TestWaves(bool big_endian, const std::string& order)
{
    // A looping DSP-ADPCM wave in stereo, and a PCM16 one.
    fsar_files::WaveSpec spec;
    spec.loop = true;
    spec.sample_rate = 48000;
    spec.loop_start = 14;
    spec.loop_end = 28;
    spec.channels = {{0x42, 1, 2, 3, 4, 5, 6, 7}, {0x17, 9, 8, 7, 6, 5, 4, 3}};
    const std::vector<uint8_t> adpcm = fsar_files::Wave(big_endian, spec);
    spec = {};
    spec.encoding = 1;
    spec.loop_end = 4;
    spec.channels = {{1, 0, 2, 0, 3, 0, 4, 0}};
    const std::vector<uint8_t> pcm = fsar_files::Wave(big_endian, spec);

    const csf::fsar::Wave wave = csf::fsar::Wave::Parse(adpcm);
    Check(wave.encoding == csf::fsar::WaveEncoding::kDspAdpcm && wave.loop && wave.sample_rate == 48000 &&
              wave.loop_start == 14 && wave.loop_end == 28,
          order + ": the DSP-ADPCM wave's settings");
    Check(wave.channels.size() == 2 && StartsWith(wave.channels[0].data, {0x42, 1, 2, 3, 4, 5, 6, 7}) &&
              StartsWith(wave.channels[1].data, {0x17, 9, 8, 7, 6, 5, 4, 3}),
          order + ": the DSP-ADPCM wave's samples");
    const bool info = wave.channels.size() == 2 && wave.channels[1].adpcm;
    Check(info && wave.channels[1].adpcm->coefs[0] == 1 && wave.channels[1].adpcm->coefs[15] == 16 &&
              wave.channels[1].adpcm->pred_scale == 0x17 && wave.channels[1].adpcm->yn1 == 100 &&
              wave.channels[1].adpcm->loop_yn2 == 104,
          order + ": the DSP-ADPCM wave's coefficients and context");

    const csf::fsar::Wave plain = csf::fsar::Wave::Parse(pcm);
    Check(plain.encoding == csf::fsar::WaveEncoding::kPcm16 && !plain.loop && plain.sample_rate == 32000 &&
              plain.channels.size() == 1 && !plain.channels[0].adpcm &&
              StartsWith(plain.channels[0].data, {1, 0, 2, 0, 3, 0, 4, 0}),
          order + ": the PCM16 wave");

    // A wave archive of both. Its waves point into the file.
    const std::vector<uint8_t> file = fsar_files::WaveArchive(big_endian, {adpcm, pcm});
    const csf::fsar::WaveArchive archive = csf::fsar::WaveArchive::Parse(file);
    Check(archive.waves.size() == 2 &&
              std::equal(archive.waves[0].begin(), archive.waves[0].end(), adpcm.begin(), adpcm.end()) &&
              std::equal(archive.waves[1].begin(), archive.waves[1].end(), pcm.begin(), pcm.end()),
          order + ": the wave archive's waves");
}

void TestBank(bool big_endian, const std::string& order)
{
    // Instruments with each kind of key table: a direct table, none at all, a range table and an index table, with
    // regions that set every option, regions that set none, and velocities that play nothing.
    Region full;
    full.wave = 1;
    full.original_key = 72;
    full.volume = 90;
    full.pan = 20;
    full.pitch = 1.5f;
    full.flags = 1 | 3 << 8 | 2 << 16;
    full.adshr = {{10, 20, 30, 40, 50}};
    Region plain;

    VelocityTable direct;
    direct.entries = {{0, full}};
    VelocityTable split;
    split.kind = VelocityTable::kRange;
    split.entries = {{63, plain}, {127, std::nullopt}};
    VelocityTable by_velocity;
    by_velocity.kind = VelocityTable::kIndex;
    by_velocity.first = 100;
    by_velocity.entries = {{0, full}, {0, std::nullopt}};

    KeyTable whole;
    whole.entries = {{0, direct}};
    KeyTable ranges;
    ranges.kind = KeyTable::kRange;
    ranges.entries = {{40, split}, {127, std::nullopt}};
    KeyTable keys;
    keys.kind = KeyTable::kIndex;
    keys.first = 60;
    keys.entries = {{0, direct}, {0, by_velocity}, {0, std::nullopt}};

    fsar_files::BankSpec spec;
    spec.waves = {{0x05000000, 0}, {0x05000003, 7}};
    spec.instruments = {whole, std::nullopt, ranges, keys};
    const csf::fsar::Bank bank = csf::fsar::Bank::Parse(fsar_files::Bank(big_endian, spec));

    Check(bank.waves.size() == 2 && bank.waves[1].wave_archive_item == 0x05000003 && bank.waves[1].index == 7,
          order + ": the bank's waves");
    Check(bank.instruments.size() == 4 && bank.instruments[0] && !bank.instruments[1] && bank.instruments[2] &&
              bank.instruments[3],
          order + ": the bank's instruments");
    if (bank.instruments.size() != 4 || !bank.instruments[0] || !bank.instruments[2] || !bank.instruments[3])
    {
        return;
    }

    const csf::fsar::VelocityRegion* region = bank.instruments[0]->Find(0, 1);
    Check(region && region->wave_id_index == 1 && region->original_key == 72 && region->volume == 90 &&
              region->pan == 20 && region->pitch == 1.5f && region->ignore_note_off && region->key_group == 3 &&
              region->interpolation_type == 2 && region->adshr.attack == 10 && region->adshr.release == 50,
          order + ": a region that sets every option");
    Check(bank.instruments[0]->Find(127, 127) == region, order + ": a direct table covers every key and velocity");

    region = bank.instruments[2]->Find(40, 63);
    Check(region && region->wave_id_index == 0 && region->original_key == 60 && region->volume == 127 &&
              region->pan == 64 && region->pitch == 1.0f && region->adshr.decay == 127,
          order + ": a region that sets no option has the defaults");
    Check(!bank.instruments[2]->Find(40, 64) && !bank.instruments[2]->Find(41, 0),
          order + ": ranges that play nothing");

    Check(bank.instruments[3]->Find(60, 5) && bank.instruments[3]->Find(61, 100) &&
              !bank.instruments[3]->Find(61, 101) && !bank.instruments[3]->Find(61, 99) &&
              !bank.instruments[3]->Find(62, 0) && !bank.instruments[3]->Find(59, 0) &&
              !bank.instruments[3]->Find(63, 0),
          order + ": index tables");
}

void TestSequence(bool big_endian, const std::string& order)
{
    // The parsed sequence's bytecode points into the file.
    const std::vector<uint8_t> bytecode = {0xFE, 0x00, 0x06, 0x88, 0x01, 0x00, 0x00, 0x10, 0xFF};
    const std::vector<uint8_t> file = fsar_files::Sequence(big_endian, bytecode, {{"Start", 0}, {"Second", 3}});

    const csf::fsar::Sequence sequence = csf::fsar::Sequence::Parse(file);

    Check(StartsWith(sequence.data, bytecode), order + ": the sequence's bytecode");
    Check(sequence.labels.size() == 2 && sequence.labels[0].name == "Start" && sequence.labels[0].offset == 0 &&
              sequence.labels[1].name == "Second" && sequence.labels[1].offset == 3,
          order + ": the sequence's labels");
}

fsar_files::ArchiveSpec ArchiveSpec(bool big_endian)
{
    // A sequence, a wave sound and a stream. The sequence's bank is in its first and third slots, and the second slot
    // is left empty. The bank is in the archive, and its wave archive only in a group. The stream is outside the
    // archive.
    fsar_files::ArchiveSpec spec;
    fsar_files::SoundSpec sequence;
    sequence.name = "SEQ_TEST";
    sequence.file = 0;
    sequence.player = 1;
    sequence.volume = 90;
    sequence.pan = 1 | 2 << 8;
    sequence.priority = 70 | 3 << 8;
    sequence.banks = {0, 0xFFFFFFFF, 0};
    sequence.allocate_track_flags = 0x7;
    sequence.start_offset = 3;
    sequence.channel_priority = 80 | 1 << 8;
    fsar_files::SoundSpec wave_sound;
    wave_sound.name = "WSD_TEST";
    wave_sound.kind = fsar_files::SoundSpec::kWaveSound;
    wave_sound.file = 4;
    fsar_files::SoundSpec stream;
    stream.name = "STRM_TEST";
    stream.kind = fsar_files::SoundSpec::kStream;
    stream.file = 3;
    spec.sounds = {sequence, wave_sound, stream};

    fsar_files::WaveSpec wave;
    wave.channels = {{0x20, 0, 0, 0, 0, 0, 0, 0}};
    const std::vector<uint8_t> wave_archive = fsar_files::WaveArchive(big_endian, {fsar_files::Wave(big_endian, wave)});
    fsar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    spec.banks = {{"BANK_TEST", 1}};
    spec.wave_archives = {{"WARC_TEST", 2}};
    spec.players = {{"PLAYER_SE", 4}, {"PLAYER_BGM", 1}};
    spec.groups = {{"GROUP_TEST", 5}};
    spec.files = {{fsar_files::Sequence(big_endian, {0xFE, 0x00, 0x06, 0xFF}, {{"Test", 0}}), "", false},
                  {fsar_files::Bank(big_endian, bank), "", false},
                  {{}, "", true},
                  {{}, "stream/STRM_TEST.bfstm", false},
                  {Bytes("FWSD"), "", false},
                  {fsar_files::Group(big_endian, {{2, wave_archive}}), "", false}};

    return spec;
}

void TestArchive(bool big_endian, const std::string& order)
{
    const fsar_files::ArchiveSpec spec = ArchiveSpec(big_endian);
    const csf::fsar::SoundArchive archive = csf::fsar::SoundArchive::Load(fsar_files::Archive(big_endian, spec));

    Check(archive.Version() == 0x00020200 && !archive.Truncated() && archive.Sounds().size() == 3 &&
              archive.FileCount() == 6,
          order + ": the archive's version, sounds and files");
    if (archive.Sounds().size() != 3)
    {
        return;
    }

    const csf::fsar::SoundInfo& sequence = archive.Sounds()[0];
    Check(sequence.name == "SEQ_TEST" && sequence.type == csf::fsar::SoundType::kSequence && sequence.file_id == 0 &&
              sequence.player_id == 0x04000001 && sequence.volume == 90 && sequence.pan_mode == 1 &&
              sequence.pan_curve == 2 && sequence.player_priority == 70 && sequence.actor_player_id == 3,
          order + ": the sequence sound's settings");
    Check(sequence.banks == std::vector<uint32_t>{0x03000000, 0xFFFFFFFF, 0x03000000} &&
              sequence.allocate_track_flags == 7 && sequence.start_offset == 3 && sequence.channel_priority == 80 &&
              sequence.release_priority_fix,
          order + ": the sequence's settings");
    Check(archive.Sounds()[1].name == "WSD_TEST" && archive.Sounds()[1].type == csf::fsar::SoundType::kWaveSound &&
              archive.Sounds()[1].player_priority == 64 && archive.Sounds()[1].banks.empty(),
          order + ": the wave sound");
    Check(archive.Sounds()[2].name == "STRM_TEST" && archive.Sounds()[2].type == csf::fsar::SoundType::kStream,
          order + ": the stream");
    Check(archive.FindSound("STRM_TEST") == 2u && !archive.FindSound("NOTHING"), order + ": finding sounds by name");

    Check(archive.Banks().size() == 1 && archive.Banks()[0].name == "BANK_TEST" && archive.Banks()[0].file_id == 1 &&
              archive.WaveArchives().size() == 1 && archive.WaveArchives()[0].name == "WARC_TEST" &&
              archive.Players().size() == 2 && archive.Players()[1].name == "PLAYER_BGM" &&
              archive.Players()[1].playable_sound_max == 1 && archive.Groups().size() == 1 &&
              archive.Groups()[0].file_id == 5,
          order + ": the banks, wave archives, players and groups");

    // The files: in the archive, only in the group, outside it.
    Check(StartsWith(archive.FileData(0), spec.files[0].bytes) && StartsWith(archive.FileData(1), spec.files[1].bytes),
          order + ": files the archive holds");
    Check(archive.FileData(3).empty() && archive.ExternalPath(3) == "stream/STRM_TEST.bfstm" &&
              archive.ExternalPath(0).empty() && archive.FileData(99).empty() && archive.ExternalPath(99).empty(),
          order + ": a file outside the archive");
    try
    {
        const csf::fsar::WaveArchive wave_archive = csf::fsar::WaveArchive::Parse(archive.FileData(2));
        const csf::fsar::Wave wave = csf::fsar::Wave::Parse(wave_archive.waves.at(0));
        Check(!wave.channels.empty() && wave.channels[0].data[0] == 0x20, order + ": a file only a group holds");
    }
    catch (const std::exception& e)
    {
        Check(false, order + ": a file only a group holds: " + e.what());
    }

    // Without the string block, the sounds are named by their kinds and indices.
    fsar_files::ArchiveSpec nameless = spec;
    nameless.names = false;
    const csf::fsar::SoundArchive unnamed = csf::fsar::SoundArchive::Load(fsar_files::Archive(big_endian, nameless));
    Check(unnamed.Sounds().size() == 3 && unnamed.Sounds()[0].name == "SEQ_0" && unnamed.Sounds()[1].name == "WSD_1" &&
              unnamed.Sounds()[2].name == "STRM_2" && unnamed.Banks()[0].name.empty(),
          order + ": an archive without names");

    // An archive cut off in its FILE block reports that it's truncated. The files past the cut are missing.
    std::vector<uint8_t> cut = fsar_files::Archive(big_endian, spec);
    cut.resize(cut.size() - 0x20);
    const csf::fsar::SoundArchive truncated = csf::fsar::SoundArchive::Load(cut);
    Check(truncated.Truncated() && truncated.FileData(5).empty() && truncated.FileData(2).empty() &&
              !truncated.FileData(0).empty(),
          order + ": a truncated archive");
}

// A group that the game keeps outside the archive. The archive lists the group with no file. Once the group file is
// added, the files that it holds and the archive lacks can be read. The archive keeps its copies of the others.
void TestGroupFile(bool big_endian, const std::string& order)
{
    fsar_files::ArchiveSpec spec = ArchiveSpec(big_endian);
    const std::vector<uint8_t> group = spec.files.back().bytes;
    spec.files.pop_back();
    spec.groups = {{"GROUP_TEST", 0xFFFFFFFF}};
    csf::fsar::SoundArchive archive = csf::fsar::SoundArchive::Load(fsar_files::Archive(big_endian, spec));
    Check(archive.Groups().size() == 1 && archive.Groups()[0].file_id == 0xFFFFFFFF && archive.FileData(2).empty(),
          order + ": a group the archive doesn't hold");

    archive.AddGroupFile(group);
    archive.AddGroupFile(fsar_files::Group(big_endian, {{0, Bytes("FSEQ copy")}, {99, Bytes("not a file")}}));

    try
    {
        const csf::fsar::WaveArchive wave_archive = csf::fsar::WaveArchive::Parse(archive.FileData(2));
        const csf::fsar::Wave wave = csf::fsar::Wave::Parse(wave_archive.waves.at(0));
        Check(!wave.channels.empty() && wave.channels[0].data[0] == 0x20, order + ": a file a group file adds");
    }
    catch (const std::exception& e)
    {
        Check(false, order + ": a file a group file adds: " + e.what());
    }
    Check(StartsWith(archive.FileData(0), spec.files[0].bytes) && archive.FileData(99).empty(),
          order + ": a group file's copy of a file the archive holds");
}

// A bank of `instruments` instruments that all point at one key table of `keys` ranges. The ranges all point at one
// velocity table of `velocities` ranges that play nothing. The bank names far more regions than it has bytes.
std::vector<uint8_t> SharedBank(uint32_t instruments, uint32_t keys, uint32_t velocities)
{
    const auto align4 = [](size_t size)
    {
        return (size + 3) & ~size_t{3};
    };

    fsar_files::Bytes b(true);
    b.Add(16);
    b.Ref(0, 0x0100, b.Add(4), 0);
    const size_t table = b.Add(4 + 8 * instruments);
    b.Ref(8, 0x0101, table, 0);
    b.U32(table, instruments);
    const size_t instrument = b.Add(8);
    for (uint32_t i = 0; i < instruments; ++i)
    {
        b.Ref(table + 4 + 8 * i, 0x5900, instrument, table);
    }

    const size_t key_table = b.Add(4 + align4(keys) + 8 * keys);
    const size_t key_region = b.Add(8);
    b.Ref(instrument, 0x6001, key_table, instrument);
    b.U32(key_table, keys);
    for (uint32_t i = 0; i < keys; ++i)
    {
        b.U8(key_table + 4 + i, static_cast<uint8_t>(i));
        b.Ref(key_table + 4 + align4(keys) + 8 * i, 0x5901, key_region, key_table);
    }

    const size_t velocity_table = b.Add(4 + align4(velocities) + 8 * velocities);
    b.Ref(key_region, 0x6001, velocity_table, key_region);
    b.U32(velocity_table, velocities);
    for (uint32_t i = 0; i < velocities; ++i)
    {
        b.U8(velocity_table + 4 + i, static_cast<uint8_t>(i));
        b.NullRef(velocity_table + 4 + align4(velocities) + 8 * i, 0x5903);
    }

    return fsar_files::File(true, "FBNK", 0x00010000, {{0x5800, "INFO", b.data}});
}

void TestRefusals()
{
    // Other kinds of file, a byte order mark that's neither order, and archives cut short.
    const std::vector<uint8_t> archive = fsar_files::Archive(true, ArchiveSpec(true));
    std::vector<uint8_t> damaged = archive;
    const auto load = [&]
    {
        csf::fsar::SoundArchive::Load(damaged);
    };
    std::copy_n("CSAR", 4, damaged.begin());
    CheckThrows("a 3DS archive", load, "3DS sound archive");
    std::copy_n("RSAR", 4, damaged.begin());
    CheckThrows("a Wii archive", load, "Wii sound archive");
    damaged = archive;
    damaged[4] = 0x12;
    CheckThrows("an unknown byte order", load, "byte order");
    damaged = {'F', 'S', 'A', 'R'};
    CheckThrows("a short file", load, "too short");
    damaged.assign(archive.begin(), archive.begin() + 0x100);
    CheckThrows("an archive cut in its INFO block", load, "out of bounds");

    // A group file: an archive in place of one.
    csf::fsar::SoundArchive sound_archive = csf::fsar::SoundArchive::Load(archive);
    const auto add_group = [&]
    {
        sound_archive.AddGroupFile(archive);
    };
    CheckThrows("an archive as a group file", add_group, "expected FGRP");

    // Banks: an archive in place of one, one whose instrument table runs past its end, and one that names far more
    // regions than it has bytes.
    std::vector<uint8_t> bank = archive;
    const auto parse_bank = [&]
    {
        csf::fsar::Bank::Parse(bank);
    };
    CheckThrows("an archive as a bank", parse_bank, "expected FBNK");
    fsar_files::BankSpec spec;
    spec.instruments = {std::nullopt};
    bank = fsar_files::Bank(true, spec);
    bank[0x20 + 8 + 0x14] = 0x7F; // the top byte of the instrument count
    CheckThrows("a bank whose instrument table runs past its end", parse_bank, "out of bounds");
    bank = SharedBank(1000, 128, 128);
    CheckThrows("a bank with more regions than bytes", parse_bank, "more regions than bytes");
}

} // namespace

int main()
{
    for (const bool big_endian : {true, false})
    {
        const std::string order = big_endian ? "big-endian" : "little-endian";
        TestWaves(big_endian, order);
        TestBank(big_endian, order);
        TestSequence(big_endian, order);
        TestArchive(big_endian, order);
        TestGroupFile(big_endian, order);
    }
    TestRefusals();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
