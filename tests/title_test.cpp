// SPDX-License-Identifier: MIT

// Builds a small Wii U game in every form that CSF supports, and checks how the readers read each one. The readers must
// read the game as an extracted title, a folder holding titles, and a Wii U archive (.wua) written here in ZArchive's
// layout. Where an update and downloadable content are beside the game, the readers must lay the update over the game
// and leave the downloadable content out. A folder's AppleDouble files must be left out too. The readers must pass over
// a system title beside the game. Where a system title's XML files replace the game's, the readers must find the game
// by its executable. The readers must read the disc ID of a WUD, whole and in wudump's parts, and of a WUX, and refuse
// them. They must also refuse an installable title, other consoles' files and damaged archives. `title_test <folder>`
// keeps the files in the folder.

#include "title/title.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"
#include <zstd.h>

#include "title/disc.h"
#include "title/source.h"
#include "title/wua.h"

using xsflib::testing::Check;

namespace
{

namespace fs = std::filesystem;

using xsflib::LoadBe64;
using xsflib::StoreBe16;
using xsflib::StoreBe32;
using xsflib::StoreBe64;
using xsflib::StoreLe32;
using xsflib::StoreLe64;

// A title's files by their paths.
using Files = std::map<std::string, std::vector<uint8_t>>;

constexpr uint64_t kGameId = 0x0005000012345600;
constexpr uint64_t kUpdateId = 0x0005000E12345600;
constexpr uint64_t kContentId = 0x0005000C12345600;
constexpr uint64_t kSystemId = 0x0005001012345600;
constexpr char kDiscId[] = "WUP-P-ATSE-00-000USA-0";

std::vector<uint8_t> Bytes(const std::string& text)
{
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> Random(size_t size, uint32_t seed)
{
    std::mt19937 random(seed);
    std::vector<uint8_t> data(size);
    for (uint8_t& byte : data)
    {
        byte = static_cast<uint8_t>(random());
    }

    return data;
}

std::string Hex(uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(value));

    return text;
}

void WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
{
    fs::create_directories(path.parent_path());
    const std::optional<std::string> error = xsflib::WriteFile(path, data);
    Check(!error, error.value_or(""));
}

void WriteFiles(const fs::path& folder, const Files& files)
{
    for (const auto& [path, data] : files)
    {
        WriteFile(folder / path, data);
    }
}

// A title's XML files. They name `executable` as the title's executable. The system writes them in this form.
Files TitleXml(uint64_t title_id, uint32_t version, uint32_t sdk_version, const std::string& executable)
{
    Files files;
    files["code/app.xml"] = Bytes(
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<app type=\"complex\" access=\"777\">\n"
        "  <title_id type=\"hexBinary\" length=\"8\">" +
        Hex(title_id) + "</title_id>\n  <sdk_version type=\"unsignedInt\" length=\"4\">" + std::to_string(sdk_version) +
        "</sdk_version>\n</app>\n");
    files["code/cos.xml"] = Bytes(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<app type=\"complex\" access=\"777\">\n"
        "  <argstr type=\"string\" length=\"4096\">" +
        executable + "</argstr>\n</app>\n");
    files["meta/meta.xml"] = Bytes(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<menu type=\"complex\" access=\"777\">\n"
        "  <product_code type=\"string\" length=\"32\">WUP-P-ATSE</product_code>\n"
        "  <title_version type=\"unsignedInt\" length=\"4\">" +
        std::to_string(version) + "</title_version>\n  <title_id type=\"hexBinary\" length=\"8\">" + Hex(title_id) +
        "</title_id>\n  <longname_en type=\"string\" length=\"512\">Sound &amp; Test\nDeluxe&#x2122;</longname_en>\n"
        "</menu>\n");

    return files;
}

// The game: its XML files, an executable, sound archives, a stream, a file that spans blocks of a .wua, one that
// doesn't compress, a long name, and a folder in its content that looks like a title.
Files Game()
{
    Files files = TitleXml(kGameId, 0, 20703, "test.rpx");
    files["code/test.rpx"] = Random(5000, 1);
    files["content/sound/test.bfsar"] = Bytes("FSAR" + std::string(1000, 'x'));
    files["content/sound/stream/music.bfstm"] = Bytes("FSTM");
    files["content/Voice/Second.BFSAR"] = Bytes("FSAR");

    std::vector<uint8_t> pattern;
    while (pattern.size() < 200000)
    {
        const std::string line = "line " + std::to_string(pattern.size()) + " of a file that compresses well\n";
        pattern.insert(pattern.end(), line.begin(), line.end());
    }
    files["content/extra/pattern.bin"] = pattern;
    files["content/extra/noise.bin"] = Random(70000, 2);
    files["content/extra/" + std::string(130, 'n') + ".bin"] = Bytes("a name of 130 characters");
    files["content/code/app.xml"] = Bytes("<title_id>0005000099999900</title_id>");

    return files;
}

// The game's update: a new executable and XML files, a sound archive that replaces one of the game's archives, and one
// that's new.
Files Update()
{
    Files files = TitleXml(kUpdateId, 32, 21212, "test.rpx");
    files["code/test.rpx"] = Random(6000, 3);
    files["content/sound/test.bfsar"] = Bytes("FSAR, updated");
    files["content/sound/new.bfsar"] = Bytes("FSAR, new");

    return files;
}

Files DownloadableContent()
{
    Files files;
    files["meta/meta.xml"] = Bytes("<title_id type=\"hexBinary\" length=\"8\">" + Hex(kContentId) + "</title_id>");
    files["content/0010/dlc.bfsar"] = Bytes("FSAR, downloadable");

    return files;
}

// A system title's XML files. They name an executable that the game doesn't have.
Files SystemXml()
{
    return TitleXml(kSystemId, 0, 21001, "system.rpx");
}

// The game, with a system title's XML files in place of the game's. Extracting a disc's system title into the game's
// folder leaves the folder that way.
Files GameUnderSystemXml()
{
    Files files = Game();
    for (const auto& [path, data] : SystemXml())
    {
        files[path] = data;
    }

    return files;
}

// An AppleDouble file with no entries: the magic 0x00051607, version 2, 16 bytes of filler and an entry count of 0 (RFC
// 1740).
std::vector<uint8_t> AppleDouble()
{
    std::vector<uint8_t> file(26, 0);
    StoreBe32(file.data(), 0x00051607);
    StoreBe32(file.data() + 4, 0x00020000);

    return file;
}

// `files` with an AppleDouble file beside each of them. macOS writes one, named "._" and the file's name, beside each
// file that it copies to a FAT or exFAT drive.
Files WithAppleDouble(const Files& files)
{
    Files copied = files;
    for (const auto& [path, data] : files)
    {
        const std::size_t name = path.rfind('/') + 1;
        copied[path.substr(0, name) + "._" + path.substr(name)] = AppleDouble();
    }

    return copied;
}

// `titles`' files under folders named by their title IDs and versions. Cemu's archives keep them that way.
Files InFolders(const std::vector<std::pair<std::string, Files>>& titles)
{
    Files files;
    for (const auto& [folder, title] : titles)
    {
        for (const auto& [path, data] : title)
        {
            files[folder + "/" + path] = data;
        }
    }

    return files;
}

bool LessIgnoringCase(const std::string& a, const std::string& b)
{
    const std::string x = xsflib::AsciiLower(a);
    const std::string y = xsflib::AsciiLower(b);
    return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());
}

// The size of a ZArchive's blocks: 64 KiB.
constexpr size_t kZArchiveBlock = 64 * 1024;

// Where a file's contents lie in a ZArchive's data: their offset and size.
using Placed = std::map<std::string, std::pair<uint64_t, uint64_t>>;

// The contents of `files` one after another, padded to whole blocks. Sets `placed` to where each file's contents lie.
std::vector<uint8_t> ZArchiveData(const Files& files, Placed& placed)
{
    std::vector<uint8_t> data;
    for (const auto& [path, contents] : files)
    {
        placed[path] = {data.size(), contents.size()};
        data.insert(data.end(), contents.begin(), contents.end());
    }
    data.resize((data.size() + kZArchiveBlock - 1) / kZArchiveBlock * kZArchiveBlock);

    return data;
}

// Appends the blocks of `data` to `out`, each compressed with Zstandard where that makes it smaller. Returns the offset
// records: for each 16 blocks, the offset of the first and the size of each, less one.
std::vector<uint8_t> AppendZArchiveBlocks(const std::vector<uint8_t>& data, std::vector<uint8_t>& out)
{
    std::vector<uint8_t> records;
    std::vector<uint8_t> packed(ZSTD_compressBound(kZArchiveBlock));
    for (size_t block = 0; block < data.size() / kZArchiveBlock; ++block)
    {
        if (block % 16 == 0)
        {
            records.resize(records.size() + 40);
            xsflib::StoreBe64(records.data() + records.size() - 40, out.size());
        }

        const uint8_t* raw = data.data() + block * kZArchiveBlock;
        size_t size = ZSTD_compress(packed.data(), packed.size(), raw, kZArchiveBlock, 3);
        if (ZSTD_isError(size) || size >= kZArchiveBlock)
        {
            size = kZArchiveBlock;
            out.insert(out.end(), raw, raw + kZArchiveBlock);
        }
        else
        {
            out.insert(out.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(size));
        }

        xsflib::StoreBe16(records.data() + records.size() - 40 + 8 + (block % 16) * 2, static_cast<uint16_t>(size - 1));
    }

    return records;
}

// A folder of a ZArchive's tree: its entries by name.
struct ZArchiveFolder
{
    std::map<std::string, ZArchiveFolder> folders;
    std::map<std::string, std::string> files; // name → path
};

// The tree of the folders and files that `files`' paths name.
ZArchiveFolder ZArchiveTree(const Files& files)
{
    ZArchiveFolder root;
    for (const auto& [path, contents] : files)
    {
        ZArchiveFolder* folder = &root;
        size_t start = 0;
        for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', start))
        {
            folder = &folder->folders[path.substr(start, slash - start)];
            start = slash + 1;
        }
        folder->files[path.substr(start)] = path;
    }

    return root;
}

// A ZArchive's names: each name once, after its length, in the order of the first lookups.
class ZArchiveNames
{
public:
    // The offset of `name` in the names. A name that isn't there yet is added.
    uint32_t Offset(const std::string& name)
    {
        if (!offsets_.count(name))
        {
            offsets_[name] = static_cast<uint32_t>(bytes_.size());
            if (name.size() >= 0x80)
            {
                bytes_.push_back(static_cast<uint8_t>(0x80 | (name.size() & 0x7F)));
                bytes_.push_back(static_cast<uint8_t>(name.size() >> 7));
            }
            else
            {
                bytes_.push_back(static_cast<uint8_t>(name.size()));
            }
            bytes_.insert(bytes_.end(), name.begin(), name.end());
        }

        return offsets_[name];
    }

    const std::vector<uint8_t>& Bytes() const
    {
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    std::map<std::string, uint32_t> offsets_;
};

// The entries of the tree from `root`, breadth first, with each folder's entries sorted by name ignoring case. A
// file's entry says where `placed` puts its contents. Each entry's name goes into `names`.
std::vector<uint8_t> ZArchiveEntries(const ZArchiveFolder& root, const Placed& placed, ZArchiveNames& names)
{
    struct Pending
    {
        std::string name;
        const ZArchiveFolder* folder; // null for a file
        std::string path;
    };
    std::vector<uint8_t> tree;
    std::deque<Pending> queue = {{"", &root, ""}};
    uint32_t next = 1;
    while (!queue.empty())
    {
        const Pending item = queue.front();
        queue.pop_front();

        uint8_t entry[16] = {};
        const uint32_t name = item.name.empty() ? 0x7FFFFFFF : names.Offset(item.name);
        if (item.folder)
        {
            std::vector<Pending> children;
            for (const auto& [child, folder] : item.folder->folders)
            {
                children.push_back({child, &folder, ""});
            }
            for (const auto& [child, path] : item.folder->files)
            {
                children.push_back({child, nullptr, path});
            }
            const auto by_name = [](const Pending& a, const Pending& b)
            {
                return LessIgnoringCase(a.name, b.name);
            };
            std::sort(children.begin(), children.end(), by_name);

            xsflib::StoreBe32(entry, name);
            xsflib::StoreBe32(entry + 4, next);
            xsflib::StoreBe32(entry + 8, static_cast<uint32_t>(children.size()));
            next += static_cast<uint32_t>(children.size());
            queue.insert(queue.end(), children.begin(), children.end());
        }
        else
        {
            const auto [offset, size] = placed.at(item.path);
            xsflib::StoreBe32(entry, 0x80000000 | name);
            xsflib::StoreBe32(entry + 4, static_cast<uint32_t>(offset));
            xsflib::StoreBe32(entry + 8, static_cast<uint32_t>(size));
            xsflib::StoreBe32(entry + 12, static_cast<uint32_t>(((size >> 32) << 16) | (offset >> 32)));
        }

        tree.insert(tree.end(), entry, entry + sizeof(entry));
    }

    return tree;
}

// Writes `files` as a ZArchive, in the layout that ZArchive's writer uses: the data cut into 64 KiB blocks, compressed
// with Zstandard where that makes them smaller; the offset records; the names; the tree, breadth first, with each
// folder's entries sorted by name ignoring case; and the footer, with its hash left as zeros.
std::vector<uint8_t> ZArchiveOf(const Files& files)
{
    Placed placed;
    std::vector<uint8_t> out;
    const std::vector<uint8_t> records = AppendZArchiveBlocks(ZArchiveData(files, placed), out);
    const uint64_t data_size = out.size();
    out.resize((out.size() + 7) / 8 * 8);

    ZArchiveNames names;
    const std::vector<uint8_t> tree = ZArchiveEntries(ZArchiveTree(files), placed, names);

    // The sections, then the footer that locates them.
    uint64_t sections[12] = {0, data_size, out.size(), records.size()};
    out.insert(out.end(), records.begin(), records.end());
    sections[4] = out.size();
    sections[5] = names.Bytes().size();
    out.insert(out.end(), names.Bytes().begin(), names.Bytes().end());
    sections[6] = out.size();
    sections[7] = tree.size();
    out.insert(out.end(), tree.begin(), tree.end());
    sections[8] = sections[10] = out.size();

    uint8_t footer[144] = {};
    for (size_t i = 0; i < 12; ++i)
    {
        xsflib::StoreBe64(footer + i * 8, sections[i]);
    }
    xsflib::StoreBe64(footer + 128, out.size() + sizeof(footer));
    xsflib::StoreBe32(footer + 136, 0x61BF3A01);
    xsflib::StoreBe32(footer + 140, 0x169F52D6);
    out.insert(out.end(), footer, footer + sizeof(footer));

    return out;
}

// A disc: its ID, then sectors of random bytes, repeats of them and zeros, and a last sector that's only partly there.
// A WUX of it then stores fewer sectors than the disc has.
std::vector<uint8_t> Disc()
{
    constexpr size_t kSector = 0x8000;
    std::vector<uint8_t> disc(kSector * 9 + 0x1234);
    const std::vector<uint8_t> noise = Random(kSector * 2, 4);
    std::copy(noise.begin(), noise.end(), disc.begin() + kSector);
    std::copy(noise.begin(), noise.begin() + kSector, disc.begin() + kSector * 3);
    std::copy(noise.begin(), noise.begin() + kSector, disc.begin() + kSector * 6);
    std::fill(disc.begin() + kSector * 8, disc.end(), uint8_t{0x5A});
    std::memcpy(disc.data(), kDiscId, std::strlen(kDiscId));

    return disc;
}

// A WUX of `disc`: the header, the table of sectors, and each distinct sector once, from the first multiple of the
// sector size after the table.
std::vector<uint8_t> Wux(const std::vector<uint8_t>& disc, uint32_t sector_size)
{
    const size_t sectors = (disc.size() + sector_size - 1) / sector_size;
    std::vector<uint8_t> out(0x20 + sectors * 4);
    xsflib::StoreLe32(out.data(), 0x30585557);
    xsflib::StoreLe32(out.data() + 4, 0x1099D02E);
    xsflib::StoreLe32(out.data() + 8, sector_size);
    xsflib::StoreLe64(out.data() + 0x10, disc.size());

    std::vector<std::vector<uint8_t>> stored;
    for (size_t i = 0; i < sectors; ++i)
    {
        std::vector<uint8_t> sector(sector_size);
        const size_t size = std::min<size_t>(sector_size, disc.size() - i * sector_size);
        std::copy_n(disc.begin() + static_cast<std::ptrdiff_t>(i * sector_size), size, sector.begin());
        const auto found = std::find(stored.begin(), stored.end(), sector);
        xsflib::StoreLe32(out.data() + 0x20 + i * 4, static_cast<uint32_t>(found - stored.begin()));
        if (found == stored.end())
        {
            stored.push_back(sector);
        }
    }

    out.resize((out.size() + sector_size - 1) / sector_size * sector_size);
    for (const std::vector<uint8_t>& sector : stored)
    {
        out.insert(out.end(), sector.begin(), sector.end());
    }

    return out;
}

// Checks that `game` holds exactly the files `expected`, and that each reads back.
void CheckFiles(const std::string& what, csf::title::GameFiles& game, const Files& expected)
{
    std::vector<std::string> paths;
    for (const csf::title::TitleFile& file : game.files)
    {
        paths.push_back(file.path);
    }
    std::vector<std::string> expected_paths;
    for (const auto& [path, data] : expected)
    {
        expected_paths.push_back(path);
    }
    Check(paths == expected_paths, what + ": the files' paths");

    for (const auto& [path, data] : expected)
    {
        std::vector<uint8_t> read;
        Check(game.read_file(path, read) && read == data, what + ": " + path + " reads back");
    }
}

// Checks that `path` loads as the test game, with the update's files laid over it when `updated`. Returns the game.
csf::title::GameFiles CheckGame(const std::string& what, const fs::path& path, bool updated)
{
    csf::title::GameFiles game;
    const std::optional<std::string> error = csf::title::LoadGame(path, game);
    Check(!error, what + " loads" + (error ? ": " + *error : ""));
    if (error)
    {
        return game;
    }

    Files expected = Game();
    if (updated)
    {
        for (const auto& [path, data] : Update())
        {
            expected[path] = data;
        }
    }

    Check(game.title_id == kGameId, what + ": title ID " + game.TitleIdText());
    Check(game.title_version == (updated ? 32u : 0u), what + ": version " + std::to_string(game.title_version));
    Check(game.updated == updated, what + ": updated");
    Check(game.product_code == "WUP-P-ATSE", what + ": product code " + game.product_code);
    Check(game.name == "Sound & Test Deluxe\xE2\x84\xA2", what + ": name " + game.name);
    Check(game.SdkVersionText() == (updated ? "2.12.12" : "2.07.03"), what + ": SDK " + game.SdkVersionText());
    Check(game.executable == "test.rpx", what + ": executable " + game.executable);
    Check(game.foreign_title_id == 0, what + ": the XML files are the game's");
    CheckFiles(what, game, expected);

    const std::vector<std::string> archives = game.SoundArchives();
    std::vector<std::string> expected_archives = {"content/Voice/Second.BFSAR", "content/sound/test.bfsar"};
    if (updated)
    {
        expected_archives.insert(expected_archives.begin() + 1, "content/sound/new.bfsar");
    }
    std::sort(expected_archives.begin(), expected_archives.end());
    Check(archives == expected_archives, what + ": the sound archives");

    std::vector<uint8_t> read;
    Check(!game.read_file("content/0010/dlc.bfsar", read), what + ": downloadable content is left out");

    return game;
}

// Checks that loading `path` fails with a message that has `words` in it.
void CheckRefused(const std::string& what, const fs::path& path, const std::string& words)
{
    csf::title::GameFiles game;
    const std::optional<std::string> error = csf::title::LoadGame(path, game);
    Check(error && error->find(words) != std::string::npos,
          what + " is refused with \"" + words + "\": " + error.value_or("it loaded"));
}

// Checks that `path` loads as the test game under a system title's XML files: found by its executable alone, with every
// file of the folder.
void CheckGameUnderSystemXml(const std::string& what, const fs::path& path)
{
    csf::title::GameFiles game;
    const std::optional<std::string> error = csf::title::LoadGame(path, game);
    Check(!error, what + " loads" + (error ? ": " + *error : ""));
    if (error)
    {
        return;
    }

    Check(game.foreign_title_id == kSystemId,
          what + ": the XML files' title " + csf::title::TitleIdText(game.foreign_title_id));
    Check(game.title_id == 0 && game.title_version == 0, what + ": no title ID or version");
    Check(game.product_code.empty() && game.name.empty(), what + ": no product code or name");
    Check(game.sdk_version == 0 && !game.updated, what + ": no SDK version or update");
    Check(game.executable == "test.rpx", what + ": executable " + game.executable);
    CheckFiles(what, game, GameUnderSystemXml());
    Check(game.SoundArchives().size() == 2, what + ": the sound archives");
}

void TestTitles(const fs::path& dir)
{
    // An extracted title, the same through its executable, and a folder holding the game, its update and its
    // downloadable content.
    WriteFiles(dir / "game", Game());
    const std::string extracted = CheckGame("an extracted title", dir / "game", false).source;
    Check(extracted == "extracted title", "an extracted title's source: " + extracted);
    CheckGame("an extracted title's executable", dir / "game" / "code" / "test.rpx", false);

    const Files all = InFolders({{"0005000012345600_v0", Game()},
                                 {"0005000e12345600_v32", Update()},
                                 {"0005000c12345600_v16", DownloadableContent()}});
    WriteFiles(dir / "titles", all);
    CheckGame("a folder of titles", dir / "titles", true);

    // The same copied by macOS to a FAT or exFAT drive. The AppleDouble files are left out.
    WriteFiles(dir / "copied game", WithAppleDouble(Game()));
    CheckGame("an extracted title with AppleDouble files", dir / "copied game", false);
    WriteFiles(dir / "copied titles", WithAppleDouble(all));
    CheckGame("a folder of titles with AppleDouble files", dir / "copied titles", true);

    // The same as Wii U archives.
    WriteFile(dir / "game.wua", ZArchiveOf(InFolders({{"0005000012345600_v0", Game()}})));
    const std::string wua = CheckGame("a .wua", dir / "game.wua", false).source;
    Check(wua == "WUA", "a .wua's source: " + wua);
    WriteFile(dir / "updated.wua", ZArchiveOf(all));
    CheckGame("a .wua with an update", dir / "updated.wua", true);

    // The archive's files as the reader lists them.
    const std::unique_ptr<csf::title::ZArchive> archive = csf::title::ZArchive::Open(dir / "updated.wua");
    Check(archive && archive->Files().size() == all.size(), "the .wua lists every file");

    // Titles without a game, or with two, can't be ripped.
    WriteFile(dir / "update.wua", ZArchiveOf(InFolders({{"0005000e12345600_v32", Update()}})));
    CheckRefused("a .wua with only an update", dir / "update.wua", "only other titles (0005000E-12345600)");

    Files two_games = InFolders({{"0005000012345600_v0", Game()}});
    for (const auto& [path, data] : TitleXml(0x0005000012345700, 0, 20703, "test.rpx"))
    {
        two_games["0005000012345700_v0/" + path] = data;
    }
    WriteFile(dir / "two.wua", ZArchiveOf(two_games));
    CheckRefused("a .wua with two games", dir / "two.wua", "2 games");

    fs::create_directories(dir / "empty");
    CheckRefused("an empty folder", dir / "empty", "no Wii U title");
}

void TestSystemTitles(const fs::path& dir)
{
    // A system title beside the game is passed over.
    WriteFile(dir / "beside.wua",
              ZArchiveOf(InFolders({{"0005001012345600_v0", SystemXml()}, {"0005000012345600_v0", Game()}})));
    CheckGame("a .wua with a system title beside the game", dir / "beside.wua", false);

    // A game's folder that holds a system title's XML files in place of the game's, in a .wua and as an extracted
    // title. The game is found by its executable.
    WriteFile(dir / "under.wua", ZArchiveOf(InFolders({{"0000000000000000_v0", GameUnderSystemXml()}})));
    CheckGameUnderSystemXml("a .wua with a system title's XML files over the game's", dir / "under.wua");
    WriteFiles(dir / "under", GameUnderSystemXml());
    CheckGameUnderSystemXml("an extracted title with a system title's XML files over the game's", dir / "under");

    // A system title whose code/ holds the executable that its code/cos.xml names is no game. Nor is a folder whose
    // code/ holds two executables, neither of them the one its XML files name.
    Files system = SystemXml();
    system["code/system.rpx"] = Random(3000, 8);
    system["content/sound/system.bfsar"] = Bytes("FSAR, the system's");
    WriteFile(dir / "system.wua", ZArchiveOf(InFolders({{"0005001012345600_v0", system}})));
    CheckRefused("a .wua with only a system title", dir / "system.wua", "only other titles (00050010-12345600)");

    Files two = GameUnderSystemXml();
    two["code/other.rpx"] = Random(3000, 9);
    WriteFile(dir / "two executables.wua", ZArchiveOf(InFolders({{"0000000000000000_v0", two}})));
    CheckRefused("a .wua with two executables under a system title's XML files", dir / "two executables.wua",
                 "only other titles (00050010-12345600)");
}

void TestDamagedArchives(const fs::path& dir)
{
    const std::vector<uint8_t> wua = ZArchiveOf(InFolders({{"0005000012345600_v0", Game()}}));

    // Without its footer, a .wua is only recognised by its name.
    std::vector<uint8_t> truncated(wua.begin(), wua.end() - 10);
    WriteFile(dir / "truncated.wua", truncated);
    CheckRefused("a truncated .wua", dir / "truncated.wua", "incomplete or damaged .wua");

    // The footer's size of the whole file, a section past the end, a folder whose entries run off the tree, and blocks
    // that don't decompress.
    std::vector<uint8_t> damaged = wua;
    xsflib::StoreBe64(damaged.data() + damaged.size() - 16, damaged.size() + 1);
    WriteFile(dir / "size.wua", damaged);
    CheckRefused("a .wua whose size is wrong", dir / "size.wua", "truncated or damaged");

    damaged = wua;
    xsflib::StoreBe64(damaged.data() + damaged.size() - 144 + 3 * 16 + 8, damaged.size());
    WriteFile(dir / "section.wua", damaged);
    CheckRefused("a .wua with a section past its end", dir / "section.wua", "damaged");

    damaged = wua;
    const uint64_t tree = xsflib::LoadBe64(damaged.data() + damaged.size() - 144 + 3 * 16);
    xsflib::StoreBe32(damaged.data() + tree + 8, 0x10000);
    WriteFile(dir / "tree.wua", damaged);
    CheckRefused("a .wua whose tree runs off its end", dir / "tree.wua", "file tree is damaged");

    damaged = wua;
    const uint64_t data_size = xsflib::LoadBe64(damaged.data() + damaged.size() - 144 + 8);
    std::fill(damaged.begin(), damaged.begin() + static_cast<std::ptrdiff_t>(data_size), uint8_t{0xFF});
    WriteFile(dir / "blocks.wua", damaged);
    CheckRefused("a .wua whose blocks are damaged", dir / "blocks.wua", "damaged");
}

// Checks the disc image `name` in `dir`, a copy of `disc` in the format `format`: it opens, reads as the disc with
// zeros past its end and gives the disc's ID, and LoadGame refuses it.
void CheckDiscImage(const fs::path& dir, const std::string& name, const std::string& format,
                    const std::vector<uint8_t>& disc)
{
    try
    {
        const std::unique_ptr<csf::title::DiscImage> image = csf::title::OpenDiscImage(dir / name);
        Check(image != nullptr, name + " opens as a disc image");
        if (!image)
        {
            return;
        }

        std::vector<uint8_t> read(disc.size() + 100, 0xEE);
        image->Read(0, read.data(), read.size());

        Check(image->Format() == format, name + "'s format: " + image->Format());
        Check(image->Size() == disc.size(), name + "'s size: " + std::to_string(image->Size()));
        Check(std::equal(disc.begin(), disc.end(), read.begin()), name + " reads as the disc");
        Check(std::all_of(read.begin() + static_cast<std::ptrdiff_t>(disc.size()), read.end(),
                          [](uint8_t byte) { return byte == 0; }),
              name + " reads zeros past the disc's end");
        Check(csf::title::DiscId(*image).value_or("") == kDiscId, name + "'s disc ID");
    }
    catch (const std::exception& e)
    {
        Check(false, name + ": " + e.what());
    }

    CheckRefused(name, dir / name, "disc image (" + format + ") of WUP-P-ATSE. Its files are encrypted");
}

void TestDiscs(const fs::path& dir)
{
    const std::vector<uint8_t> disc = Disc();

    // A WUD whole and in parts, and a WUX: each reads as the disc, and zeros past its end.
    WriteFile(dir / "game.wud", disc);
    WriteFile(dir / "split" / "game_part1.wud", std::vector<uint8_t>(disc.begin(), disc.begin() + 0x10000));
    WriteFile(dir / "split" / "game_part2.wud", std::vector<uint8_t>(disc.begin() + 0x10000, disc.begin() + 0x30000));
    WriteFile(dir / "split" / "game_part3.wud", std::vector<uint8_t>(disc.begin() + 0x30000, disc.end()));
    const std::vector<uint8_t> wux = Wux(disc, 0x8000);
    WriteFile(dir / "game.wux", wux);
    Check(wux.size() < disc.size(), "the WUX stores repeated sectors once");

    CheckDiscImage(dir, "game.wud", "WUD", disc);
    CheckDiscImage(dir, "split/game_part1.wud", "WUD", disc);
    CheckDiscImage(dir, "game.wux", "WUX", disc);

    // A second part alone isn't a disc image.
    Check(csf::title::OpenDiscImage(dir / "split" / "game_part2.wud") == nullptr, "a second part alone isn't a disc");
}

// Whether reading the first `size` bytes of the disc image at `path` throws csf::title::Error.
bool ReadThrows(const fs::path& path, size_t size)
{
    try
    {
        const std::unique_ptr<csf::title::DiscImage> image = csf::title::OpenDiscImage(path);
        std::vector<uint8_t> read(size);
        image->Read(0, read.data(), read.size());
    }
    catch (const csf::title::Error&)
    {
        return true;
    }

    return false;
}

void TestDamagedWux(const fs::path& dir)
{
    const std::vector<uint8_t> disc = Disc();
    const std::vector<uint8_t> wux = Wux(disc, 0x8000);

    // A table that runs past the end, a sector past the end, and a sector size that isn't a power of two.
    WriteFile(dir / "table.wux", std::vector<uint8_t>(wux.begin(), wux.begin() + 0x24));
    Check(ReadThrows(dir / "table.wux", disc.size()), "a WUX cut in its table");

    std::vector<uint8_t> damaged = wux;
    xsflib::StoreLe32(damaged.data() + 0x20 + 4 * 4, 1000);
    WriteFile(dir / "sector.wux", damaged);
    Check(ReadThrows(dir / "sector.wux", disc.size()), "a WUX naming a sector past its end");

    damaged = wux;
    xsflib::StoreLe32(damaged.data() + 8, 0x7FFF);
    WriteFile(dir / "size.wux", damaged);
    Check(ReadThrows(dir / "size.wux", disc.size()), "a WUX with a sector size that isn't a power of two");
}

void TestOtherFiles(const fs::path& dir)
{
    // An installable title, as a folder and by its files.
    WriteFile(dir / "nus" / "title.tmd", Random(0xB04, 5));
    WriteFile(dir / "nus" / "title.tik", Random(0x350, 6));
    WriteFile(dir / "nus" / "00000000.app", Random(0x8000, 7));
    CheckRefused("an installable title", dir / "nus", "installable Wii U title (title.tmd and .app files)");
    CheckRefused("an installable title's TMD", dir / "nus" / "title.tmd", "installable Wii U title");
    CheckRefused("an installable title's content", dir / "nus" / "00000000.app", "installable Wii U title");

    // Other consoles' files, a sound archive and a file that's none of these.
    std::vector<uint8_t> data(0x400);
    std::memcpy(data.data(), "PFS0", 4);
    WriteFile(dir / "game.nsp", data);
    CheckRefused("a Switch package", dir / "game.nsp", "Nintendo Switch");

    data.assign(0x400, 0);
    std::memcpy(data.data() + 0x100, "HEAD", 4);
    WriteFile(dir / "game.xci", data);
    CheckRefused("a Switch cartridge image", dir / "game.xci", "Nintendo Switch");

    data.assign(0x400, 0);
    xsflib::StoreBe32(data.data() + 0x18, 0x5D1C9EA3);
    WriteFile(dir / "game.iso", data);
    CheckRefused("a Wii disc image", dir / "game.iso", "Wii or GameCube");

    // A GameCube or Wii disc image in a container: GCZ, CISO or WDF.
    const std::vector<std::pair<std::string, std::vector<uint8_t>>> kContainers = {
        {"game.gcz", {0x01, 0xC0, 0x0B, 0xB1}},
        {"game.ciso", Bytes("CISO")},
        {"game.wdf", Bytes("WII\x01"
                           "DISC")}};
    for (const auto& [name, magic] : kContainers)
    {
        data.assign(0x400, 0);
        std::copy(magic.begin(), magic.end(), data.begin());
        WriteFile(dir / name, data);
        CheckRefused("a disc image in " + name, dir / name, "Wii or GameCube");
    }

    data.assign(0x400, 0);
    std::memcpy(data.data() + 0x100, "NCSD", 4);
    WriteFile(dir / "game.3ds", data);
    CheckRefused("a 3DS image", dir / "game.3ds", "3DS game");

    WriteFile(dir / "sound.bfsar", Bytes("FSAR"));
    CheckRefused("a sound archive", dir / "sound.bfsar", "sound archive (.bfsar), not a game");

    WriteFile(dir / "notes.txt", Bytes("notes"));
    CheckRefused("a text file", dir / "notes.txt", "not a Wii U game");

    CheckRefused("a missing file", dir / "missing.wua", "no such file");
}

} // namespace

int main(int argc, char** argv)
{
    const bool keep = argc > 1;
    const fs::path dir = keep ? fs::path(argv[1]) : fs::temp_directory_path() / "csf_title_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    TestTitles(dir);
    TestSystemTitles(dir);
    TestDamagedArchives(dir);
    TestDiscs(dir);
    TestDamagedWux(dir);
    TestOtherFiles(dir);

    if (!keep)
    {
        fs::remove_all(dir);
    }

    return xsflib::testing::Summary("title");
}
