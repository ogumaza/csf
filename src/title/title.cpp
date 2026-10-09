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
#include <system_error>
#include <utility>
#include <vector>

#include "common/ascii.h"
#include "common/bytes.h"
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

// The messages that refuse an input. Each follows the input's name.
constexpr char kWhatCsfReads[] =
    "csfrip reads Wii U archives (.wua) and extracted titles, the folders holding code/, content/ and meta/.";
constexpr char kNoTitle[] =
    "no Wii U title in it. csfrip reads Wii U archives (.wua) and extracted titles, the folders holding code/, "
    "content/ and meta/.";

// The message that refuses `what`, an encrypted form of a game, and says how to convert it.
std::string Encrypted(const std::string& what)
{
    return what +
           ". Its files are encrypted. Cemu converts it to a .wua file, and csfrip reads that. In Cemu's Title Manager "
           "(Tools menu), right-click the game and choose Convert to compressed Wii U archive (.wua).";
}

// A path as UTF-8 with '/' between folders, and back.
std::string PathText(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();

    return std::string(text.begin(), text.end());
}

fs::path TextPath(const std::string& text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

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

    // The files under the folders `titles` of `root`. In `titles`, "" stands for the root itself.
    Store(const fs::path& root, const std::vector<std::string>& titles)
    {
        for (const std::string& title : titles)
        {
            const fs::path folder = title.empty() ? root : root / TextPath(title);
            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(folder))
            {
                if (entry.is_regular_file())
                {
                    Add(PathText(entry.path().lexically_relative(root)), entry.file_size(), entry.path(), 0);
                }
            }
        }
    }

    const std::vector<TitleFile>& Files() const
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
            Source source({entry.file});
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
    std::vector<TitleFile> files_;
    std::vector<Entry> entries_;
    std::map<std::string, size_t> index_;
};

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

        // Undo the escapes: the five named ones and numbered characters.
        const std::string raw = xml.substr(text + 1, close - text - 1);
        std::string value;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            const size_t semicolon = raw[i] == '&' ? raw.find(';', i) : std::string::npos;
            if (semicolon == std::string::npos)
            {
                value += raw[i];
                continue;
            }

            const std::string name = raw.substr(i + 1, semicolon - i - 1);
            uint32_t code = 0;
            if (name == "amp" || name == "lt" || name == "gt" || name == "quot" || name == "apos")
            {
                code = name == "amp" ? '&' : name == "lt" ? '<' : name == "gt" ? '>' : name == "quot" ? '"' : '\'';
            }
            else if (name.size() > 1 && name[0] == '#')
            {
                const bool hex = name[1] == 'x' || name[1] == 'X';
                const char* digits = name.data() + (hex ? 2 : 1);
                std::from_chars(digits, name.data() + name.size(), code, hex ? 16 : 10);
            }
            if (!code || code > 0x10FFFF)
            {
                value += raw[i];
                continue;
            }

            // The character as UTF-8.
            if (code < 0x80)
            {
                value += static_cast<char>(code);
            }
            else if (code < 0x800)
            {
                value += static_cast<char>(0xC0 | (code >> 6));
                value += static_cast<char>(0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
                value += static_cast<char>(0xE0 | (code >> 12));
                value += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                value += static_cast<char>(0x80 | (code & 0x3F));
            }
            else
            {
                value += static_cast<char>(0xF0 | (code >> 18));
                value += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                value += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                value += static_cast<char>(0x80 | (code & 0x3F));
            }
            i = semicolon;
        }

        return value;
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
    for (const TitleFile& file : store.Files())
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

std::string TitleIdText(uint64_t title_id)
{
    char text[18];
    std::snprintf(text, sizeof(text), "%08X-%08X", static_cast<uint32_t>(title_id >> 32),
                  static_cast<uint32_t>(title_id));

    return text;
}

// Fills `out` from the game's title folder in `store`. The update's folder, if there is one, is laid over it.
std::optional<std::string> LoadFromStore(std::shared_ptr<Store> store, const std::string& source, GameFiles& out)
{
    const std::vector<TitleFolder> titles = FindTitles(*store);
    if (titles.empty())
    {
        return std::string(kNoTitle);
    }

    // The game, and its update if it's there.
    std::vector<const TitleFolder*> games;
    for (const TitleFolder& title : titles)
    {
        const uint32_t high = static_cast<uint32_t>(title.title_id >> 32);
        if (high == kGame || high == kDemo)
        {
            games.push_back(&title);
        }
    }
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

    const TitleFolder& game = *games[0];
    const TitleFolder* update = nullptr;
    for (const TitleFolder& title : titles)
    {
        if (title.title_id == ((static_cast<uint64_t>(kUpdate) << 32) | (game.title_id & 0xFFFFFFFF)))
        {
            update = &title;
        }
    }

    // Each of the game's paths, and where it is in the input: the update's file where it has one.
    auto paths = std::make_shared<std::map<std::string, std::string>>();
    std::map<std::string, uint64_t> sizes;
    for (const TitleFolder* title : {&game, update})
    {
        if (!title)
        {
            continue;
        }

        for (const TitleFile& file : store->Files())
        {
            if (file.path.starts_with(title->folder))
            {
                const std::string path = file.path.substr(title->folder.size());
                (*paths)[path] = file.path;
                sizes[path] = file.size;
            }
        }
    }

    out = GameFiles{};
    out.source = source;
    out.updated = update != nullptr;
    for (const auto& [path, size] : sizes)
    {
        out.files.push_back({path, size});
    }
    out.read_file = [store, paths](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto found = paths->find(path);

        return found != paths->end() && store->Read(found->second, data);
    };

    // What the title's XML files say about it.
    std::vector<uint8_t> data;
    const std::string meta = out.read_file("meta/meta.xml", data) ? std::string(data.begin(), data.end()) : "";
    const std::string app = out.read_file("code/app.xml", data) ? std::string(data.begin(), data.end()) : "";
    const std::string cos = out.read_file("code/cos.xml", data) ? std::string(data.begin(), data.end()) : "";
    out.title_id = game.title_id;
    out.product_code = XmlValue(meta, "product_code").value_or("");
    out.title_version = XmlNumber<uint32_t>(meta, "title_version", 10)
                            .value_or(XmlNumber<uint32_t>(app, "title_version", 16).value_or(0));
    out.sdk_version = XmlNumber<uint32_t>(app, "sdk_version", 10).value_or(0);
    out.executable = XmlValue(cos, "argstr").value_or("");

    // A long name may have its lines broken where the system menu shows them.
    for (const char c : XmlValue(meta, "longname_en").value_or(""))
    {
        const bool space = c == '\n' || c == '\r' || c == ' ';
        if (!space)
        {
            out.name += c;
        }
        else if (!out.name.empty() && out.name.back() != ' ')
        {
            out.name += ' ';
        }
    }
    if (!out.name.empty() && out.name.back() == ' ')
    {
        out.name.pop_back();
    }

    return std::nullopt;
}

// The message that refuses a file that isn't a game CSF reads. The message says what the file is.
std::string ForeignFile(const fs::path& path)
{
    const std::string name = AsciiLower(path.filename().string());
    const std::string extension = AsciiLower(path.extension().string());
    if (name == "title.tmd" || name == "title.tik" || name == "title.cert" || extension == ".app" || extension == ".h3")
    {
        return Encrypted("part of an installable Wii U title (title.tmd and .app files)");
    }

    if (std::unique_ptr<DiscImage> disc = OpenDiscImage(path))
    {
        const std::optional<std::string> id = DiscId(*disc);

        return Encrypted("a Wii U disc image (" + disc->Format() + ")" + (id ? " of " + id->substr(0, 10) : ""));
    }

    Source source({path});
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
    if ((head.size() >= 0x20 &&
         (LoadBe32(head.data() + 0x18) == 0x5D1C9EA3 || LoadBe32(head.data() + 0x1C) == 0xC2339F3D)) ||
        magic_at(0, "RVZ\x01") || magic_at(0, "WIA\x01") || magic_at(0, "WBFS"))
    {
        return "a Wii or GameCube disc image, not a Wii U game";
    }
    if (extension == ".wua")
    {
        return "an incomplete or damaged .wua file";
    }

    return std::string("not a Wii U game. ") + kWhatCsfReads;
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

        return std::string(kNoTitle);
    }

    return LoadFromStore(std::make_shared<Store>(path, titles), "extracted title", out);
}

} // namespace

std::vector<std::string> GameFiles::SoundArchives() const
{
    std::vector<std::string> archives;
    for (const TitleFile& file : files)
    {
        if (file.path.starts_with("content/") && AsciiLower(file.path).ends_with(".bfsar"))
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
        if (AsciiLower(path.extension().string()) == ".rpx" &&
            AsciiLower(path.parent_path().filename().string()) == "code")
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

} // namespace csf::title
