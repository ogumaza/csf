// SPDX-License-Identifier: MIT

// The Wii U's sound archives (FSAR) and the files in them, read by ntdlib's readers with the players' rules and CSF's
// messages.

#pragma once

#include "ntdlib/sar/formats.h"

namespace csf
{

namespace fsar = ntdlib::sar;

// How CSF reads sound files: as the Wii U's, by the players' rules. The readers take either byte order, by each file's
// byte order mark. csfrip refuses the Switch's archives before it reads them.
inline constexpr fsar::Options kSarOptions{fsar::Flavor::kCafe, {}, false, fsar::Rules::kStopAtFirstProblem};

} // namespace csf
