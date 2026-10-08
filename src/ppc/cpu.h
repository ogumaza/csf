// SPDX-License-Identifier: MIT

// Broadway (IBM PowerPC 750CL) interpreter: the integer, floating-point and paired-single instruction sets, the
// supervisor-mode registers the Revolution OS uses (BATs, HIDs, SRRs, the decrementer and time base), and the
// exceptions it handles (external interrupts, the decrementer, system calls, floating-point unavailable and traps).
// The Wii U's Espresso runs the same instruction sets.
//
// This is WSF's interpreter, written for WSF's game mode, and adapted for CSF's game mode. WSF's game mode runs a Wii
// game's code on emulated hardware. CSF's game mode runs a Wii U game's code with Cafe OS at a high level. CSF maps its
// memory with the BATs, and stops at an illegal instruction where a call into the system's libraries goes. The
// interpreter follows IBM's PowerPC architecture books and the 750CL user's manual. Memory translation uses the BATs
// only. An access that no BAT translates stops execution as a fault rather than raising a DSI or ISI exception.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace csf::ppc
{

// A physical address.
using PAddr = uint32_t;

// Physical memory and devices on the CPU's bus.
class Bus
{
public:
    virtual ~Bus() = default;

    // The host memory of the 4 KiB page of RAM at physical address `page`, or nullptr for an I/O or unmapped address.
    // A page's host memory must stay put while the CPU runs.
    virtual uint8_t* RamPage(PAddr page) = 0;

    // A read or write of 1, 2 or 4 bytes at an I/O address. Returns false where no device responds.
    virtual bool ReadIo(PAddr address, int size, uint32_t& value) = 0;
    virtual bool WriteIo(PAddr address, int size, uint32_t value) = 0;
};

// MSR bits.
namespace msr
{

constexpr uint32_t kLe = 1u << 0;
constexpr uint32_t kRi = 1u << 1;
constexpr uint32_t kDr = 1u << 4;
constexpr uint32_t kIr = 1u << 5;
constexpr uint32_t kIp = 1u << 6;
constexpr uint32_t kFe1 = 1u << 8;
constexpr uint32_t kBe = 1u << 9;
constexpr uint32_t kSe = 1u << 10;
constexpr uint32_t kFe0 = 1u << 11;
constexpr uint32_t kMe = 1u << 12;
constexpr uint32_t kFp = 1u << 13;
constexpr uint32_t kPr = 1u << 14;
constexpr uint32_t kEe = 1u << 15;
constexpr uint32_t kIle = 1u << 16;
constexpr uint32_t kPow = 1u << 18;

} // namespace msr

// Special-purpose register numbers.
namespace spr
{

constexpr int kXer = 1;
constexpr int kLr = 8;
constexpr int kCtr = 9;
constexpr int kDsisr = 18;
constexpr int kDar = 19;
constexpr int kDec = 22;
constexpr int kSdr1 = 25;
constexpr int kSrr0 = 26;
constexpr int kSrr1 = 27;
constexpr int kTblRead = 268;
constexpr int kTbuRead = 269;
constexpr int kSprg0 = 272;
constexpr int kEar = 282;
constexpr int kTblWrite = 284;
constexpr int kTbuWrite = 285;
constexpr int kPvr = 287;
constexpr int kIbat0u = 528; // to IBAT3L at 535, then DBAT0U to DBAT3L at 536 to 543
constexpr int kIbat4u = 560; // to IBAT7L at 567, then DBAT4U to DBAT7L at 568 to 575
constexpr int kGqr0 = 912;   // to GQR7 at 919
constexpr int kHid2 = 920;
constexpr int kWpar = 921;
constexpr int kDmau = 922;
constexpr int kDmal = 923;
constexpr int kHid0 = 1008;
constexpr int kHid1 = 1009;
constexpr int kHid4 = 1011;
constexpr int kL2cr = 1017;

} // namespace spr

// A floating-point register: two slots for paired singles, each a double. Ordinary floating-point instructions use
// ps0.
struct Fpr
{
    double ps0 = 0.0;
    double ps1 = 0.0;
};

struct State
{
    std::array<uint32_t, 32> gpr{};
    std::array<Fpr, 32> fpr{};
    uint32_t pc = 0;
    uint32_t msr = 0;
    uint32_t cr = 0;
    uint32_t xer = 0;
    uint32_t lr = 0;
    uint32_t ctr = 0;
    uint32_t fpscr = 0;
    std::array<uint32_t, 16> sr{};
    std::array<uint32_t, 1024> spr{}; // the rest, by number; XER, LR, CTR, DEC and the time base are kept elsewhere
};

enum class StopReason
{
    kBudget,  // the cycles asked for have run
    kFault,   // an access that no BAT translates, or that no RAM or device responds to; see Cpu::fault_address_
    kIllegal, // an instruction that the interpreter doesn't support, at stop_pc_
};

class Cpu
{
public:
    // The CPU runs at 729 MHz, and its time base and decrementer at a quarter of the 243 MHz bus clock.
    static constexpr uint32_t kCyclesPerTick = 12;

    // The CPU's state between runs, for a snapshot of the system.
    struct Snapshot
    {
        State state;
        uint64_t cycles = 0;
        bool external = false;
        bool reservation = false;
        uint32_t reservation_address = 0;
        uint64_t tb_base = 0;
        uint64_t tb_cycle = 0;
        uint32_t dec_base = 0;
        uint64_t dec_cycle = 0;
        uint64_t dec_event = UINT64_MAX;
        bool dec_pending = false;
    };

    explicit Cpu(Bus& bus);

    // Runs for `cycles` cycles (one an instruction), or until it stops for another reason. The host's floating-point
    // rounding follows FPSCR while the CPU runs. The rounding is restored when Run returns.
    StopReason Run(uint64_t cycles);

    // The CPU's state between runs, and its restoration. Restoring the state rebuilds the address translation from the
    // BATs.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

    // The external interrupt line. The processor interface holds it while any of its interrupts is pending and enabled.
    // The interrupt is taken when the MSR allows, before the next instruction.
    void SetExternalInterrupt(bool asserted)
    {
        external_ = asserted;
        next_check_ = 0;
    }

    // Ends the current Run by cycle `cycle` at the latest, for an event that a device scheduled while the CPU ran.
    void LimitRun(uint64_t cycle)
    {
        end_ = std::min(end_, cycle);
        next_check_ = std::min(next_check_, cycle);
    }

    // Cycles run since construction.
    uint64_t Cycles() const
    {
        return cycles_;
    }

    // The 64-bit time base.
    uint64_t TimeBase() const
    {
        return tb_base_ + (cycles_ - tb_cycle_) / kCyclesPerTick;
    }

    // Rebuilds the address translation, after a change to the BATs or HID4 that didn't come from mtspr.
    void InvalidateTranslation();

    // Reads and writes through the CPU's address translation, for the system's high-level parts. They return false
    // at an address no BAT or RAM covers.
    bool ReadVirtual(uint32_t address, void* out, std::size_t size);
    bool WriteVirtual(uint32_t address, const void* data, std::size_t size);

    // A one-line summary of the registers, for diagnostics.
    std::string Describe() const;

    State state_;
    uint32_t stop_pc_ = 0;       // the instruction that stopped execution
    uint32_t stop_opcode_ = 0;   // and its encoding, for kIllegal
    uint32_t fault_address_ = 0; // the address of a kFault
    bool fault_write_ = false;

private:
    enum class Exception : uint32_t
    {
        kExternal = 0x500,
        kProgram = 0x700,
        kFpUnavailable = 0x800,
        kDecrementer = 0x900,
        kSystemCall = 0xc00,
    };

    // Each returns false to stop execution.
    bool Step(uint32_t op);
    bool Op19(uint32_t op);
    bool Op31(uint32_t op);
    bool Op59(uint32_t op);
    bool Op63(uint32_t op);
    bool Op4(uint32_t op);
    bool LoadStoreFloat(uint32_t op);
    bool PairedLoadStore(uint32_t op);
    bool Illegal(uint32_t op);

    // At the backward branch of a three-instruction loop that polls a word in memory (compares it in CR field
    // `cr_field`), runs the loop's whole iterations up to the next check for interrupts at once.
    void SkipIdleLoop(int cr_field);

    // Takes an exception: SRR0 gets `return_pc` and SRR1 the MSR (with `srr1_bits`), and execution goes to the vector.
    void TakeException(Exception exception, uint32_t return_pc, uint32_t srr1_bits = 0);

    // Checks for and takes an interrupt that the MSR allows.
    void CheckInterrupts();

    uint32_t MfSpr(int number);
    bool MtSpr(int number, uint32_t value);
    void SetMsr(uint32_t value);
    uint32_t Decrementer() const;
    void SetDecrementer(uint32_t value);

    // Address translation: a page map for the BAT-translated data and instruction spaces. The map is rebuilt when the
    // BATs change. A null entry takes the slow path, for I/O, real mode and accesses that cross pages.
    void RebuildMap(bool instruction);
    bool Translate(uint32_t address, bool instruction, PAddr& physical) const;

    uint8_t* DataPage(uint32_t address) const
    {
        return (state_.msr & msr::kDr) ? data_map_[address >> 12] : nullptr;
    }

    uint8_t Read8(uint32_t address);
    uint16_t Read16(uint32_t address);
    uint32_t Read32(uint32_t address);
    uint64_t Read64(uint32_t address);
    void Write8(uint32_t address, uint8_t value);
    void Write16(uint32_t address, uint16_t value);
    void Write32(uint32_t address, uint32_t value);
    void Write64(uint32_t address, uint64_t value);
    uint32_t ReadSlow(uint32_t address, int size);
    void WriteSlow(uint32_t address, int size, uint32_t value);
    bool Fetch(uint32_t address, uint32_t& op);
    void Fault(uint32_t address, bool write);

    // Condition register helpers.
    void SetCr0(uint32_t result);
    void SetCrField(int field, uint32_t value);

    uint32_t CrField(int field) const
    {
        return state_.cr >> (28 - field * 4) & 0xf;
    }

    bool CrBit(int bit) const
    {
        return (state_.cr >> (31 - bit) & 1) != 0;
    }

    // Floating point.
    bool FpEnabled(uint32_t op);
    void SetFprf(double value);
    void FpCompare(int field, double a, double b, bool ordered);
    void UpdateRounding();
    double RoundToSingle(double value) const;

    Bus& bus_;
    std::vector<uint8_t*> data_map_;
    std::vector<uint8_t*> fetch_map_;
    uint64_t cycles_ = 0;
    uint64_t end_ = 0;        // the cycle the current Run stops at
    uint64_t next_check_ = 0; // the next cycle to look for interrupts at
    bool external_ = false;
    bool stopped_ = false; // a Step asked Run to stop
    StopReason stop_reason_ = StopReason::kBudget;
    bool reservation_ = false; // lwarx's reservation
    uint32_t reservation_address_ = 0;

    // The time base counts from tb_base_ at tb_cycle_. The decrementer counts down from dec_base_ at dec_cycle_. Its
    // exception is pending from when it goes from 0 to -1 until it's taken.
    uint64_t tb_base_ = 0;
    uint64_t tb_cycle_ = 0;
    uint32_t dec_base_ = 0;
    uint64_t dec_cycle_ = 0;
    uint64_t dec_event_ = UINT64_MAX; // the cycle the decrementer next goes negative
    bool dec_pending_ = false;
};

} // namespace csf::ppc
