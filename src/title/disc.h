// SPDX-License-Identifier: MIT

// Wii U disc images. A WUD is the disc's raw 25,025,314,816 bytes, whole or in the parts that wudump writes
// (game_part1.wud, game_part2.wud and so on). A WUX stores each distinct 32 KiB sector of a WUD once. Past the disc
// header, everything on a Wii U disc is encrypted. A dump keeps the key beside the image rather than in it. So CSF
// reads no further than the header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "title/source.h"

namespace csf::title
{

// Random access to a Wii U disc image's bytes.
class DiscImage
{
public:
    virtual ~DiscImage() = default;

    // The container's name for messages: "WUD" or "WUX".
    virtual std::string Format() const = 0;

    // The disc's size in bytes.
    virtual uint64_t Size() const = 0;

    // Reads `size` bytes of the disc at `offset` into `out`. Bytes past the disc's end read as zeros. Throws Error.
    virtual void Read(uint64_t offset, uint8_t* out, size_t size) = 0;
};

// Opens a WUD or WUX by its contents rather than its name. A WUD in parts is opened by its first part. The first part's
// name ends in "_part1.wud", and the disc continues in the parts numbered after it. Returns null if the file is neither
// format. Throws Error for one that's damaged.
std::unique_ptr<DiscImage> OpenDiscImage(const std::filesystem::path& path);

// The disc's ID from its header ("WUP-P-ATSE-00-000USA-0"). The ID's first ten characters are the game's product code.
// Nothing if the disc doesn't start with one. Throws Error.
std::optional<std::string> DiscId(DiscImage& disc);

} // namespace csf::title
