# Third-party notices

CSF is under the MIT licence (`LICENSE`). CSF's dependencies have the following licences.

| Component | Licence | Use in CSF |
|---|---|---|
| [Zstandard](https://github.com/facebook/zstd), Copyright (c) Meta Platforms, Inc. and affiliates | BSD-3-Clause | Wii U archives (.wua). The system's copy is used, or v1.5.7 is downloaded when configuring. |
| [zlib](https://zlib.net), Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler | zlib | The PSF container, and the compressed sections of the Wii U's executables (RPX). The system's copy is used, or v1.3.1 is downloaded when configuring. |
| [libFLAC](https://xiph.org/flac/), Copyright the Xiph.Org Foundation | BSD-3-Clause | FLAC output in `csfplay`. The system's copy is used, or 1.5.0 is downloaded when configuring. |
| [foobar2000 SDK](https://www.foobar2000.org/SDK), Copyright Peter Pawlowski | BSD-style (the SDK's `sdk-license.txt`, and `pfc-license.txt` for its pfc library) | Only for `plugins/foobar2000`. Not included here. The SDK is supplied when building the component. |

Binary distributions must include CSF's MIT notice. The Zstandard notice is required when Zstandard is linked in:
`csfrip` links it, and `csfplay` and the foobar2000 component don't. The libFLAC notice is required when libFLAC is
linked in: `csfplay` links it, and the foobar2000 component doesn't. Shipping `LICENSE` and this file meets all three
requirements. The licence texts are at the end of this file. The licences of zlib, the foobar2000 SDK and its pfc
library require no notice in binaries.

`src/ppc`, the PowerPC interpreter, comes from WSF (the Wii counterpart of CSF, MIT, Copyright (c) 2026 the WSF
authors). It was adapted for the Wii U.

## Reference material (no code taken)

- [ZArchive](https://github.com/Exzap/ZArchive) (MIT No Attribution) defines the format of Cemu's Wii U archives (.wua).
  CSF's reader was written from ZArchive's description of the format's structures.
- [WiiUBrew](https://wiiubrew.org) documents the Wii U's hardware, its title IDs and its file formats.
- [Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0) and [decaf-emu](https://github.com/decaf-emu/decaf-emu)
  (GPL-3.0) are the references for how the Wii U behaves. No code is taken from them. Where CSF follows their behaviour,
  the code is written from a description of that behaviour. The person who writes that code hasn't read their source for
  it.

## Game data

Nothing in this repository licenses game data. Rips (`.csflib`) contain a game's sound archive, and in game mode its
executable. Those belong to their copyright holders.

`src/ax/system.cpp` holds the taps of the FIR filter that the Wii U system's sound library uses to convert its output to
48 kHz. The sound model uses the library's exact values to reproduce the library's output.

`src/ax/four_tap.h` holds the coefficients of the three 4-tap resampling filters in the coefficient ROM of the
GameCube's and Wii's DSP. The Wii U's DSP is taken to share them. The sound model uses the exact values to reproduce the
hardware's output.

`src/nwsnd/tables.h` holds the numeric tables nw::snd uses: powers of two, decibel levels, envelope attack multipliers,
pan curves, low-pass cutoffs, biquad filter coefficients and sines. The sound model uses the game's exact values to
reproduce the game's output.

## Licence texts

### Zstandard

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### libFLAC

```
Copyright (C) 2000-2009  Josh Coalson
Copyright (C) 2011-2025  Xiph.Org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zlib

```
Copyright notice:

 (C) 1995-2024 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```
