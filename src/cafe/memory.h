// SPDX-License-Identifier: MIT

// The guest's memory in game mode: regions of RAM at fixed addresses. The CPU reaches them through its BATs. The
// system's high-level parts reach them through the accessors here. Addresses are the guest's virtual addresses. Game
// mode maps them one to one.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ppc/cpu.h"

namespace csf::cafe
{

class Memory : public ppc::Bus
{
public:
    // Adds `size` zeroed bytes at `address`. Both are multiples of 4 KiB. The new region must not overlap another
    // region.
    void AddRegion(uint32_t address, uint32_t size);

    // The region holding `address` in host memory, or an empty span.
    std::span<uint8_t> RegionAt(uint32_t address);

    // Host memory for the `size` bytes at `address`. The bytes must lie in one region. Throws std::runtime_error if
    // they don't.
    uint8_t* Pointer(uint32_t address, uint32_t size);

    uint8_t Read8(uint32_t address)
    {
        return *Pointer(address, 1);
    }

    uint16_t Read16(uint32_t address);
    uint32_t Read32(uint32_t address);
    float ReadFloat(uint32_t address);
    void Write8(uint32_t address, uint8_t value)
    {
        *Pointer(address, 1) = value;
    }

    void Write16(uint32_t address, uint16_t value);
    void Write32(uint32_t address, uint32_t value);
    void WriteFloat(uint32_t address, float value);

    // The NUL-terminated string at `address`.
    std::string ReadString(uint32_t address);

    // ppc::Bus: RAM only, no devices.
    uint8_t* RamPage(ppc::PAddr page) override;
    bool ReadIo(ppc::PAddr address, int size, uint32_t& value) override;
    bool WriteIo(ppc::PAddr address, int size, uint32_t value) override;

private:
    struct Region
    {
        uint32_t address;
        std::vector<uint8_t> bytes;
    };

    std::vector<Region> regions_;
};

} // namespace csf::cafe
