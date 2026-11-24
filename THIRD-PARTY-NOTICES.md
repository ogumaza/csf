# Third-party notices

CSF is under the MIT licence (`LICENSE`). The components that CSF uses have the following licences.

| Component | Licence | Use in CSF |
|---|---|---|
| xsflib, Copyright (c) 2026 the xsflib authors | MIT | A library for players and rippers of PSF-family formats. CSF uses its PSF container, the common fields of the archive descriptor and its list of track volumes, the checks of the descriptors, the reading of the variables tag, the player framework, playback and rendering ahead, the length analysis of the ripper, the names of a rip's files, the frame of the command-line tools, and the templates of the foobar2000 component and the XMPlay plugin. A Git submodule in `externals/xsflib`, built into CSF's programs. |
| ntdlib, Copyright (c) 2026 the ntdlib authors | MIT | A library for the hardware and formats of Nintendo's consoles. CSF uses its PowerPC interpreter and the fused arithmetic of the PowerPC, parts of AX (its constants, voice allocation and low-pass coefficients), the coefficient ROM of the DSP and its accelerator, random access to files stored whole or in parts, the recognition of GameCube and Wii disc images, the readers of the sound archives and of the start-up archives of JAudio, the archive-mode parts of the ripper, the lists of how Wii U games start their sequences (the variants and the states of the tracks), the setting of the track volumes of a sequence, the library descriptor, the length analysis of JAudio games, and the model of NintendoWare's sound library. A Git submodule in `externals/ntdlib`, built into CSF's programs. |
| Data from Unicode: CP932.TXT, Microsoft's table of code page 932 | Unicode License v3 (`Unicode-3.0`) | In ntdlib's `src/disc/cp932.h`, compiled into `csfrip` with the disc reader of ntdlib. The table decodes the Shift-JIS text of GameCube and Wii discs: the names in their file systems and the titles of Japanese discs. The notices of ntdlib give its source. |
| seqlib, Copyright (c) 2026 the seqlib authors | MIT (`externals/xsflib/externals/seqlib/LICENSE`) | In `externals/xsflib/externals/seqlib`, a submodule of xsflib. Only the tests use it: their checks come from it, through `xsflib::testing`. |
| [Zstandard](https://github.com/facebook/zstd), Copyright (c) Meta Platforms, Inc. and affiliates | BSD-3-Clause | Wii U archives (.wua). The installed copy is used if there is one. Otherwise v1.5.7 is downloaded when configuring. |
| [zlib](https://zlib.net), Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler | zlib | The PSF container, and the compressed sections of the Wii U's executables (RPX). The installed copy is used if there is one. Otherwise v1.4 is downloaded when configuring. |
| [libFLAC](https://xiph.org/flac/), Copyright the Xiph.Org Foundation | BSD-3-Clause | FLAC output in `csfplay`. The installed copy is used if there is one. Otherwise 1.5.0 is downloaded when configuring. |
| [foobar2000 SDK](https://www.foobar2000.org/SDK), Copyright Peter Pawlowski | BSD-style (the SDK's `sdk-license.txt`, and `pfc-license.txt` for its pfc library) | Only for `plugins/foobar2000`. Not included here. Whoever builds the component supplies the SDK. |
| [XMPlay plugin SDK](https://www.un4seen.com/xmplay.html), from Un4seen Developments | None stated | Only for `plugins/xmplay`. Not included here. Whoever builds the plugin supplies the SDK, and CI downloads it from un4seen.com. The plugin uses only the declarations of XMPlay's interface in the SDK. |

Binary distributions must include the MIT notices of CSF, xsflib and ntdlib. Every program contains xsflib and ntdlib.
The Zstandard notice is required where Zstandard is linked in: `csfrip` links it, and `csfplay`, the foobar2000
component and the XMPlay plugin don't. The libFLAC notice is required where libFLAC is linked in: `csfplay` links it,
and the foobar2000 component and the XMPlay plugin don't. The notice of the Unicode License v3 is required where the
disc reader of ntdlib is linked in: `csfrip` links it, and `csfplay`, the foobar2000 component and the XMPlay plugin
don't. Shipping `LICENSE` and this file meets all six requirements. The licence texts are at the end of this file. The
licences of zlib, the foobar2000 SDK and its pfc library require no notice in binaries. The checks of seqlib go only
into tests. No code from the XMPlay plugin SDK goes into the XMPlay plugin.

The submodules also contain code that no CSF program contains: Teakra in ntdlib, with Catch2 for the Teakra tests, and
puff in ntdlib's `src/common/inflate.cpp`. xsflib also offers liblzma and bzip2 for disc images. CSF's build fetches
neither. The notices of ntdlib and xsflib list those components and their licences.

## Reference material (no code taken)

- [ZArchive](https://github.com/Exzap/ZArchive) (MIT No Attribution) defines the format of Cemu's Wii U archives (.wua).
  CSF's reader was written from ZArchive's description of the structures of the format.
- [WiiUBrew](https://wiiubrew.org) documents the Wii U's hardware, its title IDs and its file formats.
- [Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0) and [decaf-emu](https://github.com/decaf-emu/decaf-emu)
  (GPL-3.0) are the references for how the Wii U behaves. No code is taken from them. Where CSF follows their behaviour,
  the code is written from a description of that behaviour. The person who writes that code hasn't read their source for
  it.

## Game data

Nothing in this repository licenses game data. Rips (`.csflib`) contain a game's sound data, and game-mode rips contain
its executable too. Those belong to their copyright holders.

ntdlib's `src/cafe/ax/system_output.cpp` contains the taps of the FIR filter that the sound library of the Wii U system
uses to convert its output to 48 kHz. The sound model uses the exact values of the library to reproduce its output.

ntdlib's `include/ntdlib/dsp/coef_rom.h` contains the coefficient ROM of the DSP of the GameCube and the Wii, with the
coefficients of its three 4-tap resampling filters. The Wii U's DSP is taken to share them. The sound model uses the
exact values to reproduce the output of the hardware.

ntdlib's `include/ntdlib/nwsnd/tables.h` contains the numeric tables that nw::snd uses: powers of two, decibel levels,
envelope attack multipliers, pan curves, low-pass cutoffs, biquad filter coefficients and sines. The sound model uses
the game's exact values to reproduce the game's output.

## Licence texts

### xsflib

```
MIT License

Copyright (c) 2026 the xsflib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ntdlib

```
MIT License

Copyright (c) 2026 the ntdlib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

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

 (C) 1995-2026 Jean-loup Gailly and Mark Adler

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

### Unicode License v3

```
UNICODE LICENSE V3

COPYRIGHT AND PERMISSION NOTICE

Copyright © 1991-2026 Unicode, Inc.

NOTICE TO USER: Carefully read the following legal agreement. BY
DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

Permission is hereby granted, free of charge, to any person obtaining a
copy of data files and any associated documentation (the "Data Files") or
software and any associated documentation (the "Software") to deal in the
Data Files or Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, and/or sell
copies of the Data Files or Software, and to permit persons to whom the
Data Files or Software are furnished to do so, provided that either (a)
this copyright and permission notice appear with all copies of the Data
Files or Software, or (b) this copyright and permission notice appear in
associated Documentation.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
THIRD PARTY RIGHTS.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or other
dealings in these Data Files or Software without prior written
authorization of the copyright holder.
```
