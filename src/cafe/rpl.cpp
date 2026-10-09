// SPDX-License-Identifier: MIT

#include "cafe/rpl.h"

#include <zlib.h>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "common/bytes.h"

namespace csf::cafe
{
namespace
{

constexpr uint32_t kSectionHeaderSize = 40;
constexpr uint32_t kSymbolSize = 16;
constexpr uint32_t kRelaSize = 12;
constexpr uint8_t kCafeAbi = 0xCA;
constexpr uint16_t kPowerPc = 20;

[[noreturn]] void Fail(const std::string& what)
{
    throw std::runtime_error("not a valid RPX or RPL: " + what);
}

// The NUL-terminated string at `offset` in a string table.
std::string StringAt(const std::vector<uint8_t>& table, uint32_t offset)
{
    if (offset >= table.size())
    {
        Fail("a name lies outside its string table");
    }
    const auto* start = reinterpret_cast<const char*>(table.data() + offset);
    const std::string_view rest(start, table.size() - offset);
    const size_t end = rest.find('\0');
    if (end == std::string_view::npos)
    {
        Fail("a name runs past its string table");
    }

    return std::string(rest.substr(0, end));
}

// A section's contents as they are in memory: a compressed section starts with its inflated size, then a zlib stream.
std::vector<uint8_t> SectionData(std::span<const uint8_t> file, uint32_t offset, uint32_t size, uint32_t flags,
                                 uint32_t& memory_size)
{
    if (offset > file.size() || size > file.size() - offset)
    {
        Fail("a section lies outside the file");
    }
    const std::span<const uint8_t> raw = file.subspan(offset, size);
    if (!(flags & Rpl::kZlib))
    {
        memory_size = size;
        return std::vector<uint8_t>(raw.begin(), raw.end());
    }

    if (raw.size() < 4)
    {
        Fail("a compressed section is too short");
    }
    memory_size = LoadBe32(raw.data());
    std::vector<uint8_t> data(memory_size);
    uLongf inflated = memory_size;
    if (uncompress(data.data(), &inflated, raw.data() + 4, static_cast<uLong>(raw.size() - 4)) != Z_OK ||
        inflated != memory_size)
    {
        Fail("a compressed section is damaged");
    }

    return data;
}

} // namespace

Rpl Rpl::Read(std::span<const uint8_t> file)
{
    if (file.size() < 0x34 || LoadBe32(file.data()) != 0x7F454C46)
    {
        Fail("it isn't an ELF file");
    }
    if (file[4] != 1 || file[5] != 2 || file[7] != kCafeAbi || LoadBe16(file.data() + 0x12) != kPowerPc)
    {
        Fail("it isn't a 32-bit big-endian PowerPC file for Cafe OS");
    }

    Rpl rpl;
    rpl.entry = LoadBe32(file.data() + 0x18);
    const uint32_t header_offset = LoadBe32(file.data() + 0x20);
    const uint16_t header_size = LoadBe16(file.data() + 0x2E);
    const uint16_t count = LoadBe16(file.data() + 0x30);
    const uint16_t names_index = LoadBe16(file.data() + 0x32);
    if (header_size < kSectionHeaderSize || header_offset > file.size() ||
        static_cast<uint64_t>(count) * header_size > file.size() - header_offset || names_index >= count)
    {
        Fail("its section headers are damaged");
    }

    std::vector<uint32_t> name_offsets;
    for (uint16_t i = 0; i < count; ++i)
    {
        const uint8_t* header = file.data() + header_offset + static_cast<size_t>(i) * header_size;
        Section section;
        name_offsets.push_back(LoadBe32(header));
        section.type = LoadBe32(header + 4);
        section.flags = LoadBe32(header + 8);
        section.address = LoadBe32(header + 12);
        const uint32_t offset = LoadBe32(header + 16);
        const uint32_t size = LoadBe32(header + 20);
        section.link = LoadBe32(header + 24);
        section.info = LoadBe32(header + 28);
        if (section.type == kNoBits)
        {
            section.size = size;
        }
        else if (section.type != 0 && size != 0)
        {
            section.data = SectionData(file, offset, size, section.flags, section.size);
        }
        rpl.sections.push_back(std::move(section));
    }
    for (uint16_t i = 0; i < count; ++i)
    {
        rpl.sections[i].name = StringAt(rpl.sections[names_index].data, name_offsets[i]);
    }

    for (const Section& section : rpl.sections)
    {
        if (section.type == kFileInfo && section.data.size() >= 0x30)
        {
            rpl.sda_base = LoadBe32(section.data.data() + 0x24);
            rpl.sda2_base = LoadBe32(section.data.data() + 0x28);
            rpl.stack_size = LoadBe32(section.data.data() + 0x2C);
        }
    }

    // The symbol table, with its names.
    for (const Section& section : rpl.sections)
    {
        if (section.type != kSymTab)
        {
            continue;
        }
        if (!rpl.symbols.empty() || section.link >= rpl.sections.size())
        {
            Fail("its symbol tables are damaged");
        }
        const std::vector<uint8_t>& names = rpl.sections[section.link].data;
        for (size_t at = 0; at + kSymbolSize <= section.data.size(); at += kSymbolSize)
        {
            const uint8_t* entry = section.data.data() + at;
            Symbol symbol;
            const uint32_t name = LoadBe32(entry);
            symbol.name = name ? StringAt(names, name) : std::string();
            symbol.value = LoadBe32(entry + 4);
            symbol.size = LoadBe32(entry + 8);
            symbol.type = entry[12] & 0xF;
            symbol.section = LoadBe16(entry + 14);
            rpl.symbols.push_back(std::move(symbol));
        }
    }

    // The relocations, and the imports: the symbols in the import sections.
    for (size_t i = 0; i < rpl.sections.size(); ++i)
    {
        const Section& section = rpl.sections[i];
        if (section.type == kRela)
        {
            if (section.link >= rpl.sections.size() || rpl.sections[section.link].type != kSymTab)
            {
                Fail("a relocation section has no symbol table");
            }
            for (size_t at = 0; at + kRelaSize <= section.data.size(); at += kRelaSize)
            {
                const uint8_t* entry = section.data.data() + at;
                Relocation relocation;
                relocation.address = LoadBe32(entry);
                const uint32_t info = LoadBe32(entry + 4);
                relocation.type = info & 0xFF;
                relocation.symbol = info >> 8;
                relocation.addend = static_cast<int32_t>(LoadBe32(entry + 8));
                if (relocation.symbol >= rpl.symbols.size())
                {
                    Fail("a relocation's symbol doesn't exist");
                }
                rpl.relocations.push_back(relocation);
            }
        }
        else if (section.type == kImports)
        {
            const bool data = section.name.starts_with(".dimport_");
            if (!data && !section.name.starts_with(".fimport_"))
            {
                Fail("an import section's name is unknown: " + section.name);
            }
            const std::string module = section.name.substr(9);
            for (uint32_t s = 0; s < rpl.symbols.size(); ++s)
            {
                if (rpl.symbols[s].section == i && !rpl.symbols[s].name.empty() && rpl.symbols[s].type != 3)
                {
                    rpl.imports.push_back({module, rpl.symbols[s].name, s, data});
                }
            }
        }
    }

    return rpl;
}

const Rpl::Symbol* Rpl::FindSymbol(const std::string& name) const
{
    for (const Symbol& symbol : symbols)
    {
        if (symbol.name == name)
        {
            return &symbol;
        }
    }

    return nullptr;
}

} // namespace csf::cafe
