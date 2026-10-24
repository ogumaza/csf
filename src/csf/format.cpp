// SPDX-License-Identifier: MIT

// The CSF file format (see format.h and docs/csf.md).

#include "csf/format.h"

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

std::vector<uint8_t> ArchiveDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    xsflib::psf::PutArchiveStart(v, *this);
    Put32(v, ax_settings);
    Put32(v, sdk_version);
    xsflib::psf::PutArchivePaths(v, *this);
    if (effects.empty())
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

    // The effects, after the paths' padding.
    pos = (pos + 3) & ~std::size_t{3};
    if (pos >= data.size())
    {
        return d;
    }
    if (data.size() - pos < 4)
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

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    if (auto err = xsflib::psf::CheckArchiveStart(*this, kDefaultVoices))
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
    const auto tag = tags.find(kVariablesTag);
    if (tag == tags.end())
    {
        return std::nullopt;
    }

    std::optional<xsflib::psf::Variables> ports = xsflib::psf::ParseVariables(tag->second);
    if (!ports || ports->rbegin()->first >= kMaxPorts)
    {
        return std::nullopt;
    }

    return ports;
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
