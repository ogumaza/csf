// SPDX-License-Identifier: MIT

#include "title/title.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "xsflib/ascii.h"
#include "xsflib/files.h"

#include "title/disc.h"
#include "title/source.h"
#include "title/wua.h"

namespace csf::title
{
namespace
{

namespace fs = std::filesystem;

// The high words of the title IDs of games, demos and updates.
constexpr uint32_t kGame = 0x00050000;
constexpr uint32_t kDemo = 0x00050002;
constexpr uint32_t kUpdate = 0x0005000E;

// The message that refuses an input for `reason`, and says what csfrip reads. It follows the input's name.
std::string Unreadable(const std::string& reason)
{
    return reason +
           ". csfrip reads Wii U archives (.wua) and extracted titles, the folders holding code/, content/ and meta/.";
}

// The reason that Unreadable gives for a folder or an archive that holds no title.
constexpr char kNoTitle[] = "no Wii U title in it";

// The message that refuses `what`, an encrypted form of a game, and says how to convert it.
std::string Encrypted(const std::string& what)
{
    return what +
           ". Its files are encrypted. Cemu converts it to a .wua file, and csfrip reads that. In Cemu's Title Manager "
           "(Tools menu), right-click the game and choose Convert to compressed Wii U archive (.wua).";
}

// A path as UTF-8 with '/' between folders.
std::string PathText(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// A file that an input holds. The path runs from the input's root, without a leading '/'. It starts with the title's
// folder unless the root is the title ("0005000012345600_v0/content/sound/test.bfsar").
struct StoreFile
{
    std::string path;
    uint64_t size;
};

// The files an input holds, by their paths from its root: the files of a .wua, or those of the title folders in a
// folder.
class Store
{
public:
    explicit Store(std::unique_ptr<ZArchive> archive) : archive_(std::move(archive))
    {
        const std::vector<ZArchive::File>& files = archive_->Files();
        for (size_t i = 0; i < files.size(); ++i)
        {
            Add(files[i].path, files[i].size, {}, i);
        }
    }

    // The files under the folders `titles` of `root`. In `titles`, "" stands for the root itself. AppleDouble files are
    // left out. macOS writes one, named "._" and the file's name, beside each file that it copies to a FAT or exFAT
    // drive.
    Store(const fs::path& root, const std::vector<std::string>& titles)
    {
        for (const std::string& title : titles)
        {
            const fs::path folder = title.empty() ? root : root / xsflib::PathFromUtf8(title);
            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(folder))
            {
                if (entry.is_regular_file() && !entry.path().filename().u8string().starts_with(u8"._"))
                {
                    Add(PathText(entry.path().lexically_relative(root)), entry.file_size(), entry.path(), 0);
                }
            }
        }
    }

    const std::vector<StoreFile>& Files() const
    {
        return files_;
    }

    bool Read(const std::string& path, std::vector<uint8_t>& data)
    {
        const auto found = index_.find(path);
        if (found == index_.end())
        {
            return false;
        }

        const Entry& entry = entries_[found->second];
        data.resize(static_cast<size_t>(files_[found->second].size));
        if (archive_)
        {
            archive_->Read(archive_->Files()[entry.archive_index], 0, data.data(), data.size());
        }
        else
        {
            Source source({entry.file}, kFile);
            if (source.Size() != data.size())
            {
                throw Error(path + " changed while it was being read");
            }

            source.Read(0, data.data(), data.size());
        }

        return true;
    }

private:
    struct Entry
    {
        fs::path file;           // a folder's file
        size_t archive_index{0}; // an archive's file
    };

    void Add(const std::string& path, uint64_t size, const fs::path& file, size_t archive_index)
    {
        index_[path] = files_.size();
        files_.push_back({path, size});
        entries_.push_back({file, archive_index});
    }

    std::unique_ptr<ZArchive> archive_;
    std::vector<StoreFile> files_;
    std::vector<Entry> entries_;
    std::map<std::string, size_t> index_;
};

// Appends the Unicode character `code` to `text` as UTF-8.
void AppendUtf8(uint32_t code, std::string& text)
{
    if (code < 0x80)
    {
        text += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        text += static_cast<char>(0xC0 | (code >> 6));
        text += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        text += static_cast<char>(0xE0 | (code >> 12));
        text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
        text += static_cast<char>(0xF0 | (code >> 18));
        text += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// The character that the XML escape `&name;` stands for: one of the five named ones or a numbered character. Nothing
// for any other name, or for a number that is 0 or above 0x10FFFF.
std::optional<uint32_t> EscapedCharacter(const std::string& name)
{
    static constexpr std::pair<std::string_view, uint32_t> kNamed[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}};
    for (const auto& [named, code] : kNamed)
    {
        if (name == named)
        {
            return code;
        }
    }

    uint32_t code = 0;
    if (name.size() > 1 && name[0] == '#')
    {
        const bool hex = name[1] == 'x' || name[1] == 'X';
        const char* digits = name.data() + (hex ? 2 : 1);
        std::from_chars(digits, name.data() + name.size(), code, hex ? 16 : 10);
    }
    if (!code || code > 0x10FFFF)
    {
        return std::nullopt;
    }

    return code;
}

// `raw` with XML's escapes undone. An '&' stays as it is where EscapedCharacter reads no escape after it.
std::string UndoXmlEscapes(const std::string& raw)
{
    std::string value;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const size_t semicolon = raw[i] == '&' ? raw.find(';', i) : std::string::npos;
        const std::optional<uint32_t> code =
            semicolon == std::string::npos ? std::nullopt : EscapedCharacter(raw.substr(i + 1, semicolon - i - 1));
        if (!code)
        {
            value += raw[i];
            continue;
        }

        AppendUtf8(*code, value);
        i = semicolon;
    }

    return value;
}

// The text of the first <tag ...>text</tag> in `xml`, with XML's escapes undone, or nothing.
std::optional<std::string> XmlValue(const std::string& xml, const std::string& tag)
{
    const std::string open = "<" + tag;
    for (size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at + 1))
    {
        const size_t after = at + open.size();
        if (after >= xml.size() || std::strchr(" \t\r\n>", xml[after]) == nullptr)
        {
            continue;
        }

        const size_t text = xml.find('>', after);
        const size_t close = text == std::string::npos ? text : xml.find("</" + tag + ">", text);
        if (close == std::string::npos)
        {
            return std::nullopt;
        }

        return UndoXmlEscapes(xml.substr(text + 1, close - text - 1));
    }

    return std::nullopt;
}

// A number in an XML value, in hex or decimal, or nothing if it isn't one.
template <typename T>
std::optional<T> XmlNumber(const std::string& xml, const std::string& tag, int base)
{
    const std::optional<std::string> text = XmlValue(xml, tag);
    if (!text)
    {
        return std::nullopt;
    }

    const size_t first = text->find_first_not_of(" \t\r\n");
    const size_t last = text->find_last_not_of(" \t\r\n");
    T value = 0;
    if (first == std::string::npos ||
        std::from_chars(text->data() + first, text->data() + last + 1, value, base).ptr != text->data() + last + 1)
    {
        return std::nullopt;
    }

    return value;
}

// A title in an input's files: the folder holding its code/ and meta/, from the input's root ("" when the root is the
// title, otherwise one folder with a trailing '/'), and its title ID.
struct TitleFolder
{
    std::string folder;
    uint64_t title_id = 0;
};

// The titles among `store`'s files, found by their app.xml or meta.xml. Each title's ID comes from its meta.xml, or
// from app.xml where there's no meta.xml.
std::vector<TitleFolder> FindTitles(Store& store)
{
    std::vector<TitleFolder> titles;
    for (const StoreFile& file : store.Files())
    {
        for (const std::string marker : {"code/app.xml", "meta/meta.xml"})
        {
            if (!file.path.ends_with(marker))
            {
                continue;
            }

            const std::string folder = file.path.substr(0, file.path.size() - marker.size());
            const auto same = [&](const TitleFolder& title)
            {
                return title.folder == folder;
            };
            if ((folder.empty() || folder.find('/') == folder.size() - 1) &&
                std::find_if(titles.begin(), titles.end(), same) == titles.end())
            {
                titles.push_back({folder, 0});
            }
        }
    }

    // When the root is a title, a folder in it that looks like one is part of its content.
    const auto root = [](const TitleFolder& title)
    {
        return title.folder.empty();
    };
    if (std::find_if(titles.begin(), titles.end(), root) != titles.end())
    {
        titles = {TitleFolder{}};
    }

    for (TitleFolder& title : titles)
    {
        std::vector<uint8_t> data;
        for (const char* xml : {"meta/meta.xml", "code/app.xml"})
        {
            if (store.Read(title.folder + xml, data))
            {
                const std::string text(data.begin(), data.end());
                title.title_id = XmlNumber<uint64_t>(text, "title_id", 16).value_or(0);
                if (title.title_id)
                {
                    break;
                }
            }
        }
    }

    return titles;
}

// The executable of a game in `title`'s folder when the folder holds another title's XML files in place of the game's:
// the one RPX in the folder's code/, if the folder's code/cos.xml doesn't name it. Extracting a disc's system title
// into the game's folder leaves the folder that way. Nothing for any other folder.
std::optional<std::string> ExecutableUnderForeignXml(Store& store, const TitleFolder& title)
{
    std::vector<uint8_t> data;
    const std::string cos =
        store.Read(title.folder + "code/cos.xml", data) ? std::string(data.begin(), data.end()) : "";
    const std::string named = XmlValue(cos, "argstr").value_or("");

    const std::string code = title.folder + "code/";
    std::vector<std::string> executables;
    for (const StoreFile& file : store.Files())
    {
        if (file.path.starts_with(code) && file.path.find('/', code.size()) == std::string::npos &&
            xsflib::AsciiLower(file.path).ends_with(".rpx"))
        {
            executables.push_back(file.path.substr(code.size()));
        }
    }
    if (executables.size() != 1 || executables[0] == named)
    {
        return std::nullopt;
    }

    return executables[0];
}

// A title's long name on one line. A long name may have its lines broken where the system menu shows them. Each run of
// line breaks and spaces becomes one space, and the name starts and ends with neither.
std::string OneLineName(const std::string& long_name)
{
    std::string name;
    for (const char c : long_name)
    {
        const bool space = c == '\n' || c == '\r' || c == ' ';
        if (!space)
        {
            name += c;
        }
        else if (!name.empty() && name.back() != ' ')
        {
            name += ' ';
        }
    }
    if (!name.empty() && name.back() == ' ')
    {
        name.pop_back();
    }

    return name;
}

// Sets what the game's XML files say about it in `out`, which reads them: its product code, version and SDK version,
// its executable and its name.
void ReadTitleXml(GameFiles& out)
{
    std::vector<uint8_t> data;
    const std::string meta = out.read_file("meta/meta.xml", data) ? std::string(data.begin(), data.end()) : "";
    const std::string app = out.read_file("code/app.xml", data) ? std::string(data.begin(), data.end()) : "";
    const std::string cos = out.read_file("code/cos.xml", data) ? std::string(data.begin(), data.end()) : "";
    out.product_code = XmlValue(meta, "product_code").value_or("");
    out.title_version = XmlNumber<uint32_t>(meta, "title_version", 10)
                            .value_or(XmlNumber<uint32_t>(app, "title_version", 16).value_or(0));
    out.sdk_version = XmlNumber<uint32_t>(app, "sdk_version", 10).value_or(0);
    out.executable = XmlValue(cos, "argstr").value_or("");
    out.name = OneLineName(XmlValue(meta, "longname_en").value_or(""));
}

// The games among `titles`: the titles of games and demos. Without one, a title's folder may hold a game's files under
// the title's XML files. The game is then found by its executable alone. `foreign_executable` is set to the name of
// that executable.
std::vector<const TitleFolder*> FindGames(Store& store, const std::vector<TitleFolder>& titles,
                                          std::string& foreign_executable)
{
    std::vector<const TitleFolder*> games;
    for (const TitleFolder& title : titles)
    {
        const uint32_t high = static_cast<uint32_t>(title.title_id >> 32);
        if (high == kGame || high == kDemo)
        {
            games.push_back(&title);
        }
    }
    if (!games.empty())
    {
        return games;
    }

    for (const TitleFolder& title : titles)
    {
        if (const std::optional<std::string> executable = ExecutableUnderForeignXml(store, title))
        {
            games.push_back(&title);
            foreign_executable = *executable;
        }
    }

    return games;
}

// The update of `game` among `titles`, or null.
const TitleFolder* FindUpdate(const std::vector<TitleFolder>& titles, const TitleFolder& game)
{
    const uint64_t update_id = (static_cast<uint64_t>(kUpdate) << 32) | (game.title_id & 0xFFFFFFFF);
    const TitleFolder* update = nullptr;
    for (const TitleFolder& title : titles)
    {
        if (title.title_id == update_id)
        {
            update = &title;
        }
    }

    return update;
}

// The files of `game` in `store`, by their paths in the game, with the files of `update` (if not null) laid over them.
std::map<std::string, StoreFile> FilesOfGame(const Store& store, const TitleFolder& game, const TitleFolder* update)
{
    std::map<std::string, StoreFile> files;
    for (const TitleFolder* title : {&game, update})
    {
        if (!title)
        {
            continue;
        }

        for (const StoreFile& file : store.Files())
        {
            if (file.path.starts_with(title->folder))
            {
                files[file.path.substr(title->folder.size())] = file;
            }
        }
    }

    return files;
}

// Fills `out` from the game's title folder in `store`. The update's folder, if there is one, is laid over it.
std::optional<std::string> LoadFromStore(std::shared_ptr<Store> store, const std::string& source, GameFiles& out)
{
    const std::vector<TitleFolder> titles = FindTitles(*store);
    if (titles.empty())
    {
        return Unreadable(kNoTitle);
    }

    std::string foreign_executable;
    const std::vector<const TitleFolder*> games = FindGames(*store, titles, foreign_executable);
    if (games.empty())
    {
        std::string ids;
        for (const TitleFolder& title : titles)
        {
            ids += (ids.empty() ? "" : ", ") + TitleIdText(title.title_id);
        }

        return "no game in it, only other titles (" + ids + ")";
    }
    if (games.size() > 1)
    {
        return std::to_string(games.size()) + " games in it, and csfrip takes one at a time";
    }

    // Without the game's title ID, no update can be matched to a game found by its executable alone.
    const TitleFolder& game = *games[0];
    const bool foreign = !foreign_executable.empty();
    const TitleFolder* update = foreign ? nullptr : FindUpdate(titles, game);
    const auto files = std::make_shared<const std::map<std::string, StoreFile>>(FilesOfGame(*store, game, update));

    out = GameFiles{};
    out.source = source;
    out.updated = update != nullptr;
    for (const auto& [path, file] : *files)
    {
        out.files.push_back({path, file.size});
    }

    out.read_file = [store, files](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto found = files->find(path);
        return found != files->end() && store->Read(found->second.path, data);
    };

    // The XML files of a game found by its executable alone say nothing about the game.
    if (foreign)
    {
        out.foreign_title_id = game.title_id;
        out.executable = foreign_executable;
        return std::nullopt;
    }

    out.title_id = game.title_id;
    ReadTitleXml(out);

    return std::nullopt;
}

// The message that refuses a file that isn't a game CSF reads. The message says what the file is.
std::string ForeignFile(const fs::path& path)
{
    const std::string name = xsflib::AsciiLower(path.filename().string());
    const std::string extension = xsflib::AsciiLower(path.extension().string());
    if (name == "title.tmd" || name == "title.tik" || name == "title.cert" || extension == ".app" || extension == ".h3")
    {
        return Encrypted("part of an installable Wii U title (title.tmd and .app files)");
    }

    if (std::unique_ptr<DiscImage> disc = OpenDiscImage(path))
    {
        const std::optional<std::string> id = DiscId(*disc);
        return Encrypted("a Wii U disc image (" + disc->Format() + ")" + (id ? " of " + id->substr(0, 10) : ""));
    }

    Source source({path}, kFile);
    std::vector<uint8_t> head(0x200);
    head.resize(static_cast<size_t>(std::min<uint64_t>(head.size(), source.Size())));
    source.Read(0, head.data(), head.size());

    const auto magic_at = [&](size_t offset, const char* magic)
    {
        const size_t size = std::strlen(magic);
        return head.size() >= offset + size && std::memcmp(head.data() + offset, magic, size) == 0;
    };

    if (magic_at(0, "FSAR"))
    {
        return "a sound archive (.bfsar), not a game";
    }
    if (magic_at(0, "CSAR") || magic_at(0, "RSAR"))
    {
        return std::string("a ") + (magic_at(0, "CSAR") ? "3DS" : "Wii") + " sound archive, not a Wii U one";
    }
    if (magic_at(0, "PFS0") || magic_at(0x100, "HEAD"))
    {
        return "a Nintendo Switch game, not a Wii U one";
    }
    if (magic_at(0x100, "NCSD") || magic_at(0x100, "NCCH"))
    {
        return "a Nintendo 3DS game, not a Wii U one";
    }
    if (ntdlib::disc::ImageFormat(head))
    {
        return "a Wii or GameCube disc image, not a Wii U game";
    }
    if (extension == ".wua")
    {
        return "an incomplete or damaged .wua file";
    }

    return Unreadable("not a Wii U game");
}

// Loads a folder: a title folder itself, or one holding title folders. Installable titles are refused.
std::optional<std::string> LoadFolder(const fs::path& path, GameFiles& out)
{
    std::error_code error;
    std::vector<std::string> titles;
    if (fs::is_directory(path / "code", error))
    {
        titles.push_back("");
    }
    else
    {
        for (const fs::directory_entry& entry : fs::directory_iterator(path))
        {
            if (entry.is_directory(error) && fs::is_directory(entry.path() / "code", error))
            {
                titles.push_back(PathText(entry.path().filename()));
            }
        }
    }

    if (titles.empty())
    {
        if (fs::is_regular_file(path / "title.tmd", error))
        {
            return Encrypted("an installable Wii U title (title.tmd and .app files)");
        }

        return Unreadable(kNoTitle);
    }

    return LoadFromStore(std::make_shared<Store>(path, titles), "extracted title", out);
}

} // namespace

std::vector<std::string> GameFiles::SoundArchives() const
{
    std::vector<std::string> archives;
    for (const TitleFile& file : files)
    {
        if (file.path.starts_with("content/") && xsflib::AsciiLower(file.path).ends_with(".bfsar"))
        {
            archives.push_back(file.path);
        }
    }

    return archives;
}

std::string GameFiles::TitleIdText() const
{
    return title::TitleIdText(title_id);
}

std::string GameFiles::SdkVersionText() const
{
    if (!sdk_version)
    {
        return "unknown";
    }

    char text[16];
    std::snprintf(text, sizeof(text), "%u.%02u.%02u", sdk_version / 10000, sdk_version / 100 % 100, sdk_version % 100);

    return text;
}

std::string GameFiles::ExecutablePath() const
{
    return "code/" + executable;
}

std::optional<std::string> LoadGame(const fs::path& path, GameFiles& out)
{
    try
    {
        std::error_code error;
        if (fs::is_directory(path, error))
        {
            return LoadFolder(path, out);
        }
        if (!fs::is_regular_file(path, error))
        {
            return "no such file or folder";
        }

        // An .rpx in a title's code/ folder stands for the title. Cemu treats it the same way.
        if (xsflib::AsciiLower(path.extension().string()) == ".rpx" &&
            xsflib::AsciiLower(path.parent_path().filename().string()) == "code")
        {
            return LoadFolder(fs::absolute(path).parent_path().parent_path(), out);
        }

        if (std::unique_ptr<ZArchive> archive = ZArchive::Open(path))
        {
            return LoadFromStore(std::make_shared<Store>(std::move(archive)), "WUA", out);
        }

        return ForeignFile(path);
    }
    catch (const std::exception& e)
    {
        return std::string(e.what());
    }
}

std::string TitleIdText(uint64_t title_id)
{
    char text[18];
    std::snprintf(text, sizeof(text), "%08X-%08X", static_cast<uint32_t>(title_id >> 32),
                  static_cast<uint32_t>(title_id));

    return text;
}

} // namespace csf::title
