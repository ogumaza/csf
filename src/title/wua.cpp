// SPDX-License-Identifier: MIT

// The layout, from ZArchive's zarchivecommon.h. Everything is big-endian. The footer is the file's last 144 bytes. It
// gives six sections as a 64-bit offset and size each: the compressed data, the offset records, the names, the file
// tree and two metadata sections that nothing uses yet. Then come the SHA-256 of the whole file with this field zeroed,
// the file's size, a version word and a magic word.
//
// The data is the archive's files one after another, cut into 64 KiB blocks. Each block is stored compressed with
// Zstandard, or as it is when compression doesn't make it smaller. A name is a length byte, then a second length byte
// when the first one's top bit is set, and then the name's bytes. (As of its commit 965b66c, ZArchive's reader reads
// the first byte twice there instead of the second. That reader can't find a name of 128 bytes or more. This reader
// follows the writer.) Each entry of the tree is 16 bytes. The first word's top bit marks a file, and its other bits
// give the name's offset. For a file, the rest is the low 32 bits of its offset and size, and a word with their high 16
// bits (size in the upper half). For a folder, the rest is the index of its first entry and the number of entries.
// Entry 0 is the root folder.

#include "title/wua.h"

#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/bytes.h"
#include "title/source.h"

namespace csf::title
{
namespace
{

constexpr size_t kFooterSize = 144;
constexpr uint32_t kMagic = 0x169F52D6;
constexpr uint32_t kVersion1 = 0x61BF3A01;
constexpr uint64_t kBlockSize = 64 * 1024;
constexpr uint64_t kBlocksPerRecord = 16;
constexpr size_t kRecordSize = 8 + 2 * kBlocksPerRecord;
constexpr size_t kEntrySize = 16;
constexpr uint32_t kFileFlag = 0x80000000;

// The tree's sections are read whole. This limit bounds the size that a damaged footer can request.
constexpr uint64_t kMaxTableSize = 1ull << 30;

struct Section
{
    uint64_t offset;
    uint64_t size;
};

std::string NameAt(const std::vector<uint8_t>& names, uint32_t offset)
{
    if (offset >= names.size())
    {
        throw Error("the archive's names are damaged");
    }

    size_t length = names[offset] & 0x7F;
    size_t start = offset + 1;
    if (names[offset] & 0x80)
    {
        if (start >= names.size())
        {
            throw Error("the archive's names are damaged");
        }

        length |= static_cast<size_t>(names[start]) << 7;
        ++start;
    }
    if (length > names.size() - start)
    {
        throw Error("the archive's names are damaged");
    }

    // A name that could lead out of the folder it's in when extracted is as damaged as one that runs off the table.
    std::string name(names.begin() + static_cast<std::ptrdiff_t>(start),
                     names.begin() + static_cast<std::ptrdiff_t>(start + length));
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos)
    {
        throw Error("the archive's names are damaged");
    }

    return name;
}

} // namespace

std::unique_ptr<ZArchive> ZArchive::Open(const std::filesystem::path& path)
{
    auto source = std::make_unique<Source>(std::vector<std::filesystem::path>{path});
    const uint64_t file_size = source->Size();
    if (file_size < kFooterSize)
    {
        return nullptr;
    }

    const std::vector<uint8_t> footer = source->Read(file_size - kFooterSize, kFooterSize);
    if (LoadBe32(footer.data() + 140) != kMagic)
    {
        return nullptr;
    }
    if (LoadBe32(footer.data() + 136) != kVersion1)
    {
        throw Error("the archive is in a version of ZArchive that CSF doesn't support");
    }
    if (LoadBe64(footer.data() + 128) != file_size)
    {
        throw Error("the archive is truncated or damaged");
    }

    Section sections[4];
    for (size_t i = 0; i < 4; ++i)
    {
        sections[i] = {LoadBe64(footer.data() + i * 16), LoadBe64(footer.data() + i * 16 + 8)};
        if (sections[i].offset > file_size || sections[i].size > file_size - sections[i].offset ||
            (i && sections[i].size > kMaxTableSize))
        {
            throw Error("the archive is damaged");
        }
    }
    const Section& data = sections[0];
    const Section& records = sections[1];
    const Section& names = sections[2];
    const Section& tree = sections[3];

    std::unique_ptr<ZArchive> archive(new ZArchive(std::move(source)));
    archive->data_offset_ = data.offset;
    archive->data_size_ = data.size;

    const std::vector<uint8_t> record_bytes = archive->source_->Read(records.offset, static_cast<size_t>(records.size));
    for (size_t at = 0; at + kRecordSize <= record_bytes.size(); at += kRecordSize)
    {
        Record record;
        record.base = LoadBe64(record_bytes.data() + at);
        for (size_t i = 0; i < kBlocksPerRecord; ++i)
        {
            record.sizes[i] = LoadBe16(record_bytes.data() + at + 8 + i * 2);
        }
        archive->records_.push_back(record);
    }

    archive->ReadTree(archive->source_->Read(names.offset, static_cast<size_t>(names.size)),
                      archive->source_->Read(tree.offset, static_cast<size_t>(tree.size)));

    return archive;
}

const std::vector<ZArchive::File>& ZArchive::Files() const
{
    return files_;
}

void ZArchive::Read(const File& file, uint64_t offset, uint8_t* out, size_t size)
{
    if (offset > file.size || size > file.size - offset)
    {
        throw Error("a read past the end of " + file.path);
    }

    uint64_t at = file.offset + offset;
    while (size)
    {
        LoadBlock(at / kBlockSize);
        const size_t within = static_cast<size_t>(at % kBlockSize);
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size, kBlockSize - within));
        std::copy_n(block_.begin() + static_cast<std::ptrdiff_t>(within), n, out);
        out += n;
        at += n;
        size -= n;
    }
}

ZArchive::ZArchive(std::unique_ptr<Source> source) : source_(std::move(source))
{
}

void ZArchive::ReadTree(const std::vector<uint8_t>& names, const std::vector<uint8_t>& tree)
{
    const size_t count = tree.size() / kEntrySize;
    if (!count || (LoadBe32(tree.data()) & kFileFlag))
    {
        throw Error("the archive has no root folder");
    }

    // The folders are walked breadth first, in the order the archive stores them. An entry that two folders claim, or
    // that comes before its folder, would make the tree a loop or a tangle. Such an entry is damage.
    struct Folder
    {
        size_t index;
        std::string path;
    };

    std::vector<Folder> folders = {{0, ""}};
    std::vector<bool> seen(count);
    seen[0] = true;
    for (size_t i = 0; i < folders.size(); ++i)
    {
        const uint8_t* entry = tree.data() + folders[i].index * kEntrySize;
        const uint64_t first = LoadBe32(entry + 4);
        const uint64_t entries = LoadBe32(entry + 8);
        if (first > count || entries > count - first)
        {
            throw Error("the archive's file tree is damaged");
        }

        for (uint64_t k = first; k < first + entries; ++k)
        {
            if (seen[static_cast<size_t>(k)])
            {
                throw Error("the archive's file tree is damaged");
            }
            seen[static_cast<size_t>(k)] = true;

            const uint8_t* child = tree.data() + k * kEntrySize;
            const uint32_t word = LoadBe32(child);
            const std::string path = folders[i].path + NameAt(names, word & ~kFileFlag);
            if (!(word & kFileFlag))
            {
                folders.push_back({static_cast<size_t>(k), path + "/"});
                continue;
            }

            const uint64_t high = LoadBe32(child + 12);
            files_.push_back({path, LoadBe32(child + 4) | ((high & 0xFFFF) << 32),
                              LoadBe32(child + 8) | ((high & 0xFFFF0000) << 16)});
        }
    }
}

void ZArchive::LoadBlock(uint64_t index)
{
    if (index == block_index_)
    {
        return;
    }

    // The block's stored bytes start after the blocks before it in the record. The count starts where the record's
    // first block starts.
    const uint64_t record_index = index / kBlocksPerRecord;
    if (record_index >= records_.size())
    {
        throw Error("the archive is damaged");
    }

    const Record& record = records_[static_cast<size_t>(record_index)];
    const size_t slot = static_cast<size_t>(index % kBlocksPerRecord);
    uint64_t offset = record.base;
    for (size_t i = 0; i < slot; ++i)
    {
        offset += record.sizes[i] + 1u;
    }
    const size_t stored = record.sizes[slot] + 1u;
    if (offset > data_size_ || stored > data_size_ - offset)
    {
        throw Error("the archive is damaged");
    }

    const std::vector<uint8_t> packed = source_->Read(data_offset_ + offset, stored);
    block_.resize(kBlockSize);
    block_index_ = UINT64_MAX;
    if (stored == kBlockSize)
    {
        std::copy(packed.begin(), packed.end(), block_.begin());
    }
    else
    {
        const size_t n = ZSTD_decompress(block_.data(), block_.size(), packed.data(), packed.size());
        if (ZSTD_isError(n) || n != kBlockSize)
        {
            throw Error("a block of the archive is damaged");
        }
    }
    block_index_ = index;
}

} // namespace csf::title
