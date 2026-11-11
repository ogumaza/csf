// SPDX-License-Identifier: MIT

// The CSF file format (docs/csf.md): PSF container, program chunks (FILE and SND), process and archive descriptors,
// tags.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/cafe/snd/effect_setup.h"
#include "xsflib/psf/descriptor.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/set.h"
#include "xsflib/psf/streaming.h"

#include "csf/jaudio2_files.h"
#include "csf/ntdlib_cafe.h"

namespace csf
{

// The PSF container, from xsflib.
using xsflib::psf::FileReader;
using xsflib::psf::FormatTime;
using xsflib::psf::Tags;
using xsflib::psf::WritePsf;

// PSF version byte of CSF. See docs/csf.md ("Version byte") before changing it.
constexpr uint8_t kPsfVersionCsf = 0x28;

// The output's sample rate: 48,000 Hz exactly. Players report it, and the `length` and `fade` tags count at it
// (docs/csf.md, "Tags").
constexpr int kPlayerSampleRate = 48'000;

// The tag that sets ports of a JAudio1 sound, or variables of an nw::snd sequence, before the sound starts
// (docs/csf.md, "Tags"), as assignments N=V. For a JAudio1 sound, N is a port, from 0 to kMaxPorts - 1. For an nw::snd
// sequence, N is a variable, from 0 to kMaxVariables - 1: the sequence's below kSequenceVariables, and the global ones
// from it. An nw::snd sequence also takes "T:N=V": variable N of track T, 0 to kSequenceTracks - 1. N is then 32 to 47,
// as the track's commands number its variables.
constexpr char kVariablesTag[] = "csf_var";
constexpr int kMaxPorts = 16;
constexpr int kMaxVariables = 32;
constexpr int kSequenceVariables = 16;
constexpr int kSequenceTracks = 16;
constexpr int kTrackVariables = 16;

// A JAudio1 sound's csf_var also takes the game's state, from kMaxPorts up, for a profile that names the Zelda games'
// starters of music (game::JAudio1Profile::bgm_start). The driver sets the state before it starts music: 16 the boat's
// sail (setShipSailState), 17 Link on the boat (setLinkOnBoard), 18 the sword drawn (bgmSetSwordUsing) and 19 an enemy
// near (bgmNowBattle's distance). A value other than 0 sets each. Version 3 of the format adds them.
constexpr int kJAudio1States = 4;
constexpr int kSailState = 16;
constexpr int kOnBoardState = 17;
constexpr int kSwordState = 18;
constexpr int kNearState = 19;

// The ports of a JAudio piece's root track that hold its status and its section. The game writes a status to port 9.
// Twilight Princess HD's sequences write the section that they play to port 10.
constexpr int kStatusPort = 9;
constexpr int kSectionPort = 10;

// The track that the sound library's port functions take for a sound's root track.
constexpr uint32_t kRootTrack = 0xffffffff;

// The key of variable `variable` (32 to 47) of track `track` in xsflib::psf::Variables. The keys from kMaxVariables up
// hold the tracks' variables: track 0's, then track 1's, and on.
constexpr int TrackVariableKey(int track, int variable)
{
    return kMaxVariables + track * kTrackVariables + (variable - kMaxVariables);
}

// The ports and the game's state that the csf_var tag in `tags` sets, by number. Nothing without the tag, or if any
// part of the tag is malformed or out of range.
std::optional<xsflib::psf::Variables> TagPorts(const Tags& tags);

// The variables of an nw::snd sequence that the csf_var tag in `tags` sets: the sequence's and the global ones by
// number, and the tracks' by TrackVariableKey. Nothing without the tag, or if any part of the tag is malformed or out
// of range.
std::optional<xsflib::psf::Variables> TagVariables(const Tags& tags);

// Parses a csf_var tag's value, as TagVariables does.
std::optional<xsflib::psf::Variables> ParseTagVariables(std::string_view text);

// The csf_var tag for `variables` (TagVariables' keys): the assignments in increasing order of their keys, in decimal,
// separated by ", ". A track's variable is "T:N=V".
std::string FormatTagVariables(const xsflib::psf::Variables& variables);

// Output modes: 0 mono, 1 stereo, 2 surround. Version 1 plays stereo only.
constexpr uint32_t kOutputStereo = xsflib::psf::kDescriptorOutputStereo;

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

// The volumes that the game's code gives a sound's 16 tracks through the sound's handle as the sound starts: nw::snd's
// SetTrackVolume, track 0 first (docs/csf.md, "Track volumes"). 1 leaves a track as the sequence plays it. The sound
// is its nw::snd item id.
using SoundTrackVolumes = xsflib::psf::SoundTrackVolumes;

// Descriptor of an archive-mode set: the sound archive is played by CSF's model of the SDK's sound library (nw::snd and
// AX) instead of by the game's code (docs/csf.md, "Archive mode").
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465343; // "CSFA"
    static constexpr uint32_t kVersion = 1;

    // nw::snd's voice count when the game doesn't change it, and the most AX has.
    static constexpr uint32_t kDefaultVoices = 96;

    // The flag of a descriptor that lists the track volumes of sounds (`track_volumes`), after the effects. CSF 1.5.0
    // and older players refuse a descriptor with it.
    static constexpr uint32_t kFlagTrackVolumes = 1;

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    // The track volumes that the descriptor gives `sound`, an nw::snd item id, or null.
    const SoundTrackVolumes* TrackVolumesOf(uint32_t sound) const;

    uint32_t version = kVersion;
    uint32_t flags = 0;
    uint32_t output_mode = kOutputStereo;
    uint32_t voice_count = kDefaultVoices;
    uint32_t ax_settings = kAxUpsampleAfterFinalMix;
    uint32_t sdk_version = 0;             // major * 10000 + minor * 100 + micro, or 0 if unknown
    std::string archive_path;             // FILE chunk holding the sound archive (.bfsar)
    std::vector<std::string> group_paths; // FILE chunks holding group files (.bfgrp) the game keeps outside it

    // The effects that the game puts on the aux buses (docs/csf.md, "Effects"). A descriptor without effects ends after
    // the paths. Parse leaves out an effect of a type that it doesn't know, or with fewer words than the type has.
    std::vector<nwsnd::EffectSetup> effects;

    // With kFlagTrackVolumes: the track volumes of sounds, each sound once.
    std::vector<SoundTrackVolumes> track_volumes;
};

// The program and its chunks (docs/csf.md, "Program: chunks"), from xsflib: FILE chunks, each a file of the game's
// title by its path in the title, and a SND chunk with the sound's nw::snd item id. The program holds each file once.
using ProgramBuilder = xsflib::psf::StreamingProgramBuilder;
using xsflib::psf::FileChunk;

// The CSF format for xsflib's set loader: both modes, and each file held once.
struct SetFormat
{
    using ProcessDescriptor = csf::ProcessDescriptor;
    using ArchiveDescriptor = csf::ArchiveDescriptor;

    static constexpr uint8_t kPsfVersion = kPsfVersionCsf;
    static constexpr const char* kName = "CSF";
    static constexpr bool kStreaming = true;
};

// A complete set: the files of a file and its libraries in loading order, the effective descriptor (exactly one of
// `descriptor`, game mode's, and `archive`), the sound, and the tags of the top-level file. LoadSet loads a CSF or
// miniCSF file with its _lib and _libN chain. Paths in _lib tags are relative to the directory of the file that names
// them: its path up to the last of `separators`. It returns an error for a set a version 1 player can't play: one
// without a descriptor or a sound, or whose descriptor fails its Check().
using LoadedSet = xsflib::psf::LoadedSet<SetFormat>;
using xsflib::psf::LoadSet;

// The FILE chunk that lists the wave groups that a JAudio driver loads before it starts the sound (docs/csf.md, "Wave
// groups"): a little-endian word for each group, the wave bank in the high half and the group in the low half.
inline constexpr char kWavesPath[] = "waves";

// The wave groups in `set`'s waves file: none without the file, or nothing if its size isn't a multiple of 4.
std::optional<std::vector<uint32_t>> WaveGroups(const LoadedSet& set);

// The waves file that lists `groups`.
std::vector<uint8_t> WavesFile(std::span<const uint32_t> groups);

// The FILE chunk that gives the children of the sound's track 0 that the JAudio2 driver mutes after it starts the sound
// (docs/csf.md, "JAudio2 driver"): one little-endian word, bit n for child n.
inline constexpr char kMutedPath[] = "muted";

// The children in `set`'s muted file: 0 without the file, or nothing if the file isn't 4 bytes.
std::optional<uint32_t> MutedChildren(const LoadedSet& set);

// The muted file that gives `children`.
std::vector<uint8_t> MutedFile(uint32_t children);

// The FILE chunk that gives children of the sound's track 0 that the JAudio2 driver sets to another volume after it
// starts the sound (docs/csf.md, "Child volumes"). Each group is a little-endian word, bit n for child n, and the
// volume, a little-endian 32-bit float.
inline constexpr char kVolumesPath[] = "volumes";

// The groups in `set`'s volumes file: none without the file, or nothing if the file's size isn't a multiple of 8.
std::optional<std::vector<ChildVolume>> ChildVolumes(const LoadedSet& set);

// The volumes file that gives `groups`.
std::vector<uint8_t> VolumesFile(std::span<const ChildVolume> groups);

// The FILE chunk that gives the statuses that the JAudio2 driver writes to port 9 of the sound's root track in each
// frame, by the section on port 10 (docs/csf.md, "Statuses by section"). Each entry is two bytes: the section and the
// status.
inline constexpr char kSectionsPath[] = "sections";

// The entries in `set`'s sections file: none without the file, or nothing if the file's size isn't a multiple of 2.
std::optional<std::vector<SectionStatus>> SectionStatuses(const LoadedSet& set);

// The sections file that gives `statuses`.
std::vector<uint8_t> SectionsFile(std::span<const SectionStatus> statuses);

// The title's content folder. Game mode gives the game a set's files under it at /vol/content/ (docs/csf.md, "The
// emulated system").
inline constexpr std::string_view kContentFolder = "content/";

// The path under the content folder of the title's file at `path`, or nothing if the file isn't in that folder.
std::optional<std::string> ContentPath(std::string_view path);

// The files of `files` that are under the content folder, by their paths under it. A later file replaces an earlier one
// with the same path, as a later FILE chunk does.
std::map<std::string, std::vector<uint8_t>> ContentFiles(const std::vector<FileChunk>& files);

} // namespace csf
