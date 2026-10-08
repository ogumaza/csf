// SPDX-License-Identifier: MIT

// Builds small NintendoWare for Cafe sound files for the tests, in either byte order and laid out as the real ones are:
// waves (FWAV), wave archives (FWAR), banks (FBNK), sequences (FSEQ), groups (FGRP) and sound archives (FSAR).

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/bytes.h"

namespace fsar_files
{

// Bytes in one byte order, with references between what's in them.
class Bytes
{
public:
    explicit Bytes(bool big_endian) : big_endian_(big_endian)
    {
    }

    size_t Size() const
    {
        return data.size();
    }

    // Appends `size` zero bytes, and returns where they start.
    size_t Add(size_t size)
    {
        const size_t at = data.size();
        data.resize(at + size);

        return at;
    }

    // Appends `bytes`, and returns where they start.
    size_t Add(const std::vector<uint8_t>& bytes)
    {
        const size_t at = data.size();
        data.insert(data.end(), bytes.begin(), bytes.end());

        return at;
    }

    void Align(size_t alignment)
    {
        data.resize((data.size() + alignment - 1) / alignment * alignment);
    }

    void U8(size_t at, uint8_t value)
    {
        data[at] = value;
    }

    void U16(size_t at, uint16_t value)
    {
        big_endian_ ? csf::StoreBe16(&data[at], value) : csf::StoreLe16(&data[at], value);
    }

    void U32(size_t at, uint32_t value)
    {
        big_endian_ ? csf::StoreBe32(&data[at], value) : csf::StoreLe32(&data[at], value);
    }

    // A reference: its type, and the target's offset from `base`.
    void Ref(size_t at, uint16_t type, size_t target, size_t base)
    {
        U16(at, type);
        U32(at + 4, static_cast<uint32_t>(target - base));
    }

    // A reference to nothing.
    void NullRef(size_t at, uint16_t type)
    {
        U16(at, type);
        U32(at + 4, 0xFFFFFFFF);
    }

    // A reference followed by the size of what it refers to.
    void SizedRef(size_t at, uint16_t type, size_t target, size_t base, size_t size)
    {
        Ref(at, type, target, base);
        U32(at + 8, static_cast<uint32_t>(size));
    }

    // Appends option parameters: a bit field and, for each bit that's set, its value in the order of the bits.
    size_t Options(const std::vector<std::pair<int, uint32_t>>& values)
    {
        const size_t at = Add(4 + 4 * values.size());
        uint32_t flags = 0;
        for (size_t i = 0; i < values.size(); ++i)
        {
            flags |= 1u << values[i].first;
            U32(at + 4 + 4 * i, values[i].second);
        }
        U32(at, flags);

        return at;
    }

    std::vector<uint8_t> data;

private:
    bool big_endian_;
};

struct Block
{
    uint16_t type;
    const char* magic;
    std::vector<uint8_t> body;
};

// A whole file: the header and its block table, then each block's magic, size and body. Each part is aligned to 32
// bytes.
inline std::vector<uint8_t> File(bool big_endian, const char* magic, uint32_t version, const std::vector<Block>& blocks)
{
    Bytes file(big_endian);
    file.Add(0x14 + 12 * blocks.size());
    file.Align(0x20);
    std::memcpy(file.data.data(), magic, 4);
    file.U16(4, 0xFEFF);
    file.U16(6, static_cast<uint16_t>(file.Size()));
    file.U32(8, version);
    file.U16(0x10, static_cast<uint16_t>(blocks.size()));

    for (size_t i = 0; i < blocks.size(); ++i)
    {
        const size_t start = file.Add(8);
        std::memcpy(file.data.data() + start, blocks[i].magic, 4);
        file.Add(blocks[i].body);
        file.Align(0x20);
        file.U32(start + 4, static_cast<uint32_t>(file.Size() - start));

        file.U16(0x14 + i * 12, blocks[i].type);
        file.U32(0x18 + i * 12, static_cast<uint32_t>(start));
        file.U32(0x1C + i * 12, static_cast<uint32_t>(file.Size() - start));
    }
    file.U32(0x0C, static_cast<uint32_t>(file.Size()));

    return file.data;
}

// A wave. Each DSP-ADPCM channel gets coefficients of 1 to 16 and a frame header of its first byte, and loop context
// values of 100 onwards.
struct WaveSpec
{
    uint8_t encoding = 2;
    bool loop = false;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    std::vector<std::vector<uint8_t>> channels;
};

inline std::vector<uint8_t> Wave(bool big_endian, const WaveSpec& spec)
{
    Bytes info(big_endian);
    Bytes data(big_endian);
    info.Add(0x14);
    info.U8(0, spec.encoding);
    info.U8(1, spec.loop);
    info.U32(4, spec.sample_rate);
    info.U32(8, spec.loop_start);
    info.U32(12, spec.loop_end);

    const size_t table = info.Add(4 + 8 * spec.channels.size());
    info.U32(table, static_cast<uint32_t>(spec.channels.size()));
    for (size_t i = 0; i < spec.channels.size(); ++i)
    {
        const size_t channel = info.Add(0x14);
        info.Ref(table + 4 + 8 * i, 0x7100, channel, table);

        data.Align(0x20);
        info.Ref(channel, 0x1F00, data.Add(spec.channels[i]), 0);
        if (spec.encoding != 2)
        {
            continue;
        }

        const size_t adpcm = info.Add(0x30);
        info.Ref(channel + 8, 0x0300, adpcm, channel);
        for (int k = 0; k < 16; ++k)
        {
            info.U16(adpcm + 2 * k, static_cast<uint16_t>(k + 1));
        }
        info.U16(adpcm + 32, spec.channels[i].empty() ? 0 : spec.channels[i][0]);
        for (int k = 0; k < 5; ++k)
        {
            info.U16(adpcm + 34 + 2 * k, static_cast<uint16_t>(100 + k));
        }
    }

    return File(big_endian, "FWAV", 0x00010100, {{0x7000, "INFO", info.data}, {0x7001, "DATA", data.data}});
}

inline std::vector<uint8_t> WaveArchive(bool big_endian, const std::vector<std::vector<uint8_t>>& waves)
{
    Bytes info(big_endian);
    Bytes files(big_endian);
    info.Add(4 + 12 * waves.size());
    info.U32(0, static_cast<uint32_t>(waves.size()));
    for (size_t i = 0; i < waves.size(); ++i)
    {
        files.Align(0x20);
        info.SizedRef(4 + 12 * i, 0x1F00, files.Add(waves[i]), 0, waves[i].size());
    }

    return File(big_endian, "FWAR", 0x00010000, {{0x6800, "INFO", info.data}, {0x6801, "FILE", files.data}});
}

// A velocity region. The bytes are ints here. Tests can then give them as plain numbers. Otherwise compilers would warn
// about narrowing inside std::optional.
struct Region
{
    uint32_t wave = 0; // an index into the bank's waves
    std::optional<int> original_key, volume, pan;
    std::optional<float> pitch;
    std::optional<uint32_t> flags; // ignore note off, key group << 8 and interpolation type << 16
    std::optional<std::array<uint8_t, 5>> adshr;
};

// A region table: kDirect has one entry, kRange's entries each run up to their key, and kIndex has one entry for each
// key from `first`. An empty entry plays nothing. Region's bytes and the keys are both ints.
template <typename Entry>
struct Table
{
    enum Kind
    {
        kDirect,
        kRange,
        kIndex
    };

    Kind kind = kDirect;
    int first = 0;
    std::vector<std::pair<int, std::optional<Entry>>> entries;
};

using VelocityTable = Table<Region>;
using KeyTable = Table<VelocityTable>;

struct BankSpec
{
    std::vector<std::pair<uint32_t, uint32_t>> waves; // wave archive item ID and index
    std::vector<std::optional<KeyTable>> instruments;
};

namespace detail
{

inline size_t PutRegion(Bytes& b, const Region& region)
{
    const size_t at = b.Add(4);
    b.U32(at, region.wave);

    std::vector<std::pair<int, uint32_t>> values;
    if (region.original_key)
    {
        values.push_back({0, static_cast<uint8_t>(*region.original_key)});
    }
    if (region.volume)
    {
        values.push_back({1, static_cast<uint8_t>(*region.volume)});
    }
    if (region.pan)
    {
        values.push_back({2, static_cast<uint8_t>(*region.pan)});
    }
    if (region.pitch)
    {
        values.push_back({3, std::bit_cast<uint32_t>(*region.pitch)});
    }
    if (region.flags)
    {
        values.push_back({4, *region.flags});
    }
    if (region.adshr)
    {
        values.push_back({9, 0});
    }
    const size_t options = b.Options(values);

    // The envelope: a reference from the region, to the five bytes.
    if (region.adshr)
    {
        const size_t ref = b.Add(8);
        const size_t envelope = b.Add(8);
        b.U32(options + 4 * values.size(), static_cast<uint32_t>(ref - at));
        b.Ref(ref, 0x0000, envelope, ref);
        std::memcpy(b.data.data() + envelope, region.adshr->data(), 5);
    }

    return at;
}

// Writes the reference at `owner` to a region table of `table`, and the table, and what its entries name.
template <typename Entry, typename PutEntry>
void PutTable(Bytes& b, size_t owner, const Table<Entry>& table, PutEntry put_entry)
{
    const size_t n = table.entries.size();
    size_t refs = 0;
    size_t q = 0;
    if (table.kind == Table<Entry>::kDirect)
    {
        q = refs = b.Add(8);
        b.U16(owner, 0x6000);
    }
    else if (table.kind == Table<Entry>::kRange)
    {
        q = b.Add(4 + ((n + 3) & ~size_t{3}) + 8 * n);
        refs = q + 4 + ((n + 3) & ~size_t{3});
        b.U16(owner, 0x6001);
        b.U32(q, static_cast<uint32_t>(n));
        for (size_t i = 0; i < n; ++i)
        {
            b.U8(q + 4 + i, static_cast<uint8_t>(table.entries[i].first));
        }
    }
    else
    {
        q = b.Add(4 + 8 * n);
        refs = q + 4;
        b.U16(owner, 0x6002);
        b.U8(q, static_cast<uint8_t>(table.first));
        b.U8(q + 1, static_cast<uint8_t>(static_cast<size_t>(table.first) + n - 1));
    }
    b.U32(owner + 4, static_cast<uint32_t>(q - owner));

    for (size_t i = 0; i < n; ++i)
    {
        if (table.entries[i].second)
        {
            b.Ref(refs + 8 * i, 0x5901, put_entry(b, *table.entries[i].second), q);
        }
        else
        {
            b.NullRef(refs + 8 * i, 0x5903);
        }
    }
}

inline size_t PutVelocityTable(Bytes& b, const VelocityTable& table)
{
    const size_t at = b.Add(8);
    PutTable(b, at, table, PutRegion);

    return at;
}

} // namespace detail

inline std::vector<uint8_t> Bank(bool big_endian, const BankSpec& spec)
{
    Bytes info(big_endian);
    info.Add(16);
    const size_t waves = info.Add(4 + 8 * spec.waves.size());
    info.Ref(0, 0x0100, waves, 0);
    info.U32(waves, static_cast<uint32_t>(spec.waves.size()));
    for (size_t i = 0; i < spec.waves.size(); ++i)
    {
        info.U32(waves + 4 + 8 * i, spec.waves[i].first);
        info.U32(waves + 8 + 8 * i, spec.waves[i].second);
    }

    const size_t instruments = info.Add(4 + 8 * spec.instruments.size());
    info.Ref(8, 0x0101, instruments, 0);
    info.U32(instruments, static_cast<uint32_t>(spec.instruments.size()));
    for (size_t i = 0; i < spec.instruments.size(); ++i)
    {
        if (!spec.instruments[i])
        {
            info.NullRef(instruments + 4 + 8 * i, 0x5902);
            continue;
        }

        const size_t instrument = info.Add(8);
        info.Ref(instruments + 4 + 8 * i, 0x5900, instrument, instruments);
        detail::PutTable(info, instrument, *spec.instruments[i], detail::PutVelocityTable);
    }

    return File(big_endian, "FBNK", 0x00010000, {{0x5800, "INFO", info.data}});
}

// A sequence file: its bytecode, and labels by name and offset.
inline std::vector<uint8_t> Sequence(bool big_endian, const std::vector<uint8_t>& bytecode,
                                     const std::vector<std::pair<std::string, uint32_t>>& labels)
{
    Bytes data(big_endian);
    data.Add(bytecode);

    Bytes table(big_endian);
    table.Add(4 + 8 * labels.size());
    table.U32(0, static_cast<uint32_t>(labels.size()));
    for (size_t i = 0; i < labels.size(); ++i)
    {
        const size_t label = table.Add(12 + labels[i].first.size() + 1);
        table.Align(4);
        table.Ref(4 + 8 * i, 0x5100, label, 0);
        table.Ref(label, 0x1F00, labels[i].second, 0);
        table.U32(label + 8, static_cast<uint32_t>(labels[i].first.size()));
        std::memcpy(table.data.data() + label + 12, labels[i].first.data(), labels[i].first.size());
    }

    return File(big_endian, "FSEQ", 0x00020000, {{0x5000, "DATA", data.data}, {0x5001, "LABL", table.data}});
}

// A group file holding copies of files, by their file IDs.
inline std::vector<uint8_t> Group(bool big_endian, const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& items)
{
    Bytes info(big_endian);
    Bytes files(big_endian);
    info.Add(4 + 8 * items.size());
    info.U32(0, static_cast<uint32_t>(items.size()));
    for (size_t i = 0; i < items.size(); ++i)
    {
        const size_t item = info.Add(16);
        info.Ref(4 + 8 * i, 0x7900, item, 0);
        info.U32(item, items[i].first);
        files.Align(0x20);
        info.SizedRef(item + 4, 0x1F00, files.Add(items[i].second), 0, items[i].second.size());
    }

    return File(big_endian, "FGRP", 0x00010000, {{0x7800, "INFO", info.data}, {0x7801, "FILE", files.data}});
}

// A sound archive.
struct SoundSpec
{
    enum Kind
    {
        kStream = 0x2201,
        kWaveSound = 0x2202,
        kSequence = 0x2203
    };

    std::string name;
    Kind kind = kSequence;
    uint32_t file = 0;
    uint32_t player = 0; // an index into the players
    uint8_t volume = 100;
    std::optional<uint32_t> pan;      // pan mode | pan curve << 8
    std::optional<uint32_t> priority; // player priority | actor player ID << 8

    // Sequences only.
    std::vector<uint32_t> banks; // indices into the banks, or 0xFFFFFFFF for an empty slot
    uint32_t allocate_track_flags = 1;
    std::optional<uint32_t> start_offset;
    std::optional<uint32_t> channel_priority; // channel priority | release priority fix << 8
};

struct FileSpec
{
    std::vector<uint8_t> bytes; // a file the archive holds
    std::string external;       // or the path of one outside it
    bool group_only = false;    // or one only a group holds
};

struct ArchiveSpec
{
    std::vector<SoundSpec> sounds;
    std::vector<std::pair<std::string, uint32_t>> banks;         // name and file
    std::vector<std::pair<std::string, uint32_t>> wave_archives; // name and file
    std::vector<std::pair<std::string, uint32_t>> players;       // name and playable sound max
    std::vector<std::pair<std::string, uint32_t>> groups;        // name and file
    std::vector<FileSpec> files;
    bool names = true; // false leaves the string block out
};

inline std::vector<uint8_t> Archive(bool big_endian, const ArchiveSpec& spec)
{
    // The names, by item ID.
    std::vector<std::pair<uint32_t, std::string>> names;
    const auto name_all = [&](uint32_t kind, const auto& items)
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            names.push_back({kind << 24 | static_cast<uint32_t>(i), items[i].first});
        }
    };
    for (size_t i = 0; i < spec.sounds.size(); ++i)
    {
        names.push_back({0x01000000 | static_cast<uint32_t>(i), spec.sounds[i].name});
    }
    name_all(3, spec.banks);
    name_all(4, spec.players);
    name_all(5, spec.wave_archives);
    name_all(6, spec.groups);

    // STRG: the string table, then a tree whose leaves name the items.
    Bytes strg(big_endian);
    strg.Add(16);
    const size_t strings = strg.Add(4 + 12 * names.size());
    strg.Ref(0, 0x2400, strings, 0);
    strg.U32(strings, static_cast<uint32_t>(names.size()));
    for (size_t i = 0; i < names.size(); ++i)
    {
        const size_t text = strg.Add(names[i].second.size() + 1);
        std::memcpy(strg.data.data() + text, names[i].second.data(), names[i].second.size());
        strg.SizedRef(strings + 4 + 12 * i, 0x1F01, text, strings, names[i].second.size() + 1);
    }
    strg.Align(4);
    const size_t tree = strg.Add(8 + 20 * names.size());
    strg.Ref(8, 0x2401, tree, 0);
    strg.U32(tree + 4, static_cast<uint32_t>(names.size()));
    for (size_t i = 0; i < names.size(); ++i)
    {
        strg.U16(tree + 8 + 20 * i, 1);
        strg.U32(tree + 20 + 20 * i, static_cast<uint32_t>(i));
        strg.U32(tree + 24 + 20 * i, names[i].first);
    }

    // FILE: the files the archive holds.
    Bytes files(big_endian);
    std::vector<size_t> file_offsets;
    for (const FileSpec& file : spec.files)
    {
        files.Align(0x20);
        file_offsets.push_back(file.bytes.empty() ? 0 : files.Add(file.bytes));
    }

    // INFO: a reference to each table, then the tables.
    Bytes info(big_endian);
    info.Add(64);
    const auto table = [&](int slot, uint16_t type, size_t count)
    {
        const size_t at = info.Add(4 + 8 * count);
        info.Ref(slot * 8, type, at, 0);
        info.U32(at, static_cast<uint32_t>(count));

        return at;
    };
    const auto entry = [&](size_t at, size_t i, uint16_t type, size_t size)
    {
        const size_t e = info.Add(size);
        info.Ref(at + 4 + 8 * i, type, e, at);

        return e;
    };

    const size_t sounds = table(0, 0x2100, spec.sounds.size());
    for (size_t i = 0; i < spec.sounds.size(); ++i)
    {
        const SoundSpec& sound = spec.sounds[i];
        const size_t s = entry(sounds, i, 0x2200, 20);
        info.U32(s, sound.file);
        info.U32(s + 4, 0x04000000 | sound.player);
        info.U8(s + 8, sound.volume);
        std::vector<std::pair<int, uint32_t>> values = {{0, static_cast<uint32_t>(i)}};
        if (sound.pan)
        {
            values.push_back({1, *sound.pan});
        }
        if (sound.priority)
        {
            values.push_back({2, *sound.priority});
        }
        info.Options(values);

        const size_t detail = info.Add(12);
        info.Ref(s + 12, static_cast<uint16_t>(sound.kind), detail, s);
        if (sound.kind != SoundSpec::kSequence)
        {
            continue;
        }

        // The detail: a reference to the bank table, the allocation of tracks, and the options right after them.
        info.U32(detail + 8, sound.allocate_track_flags);
        std::vector<std::pair<int, uint32_t>> options;
        if (sound.start_offset)
        {
            options.push_back({0, *sound.start_offset});
        }
        if (sound.channel_priority)
        {
            options.push_back({1, *sound.channel_priority});
        }
        info.Options(options);

        const size_t banks = info.Add(4 + 4 * sound.banks.size());
        info.Ref(detail, 0x0100, banks, detail);
        info.U32(banks, static_cast<uint32_t>(sound.banks.size()));
        for (size_t k = 0; k < sound.banks.size(); ++k)
        {
            info.U32(banks + 4 + 4 * k, 0x03000000 | sound.banks[k]);
        }
    }

    table(1, 0x2104, 0);
    const size_t banks = table(2, 0x2101, spec.banks.size());
    for (size_t i = 0; i < spec.banks.size(); ++i)
    {
        info.U32(entry(banks, i, 0x2206, 12), spec.banks[i].second);
    }
    const size_t wave_archives = table(3, 0x2103, spec.wave_archives.size());
    for (size_t i = 0; i < spec.wave_archives.size(); ++i)
    {
        info.U32(entry(wave_archives, i, 0x2207, 12), spec.wave_archives[i].second);
    }
    const size_t groups = table(4, 0x2105, spec.groups.size());
    for (size_t i = 0; i < spec.groups.size(); ++i)
    {
        info.U32(entry(groups, i, 0x2208, 8), spec.groups[i].second);
    }
    const size_t players = table(5, 0x2102, spec.players.size());
    for (size_t i = 0; i < spec.players.size(); ++i)
    {
        info.U32(entry(players, i, 0x2209, 8), spec.players[i].second);
    }

    const size_t file_table = table(6, 0x2106, spec.files.size());
    for (size_t i = 0; i < spec.files.size(); ++i)
    {
        const FileSpec& file = spec.files[i];
        const size_t f = entry(file_table, i, 0x220A, 8);
        if (!file.external.empty())
        {
            const size_t path = info.Add(file.external.size() + 1);
            std::memcpy(info.data.data() + path, file.external.data(), file.external.size());
            info.Align(4);
            info.Ref(f, 0x220D, path, f);
            continue;
        }

        const size_t where = info.Add(12);
        info.Ref(f, 0x220C, where, f);
        if (file.group_only)
        {
            info.NullRef(where, 0x1F00);
            info.U32(where + 8, 0xFFFFFFFF);
        }
        else
        {
            info.SizedRef(where, 0x1F00, file_offsets[i], 0, file.bytes.size());
        }
    }

    const size_t player_info = info.Add(16);
    info.Ref(7 * 8, 0x220B, player_info, 0);

    std::vector<Block> blocks;
    if (spec.names)
    {
        blocks.push_back({0x2000, "STRG", strg.data});
    }
    blocks.push_back({0x2001, "INFO", info.data});
    blocks.push_back({0x2002, "FILE", files.data});

    return File(big_endian, "FSAR", 0x00020200, blocks);
}

} // namespace fsar_files
