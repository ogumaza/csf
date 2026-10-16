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

# AX, the system library's effects for AX's aux buses (AXFX) and archive mode's model of nw::snd on AX come from
# ntdlib's cafe module (ntdlib::cafe). Game mode runs a game's code on the same AX.

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
target_link_libraries(csf_cafe PUBLIC csf_common ntdlib::cafe ntdlib::ppc PRIVATE xsflib::zlib)
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

# The CSF file format: the PSF container, chunks, descriptors and tags, and loading a set with its libraries.
add_library(csf_format STATIC
    src/csf/format.cpp
    src/csf/format.h
    src/csf/ntdlib_cafe.h
    src/csf/sar.h
)
target_include_directories(csf_format PUBLIC src)
target_link_libraries(csf_format PUBLIC csf_common ntdlib::cafe xsflib::psf)
target_compile_options(csf_format PRIVATE ${CSF_WARNINGS})

# The player: a set's sound in game mode or archive mode. xsflib's player plays it back with lengths, fades and seeking,
# and renders it ahead on a worker thread for the foobar2000 component and the XMPlay plugin.
add_library(csf_player STATIC
    src/csf/player.cpp
    src/csf/player.h
)
target_include_directories(csf_player PUBLIC src)
target_link_libraries(csf_player PUBLIC csf_format csf_game ntdlib::cafe xsflib::player)
target_compile_options(csf_player PRIVATE ${CSF_WARNINGS})

add_library(csf_rip STATIC
    src/rip/jaudio.cpp
    src/rip/rip.cpp
    src/rip/jaudio.h
    src/rip/rip.h
)
target_include_directories(csf_rip PUBLIC src)
target_link_libraries(csf_rip PUBLIC csf_format csf_game csf_title ntdlib::cafe xsflib::rip ntdlib::rip_archive
                                     ntdlib::jaudio)
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
# The Cafe OS test writes compressed sections with zlib's compressor.
xsflib_add_project_test(csf cafe csf_cafe xsflib::zlib)
xsflib_add_project_test(csf format csf_format)
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
