// SPDX-License-Identifier: MIT

// The CSF file format (see format.h and docs/csf.md).

#include "csf/format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"
#include "xsflib/text.h"

#include "csf/ntdlib_cafe.h"

namespace csf
{

using xsflib::psf::Hex;
using xsflib::psf::PadTo4;
using xsflib::psf::Put32;
using xsflib::psf::PutPath;
using xsflib::psf::ReadPath;

std::vector<uint8_t> ProcessDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    Put32(v, kMagic);
    Put32(v, version);
    Put32(v, flags);
    Put32(v, output_mode);
    if (version == kVersion2)
    {
        Put32(v, driver);
    }
    PutPath(v, executable_path);
    PutPath(v, archive_path);
    PadTo4(v);
    Put32(v, static_cast<uint32_t>(profile.size()));
    for (const uint32_t word : profile)
    {
        Put32(v, word);
    }

    return v;
}

std::optional<ProcessDescriptor> ProcessDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 16 || xsflib::LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ProcessDescriptor d;
    d.version = xsflib::LoadLe32(&data[4]);
    d.flags = xsflib::LoadLe32(&data[8]);
    d.output_mode = xsflib::LoadLe32(&data[12]);
    if (d.version != kVersion && d.version != kVersion2)
    {
        return d;
    }

    std::size_t pos = 16;
    if (d.version == kVersion2)
    {
        if (data.size() < 20)
        {
            return std::nullopt;
        }

        d.driver = xsflib::LoadLe32(&data[16]);
        pos = 20;
    }

    const std::optional<std::string> executable = ReadPath(data, pos);
    const std::optional<std::string> archive = executable ? ReadPath(data, pos) : std::nullopt;
    pos = (pos + 3) & ~std::size_t{3};
    if (!archive || pos + 4 > data.size())
    {
        return std::nullopt;
    }

    d.executable_path = *executable;
    d.archive_path = *archive;
    const uint32_t words = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (words > (data.size() - pos) / 4)
    {
        return std::nullopt;
    }

    for (uint32_t i = 0; i < words; ++i)
    {
        d.profile.push_back(xsflib::LoadLe32(&data[pos + 4 * i]));
    }

    return d;
}

std::optional<std::string> ProcessDescriptor::Check() const
{
    if (version == kVersion)
    {
        if (auto err = xsflib::psf::CheckDescriptorHeader("process", version, flags, output_mode, 0))
        {
            return err;
        }

        if (profile.size() != kProfileWords)
        {
            return "the process descriptor's game profile has " + std::to_string(profile.size()) +
                   " words; version 1 has " + std::to_string(kProfileWords);
        }

        return std::nullopt;
    }

    // Version 2 takes version 1's flags and output mode. Each driver checks its profile's words.
    if (auto err = xsflib::psf::CheckDescriptorVersion("process", version, kVersion2))
    {
        return err;
    }
    if (auto err = xsflib::psf::CheckDescriptorFlags("process", flags, 0))
    {
        return err;
    }
    if (auto err = xsflib::psf::CheckDescriptorOutputMode("process", output_mode))
    {
        return err;
    }
    if (driver >= kGameDriverKinds)
    {
        return "the process descriptor names game-mode driver " + std::to_string(driver) +
               ", which this player doesn't have";
    }

    return std::nullopt;
}

namespace
{

// The track volumes after the effects: the count of the sounds, then each sound's item id and its 16 volumes.
constexpr std::size_t kSoundTrackVolumesBytes = 4 + 4 * SoundTrackVolumes::kTracks;

// Writes the track volumes (ArchiveDescriptor::kFlagTrackVolumes).
void PutTrackVolumes(std::vector<uint8_t>& v, const std::vector<SoundTrackVolumes>& sounds)
{
    Put32(v, static_cast<uint32_t>(sounds.size()));
    for (const SoundTrackVolumes& sound : sounds)
    {
        Put32(v, sound.sound);
        for (const float volume : sound.volumes)
        {
            Put32(v, std::bit_cast<uint32_t>(volume));
        }
    }
}

// Reads the track volumes at `pos`, the end of the effects. False if `data` ends first.
bool ReadTrackVolumes(const std::vector<uint8_t>& data, std::size_t pos, std::vector<SoundTrackVolumes>& sounds)
{
    if (pos > data.size() || data.size() - pos < 4)
    {
        return false;
    }

    const uint32_t count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    if (count > (data.size() - pos) / kSoundTrackVolumesBytes)
    {
        return false;
    }

    sounds.resize(count);
    for (SoundTrackVolumes& sound : sounds)
    {
        sound.sound = xsflib::LoadLe32(&data[pos]);
        for (std::size_t t = 0; t < SoundTrackVolumes::kTracks; ++t)
        {
            sound.volumes[t] = std::bit_cast<float>(xsflib::LoadLe32(&data[pos + 4 + 4 * t]));
        }
        pos += kSoundTrackVolumesBytes;
    }

    return true;
}

// Why a version 1 player can't play a descriptor's flags and track volumes, or nothing. The only flag is
// kFlagTrackVolumes. With it, each sound is listed once, with 16 volumes that are finite numbers of 0 or more.
std::optional<std::string> CheckTrackVolumes(const ArchiveDescriptor& d)
{
    if ((d.flags & ~ArchiveDescriptor::kFlagTrackVolumes) != 0)
    {
        return "the archive descriptor's flags are " + Hex(d.flags) + "; version 1 has only bit 0 (track volumes)";
    }

    if ((d.flags & ArchiveDescriptor::kFlagTrackVolumes) == 0 && !d.track_volumes.empty())
    {
        return std::string("the archive descriptor lists track volumes without its flag for them (bit 0)");
    }

    for (std::size_t i = 0; i < d.track_volumes.size(); ++i)
    {
        const SoundTrackVolumes& sound = d.track_volumes[i];
        const auto same_sound = [&sound](const SoundTrackVolumes& other)
        {
            return other.sound == sound.sound;
        };
        if (std::any_of(d.track_volumes.begin() + static_cast<std::ptrdiff_t>(i) + 1, d.track_volumes.end(),
                        same_sound))
        {
            return "the archive descriptor lists the track volumes of sound " + Hex(sound.sound, 8) + " more than once";
        }

        const auto volume_ok = [](float volume)
        {
            return std::isfinite(volume) && volume >= 0.0f;
        };
        if (!std::all_of(sound.volumes.begin(), sound.volumes.end(), volume_ok))
        {
            return "a track volume of sound " + Hex(sound.sound, 8) +
                   " in the archive descriptor isn't a finite number of 0 or more";
        }
    }

    return std::nullopt;
}

} // namespace

const SoundTrackVolumes* ArchiveDescriptor::TrackVolumesOf(uint32_t sound) const
{
    for (const SoundTrackVolumes& entry : track_volumes)
    {
        if (entry.sound == sound)
        {
            return &entry;
        }
    }

    return nullptr;
}

std::vector<uint8_t> ArchiveDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    xsflib::psf::PutArchiveStart(v, *this);
    Put32(v, ax_settings);
    Put32(v, sdk_version);
    xsflib::psf::PutArchivePaths(v, *this);

    // A descriptor without effects or track volumes ends after the paths. The track volumes follow the effects, whose
    // count is then there even when it's 0.
    const bool track_volumes_follow = (flags & kFlagTrackVolumes) != 0;
    if (effects.empty() && !track_volumes_follow)
    {
        return v;
    }

    Put32(v, static_cast<uint32_t>(effects.size()));
    for (const nwsnd::EffectSetup& effect : effects)
    {
        v.push_back(effect.device);
        v.push_back(effect.bus);
        v.push_back(static_cast<uint8_t>(effect.type));
        v.push_back(static_cast<uint8_t>(effect.params.size()));
        Put32(v, static_cast<uint32_t>(effect.sounds.size()));
        for (const uint32_t word : effect.params)
        {
            Put32(v, word);
        }
        for (const uint32_t sound : effect.sounds)
        {
            Put32(v, sound);
        }
    }

    if (track_volumes_follow)
    {
        PutTrackVolumes(v, track_volumes);
    }

    return v;
}

std::optional<ArchiveDescriptor> ArchiveDescriptor::Parse(const std::vector<uint8_t>& data)
{
    ArchiveDescriptor d;
    if (!xsflib::psf::ReadArchiveStart(data, 28, d))
    {
        return std::nullopt;
    }

    d.ax_settings = xsflib::LoadLe32(&data[20]);
    d.sdk_version = xsflib::LoadLe32(&data[24]);
    std::size_t pos = 28;
    if (!xsflib::psf::ReadArchivePaths(data, pos, d))
    {
        return std::nullopt;
    }

    // The effects, after the paths' padding. With the flag for track volumes, the effects' count is there, and the
    // volumes follow the effects.
    const bool track_volumes_follow = (d.flags & kFlagTrackVolumes) != 0;
    pos = (pos + 3) & ~std::size_t{3};
    if (pos >= data.size() && !track_volumes_follow)
    {
        return d;
    }
    if (pos > data.size() || data.size() - pos < 4)
    {
        return std::nullopt;
    }

    const uint32_t count = xsflib::LoadLe32(&data[pos]);
    pos += 4;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (data.size() - pos < 8)
        {
            return std::nullopt;
        }

        nwsnd::EffectSetup effect;
        effect.device = data[pos];
        effect.bus = data[pos + 1];
        effect.type = static_cast<nwsnd::EffectType>(data[pos + 2]);
        const uint32_t words = data[pos + 3];
        const uint32_t sounds = xsflib::LoadLe32(&data[pos + 4]);
        pos += 8;
        if (sounds > (data.size() - pos) / 4 || words > (data.size() - pos) / 4 - sounds)
        {
            return std::nullopt;
        }

        for (uint32_t w = 0; w < words; ++w)
        {
            effect.params.push_back(xsflib::LoadLe32(&data[pos + 4 * w]));
        }
        pos += 4 * std::size_t{words};
        for (uint32_t s = 0; s < sounds; ++s)
        {
            effect.sounds.push_back(xsflib::LoadLe32(&data[pos + 4 * s]));
        }
        pos += 4 * std::size_t{sounds};

        const uint32_t needed = nwsnd::EffectParamWords(effect.type);
        if (needed != 0 && words >= needed)
        {
            d.effects.push_back(std::move(effect));
        }
    }

    if (track_volumes_follow && !ReadTrackVolumes(data, pos, d.track_volumes))
    {
        return std::nullopt;
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    // The archive's start, with the flag that version 1 has (CheckTrackVolumes) left out of xsflib's check of the
    // flags.
    ArchiveDescriptor start = *this;
    start.flags = 0;
    if (auto err = xsflib::psf::CheckArchiveStart(start, kDefaultVoices))
    {
        return err;
    }

    if (auto err = CheckTrackVolumes(*this))
    {
        return err;
    }

    if ((ax_settings & ~(kAxRenderer48k | kAxUpsampleAfterFinalMix)) != 0)
    {
        return "the archive descriptor's AX settings are " + Hex(ax_settings) +
               "; version 1 supports only bits 0 and 1";
    }

    for (const nwsnd::EffectSetup& effect : effects)
    {
        if (effect.device > nwsnd::kEffectDeviceDrc || effect.bus > 2 || effect.params.size() > 0xFF)
        {
            return "an effect in the archive descriptor is on device " + std::to_string(effect.device) + ", bus " +
                   std::to_string(effect.bus) + ", with " + std::to_string(effect.params.size()) +
                   " words; version 1 has devices 0 and 1, buses 0 to 2, and at most 255 words";
        }
    }

    return std::nullopt;
}

std::optional<xsflib::psf::Variables> TagPorts(const Tags& tags)
{
    std::optional<xsflib::psf::Variables> ports = TagVariables(tags);
    if (!ports || ports->rbegin()->first >= kMaxPorts + kJAudio1States)
    {
        return std::nullopt;
    }

    return ports;
}

std::optional<xsflib::psf::Variables> TagVariables(const Tags& tags)
{
    const auto tag = tags.find(kVariablesTag);
    if (tag == tags.end())
    {
        return std::nullopt;
    }

    return ParseTagVariables(tag->second);
}

std::optional<xsflib::psf::Variables> ParseTagVariables(std::string_view text)
{
    // The separators and numbers of xsflib::psf::ParseVariables, which takes no track.
    constexpr std::string_view kSeparators = ", \t\r\n";
    xsflib::psf::Variables variables;
    std::size_t at = 0;
    while ((at = text.find_first_not_of(kSeparators, at)) != std::string_view::npos)
    {
        const std::size_t end = std::min(text.find_first_of(kSeparators, at), text.size());
        std::string_view part = text.substr(at, end - at);
        at = end;

        std::optional<int64_t> track;
        if (const std::size_t colon = part.find(':'); colon != std::string_view::npos)
        {
            track = xsflib::ParseInteger(part.substr(0, colon));
            if (!track || *track < 0 || *track >= kSequenceTracks)
            {
                return std::nullopt;
            }
            part = part.substr(colon + 1);
        }

        const std::optional<xsflib::Assignment> assignment = xsflib::ParseAssignment(part);
        if (!assignment || assignment->value < INT16_MIN || assignment->value > INT16_MAX)
        {
            return std::nullopt;
        }

        const int64_t index = assignment->index;
        const int64_t first = track ? kMaxVariables : 0;
        const int64_t last = track ? kMaxVariables + kTrackVariables : kMaxVariables;
        if (index < first || index >= last)
        {
            return std::nullopt;
        }

        const int key =
            track ? TrackVariableKey(static_cast<int>(*track), static_cast<int>(index)) : static_cast<int>(index);
        variables[key] = static_cast<int16_t>(assignment->value);
    }

    if (variables.empty())
    {
        return std::nullopt;
    }

    return variables;
}

std::string FormatTagVariables(const xsflib::psf::Variables& variables)
{
    std::string text;
    for (const auto& [key, value] : variables)
    {
        text += text.empty() ? "" : ", ";
        if (key >= kMaxVariables)
        {
            const int track = (key - kMaxVariables) / kTrackVariables;
            text += std::to_string(track) + ":" + std::to_string(key - track * kTrackVariables);
        }
        else
        {
            text += std::to_string(key);
        }
        text += "=" + std::to_string(value);
    }

    return text;
}

std::optional<std::vector<uint32_t>> WaveGroups(const LoadedSet& set)
{
    std::vector<uint32_t> groups;
    const std::vector<uint8_t>* file = set.File(kWavesPath);
    if (file == nullptr)
    {
        return groups;
    }
    if (file->size() % 4 != 0)
    {
        return std::nullopt;
    }

    for (std::size_t i = 0; i < file->size(); i += 4)
    {
        groups.push_back(xsflib::LoadLe32(file->data() + i));
    }

    return groups;
}

std::vector<uint8_t> WavesFile(std::span<const uint32_t> groups)
{
    std::vector<uint8_t> file(groups.size() * 4);
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        xsflib::StoreLe32(file.data() + 4 * i, groups[i]);
    }

    return file;
}

std::optional<uint32_t> MutedChildren(const LoadedSet& set)
{
    const std::vector<uint8_t>* file = set.File(kMutedPath);
    if (file == nullptr)
    {
        return 0;
    }
    if (file->size() != 4)
    {
        return std::nullopt;
    }

    return xsflib::LoadLe32(file->data());
}

std::vector<uint8_t> MutedFile(uint32_t children)
{
    std::vector<uint8_t> file(4);
    xsflib::StoreLe32(file.data(), children);

    return file;
}

std::optional<std::vector<ChildVolume>> ChildVolumes(const LoadedSet& set)
{
    const std::vector<uint8_t>* file = set.File(kVolumesPath);
    if (file == nullptr)
    {
        return std::vector<ChildVolume>{};
    }
    if (file->size() % 8 != 0)
    {
        return std::nullopt;
    }

    std::vector<ChildVolume> groups;
    for (std::size_t i = 0; i < file->size(); i += 8)
    {
        groups.push_back({xsflib::LoadLe32(&(*file)[i]), std::bit_cast<float>(xsflib::LoadLe32(&(*file)[i + 4]))});
    }

    return groups;
}

std::vector<uint8_t> VolumesFile(std::span<const ChildVolume> groups)
{
    std::vector<uint8_t> file(8 * groups.size());
    for (std::size_t i = 0; i < groups.size(); i++)
    {
        xsflib::StoreLe32(&file[8 * i], groups[i].children);
        xsflib::StoreLe32(&file[8 * i + 4], std::bit_cast<uint32_t>(groups[i].volume));
    }

    return file;
}

std::optional<std::vector<SectionStatus>> SectionStatuses(const LoadedSet& set)
{
    const std::vector<uint8_t>* file = set.File(kSectionsPath);
    if (file == nullptr)
    {
        return std::vector<SectionStatus>{};
    }
    if (file->size() % 2 != 0)
    {
        return std::nullopt;
    }

    std::vector<SectionStatus> statuses;
    for (std::size_t i = 0; i < file->size(); i += 2)
    {
        statuses.push_back({(*file)[i], (*file)[i + 1]});
    }

    return statuses;
}

std::vector<uint8_t> SectionsFile(std::span<const SectionStatus> statuses)
{
    std::vector<uint8_t> file;
    for (const SectionStatus& status : statuses)
    {
        file.push_back(status.section);
        file.push_back(status.status);
    }

    return file;
}

std::optional<std::string> ContentPath(std::string_view path)
{
    if (!path.starts_with(kContentFolder))
    {
        return std::nullopt;
    }

    return std::string(path.substr(kContentFolder.size()));
}

std::map<std::string, std::vector<uint8_t>> ContentFiles(const std::vector<FileChunk>& files)
{
    std::map<std::string, std::vector<uint8_t>> content;
    for (const FileChunk& file : files)
    {
        if (const std::optional<std::string> path = ContentPath(file.path))
        {
            content[*path] = file.data;
        }
    }

    return content;
}

} // namespace csf
