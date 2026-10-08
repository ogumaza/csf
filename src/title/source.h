// SPDX-License-Identifier: MIT

// Random access to the bytes of a file, or of a file split into parts that are read as one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace csf::title
{

// A damaged or unsupported file, or a read that failed.
class Error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Source
{
public:
    // Opens the files in `parts`, in order, as one. Throws Error if one can't be opened.
    explicit Source(const std::vector<std::filesystem::path>& parts);

    uint64_t Size() const;

    // Reads exactly `size` bytes at `offset`. Throws Error if they aren't all there or the read fails.
    void Read(uint64_t offset, uint8_t* out, size_t size);

    std::vector<uint8_t> Read(uint64_t offset, size_t size);

private:
    struct Part
    {
        std::ifstream file;
        uint64_t start = 0;
        uint64_t size = 0;
    };

    std::vector<Part> parts_;
    uint64_t size_ = 0;
};

} // namespace csf::title
