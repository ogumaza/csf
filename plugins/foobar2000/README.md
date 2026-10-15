# foo_input_csf

foobar2000 input component for CSF rips (`.minicsf`, and the rarely used `.csf`). The component is built on the same
player core as `csfplay`. The player core plays a sound with the game's sound code on an emulated Espresso CPU (game
mode) or with CSF's model of the SDK's sound library (archive mode). The core then mixes the sound with CSF's model of
AX, the system's sound library.

**Status:** the component builds with clang-cl for 32-bit and 64-bit foobar2000 on Windows. On macOS, Apple Clang
builds it for Apple Silicon. CI builds the component on every push to `main` that changes more than documentation. The
Windows jobs build it with clang-cl for x64 and x86. The macOS ARM64 job builds it too. Releases include a package for
64-bit and 32-bit foobar2000 on Windows (`.github/workflows/release.yml`). `ctest` covers the player core. xsflib's
tests cover playback and rendering ahead.

## Features

- Tags are shown as metadata in the same way as in kode54's PSF-family components: `game` as album, `year` as date, and
  `replaygain_*` as ReplayGain. Tags can be edited. Editing rewrites the tag area and keeps `_lib`, `length`, `fade` and
  `volume`.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play for the default length and
  fade and stop early when the sound ends by itself.
- Advanced preferences › Decoding › CSF decoder: default length and fade, endless playback, and rendering ahead. Endless
  playback and rendering ahead apply to playback only. They never apply to conversion or scanning.
- During playback, the component renders ahead on a separate thread as fast as the CPU allows. The component buffers up
  to 10 minutes and 10 seconds of audio (about 112 MiB). That's enough for the longest track that `csfrip`'s length
  analysis produces. Seeking within the buffer is instant. Seeking further ahead waits for rendering to reach the
  requested position. Rendering ahead keeps one CPU core busy until it reaches the end of the track or the buffer limit.
  Seeking back before the buffered audio renders from the start of the track. Audio falls out of the buffer only in a
  longer track or in endless playback. With rendering ahead turned off, seeking back renders from the start of the
  track. Seeking ahead then renders up to the target.
- Output is 16-bit stereo at 48,000 Hz. That's the rate of the Wii U's audio output. The output isn't resampled.
- When a track starts, the component loads the track's `.csflib`. A `.csflib` holds a whole sound archive. The tracks of
  a game with a large archive therefore take longer to start and use more memory.

## Building

**On GitHub (no Windows machine or Mac needed):** the `foobar2000 component` workflow
(`.github/workflows/foobar2000.yml`) builds the 32-bit and 64-bit DLLs and the Mac bundle. The workflow uploads one
`.fb2k-component` with all three. Start it from the repository's Actions tab (Run workflow). GitHub lists a manual
workflow only once it's on the default branch (`main`). The workflow requires GitHub Actions to be enabled for the
repository. The workflow downloads the SDK from foobar2000.org, in the version that xsflib pins
(`externals/xsflib/ci/foobar2000-sdk/action.yml`). CI uses the same version.

To build it yourself, you need the foobar2000 SDK, from <https://www.foobar2000.org/SDK>. Unpack it and use the folder
containing `pfc` and `foobar2000` as the SDK path below. CI builds with the version that xsflib pins. A newer SDK may
require changes to xsflib's `xsflib_add_foobar2000_component` (`externals/xsflib/cmake/XsflibApps.cmake`). That function
builds the component from the SDK's sources.

**On macOS:** you need the Xcode command line tools (`xcode-select --install`), CMake 3.20 or later, and the SDK
unpacked somewhere (macOS's `tar -xf SDK-<date>.7z` unpacks it).

```
cmake -S . -B build-fb2k -DCMAKE_BUILD_TYPE=Release -DFOOBAR2000_SDK_DIR=/path/to/SDK \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-fb2k --target foo_input_csf
```

This produces `build-fb2k/plugins/foobar2000/foo_input_csf.component`. It's a bundle for Apple Silicon Macs that runs
on macOS 11 or later. That's the same minimum as the SDK's projects. Without the two `CMAKE_OSX_` options, the
bundle is for the Mac that built it only. For a `.fb2k-component`, put the bundle in a `mac` folder inside a zip. The
bundle is signed ad hoc, not with a Developer ID. It isn't notarized.

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang tools for Windows"
component, plus CMake 3.20 or later.

```
cmake -S . -B build-fb2k -A x64 -T ClangCL -DFOOBAR2000_SDK_DIR=C:/path/to/SDK
cmake --build build-fb2k --config Release --target foo_input_csf
```

This produces `build-fb2k/plugins/foobar2000/Release/foo_input_csf.dll`. For a `.fb2k-component`, zip the DLL. For
64-bit foobar2000 v2, put the DLL in an `x64` folder inside the zip. For 32-bit foobar2000, configure with `-A Win32 -T
ClangCL` and put that DLL at the top level of the zip. With the 64-bit DLL in `pkg\x64`, Windows' `tar` makes the zip:
`tar -a -c -f foo_input_csf.zip -C pkg x64`. For a 32-bit DLL in `pkg` itself, add `foo_input_csf.dll` after `x64`. Then
rename the zip to `foo_input_csf.fb2k-component`. Windows PowerShell's `Compress-Archive` writes the folder's name with
a backslash. The zip format doesn't allow that, and foobar2000 doesn't accept a package made that way. Omitting `-T
ClangCL` selects MSVC. MSVC isn't supported.

The build compiles the SDK's `pfc`, `foobar2000/SDK` and `foobar2000/foobar2000_component_client` sources directly. On
Windows, the build links `foobar2000/shared/shared-<arch>.lib` (or `shared.lib`). On macOS, the build compiles the parts
of `foobar2000/shared` and the Objective-C++ files that the SDK's Xcode projects build. If the SDK layout has changed,
adjust `xsflib_add_foobar2000_component`.

On Windows, everything is built with the static C runtime (`/MT`). So the component requires no runtime DLLs. On macOS,
the component uses only the system's libraries.

## Performance

Playback runs on one CPU core. Game mode emulates the Wii U's CPU running the game's sound code. Archive mode runs CSF's
model of the sound library instead. Archive mode is much faster. Use 64-bit foobar2000 and the 64-bit component if your
other components support them. A 64-bit process has room for the largest sound archives. A 32-bit process may not.

## Licence

Like the rest of CSF, the component is MIT-licensed. It contains xsflib and ntdlib (both MIT) and uses zlib. Ship
`LICENSE`, the licences of xsflib and ntdlib (`externals/xsflib/LICENSE` and `externals/ntdlib/LICENSE`) and
`THIRD-PARTY-NOTICES.md`, or their text, with a build. Nothing in the component is GPL. It can be distributed as a
foobar2000 component.
