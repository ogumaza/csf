# csf

CSF is a PSF-family music rip format for Nintendo Wii U sequenced audio: the music that Wii U games play from
NintendoWare sound archives (`.bfsar`), and from JAudio, the sound library of some of Nintendo's games. It's the Wii U
counterpart of the DS's 2SF and NCSF.

A rip is in one of two modes:

- **Game mode** (from a game with a driver profile): the rip contains the game's executable and its sound files. The
  player runs the game's sound code on an emulated Espresso CPU. Three games have profiles so far: Animal Crossing:
  amiibo Festival (USA), on NintendoWare's sound library, The Legend of Zelda: Twilight Princess HD (USA), on JAudio2,
  and The Legend of Zelda: The Wind Waker HD (USA), on JAudio1. A JAudio game mixes its sound on the CPU, and the
  player runs that code too.
- **Archive mode** (from any sound archive): CSF's model of the SDK's sound library plays the archive. A sound archive
  on its own is enough. Archive mode also plays the reverb, chorus and delay of a game with an effect profile (see
  `docs/csf.md`, "Effects").

The Wind Waker HD plays some of its music from streams. CSF doesn't rip streams. A rip of the game holds the music that
it plays as sequences. The game's sound effects aren't ripped.

The Wii U mixes sound on a DSP. The program that the DSP runs comes with the console's system software, not with any
game. In both modes, the player does the DSP's mixing in code instead. The code does the same arithmetic as the DSP's
program. `docs/csf.md` specifies the format.

CSF comes with a ripper (`csfrip`), a command-line renderer (`csfplay`), a foobar2000 component and an XMPlay plugin.

## Tools

| Tool | Description |
|---|---|
| `csfrip [options] <input>...` | Rips each input into a `.csflib` and a `.minicsf` for each sequence. Each `.minicsf` is tagged with the sequence's length and fade. The rip goes in a folder next to the input, with the input's name and `_csf` added. `-o` names another folder. An input is a Wii U game or a sound archive (`.bfsar`). A game can be a Wii U archive (`.wua`), an extracted title (the folder holding `code/`, `content/` and `meta/`), or a folder holding several titles (such as a game and its update). Each of a game's sound archives gets a folder of its own. A JAudio game's files go straight into the rip's folder. A game with a driver profile is ripped in game mode, and anything else in archive mode. `--mode` overrides that choice. `--list` lists a game's sound archives and what each holds (a JAudio game's sounds), `--only REGEX` rips the sounds whose labels match, `--bgm` rips the music, and `--help` shows the rest. Sequences that make no sound (such as placeholders, and sequences that only set values for the game) are listed and skipped. |
| `csfplay [options] <file>...` | Renders each `.minicsf` at 48,000 Hz to a WAV file next to it. The render lasts for the tagged length and fade. `-o FILE` writes FILE instead. The file's extension selects WAV or FLAC. `--length` and `--fade` override the tags. `--info` shows a file's tags and the files it loads. |
| `foo_input_csf` | The foobar2000 component, for foobar2000 on Windows and macOS. The component plays `.minicsf` and `.csf` files, and shows and edits their tags. Rendering ahead during playback makes seeking quick. `plugins/foobar2000/README.md` describes how to build it. |
| `xmp-csf` | The XMPlay plugin, for XMPlay on Windows: a 32-bit DLL, `xmp-csf.dll`. The plugin plays `.minicsf` and `.csf` files, and shows their tags. Rendering ahead during playback makes seeking quick. `plugins/xmplay/README.md` describes how to build it. |

`csfrip` reads games whose files are stored decrypted: Wii U archives (made by Cemu) and extracted titles. Where an
update for the game is there too, the update's files are laid over the game's files. The console does the same. Disc
images (WUD, and WUX, its compressed form) and installable titles (`title.tmd` with `.app` files) hold their files
encrypted. For those, `csfrip` names the game and prints how to convert it. Cemu's Title Manager (Tools menu) converts
any of them to a `.wua` file.

Some games keep groups of their sounds in group files (`.bfgrp`) outside the sound archive. The archive lists those
group files by name. `csfrip` looks for them by those names in the game. For a sound archive given on its own, `csfrip`
looks in the archive's folder and the folders below it. Some games keep group files in SARC archives (`.sarc`) instead,
under other names. While a group is still missing, `csfrip` also searches the SARC archives in the sound archive's
folder. It adds each group file in them whose files match the sound archive's. A compressed SARC (`.szs`) isn't
searched. Sequences whose banks or waves are in a group file that isn't found are skipped.

Both tools accept several inputs and continue if one fails. A path that doesn't exist stops the command before it writes
any files. For example, `csfrip game.wua` writes `game_csf/`. That folder holds a folder for each of the game's sound
archives. Then `csfplay game_csf/*/*.minicsf` renders every sequence in it. A JAudio game's rip has no such folders, and
`csfplay game_csf/*.minicsf` renders it. `--version` shows a tool's version.

### Downloads

The Releases page has `csfrip` and `csfplay` for 64-bit Windows, the foobar2000 component for 64-bit and 32-bit
foobar2000 on Windows, and the XMPlay plugin. On macOS, build CSF from source (see "Building").

To install the XMPlay plugin, unzip `xmp-csf-X.Y.Z.zip` and put `xmp-csf.dll` in XMPlay's folder, or in a folder
inside it. Then restart XMPlay.

### Drag and drop

- **Windows:** drop files or folders on `csfrip.exe`, or files on `csfplay.exe`. A console window shows the results and
  stays open until you press Enter.
- **macOS:** the build also makes `csfrip.app` and `csfplay.app` in the build folder. Drop files on one, or open it and
  choose them. The app shows the results for each file in a dialog, with a button that opens the output folders.

As with the command line, the results go next to the files. File and folder names can use any characters. On Windows,
that requires Windows 10 version 1903 or later.

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. Zstandard, zlib and libFLAC are downloaded and built along
with CSF when they aren't installed.

CSF shares code with other players of PSF-family formats through two libraries, each a Git submodule: xsflib in
`externals/xsflib` and ntdlib in `externals/ntdlib`. ntdlib holds the shared code for Nintendo's consoles. Clone CSF
with `git clone --recursive`. In a clone made without it, run `git submodule update --init --recursive` before you
build.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake. The component adds clang-cl. Configure with `cmake -S . -B build -A
  x64 -T ClangCL` in place of the configure command below. Run the commands in a Developer PowerShell or Developer
  Command Prompt.
- **macOS on Apple Silicon:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`).
  `brew install zstd flac` saves building Zstandard and libFLAC.

Linux, Intel Macs, MSVC and 32-bit Windows aren't supported. The exceptions are the 32-bit foobar2000 component and the
XMPlay plugin. Build them with clang-cl, configured with `-A Win32 -T ClangCL`.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps (`cmake --preset release`, `cmake --build
--preset release` and `ctest --preset release`). The presets require CMake 3.23 or later. They don't select clang-cl.
On Windows, use the commands above.

The title test (`tests/title_test.cpp`) builds a small Wii U game, with an update and downloadable content beside it.
The test stores the game in every form the readers support: an extracted title, a folder of titles and a Wii U archive.
Every file must read back. The test also builds files that the readers must refuse: disc images as WUD (whole and in
parts) and WUX, installable titles as title files, and damaged archives.

ntdlib reads the sound archives and their files, and its tests check its readers. CSF's tests build their archives with
ntdlib's builder of test files.

AX, the system library's effects for AX's aux buses and archive mode's model of `nw::snd` come from ntdlib's `cafe`
module. ntdlib's tests check them.

The format test (`tests/format_test.cpp`) checks the descriptors, the effects that an archive descriptor lists, and the
loading of libraries. The player test (`tests/player_test.cpp`) plays sets made from a small archive and checks lengths,
fades, seeks, volume, group files and effects. Playback and rendering ahead come from xsflib, and its tests check them.
The length analysis comes from xsflib too. The ripper's test (`tests/rip_test.cpp`) measures ntdlib's length cases on
CSF's model: a sequence of each kind that the analysis distinguishes.

## Layout

```
docs/         the format's specification (csf.md)
src/title/    Wii U games: Wii U archives and extracted titles, and recognising disc images
src/cafe/     the parts of Cafe OS that a game's sound code uses: the loader, coreinit and AX's imports
src/game/     game mode's NintendoWare, JAudio2 and JAudio1 drivers, for playing a sound with a game's sound code
src/csf/      the CSF format and the player
src/rip/      the ripper: sets, profiles, the length analysis on archive mode's model, and JAudio games' rips
src/cli/      csfrip and csfplay
plugins/      the foobar2000 component and the XMPlay plugin
tests/        ctest suites
externals/    xsflib and ntdlib, the code that CSF shares with other players and converters (Git submodules)
```

## Licence

CSF is under the MIT licence (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists the libraries CSF uses and what a binary
distribution must include. Game data is not covered by this licence. Rips contain a game's code and sound data, and they
are for personal use.
