// SPDX-License-Identifier: MIT

// foo_input_csf: foobar2000 input component for CSF (.csf and .minicsf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. This file gives CSF's traits: its
// player, names, extensions, GUIDs and limits.
//
// The player core (src/csf) plays the sound: with the game's sound code on an emulated Espresso in game mode, or with
// CSF's model of the SDK's sound library in archive mode. CSF's model of AX does the mixing either way.
//
// Build: see plugins/foobar2000/README.md (clang-cl on Windows, or Apple Clang on macOS, with the foobar2000 SDK).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/foobar2000/input.h"
#include "xsflib/psf/psf.h"
#include <SDK/foobar2000.h>

#include "csf/format.h"
#include "csf/player.h"

namespace
{

struct Traits
{
    using Player = csf::Player;
    using Set = csf::LoadedSet;

    static constexpr const char* kName = "CSF";
    static constexpr const char* kDecoderName = "CSF decoder";
    static constexpr std::array<const char*, 2> kExtensions = {"csf", "minicsf"};
    static constexpr uint8_t kPsfVersion = csf::kPsfVersionCsf;
    static constexpr t_filesize kMaxFileSize = t_filesize{1} << 30; // largest file read into memory
    static constexpr std::array<const char*, 0> kPlaybackTags{};    // none beyond the PSF system tags

    // {055C07E3-B971-4C58-942D-7DA2A3F7DCF4}
    static constexpr GUID kInputGuid = {0x055c07e3, 0xb971, 0x4c58, {0x94, 0x2d, 0x7d, 0xa2, 0xa3, 0xf7, 0xdc, 0xf4}};

    // Advanced preferences entries, under Decoding > CSF decoder.
    static constexpr GUID kBranchGuid = {0x93350f79, 0xc605, 0x4fd4, {0x9f, 0xf8, 0xb6, 0xd9, 0x38, 0xe5, 0x11, 0x15}};
    static constexpr GUID kLengthGuid = {0x8651c6b1, 0x9efb, 0x4bfe, {0xab, 0x8c, 0x4a, 0xa5, 0x42, 0x45, 0xd4, 0xb1}};
    static constexpr GUID kFadeGuid = {0x24193d95, 0x3830, 0x4255, {0xa9, 0x67, 0x31, 0xbb, 0x28, 0xed, 0xf2, 0x5c}};
    static constexpr GUID kEndlessGuid = {0x74f274d2, 0xdc82, 0x411a, {0xaa, 0x61, 0x77, 0x26, 0xe0, 0x76, 0x70, 0xff}};
    static constexpr GUID kRenderAheadGuid = {
        0xe4096534, 0x4c76, 0x4744, {0x85, 0x8a, 0xf8, 0x2c, 0xa4, 0x71, 0xeb, 0xd7}};

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return csf::LoadSet(path, reader, out, separators);
    }
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_csf_factory;

} // namespace

DECLARE_FILE_TYPE("CSF files", "*.CSF;*.MINICSF");

DECLARE_COMPONENT_VERSION("CSF Decoder", CSF_VERSION,
                          "Plays CSF (Nintendo Wii U sequenced music) rips, with the game's sound code on an emulated "
                          "Espresso or with CSF's model of the SDK's sound library, mixed by CSF's model of AX, the "
                          "system's sound library.\n\n"
                          "MIT licence. Uses zlib.");

VALIDATE_COMPONENT_FILENAME("foo_input_csf.dll");
