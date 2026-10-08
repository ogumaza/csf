# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for the tools. For a release, update it here and add notes in docs/release-notes/.
project(csf VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Reject unsupported compilers. Windows builds require clang-cl or MSVC. On 32-bit x86, GCC and Clang use x87 floating
# point with extra precision. That would make the output differ from other builds. clang-cl and MSVC use SSE2 there.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(compiler_name GCC)
else()
    set(compiler_name ${CMAKE_CXX_COMPILER_ID})
endif()
if (WIN32 AND NOT MSVC)
    message(FATAL_ERROR "CSF doesn't support ${compiler_name} on Windows, such as MinGW's GCC or the GNU-style "
                        "clang++. Build with Visual Studio's clang-cl (cmake -T ClangCL) or with MSVC; README.md has "
                        "the steps.")
endif()
if (NOT MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86|x86_64|AMD64)$")
    message(FATAL_ERROR "CSF doesn't support 32-bit x86 builds with ${compiler_name}: their output would differ from "
                        "other builds. Build for 64-bit instead, or on Windows use clang-cl or MSVC. They build "
                        "32-bit x86 with SSE2.")
endif()
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# MSVC: the static C runtime (also in subprojects that declare an older minimum CMake version). With it, a plugin
# requires no runtime DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
# clang-cl takes MSVC's options but is Clang underneath. clang-cl gets Clang's settings below through /clang:, and
# ignores MSVC's /d1trimfile.
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CSF_CLANG_CL ON)
endif()
# Output must not depend on the platform: no fused multiply-add contraction (clang contracts by default where the CPU
# has FMA, such as Apple Silicon). Then every build computes the same floats. MSVC's /fp:precise doesn't contract.
if (CSF_CLANG_CL)
    add_compile_options(/clang:-ffp-contract=off)
    # clang-cl does 32-bit x86 floating point on the x87 unit unless it's given SSE2. MSVC uses SSE2 there by default.
    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_compile_options(/arch:SSE2)
    endif()
elseif (NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()
# __FILE__ gives paths relative to the source or build directory. Debug information keeps absolute paths. Debuggers then
# find the sources. MSVC's /d1trimfile isn't documented, but Visual Studio 2019 16.4 and later have it.
if (CSF_CLANG_CL)
    add_compile_options("/clang:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/="
                        "/clang:-fmacro-prefix-map=${CMAKE_BINARY_DIR}/=")
elseif (MSVC)
    cmake_path(NATIVE_PATH CMAKE_SOURCE_DIR source_dir)
    cmake_path(NATIVE_PATH CMAKE_BINARY_DIR binary_dir)
    add_compile_options(/d1trimfile:${source_dir} /d1trimfile:${binary_dir})
else()
    add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/= -fmacro-prefix-map=${CMAKE_BINARY_DIR}/=)
endif()

if (MSVC)
    set(CSF_WARNINGS /W4)
    # fopen and freopen are standard C++. The _s versions in MSVC's warnings aren't portable.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(CSF_WARNINGS -Wall -Wextra)
endif()

# Zstandard, for Wii U archives (.wua). The system's copy is used where there's one. Otherwise Zstandard is downloaded
# and built along with CSF. A universal macOS build (more than one architecture in CMAKE_OSX_ARCHITECTURES) always
# builds it. Installed copies, such as Homebrew's copy, are built for the Mac's architecture only.
include(FetchContent)
list(LENGTH CMAKE_OSX_ARCHITECTURES osx_architectures)

if (osx_architectures LESS 2)
    find_package(zstd CONFIG QUIET)
endif()
if (TARGET zstd::libzstd)
    set(CSF_ZSTD zstd::libzstd)
else()
    FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.7
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake)
    foreach (option ZSTD_BUILD_PROGRAMS ZSTD_BUILD_TESTS ZSTD_BUILD_SHARED ZSTD_LEGACY_SUPPORT ZSTD_MULTITHREAD_SUPPORT)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    set_property(DIRECTORY ${zstd_SOURCE_DIR}/build/cmake PROPERTY EXCLUDE_FROM_ALL TRUE)
    # Only in the build: zstd's project installs the target. An installed target can't name the build folder.
    target_include_directories(libzstd_static INTERFACE $<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>)
    set(CSF_ZSTD libzstd_static)
endif()

# zlib, for the compressed sections of the Wii U's executables and libraries (RPX and RPL). The system's copy is used
# where there's one. Otherwise zlib is downloaded and built along with CSF. macOS has zlib for every architecture.
find_package(ZLIB QUIET)
if (NOT ZLIB_FOUND)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    # Build only the static library. zlib's DLL would be built by default too. The DLL's resource script doesn't compile
    # with clang-cl.
    set_property(DIRECTORY ${zlib_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# Small helpers the other libraries share (byte order, SHA-1, the PowerPC's negated fused multiply-add), and the include
# directory for all of src.
add_library(csf_common STATIC
    src/common/ascii.h
    src/common/bytes.h
    src/common/fused.h
    src/common/sha1.cpp
    src/common/sha1.h
)
target_include_directories(csf_common PUBLIC src)
target_compile_options(csf_common PRIVATE ${CSF_WARNINGS})

# Wii U games in every common form: Wii U archives (.wua) and extracted titles. The library also recognises disc images
# and installable titles, and refuses them. Their files are encrypted.
add_library(csf_title STATIC
    src/title/disc.cpp
    src/title/disc.h
    src/title/source.cpp
    src/title/source.h
    src/title/title.cpp
    src/title/title.h
    src/title/wua.cpp
    src/title/wua.h
)
target_link_libraries(csf_title PUBLIC csf_common PRIVATE ${CSF_ZSTD})
target_compile_options(csf_title PRIVATE ${CSF_WARNINGS})

# The NintendoWare for Cafe sound formats: FSAR, FSEQ, FBNK, FWAR, FWAV and FGRP.
add_library(csf_fsar STATIC
    src/fsar/binary.h
    src/fsar/formats.cpp
    src/fsar/formats.h
)
target_link_libraries(csf_fsar PUBLIC csf_common)
target_compile_options(csf_fsar PRIVATE ${CSF_WARNINGS})

# The PowerPC interpreter, from WSF: Broadway's instruction sets. The Wii U's Espresso runs them too.
add_library(csf_ppc STATIC
    src/ppc/cpu.cpp
    src/ppc/cpu.h
    src/ppc/fpu.cpp
)
target_link_libraries(csf_ppc PUBLIC csf_common)
target_compile_options(csf_ppc PRIVATE ${CSF_WARNINGS})

add_library(csf_ax STATIC
    src/ax/four_tap.h
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
target_compile_options(csf_ax PRIVATE ${CSF_WARNINGS})

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
target_link_libraries(csf_cafe PUBLIC csf_ax csf_common csf_ppc PRIVATE ZLIB::ZLIB)
target_compile_options(csf_cafe PRIVATE ${CSF_WARNINGS})

# Game mode's driver, for playing a sound with a game's code, and the games' profiles.
add_library(csf_game STATIC
    src/game/driver.cpp
    src/game/driver.h
)
target_link_libraries(csf_game PUBLIC csf_cafe)
target_compile_options(csf_game PRIVATE ${CSF_WARNINGS})

add_library(csf_nwsnd STATIC
    src/nwsnd/ax_voice.cpp
    src/nwsnd/ax_voice.h
    src/nwsnd/channel.cpp
    src/nwsnd/channel.h
    src/nwsnd/engine.cpp
    src/nwsnd/engine.h
    src/nwsnd/envelope.cpp
    src/nwsnd/envelope.h
    src/nwsnd/sequence.cpp
    src/nwsnd/sequence.h
    src/nwsnd/tables.h
    src/nwsnd/util.cpp
    src/nwsnd/util.h
    src/nwsnd/voice.cpp
    src/nwsnd/voice.h
)
target_include_directories(csf_nwsnd PUBLIC src)
target_link_libraries(csf_nwsnd PUBLIC csf_ax csf_fsar)
target_compile_options(csf_nwsnd PRIVATE ${CSF_WARNINGS})

# The CSF file format: the PSF container, chunks, descriptors and tags, and loading a set with its libraries.
add_library(csf_format STATIC
    src/csf/format.cpp
    src/csf/format.h
)
target_include_directories(csf_format PUBLIC src)
target_link_libraries(csf_format PUBLIC csf_common PRIVATE ZLIB::ZLIB)
target_compile_options(csf_format PRIVATE ${CSF_WARNINGS})

# The player: a set's sound in game mode or archive mode, its playback with lengths, fades and seeking, and rendering
# ahead on a worker thread for the foobar2000 component.
add_library(csf_player STATIC
    src/csf/playback.cpp
    src/csf/playback.h
    src/csf/player.cpp
    src/csf/player.h
    src/csf/render_ahead.cpp
    src/csf/render_ahead.h
)
find_package(Threads REQUIRED)
target_include_directories(csf_player PUBLIC src)
target_link_libraries(csf_player PUBLIC csf_format csf_game csf_nwsnd Threads::Threads)
target_compile_options(csf_player PRIVATE ${CSF_WARNINGS})

add_library(csf_rip STATIC
    src/rip/rip.cpp
    src/rip/rip.h
)
target_include_directories(csf_rip PUBLIC src)
target_link_libraries(csf_rip PUBLIC csf_format csf_game csf_nwsnd csf_title)
target_compile_options(csf_rip PRIVATE ${CSF_WARNINGS})

# Sets CSF_VERSION and, on Windows, adds the version resource shown in Explorer's file properties.
function(csf_add_version target description)
    target_compile_definitions(${target} PRIVATE CSF_VERSION="${csf_VERSION}")
    if (WIN32)
        get_target_property(type ${target} TYPE)
        if (type STREQUAL "EXECUTABLE")
            set(VERSION_FILE_TYPE VFT_APP)
            set(VERSION_FILE_NAME ${target}${CMAKE_EXECUTABLE_SUFFIX})
        else()
            set(VERSION_FILE_TYPE VFT_DLL)
            set(VERSION_FILE_NAME ${target}${CMAKE_SHARED_LIBRARY_SUFFIX})
        endif()
        set(VERSION_DESCRIPTION ${description})
        set(VERSION_NAME ${target})
        set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
        configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/platform/windows/version.rc.in ${rc} @ONLY)
        target_sources(${target} PRIVATE ${rc})
    endif()
endfunction()

# WAV/FLAC output and the console for the command-line tools. libFLAC comes from its CMake package (vcpkg and other
# CMake-built installs), or else from pkg-config (Homebrew and Linux distributions). When neither has it, libFLAC is
# fetched and built from source. A universal macOS build (more than one architecture in CMAKE_OSX_ARCHITECTURES) always
# builds it. An installed libFLAC (Homebrew's build, for example) is built for the Mac's architecture only.
add_library(csf_cli STATIC src/cli/audio_out.cpp src/cli/audio_out.h src/cli/console.cpp src/cli/console.h)
target_link_libraries(csf_cli PUBLIC csf_common)
target_compile_options(csf_cli PRIVATE ${CSF_WARNINGS})
if (osx_architectures LESS 2)
    find_package(FLAC CONFIG QUIET)
    if (FLAC_FOUND)
        set(CSF_FLAC FLAC::FLAC)
    else()
        find_package(PkgConfig QUIET)
        if (PKG_CONFIG_FOUND)
            pkg_check_modules(FLAC QUIET IMPORTED_TARGET flac)
        endif()
        if (FLAC_FOUND)
            set(CSF_FLAC PkgConfig::FLAC)
        endif()
    endif()
endif()
if (NOT CSF_FLAC)
    FetchContent_Declare(flac
        GIT_REPOSITORY https://github.com/xiph/flac.git
        GIT_TAG 1.5.0
        GIT_SHALLOW TRUE)
    foreach (option BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS INSTALL_MANPAGES
                    INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE WITH_OGG)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_MakeAvailable(flac)
    # Build only libFLAC: the helper libraries of flac's programs would be built by default too.
    set_property(DIRECTORY ${flac_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    set(CSF_FLAC FLAC::FLAC)
endif()
target_link_libraries(csf_cli PRIVATE ${CSF_FLAC})

# csfrip rips on several threads (--jobs). glibc before 2.34 keeps threads in a separate library.
add_executable(csfrip src/cli/csfrip.cpp)
target_link_libraries(csfrip PRIVATE csf_rip csf_cli Threads::Threads)
target_compile_options(csfrip PRIVATE ${CSF_WARNINGS})
csf_add_version(csfrip "CSF ripper")

add_executable(csfplay src/cli/csfplay.cpp)
target_link_libraries(csfplay PRIVATE csf_player csf_cli)
target_compile_options(csfplay PRIVATE ${CSF_WARNINGS})
csf_add_version(csfplay "CSF renderer")

foreach (tool csfrip csfplay)
    # Windows: UTF-8 as the tools' code page. The tools can then take and open paths outside the system's ANSI code
    # page, such as the paths of files dropped on them. The linker merges the manifest into the one it makes.
    if (MSVC)
        target_sources(${tool} PRIVATE platform/windows/utf8.manifest)
    endif()

    # macOS: a droplet app for each tool (csfrip.app, csfplay.app). Files can be dropped on the app in the Finder. Each
    # app is an AppleScript applet with the tool inside, in Contents/Resources, and the version in its Info.plist.
    if (APPLE)
        find_program(OSACOMPILE osacompile)
        find_program(CODESIGN codesign)
        find_program(PLUTIL plutil)
        if (OSACOMPILE AND CODESIGN AND PLUTIL)
            set(DROPLET_TOOL ${tool})
            set(droplet_app ${CMAKE_BINARY_DIR}/${tool}.app)
            set(droplet_script ${CMAKE_BINARY_DIR}/${tool}.applescript)
            configure_file(platform/macos/droplet.applescript.in ${droplet_script} @ONLY)
            add_custom_command(
                OUTPUT ${droplet_app}/Contents/Resources/${tool}
                COMMAND ${CMAKE_COMMAND} -E rm -rf ${droplet_app}
                COMMAND ${OSACOMPILE} -o ${droplet_app} ${droplet_script}
                COMMAND ${PLUTIL} -replace CFBundleShortVersionString -string ${csf_VERSION}
                        ${droplet_app}/Contents/Info.plist
                COMMAND ${PLUTIL} -replace CFBundleVersion -string ${csf_VERSION} ${droplet_app}/Contents/Info.plist
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${tool}> ${droplet_app}/Contents/Resources/${tool}
                COMMAND ${CODESIGN} --force --deep --sign - ${droplet_app}
                DEPENDS ${tool} ${droplet_script}
                COMMENT "Building ${tool}.app"
                VERBATIM)
            add_custom_target(${tool}_app ALL DEPENDS ${droplet_app}/Contents/Resources/${tool})
        endif()
    endif()
endforeach()

# foobar2000 component (Windows with clang-cl or MSVC, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_csf component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component requires clang-cl or MSVC on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

# The title test writes Wii U archives. That requires the compressor as well as the decompressor.
add_executable(title_test tests/title_test.cpp tests/check.h)
target_link_libraries(title_test PRIVATE csf_title ${CSF_ZSTD})
target_compile_options(title_test PRIVATE ${CSF_WARNINGS})

# The audio writer's test decodes the FLAC files it writes. For that, the test requires libFLAC's decoder too.
add_executable(audio_out_test tests/audio_out_test.cpp tests/check.h)
target_link_libraries(audio_out_test PRIVATE csf_cli ${CSF_FLAC})
target_compile_options(audio_out_test PRIVATE ${CSF_WARNINGS})

add_executable(ax_test tests/ax_test.cpp tests/ax_test_hashes.h tests/check.h)
target_link_libraries(ax_test PRIVATE csf_ax)
target_compile_options(ax_test PRIVATE ${CSF_WARNINGS})

# The Cafe OS test writes compressed sections with zlib's compressor.
add_executable(cafe_test tests/cafe_test.cpp tests/check.h)
target_link_libraries(cafe_test PRIVATE csf_cafe ZLIB::ZLIB)
target_compile_options(cafe_test PRIVATE ${CSF_WARNINGS})

add_executable(format_test tests/format_test.cpp tests/check.h)
target_link_libraries(format_test PRIVATE csf_format ZLIB::ZLIB)
target_compile_options(format_test PRIVATE ${CSF_WARNINGS})

add_executable(fsar_test tests/fsar_test.cpp tests/check.h tests/fsar_files.h)
target_link_libraries(fsar_test PRIVATE csf_fsar)
target_compile_options(fsar_test PRIVATE ${CSF_WARNINGS})

add_executable(nwsnd_test tests/nwsnd_test.cpp tests/check.h)
target_link_libraries(nwsnd_test PRIVATE csf_nwsnd csf_ax)
target_compile_options(nwsnd_test PRIVATE ${CSF_WARNINGS})

add_executable(player_test tests/player_test.cpp tests/check.h tests/fsar_files.h)
target_link_libraries(player_test PRIVATE csf_player)
target_compile_options(player_test PRIVATE ${CSF_WARNINGS})

add_executable(render_ahead_test tests/render_ahead_test.cpp tests/check.h)
target_link_libraries(render_ahead_test PRIVATE csf_player)
target_compile_options(render_ahead_test PRIVATE ${CSF_WARNINGS})

add_executable(rip_test tests/rip_test.cpp tests/check.h tests/fsar_files.h)
target_link_libraries(rip_test PRIVATE csf_rip)
target_compile_options(rip_test PRIVATE ${CSF_WARNINGS})

add_executable(sha1_test tests/sha1_test.cpp tests/check.h)
target_link_libraries(sha1_test PRIVATE csf_common)
target_compile_options(sha1_test PRIVATE ${CSF_WARNINGS})

enable_testing()
add_test(NAME audio_out COMMAND audio_out_test)
add_test(NAME ax COMMAND ax_test)
add_test(NAME cafe COMMAND cafe_test)
add_test(NAME format COMMAND format_test)
add_test(NAME fsar COMMAND fsar_test)
add_test(NAME nwsnd COMMAND nwsnd_test)
add_test(NAME player COMMAND player_test)
add_test(NAME render_ahead COMMAND render_ahead_test)
add_test(NAME rip COMMAND rip_test)
add_test(NAME sha1 COMMAND sha1_test)
add_test(NAME title COMMAND title_test)

# The tools report the version they were built as.
string(REPLACE "." "\\." version_pattern ${csf_VERSION})
foreach (tool csfrip csfplay)
    add_test(NAME version_${tool} COMMAND ${tool} --version)
    set_tests_properties(version_${tool} PROPERTIES PASS_REGULAR_EXPRESSION "^${tool} ${version_pattern}[\r\n]*$")
endforeach()

# Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
file(GLOB local_tests CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/*/local_tests.cmake)
foreach (file IN LISTS local_tests)
    include(${file})
endforeach()
