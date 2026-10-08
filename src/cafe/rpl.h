// SPDX-License-Identifier: MIT

// RPX and RPL files, the Wii U's executables and libraries: big-endian ELF files for 32-bit PowerPC with Cafe OS's ABI.
// Their sections may be compressed with zlib. They list the functions and data that they import from the system's
// libraries in sections of their own. The reader follows WiiUBrew's page on the RPL format.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace csf::cafe
{

class Rpl
{
public:
    // Section types, from ELF and from Cafe OS.
    static constexpr uint32_t kProgBits = 1;
    static constexpr uint32_t kSymTab = 2;
    static constexpr uint32_t kStrTab = 3;
    static constexpr uint32_t kRela = 4;
    static constexpr uint32_t kNoBits = 8;
    static constexpr uint32_t kExports = 0x80000001;
    static constexpr uint32_t kImports = 0x80000002;
    static constexpr uint32_t kFileInfo = 0x80000004;

    // Section flags.
    static constexpr uint32_t kWrite = 0x1;
    static constexpr uint32_t kAlloc = 0x2;
    static constexpr uint32_t kExecute = 0x4;
    static constexpr uint32_t kZlib = 0x08000000;

    // Relocation types.
    static constexpr uint32_t kAddr32 = 1;
    static constexpr uint32_t kAddr16Lo = 4;
    static constexpr uint32_t kAddr16Hi = 5;
    static constexpr uint32_t kAddr16Ha = 6;
    static constexpr uint32_t kRel24 = 10;
    static constexpr uint32_t kRel14 = 11;
    static constexpr uint32_t kRel32 = 26;

    struct Section
    {
        std::string name;
        uint32_t type = 0;
        uint32_t flags = 0;
        uint32_t address = 0;
        uint32_t size = 0; // in memory: a compressed section's inflated size, or a NOBITS section's zeros
        uint32_t link = 0;
        uint32_t info = 0;
        std::vector<uint8_t> data; // inflated, and empty for a NOBITS section
    };

    // A symbol of the symbol table. `type` is ELF's STT (2 for a function, 3 for a section).
    struct Symbol
    {
        std::string name;
        uint32_t value = 0;
        uint32_t size = 0;
        uint8_t type = 0;
        uint16_t section = 0;
    };

    // A relocation: the address of the field it fills in, and the symbol (by index) whose value, plus the addend,
    // goes there.
    struct Relocation
    {
        uint32_t address = 0;
        uint32_t type = 0;
        uint32_t symbol = 0;
        int32_t addend = 0;
    };

    // A function or data that the file imports: a symbol in a .fimport_<module> or .dimport_<module> section.
    struct Import
    {
        std::string module;
        std::string name;
        uint32_t symbol = 0;
        bool data = false;
    };

    // Reads an RPX or RPL. Throws std::runtime_error if `file` isn't one or is damaged.
    static Rpl Read(std::span<const uint8_t> file);

    // The symbol named `name`, or nullptr.
    const Symbol* FindSymbol(const std::string& name) const;

    uint32_t entry = 0;

    // From the file information section: the small data areas' bases (held in r13 and r2) and the main thread's stack
    // size.
    uint32_t sda_base = 0;
    uint32_t sda2_base = 0;
    uint32_t stack_size = 0;

    std::vector<Section> sections;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocations;
    std::vector<Import> imports;
};

} // namespace csf::cafe
