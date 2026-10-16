// SPDX-License-Identifier: MIT

// The CSF file format (docs/csf.md): PSF container, program chunks (FILE and SND), process and archive descriptors,
// tags.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/set.h"
#include "xsflib/psf/streaming.h"

#include "csf/ntdlib_cafe.h"

namespace csf
{

// The PSF container, from xsflib.
using xsflib::psf::FileReader;
using xsflib::psf::FormatTime;
using xsflib::psf::ReadWholeFile;
using xsflib::psf::Tags;
using xsflib::psf::WritePsf;

// PSF version byte of CSF. See docs/csf.md ("Version byte") before changing it.
constexpr uint8_t kPsfVersionCsf = 0x28;

// The output's sample rate: 48,000 Hz exactly. Players report it, and the `length` and `fade` tags count at it
// (docs/csf.md, "Tags").
constexpr int kPlayerSampleRate = 48'000;

// Output modes: 0 mono, 1 stereo, 2 surround. Version 1 plays stereo only.
constexpr uint32_t kOutputStereo = 1;

// AX's settings, in the archive descriptor and the game profile: the 48 kHz renderer rather than the 32 kHz one, and
// each device's output reaching 48 kHz after the final mix stage rather than before it.
constexpr uint32_t kAxRenderer48k = 1;
constexpr uint32_t kAxUpsampleAfterFinalMix = 2;

// The number of words in a version 1 game profile, which is the nw::snd driver's (docs/csf.md, "Driver").
constexpr uint32_t kProfileWords = 32;

// The game-mode drivers that a version 2 process descriptor names (docs/csf.md, "Process descriptor"): NintendoWare's
// nw::snd, JAudio2 with a game's Z2 sound layer, and JAudio1. A version 1 descriptor plays with the nw::snd driver.
enum class GameDriverKind : uint32_t
{
    kNwSnd = 0,
    kJAudio2 = 1,
    kJAudio1 = 2,
};

constexpr uint32_t kGameDriverKinds = 3;

// The process descriptor: the reserved area of a game-mode set (docs/csf.md, "Process descriptor").
struct ProcessDescriptor
{
    static constexpr uint32_t kMagic = 0x50465343; // "CSFP"
    static constexpr uint32_t kVersion = 1;

    // Version 2 names the driver, after the output mode.
    static constexpr uint32_t kVersion2 = 2;

    // A descriptor of another version than 1 and 2 gives only its header's fields. Check then refuses it.
    static std::optional<ProcessDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t flags = 0;
    uint32_t output_mode = kOutputStereo;
    uint32_t driver = 0;         // a GameDriverKind, in version 2. Version 1 has none and plays with nw::snd's.
    std::string executable_path; // FILE chunk holding the game's executable (an RPX file)
    std::string archive_path;    // FILE chunk holding the driver's main sound file, such as nw::snd's .bfsar
    std::vector<uint32_t> profile;
};

// Descriptor of an archive-mode set: the sound archive is played by CSF's model of the SDK's sound library (nw::snd and
// AX) instead of by the game's code (docs/csf.md, "Archive mode").
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465343; // "CSFA"
    static constexpr uint32_t kVersion = 1;

    // nw::snd's voice count when the game doesn't change it, and the most AX has.
    static constexpr uint32_t kDefaultVoices = 96;

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t flags = 0;
    uint32_t output_mode = kOutputStereo;
    uint32_t voice_count = kDefaultVoices;
    uint32_t ax_settings = kAxUpsampleAfterFinalMix;
    uint32_t sdk_version = 0;             // major * 10000 + minor * 100 + micro, or 0 if unknown
    std::string archive_path;             // FILE chunk holding the sound archive (.bfsar)
    std::vector<std::string> group_paths; // FILE chunks holding group files (.bfgrp) the game keeps outside it

    // The effects that the game puts on the aux buses (docs/csf.md, "Effects"). A descriptor without effects ends
    // after the paths. Parse leaves out an effect of a type that it doesn't know, or with fewer words than the type
    // has.
    std::vector<nwsnd::EffectSetup> effects;
};

// The program and its chunks (docs/csf.md, "Program: chunks"), from xsflib: FILE chunks, each a file of the game's
// title by its path in the title, and a SND chunk with the sound's nw::snd item id. The program holds each file once.
using ProgramBuilder = xsflib::psf::StreamingProgramBuilder;
using xsflib::psf::FileChunk;

// The CSF format for xsflib's set loader: both modes, and each file held once.
struct FormatTraits
{
    using ProcessDescriptor = csf::ProcessDescriptor;
    using ArchiveDescriptor = csf::ArchiveDescriptor;

    static constexpr uint8_t kPsfVersion = kPsfVersionCsf;
    static constexpr const char* kName = "CSF";
    static constexpr bool kStreaming = true;
};

// A complete set: the files of a file and its libraries in loading order, the effective descriptor (exactly one of
// `descriptor`, game mode's, and `archive`), the sound, and the tags of the top-level file.
using LoadedSet = xsflib::psf::LoadedSet<FormatTraits>;

// The FILE chunk that lists the wave groups that a JAudio driver loads before it starts the sound (docs/csf.md,
// "Wave groups"): a little-endian word for each group, the wave bank in the high half and the group in the low half.
inline constexpr char kWavesPath[] = "waves";

// The wave groups in `set`'s waves file: none without the file, or nothing if its size isn't a multiple of 4.
std::optional<std::vector<uint32_t>> WaveGroups(const LoadedSet& set);

// Loads a CSF/miniCSF file with its _lib/_libN chain. Paths in _lib tags are relative to the directory of the file that
// names them: its path up to the last of `separators`. Returns an error for a set a version 1 player can't play: one
// without a descriptor or a sound, or whose descriptor fails its Check().
inline std::optional<std::string> LoadSet(const std::string& path, const FileReader& reader, LoadedSet& out,
                                          std::string_view separators = "/\\")
{
    return xsflib::psf::LoadSet(path, reader, out, separators);
}

} // namespace csf
