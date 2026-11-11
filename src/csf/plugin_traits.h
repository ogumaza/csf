// SPDX-License-Identifier: MIT

// The traits that CSF's two plugins share, for xsflib's plugin templates: the XMPlay plugin (plugins/xmplay) and the
// foobar2000 component (plugins/foobar2000). The XMPlay plugin uses PluginTraits unchanged. The foobar2000 component
// derives its traits from PluginTraits and adds the decoder name and the GUIDs. A plugin's build defines CSF_VERSION
// (csf_add_version).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"

#include "csf/format.h"
#include "csf/player.h"

namespace csf
{

struct PluginTraits
{
    using Player = csf::Player;
    using Set = csf::LoadedSet;

    static constexpr const char* kName = "CSF";
    static constexpr std::array<const char*, 2> kExtensions = {"csf", "minicsf"};
    static constexpr uint8_t kPsfVersion = kPsfVersionCsf;
    static constexpr uint64_t kMaxFileSize = uint64_t{1} << 30; // largest file read into memory

    // An effect that waits for a port that the game sets would go silent if removing the tags dropped csf_var.
    // The tag also contains a sequence's variables and the states of JAudio music.
    static constexpr std::array<const char*, 1> kPlaybackTags = {kVariablesTag};

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

} // namespace csf
