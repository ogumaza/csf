// SPDX-License-Identifier: MIT

// xmp-csf: XMPlay input plugin for CSF (.csf and .minicsf), from xsflib's plugin template (xsflib/xmplay/input.h). The
// template's comment says what the plugin does. This file gives CSF's traits: its player, names, extensions and limits.
//
// The player core (src/csf) plays the sound: with the game's sound code on an emulated Espresso in game mode, or with
// CSF's model of the SDK's sound library in archive mode. CSF's model of AX does the mixing either way.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"
#include "xsflib/xmplay/input.h"

#include "csf/format.h"
#include "csf/player.h"

namespace
{

struct Traits
{
    using Player = csf::Player;
    using Set = csf::LoadedSet;

    static constexpr const char* kName = "CSF";
    static constexpr std::array<const char*, 2> kExtensions = {"csf", "minicsf"};
    static constexpr uint8_t kPsfVersion = csf::kPsfVersionCsf;
    static constexpr uint64_t kMaxFileSize = uint64_t{1} << 30; // largest file read into memory

    // An effect that waits for a port that the game sets would go silent if removing the tags dropped csf_var.
    static constexpr std::array<const char*, 1> kPlaybackTags = {csf::kVariablesTag};

    static constexpr const char* kVersion = CSF_VERSION;
    static constexpr const char* kAbout =
        "Plays CSF (Nintendo Wii U sequenced music) rips, with the game's sound code on an emulated Espresso or with "
        "CSF's model of the SDK's sound library, mixed by CSF's model of AX, the system's sound library.\n\n"
        "MIT licence. Uses zlib.";

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return csf::LoadSet(path, reader, out, separators);
    }
};

} // namespace

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<Traits>::GetInterface(face, faceproc);
}
