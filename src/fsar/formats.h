// SPDX-License-Identifier: MIT

// Parsers for the NintendoWare for Cafe sound formats: BFSAR (sound archive) and the FSEQ, FBNK, FWAR, FWAV and FGRP
// files it holds. They're the successors of the 3DS's CTR formats. They're laid out the same way, with the same block
// and reference types. Each file is in the byte order of its byte order mark: big-endian on the Wii U. CSF was checked
// with FSAR versions 2.0.0, 2.1.0 and 2.2.0. A version word holds the major, minor and micro versions in its three low
// bytes. The 3DS's version word holds four versions from the top byte down.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace csf::fsar
{

// FWAV: a wave.
enum class WaveEncoding : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kDspAdpcm = 2
};

struct DspAdpcmInfo
{
    std::array<int16_t, 16> coefs{};
    uint16_t pred_scale = 0;
    int16_t yn1 = 0, yn2 = 0;
    uint16_t loop_pred_scale = 0;
    int16_t loop_yn1 = 0, loop_yn2 = 0;
};

struct WaveChannel
{
    std::span<const uint8_t> data; // sample data from the channel's start to the end of DATA
    std::optional<DspAdpcmInfo> adpcm;
};

// A parsed FWAV. The channels' data points into the file passed to Parse.
struct Wave
{
    static Wave Parse(std::span<const uint8_t> file);

    WaveEncoding encoding = WaveEncoding::kPcm16;
    bool loop = false;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0; // also the length when not looping
    std::vector<WaveChannel> channels;
};

// FWAR: a wave archive. The waves point into the file passed to Parse.
struct WaveArchive
{
    static WaveArchive Parse(std::span<const uint8_t> file);

    std::vector<std::span<const uint8_t>> waves;
};

// FBNK: a bank of instruments.
struct Adshr
{
    // When a region has none: the 3DS's nw::snd fills it with 127s. The Wii U's nw::snd has yet to be checked against
    // that.
    uint8_t attack = 127, decay = 127, sustain = 127, hold = 127, release = 127;
};

struct VelocityRegion
{
    uint32_t wave_id_index = 0;
    uint8_t original_key = 60;
    uint8_t volume = 127;
    uint8_t pan = 64;
    float pitch = 1.0f;
    bool ignore_note_off = false;
    uint8_t key_group = 0;
    uint8_t interpolation_type = 0;
    Adshr adshr;
};

// A bank's key and velocity ranges come from its region tables. A direct table's one region covers every value, even a
// velocity past 127 (GetRegion, 0x022BBA30). A track's velocity range can take a velocity that far.
struct KeyRegion
{
    // A velocity range and the region it plays, if any.
    struct Vel
    {
        int lo, hi;
        std::optional<VelocityRegion> region;
    };

    int lo = 0, hi = 127;
    std::vector<Vel> velocities;
};

struct Instrument
{
    // Finds the velocity region for a key and velocity.
    const VelocityRegion* Find(int key, int velocity) const;

    std::vector<KeyRegion> keys;
};

struct WaveId
{
    uint32_t wave_archive_item = 0; // 0x05xxxxxx item ID
    uint32_t index = 0;
};

struct Bank
{
    static Bank Parse(std::span<const uint8_t> file);

    std::vector<WaveId> waves;
    std::vector<std::optional<Instrument>> instruments;
};

// FSEQ: a sequence file. Its bytecode may hold several sequences. Each starts at one of its labels.
struct Sequence
{
    struct Label
    {
        std::string name;
        uint32_t offset; // into `data`
    };

    static Sequence Parse(std::span<const uint8_t> file);

    std::span<const uint8_t> data; // the DATA block's payload: the bytecode
    std::vector<Label> labels;
};

// FSAR: the sound archive. Its items have IDs whose top byte says what they are: 0x01 sounds, 0x02 sound groups, 0x03
// banks, 0x04 players, 0x05 wave archives and 0x06 groups.
enum class SoundType
{
    kSequence,
    kStream,
    kWaveSound,
    kUnknown
};

struct SoundInfo
{
    std::string name; // from the string table, or made up from the index (SEQ_12) when the archive has none
    uint32_t file_id = 0;
    uint32_t player_id = 0; // 0x04xxxxxx item ID
    uint8_t volume = 127;
    SoundType type = SoundType::kUnknown;

    // From the sound's optional parameters. The defaults stand when it doesn't have them.
    uint8_t pan_mode = 0, pan_curve = 0;
    uint8_t player_priority = 64;
    uint8_t actor_player_id = 0;

    // Sequence sounds only.
    std::vector<uint32_t> banks; // bank item IDs (0x03xxxxxx) by slot, or 0xFFFFFFFF for an empty slot
    uint32_t allocate_track_flags = 1;
    uint32_t start_offset = 0;
    uint8_t channel_priority = 64;
    bool release_priority_fix = false;
};

struct BankInfo
{
    std::string name;
    uint32_t file_id = 0;
};

struct WaveArchiveInfo
{
    std::string name;
    uint32_t file_id = 0;
};

struct PlayerInfo
{
    std::string name;
    uint32_t playable_sound_max = 0;
};

struct GroupInfo
{
    std::string name;
    uint32_t file_id = 0; // 0xFFFFFFFF for a group the game keeps outside the archive (see AddGroupFile)
};

// A parsed BFSAR. It keeps the archive's bytes and those of the group files added to it. The spans that FileData
// returns therefore stay valid as long as the archive does.
class SoundArchive
{
public:
    static SoundArchive Load(std::vector<uint8_t> bytes);

    // The version from the header: 0x00020200 for 2.2.0.
    uint32_t Version() const
    {
        return version_;
    }

    const std::vector<SoundInfo>& Sounds() const
    {
        return sounds_;
    }

    const std::vector<BankInfo>& Banks() const
    {
        return banks_;
    }

    const std::vector<WaveArchiveInfo>& WaveArchives() const
    {
        return wave_archives_;
    }

    const std::vector<PlayerInfo>& Players() const
    {
        return players_;
    }

    const std::vector<GroupInfo>& Groups() const
    {
        return groups_;
    }

    uint32_t FileCount() const
    {
        return static_cast<uint32_t>(files_.size());
    }

    std::optional<uint32_t> FindSound(const std::string& name) const;

    // Adds a group file that the game keeps outside the archive. SoundMaker can leave a group out of the archive for
    // the game to load itself. The archive then lists the group with no file (0xFFFFFFFF). The files the group embeds
    // that the archive lacks can be read with FileData afterwards. Throws std::runtime_error when `bytes` isn't a group
    // file.
    void AddGroupFile(std::vector<uint8_t> bytes);

    // A file's contents. The archive may hold the file on its own or inside a group, or a group file added to the
    // archive may hold it. The span is empty for an external file, for one that only a group outside the archive holds,
    // and for one past the end of a truncated archive.
    std::span<const uint8_t> FileData(uint32_t file_id) const;

    // An external file's path, relative to the archive's folder ("stream/BGM.bfstm"), or "" for any other file.
    std::string ExternalPath(uint32_t file_id) const;

    // The archive is shorter than its header says, or files lie past its end.
    bool Truncated() const
    {
        return truncated_;
    }

private:
    struct FileEntry
    {
        uint32_t offset = 0, size = 0;
        bool internal = false;
        std::string external_path;
        uint32_t source = 0; // where the bytes are: 0 for the archive, n for the nth group file added
    };

    std::vector<uint8_t> bytes_;
    std::vector<std::vector<uint8_t>> group_files_;
    uint32_t version_ = 0;
    std::vector<SoundInfo> sounds_;
    std::vector<BankInfo> banks_;
    std::vector<WaveArchiveInfo> wave_archives_;
    std::vector<PlayerInfo> players_;
    std::vector<GroupInfo> groups_;
    std::vector<FileEntry> files_;
    bool truncated_ = false;
};

} // namespace csf::fsar
