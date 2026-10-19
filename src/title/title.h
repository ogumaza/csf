// SPDX-License-Identifier: MIT

// A Wii U game's files, from a Wii U archive (.wua) or an extracted title: a folder holding code/, content/ and meta/.
// Cemu, Wii U USB Helper and the decryption tools leave such a folder. Disc images (WUD, WUX) and installable titles
// (title.tmd with .app files) hold their files encrypted. They're recognised and refused. The refusal message says how
// to convert them.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace csf::title
{

// A file in a title.
struct TitleFile
{
    std::string path; // from the title's folder, without a leading '/' ("content/sound/test.bfsar")
    uint64_t size;
};

struct GameFiles
{
    // Paths of the sound archives (.bfsar) under content/, in the order that `files` lists them.
    std::vector<std::string> SoundArchives() const;

    // The title ID as Nintendo writes it, with its high and low words apart ("00050000-12345600").
    std::string TitleIdText() const;

    // The SDK version as Nintendo writes it ("2.07.03"), or "unknown".
    std::string SdkVersionText() const;

    // The executable's path in `files` ("code/test.rpx").
    std::string ExecutablePath() const;

    // What the files came from, for messages: "WUA" or "extracted title".
    std::string source;

    // From meta/meta.xml. The title ID and version come from code/app.xml where a title has no meta.xml.
    std::string product_code; // "WUP-P-ATSE"
    std::string name;         // the English long name, with its lines joined ("Sound & Test Deluxe")
    uint64_t title_id = 0;
    uint32_t title_version = 0; // the update's version if there is an update, and otherwise the game's version

    // From code/app.xml and code/cos.xml.
    uint32_t sdk_version = 0; // 20703 for SDK 2.07.03
    std::string executable;   // the RPX in code/ that the system starts ("test.rpx")

    // The ID of another title whose code/app.xml, code/cos.xml and meta/meta.xml the game's folder holds in place of
    // the game's, or 0. Extracting a disc's system title into the game's folder leaves the folder that way. The game's
    // title ID, version, product code, name and SDK version are then unknown. Those fields stay 0 or empty. The game's
    // executable is then the one RPX in code/.
    uint64_t foreign_title_id = 0;

    // True when an update's files were laid over the game's files.
    bool updated = false;

    // Every file, in order of their paths.
    std::vector<TitleFile> files;

    // Reads a file by its path in `files`. It shares the archive's cache. Only one thread may call it at a time.
    std::function<bool(const std::string& path, std::vector<uint8_t>& data)> read_file;
};

// Loads a Wii U game: a .wua, an extracted title, or a folder holding extracted titles (such as a .wua that ZArchive's
// tool extracted). Where an update for the game is there too, the update's files are laid over the game's files. The
// console does the same. Downloadable content is left out. A path to the .rpx in an extracted title's code/ folder
// loads that title. A folder that holds another title's XML files over a game's files loads as that game, without its
// title (GameFiles::foreign_title_id). Returns an error message, or nothing on success.
std::optional<std::string> LoadGame(const std::filesystem::path& path, GameFiles& out);

// A title ID as Nintendo writes it, with its high and low words apart ("00050000-12345600").
std::string TitleIdText(uint64_t title_id);

} // namespace csf::title
