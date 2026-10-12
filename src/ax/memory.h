// SPDX-License-Identifier: MIT

// The memory AX's renderer reads sample data from: regions of bytes at the addresses a game's voices name. Archive mode
// maps a sound archive's waves there.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace csf::ax
{

class Memory
{
public:
    // Maps `bytes` at `address`. The bytes must outlive the Memory, and regions mustn't overlap.
    void Map(uint32_t address, std::span<const uint8_t> bytes)
    {
        regions_.push_back({address, bytes});
    }

    // The byte at `address`, or 0 where nothing is mapped.
    uint8_t Read8(uint32_t address) const
    {
        if (last_ < regions_.size() && Contains(regions_[last_], address))
        {
            return regions_[last_].bytes[address - regions_[last_].address];
        }

        for (std::size_t i = 0; i < regions_.size(); ++i)
        {
            if (Contains(regions_[i], address))
            {
                last_ = i;
                return regions_[i].bytes[address - regions_[i].address];
            }
        }

        return 0;
    }

private:
    struct Region
    {
        uint32_t address;
        std::span<const uint8_t> bytes;
    };

    static bool Contains(const Region& region, uint32_t address)
    {
        return address - region.address < region.bytes.size();
    }

    std::vector<Region> regions_;
    mutable std::size_t last_ = 0; // the region the last read found
};

} // namespace csf::ax
