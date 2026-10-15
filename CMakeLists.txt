# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for the tools. For a release, update it here and add notes in docs/release-notes/.
project(csf VERSION 1.3.0 LANGUAGES C CXX)

# xsflib: the code that CSF shares with the other PSF players and with the converters, as a submodule. Its
# XsflibProject.cmake sets what the projects built on it start with: C++20, a release build by default, the static C
# runtime, the options that keep the output the same on every platform, and the warnings for CSF's targets
# (CSF_WARNINGS). It refuses the compilers whose output would differ from other builds.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Run: git submodule update --init --recursive")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME CSF PREFIX CSF)

# xsflib also provides the libraries that CSF uses: Zstandard for Wii U archives (.wua), zlib for the PSF container and
# for the compressed sections of the Wii U's executables and libraries (RPX and RPL), and libFLAC for FLAC output. Each
# one is the installed copy where there's one, and otherwise is downloaded and built along with CSF.
set(XSFLIB_PSF ON)
set(XSFLIB_CLI ON)
set(XSFLIB_ZSTD ON)
xsflib_add_libraries(xsflib ntdlib)

# The include directory for all of src, and xsflib's common files: byte order and SHA-1.
add_library(csf_common INTERFACE)
target_include_directories(csf_common INTERFACE src)
target_link_libraries(csf_common INTERFACE xsflib::common)

# Wii U games in every common form: Wii U archives (.wua) and extracted titles. The library also recognises disc images
# and installable titles, and refuses them. Their files are encrypted.
add_library(csf_title STATIC
    src/title/disc.cpp
    src/title/disc.h
    src/title/source.h
    src/title/title.cpp
    src/title/title.h
    src/title/wua.cpp
    src/title/wua.h
)
target_link_libraries(csf_title PUBLIC csf_common ntdlib::disc PRIVATE xsflib::zstd)
target_compile_options(csf_title PRIVATE ${CSF_WARNINGS})

add_library(csf_ax STATIC
    src/ax/memory.h
    src/ax/pb.h
    src/ax/render.cpp
    src/ax/render.h
    src/ax/resample.cpp
    src/ax/resample.h
    src/ax/system.cpp
    src/ax/system.h
    src/ax/voice.h
)
target_include_directories(csf_ax PUBLIC src)
target_link_libraries(csf_ax PUBLIC ntdlib::ax ntdlib::dsp)
target_compile_options(csf_ax PRIVATE ${CSF_WARNINGS})

# The system library's effects for AX's aux buses (AXFX): the delay, the two reverbs and the chorus.
add_library(csf_axfx STATIC
    src/axfx/axfx.h
    src/axfx/chorus.cpp
    src/axfx/chorus.h
    src/axfx/delay.cpp
    src/axfx/delay.h
    src/axfx/math.cpp
    src/axfx/math.h
    src/axfx/reverb_hi.cpp
    src/axfx/reverb_hi.h
    src/axfx/reverb_std.cpp
    src/axfx/reverb_std.h
    src/axfx/reverb_tables.h
    src/axfx/util.h
)
target_include_directories(csf_axfx PUBLIC src)
target_link_libraries(csf_axfx PUBLIC ntdlib::common)
target_compile_options(csf_axfx PRIVATE ${CSF_WARNINGS})

# Game mode's Wii U: the loader for its executables and libraries (RPX and RPL), guest memory, and the machine that
# runs a game's code with Cafe OS at a high level.
add_library(csf_cafe STATIC
    src/cafe/coreinit.cpp
    src/cafe/coreinit.h
    src/cafe/machine.cpp
    src/cafe/machine.h
    src/cafe/memory.cpp
    src/cafe/memory.h
    src/cafe/rpl.cpp
    src/cafe/rpl.h
    src/cafe/sndcore2.cpp
    src/cafe/sndcore2.h
)
target_link_libraries(csf_cafe PUBLIC csf_ax csf_common ntdlib::ppc PRIVATE xsflib::zlib)
target_compile_options(csf_cafe PRIVATE ${CSF_WARNINGS})

# Game mode's driver, for playing a sound with a game's code, and the games' profiles.
add_library(csf_game STATIC
    src/game/driver.cpp
    src/game/game_driver.cpp
    src/game/jaudio1_driver.cpp
    src/game/jaudio2_driver.cpp
    src/game/driver.h
    src/game/game_driver.h
    src/game/jaudio1_driver.h
    src/game/jaudio2_driver.h
)
target_link_libraries(csf_game PUBLIC csf_cafe xsflib::testing)
target_compile_options(csf_game PRIVATE ${CSF_WARNINGS})

add_library(csf_nwsnd STATIC
    src/nwsnd/ax_voice.cpp
    src/nwsnd/ax_voice.h
    src/nwsnd/channel.h
    src/nwsnd/effect_setup.h
    src/nwsnd/effects.cpp
    src/nwsnd/effects.h
    src/nwsnd/engine.cpp
    src/nwsnd/engine.h
    src/nwsnd/envelope.h
    src/nwsnd/renderer.cpp
    src/nwsnd/renderer.h
    src/nwsnd/sequence.cpp
    src/nwsnd/sequence.h
    src/nwsnd/util.h
    src/nwsnd/voice.h
    src/csf/sar.h
)
target_include_directories(csf_nwsnd PUBLIC src)
target_link_libraries(csf_nwsnd PUBLIC csf_ax csf_axfx ntdlib::nwsnd ntdlib::nwsnd_archive ntdlib::sar)
target_compile_options(csf_nwsnd PRIVATE ${CSF_WARNINGS})

# The CSF file format: the PSF container, chunks, descriptors and tags, and loading a set with its libraries.
add_library(csf_format STATIC
    src/csf/format.cpp
    src/csf/format.h
)
target_include_directories(csf_format PUBLIC src)
target_link_libraries(csf_format PUBLIC csf_common xsflib::psf)
target_compile_options(csf_format PRIVATE ${CSF_WARNINGS})

# The player: a set's sound in game mode or archive mode. xsflib's player plays it back with lengths, fades and seeking,
# and renders it ahead on a worker thread for the foobar2000 component and the XMPlay plugin.
add_library(csf_player STATIC
    src/csf/player.cpp
    src/csf/player.h
)
target_include_directories(csf_player PUBLIC src)
target_link_libraries(csf_player PUBLIC csf_format csf_game csf_nwsnd xsflib::player)
target_compile_options(csf_player PRIVATE ${CSF_WARNINGS})

add_library(csf_rip STATIC
    src/rip/jaudio.cpp
    src/rip/rip.cpp
    src/rip/jaudio.h
    src/rip/rip.h
)
target_include_directories(csf_rip PUBLIC src)
target_link_libraries(csf_rip PUBLIC csf_format csf_game csf_nwsnd csf_title xsflib::rip ntdlib::rip_archive ntdlib::jaudio)
target_compile_options(csf_rip PRIVATE ${CSF_WARNINGS})

# Sets CSF_VERSION and, on Windows, adds the version resource shown in Explorer's file properties.
function(csf_add_version target description)
    xsflib_add_version(${target} VERSION ${csf_VERSION} DEFINE CSF_VERSION DESCRIPTION "${description}" PRODUCT CSF
                       COPYRIGHT "Copyright (c) 2026 the CSF authors. MIT licence.")
endfunction()

# csfrip rips on several threads (--jobs). glibc before 2.34 keeps threads in a separate library.
add_executable(csfrip src/cli/csfrip.cpp)
target_link_libraries(csfrip PRIVATE csf_rip xsflib::cli Threads::Threads)
target_compile_options(csfrip PRIVATE ${CSF_WARNINGS})
csf_add_version(csfrip "CSF ripper")

add_executable(csfplay src/cli/csfplay.cpp)
target_link_libraries(csfplay PRIVATE csf_player xsflib::cli)
target_compile_options(csfplay PRIVATE ${CSF_WARNINGS})
csf_add_version(csfplay "CSF renderer")

# Windows: UTF-8 as the tools' code page. macOS: a droplet app for each tool (csfrip.app, csfplay.app), so that files
# can be dropped on it in the Finder.
foreach (tool csfrip csfplay)
    xsflib_add_utf8_manifest(${tool})
    xsflib_add_droplet(${tool} VERSION ${csf_VERSION})
endforeach()

# foobar2000 component (Windows with clang-cl, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_csf component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component requires clang-cl on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

# XMPlay plugin (32-bit Windows with clang-cl): cmake -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=<unpacked SDK> ...
set(XMPLAY_SDK_DIR "" CACHE PATH "XMPlay plugin SDK (builds the xmp-csf plugin when set)")
if (XMPLAY_SDK_DIR)
    add_subdirectory(plugins/xmplay)
endif()

enable_testing()

# The unit tests: csf_<name>_test from tests/<name>_test.cpp, registered as csf_<name>.
# The title test writes Wii U archives. That requires the compressor as well as the decompressor.
xsflib_add_project_test(csf title csf_title xsflib::zstd)
xsflib_add_project_test(csf ax csf_ax)
# The Cafe OS test writes compressed sections with zlib's compressor.
xsflib_add_project_test(csf cafe csf_cafe xsflib::zlib)
xsflib_add_project_test(csf format csf_format)
xsflib_add_project_test(csf nwsnd csf_nwsnd csf_ax)
xsflib_add_project_test(csf player csf_player csf_rip)
xsflib_add_project_test(csf rip csf_rip)

# The tools report the version they were built as.
string(REPLACE "." "\\." version_pattern ${csf_VERSION})
foreach (tool csfrip csfplay)
    add_test(NAME version_${tool} COMMAND ${tool} --version)
    set_tests_properties(version_${tool} PROPERTIES PASS_REGULAR_EXPRESSION "^${tool} ${version_pattern}[\r\n]*$")
endforeach()

# Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
xsflib_include_local_tests()
