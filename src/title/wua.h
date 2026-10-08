// SPDX-License-Identifier: MIT

// ZArchive files. Cemu's Wii U archives (.wua) are ZArchive files. A ZArchive file holds files compressed with
// Zstandard in 64 KiB blocks, then a tree of folders and files, and a footer at the end that locates everything. A .wua
// holds a folder for each title in it. The folder's name is the 16-digit title ID, "_v" and the version in decimal
// ("0005000012345600_v0"). Exzap designed the format, and Exzap's ZArchive library (MIT No Attribution) describes it.
// This reader was written from that description.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "title/source.h"

namespace csf::title
{

class ZArchive
{
public:
    struct File
    {
        std::string path; // folders separated by '/' ("0005000012345600_v0/code/app.xml")
        uint64_t offset;  // in the archive's uncompressed data
        uint64_t size;
    };

    // Opens a ZArchive file. Returns null if the file isn't one (it has no ZArchive footer). Throws Error if it's
    // damaged.
    static std::unique_ptr<ZArchive> Open(const std::filesystem::path& path);

    // Every file, folder by folder in the order of the archive's tree.
    const std::vector<File>& Files() const;

    // Reads `size` bytes of `file` at `offset` into `out`. Throws Error.
    void Read(const File& file, uint64_t offset, uint8_t* out, size_t size);

private:
    // An offset record locates 16 blocks: where the first one's stored bytes start in the compressed data, and each
    // one's stored size less one.
    struct Record
    {
        uint64_t base;
        uint16_t sizes[16];
    };

    explicit ZArchive(std::unique_ptr<Source> source);

    // Reads the names and the tree of folders and files into files_.
    void ReadTree(const std::vector<uint8_t>& names, const std::vector<uint8_t>& tree);

    // Decompresses block `index` into block_ unless it's there already.
    void LoadBlock(uint64_t index);

    std::unique_ptr<Source> source_;
    std::vector<File> files_;
    std::vector<Record> records_;
    uint64_t data_offset_ = 0;
    uint64_t data_size_ = 0;
    std::vector<uint8_t> block_;
    uint64_t block_index_ = UINT64_MAX;
};

} // namespace csf::title
