// SPDX-License-Identifier: MIT

// Checks game mode's Cafe OS on files that the test makes. The test checks the RPL reader on a small library with a
// compressed section, a NOBITS section, imports of functions and data, and relocations, and the reader's refusals of
// damaged files. The test also checks the machine that loads the library and runs its code, with a handler for the
// library's import that calls back into the library, and that AXInit starts the AX library that the game links.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include <zlib.h>

#include "cafe/machine.h"
#include "cafe/rpl.h"
#include "cafe/sndcore2.h"

using xsflib::testing::Check;

namespace
{

using csf::cafe::Machine;
using csf::cafe::Rpl;
using csf::cafe::Sndcore2;
namespace ax = csf::ax;

// A section for MakeRpl: its contents as they are in memory. MakeRpl compresses them when `flags` has Rpl::kZlib.
struct TestSection
{
    std::string name;
    uint32_t type = 0;
    uint32_t flags = 0;
    uint32_t address = 0;
    std::vector<uint8_t> data;
    uint32_t nobits_size = 0;
    uint32_t link = 0;
    uint32_t info = 0;
};

void Append32(std::vector<uint8_t>& out, uint32_t value)
{
    out.resize(out.size() + 4);
    xsflib::StoreBe32(out.data() + out.size() - 4, value);
}

std::vector<uint8_t> Words(std::initializer_list<uint32_t> words)
{
    std::vector<uint8_t> out;
    for (const uint32_t word : words)
    {
        Append32(out, word);
    }

    return out;
}

// An RPL holding `sections` after the null section, with the section names' table last.
std::vector<uint8_t> MakeRpl(std::vector<TestSection> sections, uint32_t entry)
{
    TestSection names{".shstrtab", Rpl::kStrTab, 0, 0, {0}};
    std::vector<uint32_t> name_offsets = {0};
    sections.push_back(names);
    for (TestSection& section : sections)
    {
        name_offsets.push_back(static_cast<uint32_t>(sections.back().data.size()));
        sections.back().data.insert(sections.back().data.end(), section.name.begin(), section.name.end());
        sections.back().data.push_back(0);
    }

    std::vector<uint8_t> file(0x34);
    xsflib::StoreBe32(file.data(), 0x7F454C46);
    file[4] = 1;
    file[5] = 2;
    file[6] = 1;
    file[7] = 0xCA;
    file[8] = 0xFE;
    xsflib::StoreBe16(file.data() + 0x10, 0xFE01);
    xsflib::StoreBe16(file.data() + 0x12, 20);
    xsflib::StoreBe32(file.data() + 0x18, entry);
    xsflib::StoreBe16(file.data() + 0x2E, 40);
    xsflib::StoreBe16(file.data() + 0x30, static_cast<uint16_t>(sections.size() + 1));
    xsflib::StoreBe16(file.data() + 0x32, static_cast<uint16_t>(sections.size()));

    std::vector<uint8_t> headers(40);
    for (size_t i = 0; i < sections.size(); ++i)
    {
        const TestSection& section = sections[i];
        std::vector<uint8_t> stored = section.data;
        if (section.flags & Rpl::kZlib)
        {
            uLongf size = compressBound(static_cast<uLong>(section.data.size()));
            std::vector<uint8_t> compressed(size);
            compress(compressed.data(), &size, section.data.data(), static_cast<uLong>(section.data.size()));
            compressed.resize(size);
            stored = Words({static_cast<uint32_t>(section.data.size())});
            stored.insert(stored.end(), compressed.begin(), compressed.end());
        }

        const uint32_t offset = static_cast<uint32_t>(file.size());
        file.insert(file.end(), stored.begin(), stored.end());

        std::vector<uint8_t> header;
        Append32(header, name_offsets[i + 1]);
        Append32(header, section.type);
        Append32(header, section.flags);
        Append32(header, section.address);
        Append32(header, section.type == Rpl::kNoBits ? 0 : offset);
        Append32(header, section.type == Rpl::kNoBits ? section.nobits_size : static_cast<uint32_t>(stored.size()));
        Append32(header, section.link);
        Append32(header, section.info);
        Append32(header, 4);
        Append32(header, 0);
        headers.insert(headers.end(), header.begin(), header.end());
    }

    xsflib::StoreBe32(file.data() + 0x20, static_cast<uint32_t>(file.size()));
    file.insert(file.end(), headers.begin(), headers.end());

    return file;
}

// A symbol table entry.
std::vector<uint8_t> SymbolEntry(uint32_t name, uint32_t value, uint32_t size, uint8_t type, uint16_t section)
{
    std::vector<uint8_t> entry = Words({name, value, size, 0});
    entry[12] = static_cast<uint8_t>(0x10 | type);
    xsflib::StoreBe16(entry.data() + 14, section);

    return entry;
}

// A library whose code (compressed) has two functions. The first saves the return address, calls the function `name`
// that it imports from `module` with its argument, stores the result in an imported variable and returns it. The second
// doubles its argument.
std::vector<TestSection> Library(const std::string& module = "coreinit", const std::string& name = "OSReport")
{
    std::vector<TestSection> sections;

    // 1: the code.
    sections.push_back({".text", Rpl::kProgBits, Rpl::kAlloc | Rpl::kExecute | Rpl::kZlib, 0x02000000,
                        Words({
                            0x9421FFF0, // stwu r1, -16(r1)
                            0x7C0802A6, // mflr r0
                            0x90010014, // stw r0, 20(r1)
                            0x48000001, // bl name
                            0x3C800000, // lis r4, MEMAllocFromDefaultHeap@ha
                            0x38840000, // addi r4, r4, MEMAllocFromDefaultHeap@l
                            0x90640000, // stw r3, 0(r4)
                            0x80010014, // lwz r0, 20(r1)
                            0x7C0803A6, // mtlr r0
                            0x38210010, // addi r1, r1, 16
                            0x4E800020, // blr
                            0x7C631A14, // add r3, r3, r3
                            0x4E800020, // blr
                        })});

    // 2: initialised data, and 3: zeros.
    sections.push_back({".data", Rpl::kProgBits, Rpl::kAlloc | Rpl::kWrite, 0x10000000, Words({0x12345678})});
    sections.push_back({".bss", Rpl::kNoBits, Rpl::kAlloc | Rpl::kWrite, 0x10000004, {}, 0x100});

    // 4 and 5: the imports' tables, in the loader space's layout.
    sections.push_back(
        {".fimport_" + module, Rpl::kImports, Rpl::kAlloc | Rpl::kExecute, 0xC0000000, Words({1, 0, 0, 0})});
    sections.push_back({".dimport_coreinit", Rpl::kImports, Rpl::kAlloc, 0xC0001000, Words({1, 0, 0, 0})});

    // 6: the symbols' names, and 7: the symbols.
    const std::string names = std::string("\0main\0", 6) + name + std::string("\0MEMAllocFromDefaultHeap\0", 25);
    sections.push_back({".strtab", Rpl::kStrTab, Rpl::kZlib, 0, std::vector<uint8_t>(names.begin(), names.end())});
    const auto variable = static_cast<uint32_t>(6 + name.size() + 1);
    std::vector<uint8_t> symbols(16);
    for (const auto& entry : {SymbolEntry(0, 0x02000000, 0, 3, 1), SymbolEntry(1, 0x02000000, 44, 2, 1),
                              SymbolEntry(6, 0xC0000008, 8, 2, 4), SymbolEntry(variable, 0xC0001008, 4, 1, 5)})
    {
        symbols.insert(symbols.end(), entry.begin(), entry.end());
    }
    sections.push_back({".symtab", Rpl::kSymTab, 0, 0, symbols, 0, 6});

    // 8: the code's relocations: the bl, and the high and low halves of the variable's address.
    std::vector<uint8_t> relocations;
    for (const auto& word :
         {Words({0x0200000C, (3u << 8) | Rpl::kRel24, 0}), Words({0x02000012, (4u << 8) | Rpl::kAddr16Ha, 0}),
          Words({0x02000016, (4u << 8) | Rpl::kAddr16Lo, 0})})
    {
        relocations.insert(relocations.end(), word.begin(), word.end());
    }
    sections.push_back({".rela.text", Rpl::kRela, 0, 0, relocations, 0, 7, 1});

    return sections;
}

bool Refuses(const std::vector<uint8_t>& file)
{
    try
    {
        Rpl::Read(file);
    }
    catch (const std::runtime_error&)
    {
        return true;
    }

    return false;
}

void TestRead()
{
    const std::vector<TestSection> library = Library();

    const Rpl rpl = Rpl::Read(MakeRpl(library, 0x02000000));

    XSFLIB_CHECK(rpl.entry == 0x02000000);
    if (!Check(rpl.sections.size() == library.size() + 2, "the library's sections are all read"))
    {
        return;
    }

    const Rpl::Section& text = rpl.sections[1];
    XSFLIB_CHECK(text.name == ".text" && text.address == 0x02000000 && text.data == library[0].data && text.size == 52);
    XSFLIB_CHECK(rpl.sections[3].name == ".bss" && rpl.sections[3].size == 0x100 && rpl.sections[3].data.empty());
    XSFLIB_CHECK(rpl.sections.back().name == ".shstrtab");

    XSFLIB_CHECK(rpl.symbols.size() == 5 && rpl.symbols[2].name == "main" && rpl.symbols[2].value == 0x02000000 &&
                 rpl.symbols[2].type == 2 && rpl.symbols[2].section == 1);
    XSFLIB_CHECK(rpl.symbols[3].name == "OSReport" && rpl.symbols[3].value == 0xC0000008);

    XSFLIB_CHECK(rpl.relocations.size() == 3 && rpl.relocations[0].address == 0x0200000C &&
                 rpl.relocations[0].type == Rpl::kRel24 && rpl.relocations[0].symbol == 3 &&
                 rpl.relocations[2].type == Rpl::kAddr16Lo && rpl.relocations[2].symbol == 4);

    // The import sections' symbols, functions and data apart.
    if (Check(rpl.imports.size() == 2, "the library's two imports are found"))
    {
        XSFLIB_CHECK(rpl.imports[0].module == "coreinit" && rpl.imports[0].name == "OSReport" && !rpl.imports[0].data);
        XSFLIB_CHECK(rpl.imports[1].name == "MEMAllocFromDefaultHeap" && rpl.imports[1].data &&
                     rpl.imports[1].symbol == 4);
    }
}

void TestRefusals()
{
    const std::vector<uint8_t> good = MakeRpl(Library(), 0);
    XSFLIB_CHECK(!Refuses(good));

    std::vector<uint8_t> not_elf = good;
    not_elf[1] = 'X';
    XSFLIB_CHECK(Refuses(not_elf));

    std::vector<uint8_t> not_cafe = good;
    not_cafe[7] = 0;
    XSFLIB_CHECK(Refuses(not_cafe));

    XSFLIB_CHECK(Refuses(std::vector<uint8_t>(good.begin(), good.begin() + 0x30)));

    // A compressed section whose stream is damaged.
    std::vector<uint8_t> damaged = good;
    const uint32_t text_offset = xsflib::LoadBe32(damaged.data() + xsflib::LoadBe32(damaged.data() + 0x20) + 40 + 16);
    damaged[text_offset + 6] ^= 0xFF;
    XSFLIB_CHECK(Refuses(damaged));

    // A relocation of a symbol that doesn't exist.
    std::vector<TestSection> bad_symbol = Library();
    xsflib::StoreBe32(bad_symbol[7].data.data() + 4, (9u << 8) | Rpl::kRel24);
    XSFLIB_CHECK(Refuses(MakeRpl(bad_symbol, 0)));
}

// Whether `f` throws an exception whose message holds `what`.
template <typename F>
bool ThrowsAbout(F&& f, const std::string& what)
{
    return xsflib::testing::ErrorOf(std::forward<F>(f)).find(what) != std::string::npos;
}

void TestMachine()
{
    const Rpl rpl = Rpl::Read(MakeRpl(Library(), 0x02000000));

    // The imported function's handler calls back into the library's second function. So the first returns 2x + 1.
    Machine machine(0x10000);
    const auto os_report = [](Machine& m)
    {
        const uint32_t doubled = m.Call(0x0200002C, {m.Arg(0)}).r3;
        m.Return(doubled + 1);
    };
    machine.Register("coreinit", "OSReport", os_report);
    machine.LoadExecutable(rpl);

    const uint32_t variable = machine.DataImport("coreinit", "MEMAllocFromDefaultHeap");
    XSFLIB_CHECK(variable != 0 && machine.DataImport("coreinit", "missing") == 0);
    XSFLIB_CHECK(machine.memory.Read32(0x10000000) == 0x12345678 && machine.memory.Read32(0x10000004) == 0);

    const uint32_t r1 = machine.cpu.state_.gpr[1];
    XSFLIB_CHECK(machine.Call(0x02000000, {20}).r3 == 41 && machine.memory.Read32(variable) == 41);
    XSFLIB_CHECK(machine.cpu.state_.gpr[1] == r1);

    // The heap, and a trap for a function pointer the system gives the guest.
    const uint32_t block = machine.Allocate(100, 64);
    XSFLIB_CHECK(block % 64 == 0 && block > 0x10000104 && machine.Allocate(4) >= block + 100);
    const uint32_t trap = machine.MakeTrap("test", [](Machine& m) { m.Return(m.Arg(0) * 3); });
    XSFLIB_CHECK(machine.Call(trap, {7}).r3 == 21);

    // An import with no handler stops the guest with an error that names it.
    Machine bare(0x10000);
    bare.LoadExecutable(rpl);

    XSFLIB_CHECK(ThrowsAbout([&] { bare.Call(0x02000000, {1}); }, "coreinit.OSReport"));
    XSFLIB_CHECK(ThrowsAbout([&] { bare.Call(0x10000000); }, "unknown instruction"));
    XSFLIB_CHECK(ThrowsAbout([&] { bare.Call(0x01000000); }, "unmapped"));
}

void TestSndCoreImports()
{
    // A game that links snd_core or sndcore2 reaches the AX handlers. Its AXInit starts the model as that library.
    for (const auto& [module, library] :
         {std::pair{"snd_core", ax::System::Library::kSndCore}, std::pair{"sndcore2", ax::System::Library::kSndCore2}})
    {
        const Rpl rpl = Rpl::Read(MakeRpl(Library(module, "AXInit"), 0x02000000));
        Machine machine(0x10000);
        Sndcore2 sndcore(machine);
        machine.LoadExecutable(rpl);

        XSFLIB_CHECK(machine.Call(0x02000000, {0}).r3 == 0);
        XSFLIB_CHECK(sndcore.System().LinkedLibrary() == library);
    }
}

} // namespace

int main()
{
    TestRead();
    TestRefusals();
    TestMachine();
    TestSndCoreImports();

    return xsflib::testing::Summary("cafe");
}
