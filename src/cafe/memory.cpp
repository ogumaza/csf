// SPDX-License-Identifier: MIT

#include "cafe/memory.h"

#include <bit>
#include <stdexcept>

#include "common/bytes.h"

namespace csf::cafe
{
namespace
{

constexpr uint32_t kPage = 0x1000;

std::string Hex(uint32_t value)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        text += kDigits[(value >> shift) & 0xF];
    }

    return text;
}

} // namespace

void Memory::AddRegion(uint32_t address, uint32_t size)
{
    if (address % kPage != 0 || size % kPage != 0 || size == 0 || uint64_t{address} + size > 0x100000000ull)
    {
        throw std::runtime_error("a guest memory region must be whole pages: " + Hex(address));
    }
    for (const Region& region : regions_)
    {
        if (address < region.address + region.bytes.size() && region.address < address + size)
        {
            throw std::runtime_error("guest memory regions overlap at " + Hex(address));
        }
    }

    regions_.push_back({address, std::vector<uint8_t>(size)});
}

std::span<uint8_t> Memory::RegionAt(uint32_t address)
{
    for (Region& region : regions_)
    {
        if (address - region.address < region.bytes.size())
        {
            return region.bytes;
        }
    }

    return {};
}

uint8_t* Memory::Pointer(uint32_t address, uint32_t size)
{
    for (Region& region : regions_)
    {
        const uint32_t offset = address - region.address;
        if (offset < region.bytes.size() && size <= region.bytes.size() - offset)
        {
            return region.bytes.data() + offset;
        }
    }

    throw std::runtime_error("the guest reached unmapped memory at " + Hex(address));
}

uint16_t Memory::Read16(uint32_t address)
{
    return LoadBe16(Pointer(address, 2));
}

uint32_t Memory::Read32(uint32_t address)
{
    return LoadBe32(Pointer(address, 4));
}

float Memory::ReadFloat(uint32_t address)
{
    return std::bit_cast<float>(Read32(address));
}

void Memory::Write16(uint32_t address, uint16_t value)
{
    StoreBe16(Pointer(address, 2), value);
}

void Memory::Write32(uint32_t address, uint32_t value)
{
    StoreBe32(Pointer(address, 4), value);
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

uint8_t* Memory::RamPage(ppc::PAddr page)
{
    for (Region& region : regions_)
    {
        if (page - region.address < region.bytes.size())
        {
            return region.bytes.data() + (page - region.address);
        }
    }

    return nullptr;
}

bool Memory::ReadIo(ppc::PAddr, int, uint32_t&)
{
    return false;
}

bool Memory::WriteIo(ppc::PAddr, int, uint32_t)
{
    return false;
}

} // namespace csf::cafe
