// SPDX-License-Identifier: MIT

// Bounds-checked reads for the NintendoWare for Cafe sound formats. The Wii U's files are big-endian, and the Switch's
// files are little-endian. The byte order mark at offset 4 gives a file's byte order.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#include "common/bytes.h"

namespace csf::fsar
{

// Reads values from a byte span in the file's byte order. A read past the end throws std::runtime_error.
class Reader
{
public:
    Reader(std::span<const uint8_t> data, bool big_endian) : data_(data), big_endian_(big_endian)
    {
    }

    std::size_t Size() const
    {
        return data_.size();
    }

    std::span<const uint8_t> Sub(std::size_t off, std::size_t len) const
    {
        Check(off, len);

        return data_.subspan(off, len);
    }

    uint8_t U8(std::size_t off) const
    {
        Check(off, 1);

        return data_[off];
    }

    uint16_t U16(std::size_t off) const
    {
        Check(off, 2);

        return big_endian_ ? LoadBe16(data_.data() + off) : LoadLe16(data_.data() + off);
    }

    int16_t S16(std::size_t off) const
    {
        return static_cast<int16_t>(U16(off));
    }

    uint32_t U32(std::size_t off) const
    {
        Check(off, 4);

        return big_endian_ ? LoadBe32(data_.data() + off) : LoadLe32(data_.data() + off);
    }

    int32_t S32(std::size_t off) const
    {
        return static_cast<int32_t>(U32(off));
    }

    float F32(std::size_t off) const
    {
        return std::bit_cast<float>(U32(off));
    }

    // A NUL-terminated string of at most `max_len` bytes.
    std::string CString(std::size_t off, std::size_t max_len) const
    {
        std::string s;
        while (off < data_.size() && data_[off] && s.size() < max_len)
        {
            s.push_back(static_cast<char>(data_[off++]));
        }

        return s;
    }

    bool Magic(std::size_t off, const char* m) const
    {
        Check(off, 4);

        return std::memcmp(data_.data() + off, m, 4) == 0;
    }

private:
    void Check(std::size_t off, std::size_t len) const
    {
        if (off > data_.size() || len > data_.size() - off)
        {
            throw std::runtime_error("read out of bounds at offset " + std::to_string(off));
        }
    }

    std::span<const uint8_t> data_;
    bool big_endian_;
};

// A NintendoWare "reference": uint16_t type ID, uint16_t padding and int32_t offset. The structure that holds the
// reference defines the offset's base.
struct Reference
{
    uint16_t type = 0;
    int32_t offset = -1;
};

inline Reference ReadRef(const Reader& r, std::size_t off)
{
    return Reference{r.U16(off), r.S32(off + 4)};
}

} // namespace csf::fsar
