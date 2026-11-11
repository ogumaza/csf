// SPDX-License-Identifier: MIT

// foo_input_csf: foobar2000 input component for CSF (.csf and .minicsf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. csf/plugin_traits.h gives the
// traits that both plugins share: the player, names, extensions and limits. This file adds the GUIDs.
//
// The player core (src/csf) plays the sound: with the game's sound code on an emulated Espresso in game mode, or with
// CSF's model of the SDK's sound library in archive mode. CSF's model of AX does the mixing either way.
//
// Build: see plugins/foobar2000/README.md (clang-cl on Windows, or Apple Clang on macOS, with the foobar2000 SDK).

#include "xsflib/foobar2000/input.h"
#include <SDK/foobar2000.h>

#include "csf/plugin_traits.h"

namespace
{

// The traits that both plugins share, and the component's name in foobar2000 and its GUIDs.
struct Traits : csf::PluginTraits
{
    static constexpr const char* kDecoderName = "CSF decoder";

    // {055C07E3-B971-4C58-942D-7DA2A3F7DCF4}
    static constexpr GUID kInputGuid = {0x055c07e3, 0xb971, 0x4c58, {0x94, 0x2d, 0x7d, 0xa2, 0xa3, 0xf7, 0xdc, 0xf4}};

    // Advanced preferences entries, under Decoding > CSF decoder.
    static constexpr GUID kBranchGuid = {0x93350f79, 0xc605, 0x4fd4, {0x9f, 0xf8, 0xb6, 0xd9, 0x38, 0xe5, 0x11, 0x15}};
    static constexpr GUID kLengthGuid = {0x8651c6b1, 0x9efb, 0x4bfe, {0xab, 0x8c, 0x4a, 0xa5, 0x42, 0x45, 0xd4, 0xb1}};
    static constexpr GUID kFadeGuid = {0x24193d95, 0x3830, 0x4255, {0xa9, 0x67, 0x31, 0xbb, 0x28, 0xed, 0xf2, 0x5c}};
    static constexpr GUID kEndlessGuid = {0x74f274d2, 0xdc82, 0x411a, {0xaa, 0x61, 0x77, 0x26, 0xe0, 0x76, 0x70, 0xff}};
    static constexpr GUID kRenderAheadGuid = {
        0xe4096534, 0x4c76, 0x4744, {0x85, 0x8a, 0xf8, 0x2c, 0xa4, 0x71, 0xeb, 0xd7}};
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_csf_factory;

} // namespace

DECLARE_FILE_TYPE("CSF files", "*.CSF;*.MINICSF");

DECLARE_COMPONENT_VERSION("CSF Decoder", csf::PluginTraits::kVersion, csf::PluginTraits::kAbout);

VALIDATE_COMPONENT_FILENAME("foo_input_csf.dll");
