// SPDX-License-Identifier: MIT

// Random access to the bytes of a file, or of a file split into parts that are read as one: ntdlib's disc::Source. Its
// messages call the whole a file (kFile).

#pragma once

#include "ntdlib/disc/error.h"
#include "ntdlib/disc/source.h"

namespace csf::title
{

// A damaged or unsupported file, or a read that failed.
using Error = ntdlib::disc::Error;

using ntdlib::disc::Source;

// What a Source's messages call the whole: "the file is truncated", "reading the file failed".
inline constexpr char kFile[] = "file";

} // namespace csf::title
