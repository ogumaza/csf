// SPDX-License-Identifier: MIT

// WUX is the format of Exzap's WudCompress. A WUX has a header, a table, and the distinct sectors themselves. The table
// gives each of the disc's sectors the index of the stored sector that holds its bytes. The stored sectors start at the
// first multiple of the sector size after the table. Every number in a WUX is little-endian.

#include "title/disc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "common/ascii.h"
#include "common/bytes.h"
#include "title/source.h"

namespace csf::title
{
namespace
{

namespace fs = std::filesystem;

// "WUX0" and the word after it, read as little-endian words.
constexpr uint32_t kWuxMagic = 0x30585557;
constexpr uint32_t kWuxMagic2 = 0x1099D02E;
constexpr size_t kWuxHeaderSize = 0x20;

// WudCompress writes 32 KiB sectors. The format allows any power of two. This limit bounds the sector size that a
// damaged header can give.
constexpr uint32_t kMaxWuxSectorSize = 1u << 24;

// Every Wii U disc's ID starts with this.
constexpr char kDiscIdPrefix[] = "WUP-";
constexpr size_t kDiscIdSize = 0x20;

// The name wudump gives the first part of a WUD it splits, after the dump's name.
constexpr char kFirstPartSuffix[] = "_part1.wud";

// The files of a WUD: its first part, then the parts numbered after it that exist ("game_part2.wud" and onwards), or
// the file alone if it isn't a first part.
std::vector<fs::path> WudParts(const fs::path& path)
{
    std::vector<fs::path> parts = {path};
    const std::string name = path.filename().string();
    const std::string first = kFirstPartSuffix;
    if (name.size() <= first.size() || AsciiLower(name.substr(name.size() - first.size())) != first)
    {
        return parts;
    }

    // The later parts keep the first part's spelling of everything but the number.
    const std::string stem = name.substr(0, name.size() - first.size() + first.find('1'));
    const std::string extension = name.substr(name.size() - 4);
    std::error_code error;
    for (int number = 2;; ++number)
    {
        const fs::path part = path.parent_path() / (stem + std::to_string(number) + extension);
        if (!fs::is_regular_file(part, error))
        {
            break;
        }

        parts.push_back(part);
    }

    return parts;
}

// A WUD: the disc byte for byte. A dump that ends early reads as zeros past its end.
class WudImage : public DiscImage
{
public:
    explicit WudImage(const fs::path& path) : source_(WudParts(path))
    {
    }

    std::string Format() const override
    {
        return "WUD";
    }

    uint64_t Size() const override
    {
        return source_.Size();
    }

    void Read(uint64_t offset, uint8_t* out, size_t size) override
    {
        const uint64_t end = source_.Size();
        const size_t stored = offset < end ? static_cast<size_t>(std::min<uint64_t>(size, end - offset)) : 0;
        if (stored)
        {
            source_.Read(offset, out, stored);
        }
        std::fill(out + stored, out + size, uint8_t{0});
    }

private:
    Source source_;
};

// A WUX: each of the disc's sectors reads as the stored sector that the table names for it.
class WuxImage : public DiscImage
{
public:
    explicit WuxImage(const fs::path& path) : source_({path})
    {
        uint8_t header[kWuxHeaderSize];
        source_.Read(0, header, sizeof(header));
        sector_size_ = LoadLe32(header + 0x08);
        size_ = LoadLe64(header + 0x10);
        if (sector_size_ < 0x100 || sector_size_ > kMaxWuxSectorSize || (sector_size_ & (sector_size_ - 1)))
        {
            throw Error("the WUX header is damaged");
        }

        // The table has an entry for every sector. The last sector may lie partly past the disc's end.
        const uint64_t sectors = (size_ + sector_size_ - 1) / sector_size_;
        if (sectors > (source_.Size() - kWuxHeaderSize) / 4)
        {
            throw Error("the WUX file is truncated");
        }

        const std::vector<uint8_t> table = source_.Read(kWuxHeaderSize, static_cast<size_t>(sectors * 4));
        table_.resize(static_cast<size_t>(sectors));
        for (size_t i = 0; i < table_.size(); ++i)
        {
            table_[i] = LoadLe32(table.data() + i * 4);
        }
        data_start_ = (kWuxHeaderSize + sectors * 4 + sector_size_ - 1) / sector_size_ * sector_size_;
    }

    std::string Format() const override
    {
        return "WUX";
    }

    uint64_t Size() const override
    {
        return size_;
    }

    void Read(uint64_t offset, uint8_t* out, size_t size) override
    {
        while (size && offset < size_)
        {
            const uint64_t sector = offset / sector_size_;
            const uint64_t within = offset % sector_size_;
            const size_t n = static_cast<size_t>(std::min<uint64_t>({size, sector_size_ - within, size_ - offset}));
            source_.Read(
                data_start_ + static_cast<uint64_t>(table_[static_cast<size_t>(sector)]) * sector_size_ + within, out,
                n);
            out += n;
            offset += n;
            size -= n;
        }
        std::fill(out, out + size, uint8_t{0});
    }

private:
    Source source_;
    uint32_t sector_size_ = 0;
    uint64_t size_ = 0;
    std::vector<uint32_t> table_;
    uint64_t data_start_ = 0;
};

} // namespace

std::unique_ptr<DiscImage> OpenDiscImage(const fs::path& path)
{
    Source probe({path});
    uint8_t head[8] = {};
    const size_t n = static_cast<size_t>(std::min<uint64_t>(sizeof(head), probe.Size()));
    probe.Read(0, head, n);

    if (n == sizeof(head) && LoadLe32(head) == kWuxMagic && LoadLe32(head + 4) == kWuxMagic2)
    {
        return std::make_unique<WuxImage>(path);
    }
    if (n >= 4 && std::memcmp(head, kDiscIdPrefix, 4) == 0)
    {
        return std::make_unique<WudImage>(path);
    }

    return nullptr;
}

std::optional<std::string> DiscId(DiscImage& disc)
{
    uint8_t header[kDiscIdSize];
    disc.Read(0, header, sizeof(header));
    if (std::memcmp(header, kDiscIdPrefix, 4) != 0)
    {
        return std::nullopt;
    }

    std::string id;
    for (const uint8_t c : header)
    {
        if (!c)
        {
            break;
        }
        if (c < 0x20 || c > 0x7E)
        {
            return std::nullopt;
        }

        id += static_cast<char>(c);
    }

    return id;
}

} // namespace csf::title
