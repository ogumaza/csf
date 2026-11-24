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
    static constexpr GUID kFadeExponentGuid = {
        0xe219a6d1, 0x6359, 0x409a, {0xab, 0xa3, 0x90, 0x6c, 0xb9, 0x75, 0x52, 0x43}};

    // The settings that shape a track like a track of an album (xsflib/player/settings.h).
    static constexpr GUID kSkipSilenceGuid = {
        0x0b9b7b9b, 0x8916, 0x402a, {0xae, 0x94, 0x6e, 0xfd, 0x75, 0x13, 0xbe, 0x9d}};
    static constexpr GUID kStartSilenceGuid = {
        0x8e0aad2d, 0x89e1, 0x44d2, {0xb7, 0x23, 0xd7, 0x33, 0xa8, 0xd7, 0x0f, 0x2c}};
    static constexpr GUID kLoopsGuid = {0x1af992f4, 0x2d2d, 0x429a, {0x99, 0xc7, 0x7a, 0x9e, 0x03, 0x6d, 0xc1, 0xa7}};
    static constexpr GUID kLongLoopGuid = {
        0x01285394, 0xdfa8, 0x4515, {0xba, 0x10, 0xeb, 0xf7, 0x15, 0xdb, 0x4f, 0x49}};
    static constexpr GUID kLoopFadeGuid = {
        0x30a0efe1, 0xbc41, 0x403a, {0x93, 0x42, 0x35, 0x11, 0xcd, 0xee, 0x81, 0xd8}};
    static constexpr GUID kEndSilenceGuid = {
        0x1c9e2545, 0x28e1, 0x43b2, {0x86, 0x73, 0x61, 0xa4, 0xca, 0xb9, 0x60, 0x7f}};
    static constexpr GUID kIgnoreTagsGuid = {
        0x69053084, 0x81b0, 0x41be, {0x80, 0x17, 0x92, 0x59, 0x1b, 0x4f, 0x42, 0x2f}};
    static constexpr GUID kSkipEndGuid = {0x7ff1a9f2, 0xe70f, 0x4e1b, {0x98, 0xb2, 0x72, 0xe9, 0xba, 0x80, 0x61, 0x2e}};
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_csf_factory;

} // namespace

DECLARE_FILE_TYPE("CSF files", "*.CSF;*.MINICSF");

DECLARE_COMPONENT_VERSION("CSF Decoder", csf::PluginTraits::kVersion, csf::PluginTraits::kAbout);

VALIDATE_COMPONENT_FILENAME("foo_input_csf.dll");
