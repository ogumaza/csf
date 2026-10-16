// SPDX-License-Identifier: MIT

// The namespaces of ntdlib's cafe module, under the names that CSF uses for them. The module holds the Wii U's AX (ax),
// the system library's effects for AX's aux buses (axfx) and archive mode's model of nw::snd on AX (nwsnd).

#pragma once

namespace ntdlib::cafe::ax
{
}

namespace ntdlib::cafe::axfx
{
}

namespace ntdlib::cafe::snd
{
}

namespace csf
{

namespace ax = ntdlib::cafe::ax;
namespace axfx = ntdlib::cafe::axfx;
namespace nwsnd = ntdlib::cafe::snd;

} // namespace csf
