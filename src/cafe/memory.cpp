// SPDX-License-Identifier: MIT

#include "cafe/memory.h"

#include <bit>
#include <stdexcept>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

namespace csf::cafe
{

void Memory::AddRegion(uint32_t address, uint32_t size)
{
    if (address % kPageSize != 0 || size % kPageSize != 0 || size == 0 || uint64_t{address} + size > 0x100000000ull)
    {
        throw std::runtime_error("a guest memory region must be whole pages: " + xsflib::Hex(address, 8));
    }
    for (const Region& region : regions_)
    {
        if (address < region.address + region.bytes.size() && region.address < address + size)
        {
            throw std::runtime_error("guest memory regions overlap at " + xsflib::Hex(address, 8));
        }
    }

    regions_.push_back({address, std::vector<uint8_t>(size)});
}

std::span<uint8_t> Memory::RegionAt(uint32_t address)
{
    // The regions don't overlap (AddRegion). At most one holds `address`.
    for (Region& region : regions_)
    {
        const uint32_t offset = address - region.address;
        if (offset < region.bytes.size())
        {
            return std::span(region.bytes).subspan(offset);
        }
    }

    return {};
}

uint8_t* Memory::Pointer(uint32_t address, uint32_t size)
{
    const std::span<uint8_t> bytes = RegionAt(address);
    if (bytes.empty() || size > bytes.size())
    {
        throw std::runtime_error("the guest reached unmapped memory at " + xsflib::Hex(address, 8));
    }

    return bytes.data();
}

uint16_t Memory::Read16(uint32_t address)
{
    return xsflib::LoadBe16(Pointer(address, 2));
}

uint32_t Memory::Read32(uint32_t address)
{
    return xsflib::LoadBe32(Pointer(address, 4));
}

float Memory::ReadFloat(uint32_t address)
{
    return std::bit_cast<float>(Read32(address));
}

void Memory::Write16(uint32_t address, uint16_t value)
{
    xsflib::StoreBe16(Pointer(address, 2), value);
}

void Memory::Write32(uint32_t address, uint32_t value)
{
    xsflib::StoreBe32(Pointer(address, 4), value);
}

void Memory::WriteFloat(uint32_t address, float value)
{
    Write32(address, std::bit_cast<uint32_t>(value));
}

std::string Memory::ReadString(uint32_t address)
{
    std::string text;
    for (char c = static_cast<char>(Read8(address)); c != '\0'; c = static_cast<char>(Read8(++address)))
    {
        text += c;
    }

    return text;
}

uint8_t* Memory::RamPage(ntdlib::PAddr page)
{
    const std::span<uint8_t> bytes = RegionAt(page);
    return bytes.empty() ? nullptr : bytes.data();
}

bool Memory::ReadIo(ntdlib::PAddr, int, uint32_t&)
{
    return false;
}

bool Memory::WriteIo(ntdlib::PAddr, int, uint32_t)
{
    return false;
}

} // namespace csf::cafe
