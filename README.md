# csf

CSF is a PSF-family music rip format for Nintendo Wii U sequenced audio: the music that Wii U games play from
NintendoWare sound archives (`.bfsar`). It's the Wii U counterpart of the DS's 2SF and NCSF.

A rip is in one of two modes:

- **Game mode** (from a game with a driver profile): the rip contains the game's executable and sound archive. The
  player runs the game's sound code on an emulated Espresso CPU. So far, Animal Crossing: amiibo Festival (USA) is the
  one game with a profile.
- **Archive mode** (from any sound archive): CSF's model of the SDK's sound library plays the archive. A sound archive
  on its own is enough.

The Wii U mixes sound on a DSP. The program that the DSP runs comes with the console's system software, not with any
game. In both modes, the player does the DSP's mixing in code instead. The code does the same arithmetic as the DSP's
program. `docs/csf.md` specifies the format.

CSF comes with a ripper (`csfrip`), a command-line renderer (`csfplay`) and a foobar2000 component.

## Tools

| Tool | Description |
|---|---|
| `csfrip [options] <input>...` | Rips each input into a `.csflib` and a `.minicsf` for each sequence. Each `.minicsf` is tagged with the sequence's length and fade. The rip goes in a folder next to the input, with the input's name and `_csf` added. `-o` names another folder. An input is a Wii U game or a sound archive (`.bfsar`). A game can be a Wii U archive (`.wua`), an extracted title (the folder holding `code/`, `content/` and `meta/`), or a folder holding several titles (such as a game and its update). Each of a game's sound archives gets a folder of its own. A game with a driver profile is ripped in game mode, and anything else in archive mode. `--mode` overrides that choice. `--list` lists a game's sound archives and what each holds, `--only REGEX` rips the sounds whose labels match, `--bgm` rips the music, and `--help` shows the rest. Sequences that make no sound (such as placeholders, and sequences that only set values for the game) are listed and skipped. |
| `csfplay [options] <file>...` | Renders each `.minicsf` at 48,000 Hz to a WAV file next to it. The render lasts for the tagged length and fade. `-o FILE` writes FILE instead. The file's extension selects WAV or FLAC. `--length` and `--fade` override the tags. `--info` shows a file's tags and the files it loads. |
| `foo_input_csf` | The foobar2000 component, for foobar2000 on Windows and macOS. The component plays `.minicsf` and `.csf` files, and shows and edits their tags. Rendering ahead during playback makes seeking quick. `plugins/foobar2000/README.md` describes how to build it. |

`csfrip` reads games whose files are stored decrypted: Wii U archives (made by Cemu) and extracted titles. Where an
update for the game is there too, the update's files are laid over the game's files. The console does the same. Disc
images (WUD, and WUX, its compressed form) and installable titles (`title.tmd` with `.app` files) hold their files
encrypted. For those, `csfrip` names the game and prints how to convert it. Cemu's Title Manager (Tools menu) converts
any of them to a `.wua` file.

Some games keep groups of their sounds in group files (`.bfgrp`) outside the sound archive. The archive lists those
group files by name. `csfrip` looks for them by those names in the game. For a sound archive given on its own, `csfrip`
looks in the archive's folder and the folders below it. Sequences whose banks or waves are in a group file that isn't
found are skipped.

Both tools accept several inputs and continue if one fails. A path that doesn't exist stops the command before it writes
any files. For example, `csfrip game.wua` writes `game_csf/`. That folder holds a folder for each of the game's sound
archives. Then `csfplay game_csf/*/*.minicsf` renders every sequence in it. `--version` shows a tool's version.

### Downloads

The Releases page has `csfrip` and `csfplay` for 64-bit and 32-bit Windows, and the foobar2000 component for 64-bit and
32-bit foobar2000 on Windows. On other systems, build CSF from source (see "Building").

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

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake. The component adds clang-cl. Configure with `cmake -S . -B build -T
  ClangCL` in place of the configure command below. Run the commands in a Developer PowerShell or Developer Command
  Prompt.
- **macOS:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`). `brew install
  zstd flac` saves building Zstandard and libFLAC. A universal build (two architectures in `CMAKE_OSX_ARCHITECTURES`)
  always builds them.
- **Linux:** GCC or Clang, CMake and Git from your distribution. The Zstandard, zlib and FLAC development packages (on
  Debian and Ubuntu, `libzstd-dev`, `zlib1g-dev` and `libflac-dev`) save building them.

MinGW, GCC and GNU-style Clang aren't supported on Windows. GCC and Clang aren't supported for 32-bit x86 either. In
those configurations, the floating-point arithmetic gives different results from the other builds. CMake rejects them.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps (`cmake --preset release`, `cmake --build
--preset release` and `ctest --preset release`). The presets require CMake 3.21 or later.

The audio writer's test (`tests/audio_out_test.cpp`) writes a WAV file and a FLAC file. The test checks that both files
hold the samples it wrote.

The title test (`tests/title_test.cpp`) builds a small Wii U game, with an update and downloadable content beside it.
The test stores the game in every form the readers support: an extracted title, a folder of titles and a Wii U archive.
Every file must read back. The test also builds files that the readers must refuse: disc images as WUD (whole and in
parts) and WUX, installable titles as title files, and damaged archives.

The sound archive test (`tests/fsar_test.cpp`) builds sound archives, sequences, banks with every kind of region table,
wave archives, waves, and the group files that a game keeps outside its archive. Each file is built in the Wii U's byte
order and in the Switch's byte order. The test checks what the readers read from each file, and that the readers refuse
damaged files.

The AX test (`tests/ax_test.cpp`) runs scenarios on the model of AX. The scenarios set up voices of every format the way
a game sets them up. They cover sample-rate conversion, envelopes, filters, mixes to the TV and the GamePad, priorities,
changes between frames and in the frame callback, and the final mix stage. For every stage of every frame, the test
compares a hash with the hash recorded for the scenario (`tests/ax_test_hashes.h`). The test also checks particular
behaviours: the ADPCM history, a one-shot voice's end, the interpolation, the filters, the aux buses' timing and the
upsampler.

The format test (`tests/format_test.cpp`) checks the PSF container, tags, times, chunks, descriptors and the loading of
libraries. The player test (`tests/player_test.cpp`) plays sets made from a small archive and checks lengths, fades,
seeks, volume and group files. The render-ahead test (`tests/render_ahead_test.cpp`) renders ahead from a stand-in
source and checks reads, seeks, the buffer's limit, errors and stopping. The ripper's test (`tests/rip_test.cpp`)
measures a sequence of each kind that the length analysis distinguishes.

## Layout

```
docs/         the format's specification (csf.md)
src/common/   small helpers shared by the rest (byte order, SHA-1, ASCII)
src/title/    Wii U games: Wii U archives and extracted titles, and recognising disc images
src/fsar/     the sound formats: sound archives (FSAR), sequences, banks, wave archives and waves
src/ax/       AX, the Wii U's sound library: the voices and their mixing, frame by frame
src/nwsnd/    archive mode's model of NintendoWare's sound library (nw::snd)
src/ppc/      the Espresso CPU's interpreter, for game mode
src/cafe/     the parts of Cafe OS that a game's sound code uses: the loader, coreinit and AX's imports
src/game/     game mode's driver, for playing a sound with a game's sound code
src/csf/      the CSF format, the player, playback and rendering ahead
src/rip/      the ripper: sets, profiles and the length analysis
src/cli/      csfrip and csfplay, and their WAV and FLAC writer and console
plugins/      the foobar2000 component
platform/     the Windows manifest and version resource, and the macOS droplet apps of csfrip and csfplay
tests/        ctest suites
```

## Licence

CSF is under the MIT licence (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists the libraries CSF uses and what a binary
distribution must include. Game data is not covered by this licence. Rips contain a game's code and sound data, and they
are for personal use.
