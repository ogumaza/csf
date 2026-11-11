// SPDX-License-Identifier: MIT

// xmp-csf: XMPlay input plugin for CSF (.csf and .minicsf), from xsflib's plugin template (xsflib/xmplay/input.h). The
// template's comment says what the plugin does. csf/plugin_traits.h gives CSF's traits: its player, names, extensions
// and limits.
//
// The player core (src/csf) plays the sound: with the game's sound code on an emulated Espresso in game mode, or with
// CSF's model of the SDK's sound library in archive mode. CSF's model of AX does the mixing either way.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include "xsflib/xmplay/input.h"

#include "csf/plugin_traits.h"

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<csf::PluginTraits>::GetInterface(face, faceproc);
}
