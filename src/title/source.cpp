// SPDX-License-Identifier: MIT

#include "title/source.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <utility>
#include <vector>

namespace csf::title
{

Source::Source(const std::vector<std::filesystem::path>& parts)
{
    for (const std::filesystem::path& path : parts)
    {
        Part part;
        part.file.open(path, std::ios::binary);
        if (!part.file)
        {
            throw Error("can't open " + path.filename().string());
        }

        part.file.seekg(0, std::ios::end);
        part.start = size_;
        part.size = static_cast<uint64_t>(part.file.tellg());
        size_ += part.size;
        parts_.push_back(std::move(part));
    }
}

uint64_t Source::Size() const
{
    return size_;
}

void Source::Read(uint64_t offset, uint8_t* out, size_t size)
{
    if (offset > size_ || size > size_ - offset)
    {
        throw Error("the file is truncated");
    }

    for (Part& part : parts_)
    {
        if (!size || offset >= part.start + part.size)
        {
            continue;
        }

        const uint64_t within = offset - part.start;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size, part.size - within));
        part.file.clear();
        part.file.seekg(static_cast<std::streamoff>(within));
        if (!part.file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n)))
        {
            throw Error("reading the file failed");
        }

        out += n;
        offset += n;
        size -= n;
    }
}

std::vector<uint8_t> Source::Read(uint64_t offset, size_t size)
{
    std::vector<uint8_t> data(size);
    Read(offset, data.data(), size);

    return data;
}

} // namespace csf::title
