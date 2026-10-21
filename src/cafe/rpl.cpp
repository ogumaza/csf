// SPDX-License-Identifier: MIT

#include "cafe/rpl.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include <zlib.h>

namespace csf::cafe
{
namespace
{

constexpr uint32_t kSectionHeaderSize = 40;
constexpr uint32_t kSymbolSize = 16;
constexpr uint32_t kRelaSize = 12;
constexpr uint8_t kCafeAbi = 0xCA;
constexpr uint16_t kPowerPc = 20;

// ELF's symbol type for a symbol that stands for a section (STT_SECTION).
constexpr uint8_t kSectionSymbol = 3;

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

    memory_size = xsflib::LoadBe32(raw.data());
    std::vector<uint8_t> data(memory_size);
    uLongf inflated = memory_size;
    if (uncompress(data.data(), &inflated, raw.data() + 4, static_cast<uLong>(raw.size() - 4)) != Z_OK ||
        inflated != memory_size)
    {
        Fail("a compressed section is damaged");
    }

    return data;
}

// Checks the ELF header: the file must be a 32-bit big-endian PowerPC file for Cafe OS.
void CheckHeader(std::span<const uint8_t> file)
{
    if (file.size() < 0x34 || xsflib::LoadBe32(file.data()) != 0x7F454C46)
    {
        Fail("it isn't an ELF file");
    }
    if (file[4] != 1 || file[5] != 2 || file[7] != kCafeAbi || xsflib::LoadBe16(file.data() + 0x12) != kPowerPc)
    {
        Fail("it isn't a 32-bit big-endian PowerPC file for Cafe OS");
    }
}

// The sections that the section headers give, with their contents as they are in memory and their names.
std::vector<Rpl::Section> ReadSections(std::span<const uint8_t> file)
{
    const uint32_t header_offset = xsflib::LoadBe32(file.data() + 0x20);
    const uint16_t header_size = xsflib::LoadBe16(file.data() + 0x2E);
    const uint16_t count = xsflib::LoadBe16(file.data() + 0x30);
    const uint16_t names_index = xsflib::LoadBe16(file.data() + 0x32);
    if (header_size < kSectionHeaderSize || header_offset > file.size() ||
        static_cast<uint64_t>(count) * header_size > file.size() - header_offset || names_index >= count)
    {
        Fail("its section headers are damaged");
    }

    std::vector<Rpl::Section> sections;
    std::vector<uint32_t> name_offsets;
    for (uint16_t i = 0; i < count; ++i)
    {
        const uint8_t* header = file.data() + header_offset + static_cast<size_t>(i) * header_size;
        Rpl::Section section;
        name_offsets.push_back(xsflib::LoadBe32(header));
        section.type = xsflib::LoadBe32(header + 4);
        section.flags = xsflib::LoadBe32(header + 8);
        section.address = xsflib::LoadBe32(header + 12);
        const uint32_t offset = xsflib::LoadBe32(header + 16);
        const uint32_t size = xsflib::LoadBe32(header + 20);
        section.link = xsflib::LoadBe32(header + 24);
        if (section.type == Rpl::kNoBits)
        {
            section.size = size;
        }
        else if (section.type != 0 && size != 0)
        {
            section.data = SectionData(file, offset, size, section.flags, section.size);
        }
        sections.push_back(std::move(section));
    }

    for (uint16_t i = 0; i < count; ++i)
    {
        sections[i].name = StringAt(sections[names_index].data, name_offsets[i]);
    }

    return sections;
}

// The small data areas' bases and the main thread's stack size, from the file information section.
void ReadFileInfo(Rpl& rpl)
{
    for (const Rpl::Section& section : rpl.sections)
    {
        if (section.type == Rpl::kFileInfo && section.data.size() >= 0x30)
        {
            rpl.sda_base = xsflib::LoadBe32(section.data.data() + 0x24);
            rpl.sda2_base = xsflib::LoadBe32(section.data.data() + 0x28);
            rpl.stack_size = xsflib::LoadBe32(section.data.data() + 0x2C);
        }
    }
}

// The symbol table, with its names. A file has one at most.
std::vector<Rpl::Symbol> ReadSymbols(const std::vector<Rpl::Section>& sections)
{
    std::vector<Rpl::Symbol> symbols;
    for (const Rpl::Section& section : sections)
    {
        if (section.type != Rpl::kSymTab)
        {
            continue;
        }
        if (!symbols.empty() || section.link >= sections.size())
        {
            Fail("its symbol tables are damaged");
        }

        const std::vector<uint8_t>& names = sections[section.link].data;
        for (size_t at = 0; at + kSymbolSize <= section.data.size(); at += kSymbolSize)
        {
            const uint8_t* entry = section.data.data() + at;
            Rpl::Symbol symbol;
            const uint32_t name = xsflib::LoadBe32(entry);
            symbol.name = name ? StringAt(names, name) : std::string();
            symbol.value = xsflib::LoadBe32(entry + 4);
            symbol.type = entry[12] & 0xF;
            symbol.section = xsflib::LoadBe16(entry + 14);
            symbols.push_back(std::move(symbol));
        }
    }

    return symbols;
}

// Adds the relocations of the relocation section `section` to `rpl`. Each names one of the symbol table's symbols.
void AddRelocations(const Rpl::Section& section, Rpl& rpl)
{
    if (section.link >= rpl.sections.size() || rpl.sections[section.link].type != Rpl::kSymTab)
    {
        Fail("a relocation section has no symbol table");
    }

    for (size_t at = 0; at + kRelaSize <= section.data.size(); at += kRelaSize)
    {
        const uint8_t* entry = section.data.data() + at;
        Rpl::Relocation relocation;
        relocation.address = xsflib::LoadBe32(entry);
        const uint32_t info = xsflib::LoadBe32(entry + 4);
        relocation.type = info & 0xFF;
        relocation.symbol = info >> 8;
        relocation.addend = static_cast<int32_t>(xsflib::LoadBe32(entry + 8));
        if (relocation.symbol >= rpl.symbols.size())
        {
            Fail("a relocation's symbol doesn't exist");
        }
        rpl.relocations.push_back(relocation);
    }
}

// Adds the imports of the import section `section`, section `index` of the file, to `rpl`: the symbols in the section,
// apart from the one that stands for the section. The section's name gives the module, and whether the section imports
// functions or data.
void AddImports(const Rpl::Section& section, size_t index, Rpl& rpl)
{
    const bool data = section.name.starts_with(".dimport_");
    if (!data && !section.name.starts_with(".fimport_"))
    {
        Fail("an import section's name is unknown: " + section.name);
    }

    const std::string module = section.name.substr(9);
    for (uint32_t s = 0; s < rpl.symbols.size(); ++s)
    {
        const Rpl::Symbol& symbol = rpl.symbols[s];
        if (symbol.section == index && !symbol.name.empty() && symbol.type != kSectionSymbol)
        {
            rpl.imports.push_back({module, symbol.name, s, data});
        }
    }
}

} // namespace

Rpl Rpl::Read(std::span<const uint8_t> file)
{
    CheckHeader(file);

    Rpl rpl;
    rpl.entry = xsflib::LoadBe32(file.data() + 0x18);
    rpl.sections = ReadSections(file);
    ReadFileInfo(rpl);
    rpl.symbols = ReadSymbols(rpl.sections);

    // The relocations and the imports, in the order of their sections.
    for (size_t i = 0; i < rpl.sections.size(); ++i)
    {
        const Section& section = rpl.sections[i];
        if (section.type == kRela)
        {
            AddRelocations(section, rpl);
        }
        else if (section.type == kImports)
        {
            AddImports(section, i, rpl);
        }
    }

    return rpl;
}

} // namespace csf::cafe
