// SPDX-License-Identifier: MIT

#include "ppc/cpu.h"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/bytes.h"

namespace csf::ppc
{
namespace
{

constexpr uint32_t kPageMask = 0xfff;

// XER bits.
constexpr uint32_t kXerSo = 1u << 31;
constexpr uint32_t kXerOv = 1u << 30;
constexpr uint32_t kXerCa = 1u << 29;

// Broadway's processor version.
constexpr uint32_t kBroadwayPvr = 0x00087102;

// HID4's bit that enables BATs 4 to 7.
constexpr uint32_t kHid4Sbe = 1u << 25;

// The MSR bits that SRR1 saves and rfi restores.
constexpr uint32_t kSrr1Msr = 0x0000ff73;

// The causes of a program exception, in SRR1.
constexpr uint32_t kProgramTrap = 1u << 17;

int Rd(uint32_t op)
{
    return op >> 21 & 31;
}

int Ra(uint32_t op)
{
    return op >> 16 & 31;
}

int Rb(uint32_t op)
{
    return op >> 11 & 31;
}

int32_t Simm(uint32_t op)
{
    return static_cast<int16_t>(op & 0xffff);
}

uint32_t Uimm(uint32_t op)
{
    return op & 0xffff;
}

bool Rc(uint32_t op)
{
    return (op & 1) != 0;
}

// The mask of bits MB to ME (IBM numbering, 0 the most significant). The mask wraps round if MB > ME.
uint32_t Mask(int mb, int me)
{
    const uint32_t begin = 0xffffffffu >> mb;
    const uint32_t end = 0xffffffffu << (31 - me);

    return mb <= me ? begin & end : begin | end;
}

uint32_t Rotate(uint32_t value, int amount)
{
    return std::rotl(value, amount & 31);
}

} // namespace

Cpu::Cpu(Bus& bus) : bus_(bus), data_map_(1u << 20, nullptr), fetch_map_(1u << 20, nullptr)
{
    state_.spr[spr::kPvr] = kBroadwayPvr;
}

StopReason Cpu::Run(uint64_t cycles)
{
    const int host_rounding = std::fegetround();
    UpdateRounding();
    end_ = cycles_ + cycles;
    stopped_ = false;
    stop_reason_ = StopReason::kBudget;
    while (cycles_ < end_ && !stopped_)
    {
        // The decrementer's exception is pending once it goes negative, and again 2^32 ticks later.
        if (cycles_ >= dec_event_)
        {
            dec_pending_ = true;
            dec_event_ += (uint64_t{1} << 32) * kCyclesPerTick;
        }

        CheckInterrupts();
        next_check_ = std::min(end_, dec_event_);
        while (cycles_ < next_check_)
        {
            uint32_t op = 0;
            if (!Fetch(state_.pc, op))
            {
                break;
            }

            stop_pc_ = state_.pc;
            state_.pc += 4;
            cycles_++;
            if (!Step(op))
            {
                stopped_ = true;
                break;
            }
        }
    }

    std::fesetround(host_rounding);

    return stopped_ ? stop_reason_ : StopReason::kBudget;
}

Cpu::Snapshot Cpu::Save() const
{
    Snapshot snapshot;
    snapshot.state = state_;
    snapshot.cycles = cycles_;
    snapshot.external = external_;
    snapshot.reservation = reservation_;
    snapshot.reservation_address = reservation_address_;
    snapshot.tb_base = tb_base_;
    snapshot.tb_cycle = tb_cycle_;
    snapshot.dec_base = dec_base_;
    snapshot.dec_cycle = dec_cycle_;
    snapshot.dec_event = dec_event_;
    snapshot.dec_pending = dec_pending_;

    return snapshot;
}

void Cpu::Restore(const Snapshot& snapshot)
{
    state_ = snapshot.state;
    cycles_ = snapshot.cycles;
    external_ = snapshot.external;
    reservation_ = snapshot.reservation;
    reservation_address_ = snapshot.reservation_address;
    tb_base_ = snapshot.tb_base;
    tb_cycle_ = snapshot.tb_cycle;
    dec_base_ = snapshot.dec_base;
    dec_cycle_ = snapshot.dec_cycle;
    dec_event_ = snapshot.dec_event;
    dec_pending_ = snapshot.dec_pending;
    InvalidateTranslation();
}

void Cpu::SkipIdleLoop(int cr_field)
{
    // Some loops load a word, compare it and branch back while it's unchanged. The OS waiting for a thread to become
    // ready is an example. Such a loop only spins until an interrupt changes the word. Nothing else runs while the CPU
    // does. So the loop's whole iterations up to the next check for interrupts can go at once. The loop is then left
    // where it would have been.
    uint32_t load = 0;
    uint32_t compare = 0;
    if (!Fetch(stop_pc_ - 8, load) || !Fetch(stop_pc_ - 4, compare))
    {
        return;
    }

    const int loaded = Rd(load);
    const bool is_load = load >> 26 == 32 && Ra(load) != loaded;
    const bool is_compare = (compare >> 26 == 10 || compare >> 26 == 11) && Ra(compare) == loaded &&
                            static_cast<int>(compare >> 23 & 7) == cr_field;
    const uint32_t address = (Ra(load) ? state_.gpr[Ra(load)] : 0) + static_cast<uint32_t>(Simm(load));
    if (!is_load || !is_compare || DataPage(address) == nullptr)
    {
        return;
    }

    constexpr uint64_t kLoopLength = 3;
    if (next_check_ > cycles_)
    {
        cycles_ += (next_check_ - cycles_) / kLoopLength * kLoopLength;
    }
}

void Cpu::InvalidateTranslation()
{
    RebuildMap(false);
    RebuildMap(true);
}

bool Cpu::ReadVirtual(uint32_t address, void* out, std::size_t size)
{
    auto* bytes = static_cast<uint8_t*>(out);
    for (std::size_t i = 0; i < size; i++)
    {
        PAddr physical = 0;
        uint8_t* page = nullptr;
        if (!Translate(address + static_cast<uint32_t>(i), false, physical) ||
            (page = bus_.RamPage(physical & ~kPageMask)) == nullptr)
        {
            return false;
        }

        bytes[i] = page[physical & kPageMask];
    }

    return true;
}

bool Cpu::WriteVirtual(uint32_t address, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < size; i++)
    {
        PAddr physical = 0;
        uint8_t* page = nullptr;
        if (!Translate(address + static_cast<uint32_t>(i), false, physical) ||
            (page = bus_.RamPage(physical & ~kPageMask)) == nullptr)
        {
            return false;
        }

        page[physical & kPageMask] = bytes[i];
    }

    return true;
}

std::string Cpu::Describe() const
{
    char text[160];
    std::snprintf(text, sizeof(text), "pc %08x lr %08x msr %08x cr %08x r1 %08x r3 %08x r4 %08x r5 %08x", state_.pc,
                  state_.lr, state_.msr, state_.cr, state_.gpr[1], state_.gpr[3], state_.gpr[4], state_.gpr[5]);

    return text;
}

bool Cpu::Step(uint32_t op)
{
    State& s = state_;
    const int d = Rd(op);
    const int a = Ra(op);
    const uint32_t ra0 = a == 0 ? 0 : s.gpr[a]; // (rA|0)
    switch (op >> 26)
    {
    case 3: // twi
        {
            const auto value = static_cast<int32_t>(s.gpr[a]);
            const int32_t imm = Simm(op);
            const uint32_t to = static_cast<uint32_t>(d);
            if (((to & 16) && value < imm) || ((to & 8) && value > imm) || ((to & 4) && value == imm) ||
                ((to & 2) && static_cast<uint32_t>(value) < static_cast<uint32_t>(imm)) ||
                ((to & 1) && static_cast<uint32_t>(value) > static_cast<uint32_t>(imm)))
            {
                TakeException(Exception::kProgram, stop_pc_, kProgramTrap);
            }

            return true;
        }

    case 4:
        return Op4(op);

    case 7: // mulli
        s.gpr[d] = static_cast<uint32_t>(static_cast<int32_t>(s.gpr[a]) * Simm(op));
        return true;

    case 8: // subfic
        {
            const uint32_t imm = static_cast<uint32_t>(Simm(op));
            const uint32_t inverted = ~s.gpr[a];
            const uint64_t sum = uint64_t{inverted} + imm + 1;
            s.gpr[d] = static_cast<uint32_t>(sum);
            s.xer = (sum >> 32) ? s.xer | kXerCa : s.xer & ~kXerCa;
            return true;
        }

    case 10: // cmpli
        {
            const uint32_t value = s.gpr[a];
            const uint32_t imm = Uimm(op);
            SetCrField(d >> 2, (value < imm ? 8 : value > imm ? 4 : 2) | ((s.xer & kXerSo) ? 1 : 0));
            return true;
        }

    case 11: // cmpi
        {
            const auto value = static_cast<int32_t>(s.gpr[a]);
            const int32_t imm = Simm(op);
            SetCrField(d >> 2, (value < imm ? 8 : value > imm ? 4 : 2) | ((s.xer & kXerSo) ? 1 : 0));
            return true;
        }

    case 12: // addic
    case 13: // addic.
        {
            const uint64_t sum = uint64_t{s.gpr[a]} + static_cast<uint32_t>(Simm(op));
            s.gpr[d] = static_cast<uint32_t>(sum);
            s.xer = (sum >> 32) ? s.xer | kXerCa : s.xer & ~kXerCa;
            if (op >> 26 == 13)
            {
                SetCr0(s.gpr[d]);
            }

            return true;
        }

    case 14: // addi
        s.gpr[d] = ra0 + static_cast<uint32_t>(Simm(op));
        return true;

    case 15: // addis
        s.gpr[d] = ra0 + (Uimm(op) << 16);
        return true;

    case 16: // bc
        {
            const uint32_t bo = static_cast<uint32_t>(d);
            if (!(bo & 4))
            {
                s.ctr--;
            }

            const bool ctr_ok = (bo & 4) || ((s.ctr != 0) != ((bo & 2) != 0));
            const bool cond_ok = (bo & 16) || (CrBit(a) == ((bo & 8) != 0));
            if (op & 1)
            {
                s.lr = stop_pc_ + 4;
            }
            if (ctr_ok && cond_ok)
            {
                const auto offset = static_cast<uint32_t>(static_cast<int16_t>(op & 0xfffc));
                s.pc = (op & 2) ? offset : stop_pc_ + offset;
                if (offset == static_cast<uint32_t>(-8) && (op & 3) == 0 && (bo & 4))
                {
                    SkipIdleLoop(a >> 2);
                }
            }

            return true;
        }

    case 17: // sc
        TakeException(Exception::kSystemCall, s.pc);
        return true;

    case 18: // b
        {
            uint32_t offset = op & 0x03fffffc;
            if (offset & 0x02000000)
            {
                offset |= 0xfc000000;
            }
            if (op & 1)
            {
                s.lr = stop_pc_ + 4;
            }

            s.pc = (op & 2) ? offset : stop_pc_ + offset;
            return true;
        }

    case 19:
        return Op19(op);

    case 20: // rlwimi
        {
            const uint32_t mask = Mask(op >> 6 & 31, op >> 1 & 31);
            const uint32_t rotated = Rotate(s.gpr[d], Rb(op));
            s.gpr[a] = (rotated & mask) | (s.gpr[a] & ~mask);
            if (Rc(op))
            {
                SetCr0(s.gpr[a]);
            }

            return true;
        }

    case 21: // rlwinm
        s.gpr[a] = Rotate(s.gpr[d], Rb(op)) & Mask(op >> 6 & 31, op >> 1 & 31);
        if (Rc(op))
        {
            SetCr0(s.gpr[a]);
        }

        return true;

    case 23: // rlwnm
        s.gpr[a] = Rotate(s.gpr[d], static_cast<int>(s.gpr[Rb(op)] & 31)) & Mask(op >> 6 & 31, op >> 1 & 31);
        if (Rc(op))
        {
            SetCr0(s.gpr[a]);
        }

        return true;

    case 24: // ori
        s.gpr[a] = s.gpr[d] | Uimm(op);
        return true;

    case 25: // oris
        s.gpr[a] = s.gpr[d] | (Uimm(op) << 16);
        return true;

    case 26: // xori
        s.gpr[a] = s.gpr[d] ^ Uimm(op);
        return true;

    case 27: // xoris
        s.gpr[a] = s.gpr[d] ^ (Uimm(op) << 16);
        return true;

    case 28: // andi.
        s.gpr[a] = s.gpr[d] & Uimm(op);
        SetCr0(s.gpr[a]);
        return true;

    case 29: // andis.
        s.gpr[a] = s.gpr[d] & (Uimm(op) << 16);
        SetCr0(s.gpr[a]);
        return true;

    case 31:
        return Op31(op);

    case 32: // lwz
    case 33: // lwzu
        {
            const uint32_t address = (op >> 26 == 33 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            const uint32_t value = Read32(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            if (op >> 26 == 33)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 34: // lbz
    case 35: // lbzu
        {
            const uint32_t address = (op >> 26 == 35 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            const uint8_t value = Read8(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            if (op >> 26 == 35)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 36: // stw
    case 37: // stwu
        {
            const uint32_t address = (op >> 26 == 37 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            Write32(address, s.gpr[d]);
            if (op >> 26 == 37 && !stopped_)
            {
                s.gpr[a] = address;
            }

            return !stopped_;
        }

    case 38: // stb
    case 39: // stbu
        {
            const uint32_t address = (op >> 26 == 39 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            Write8(address, static_cast<uint8_t>(s.gpr[d]));
            if (op >> 26 == 39 && !stopped_)
            {
                s.gpr[a] = address;
            }

            return !stopped_;
        }

    case 40: // lhz
    case 41: // lhzu
        {
            const uint32_t address = (op >> 26 == 41 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            const uint16_t value = Read16(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            if (op >> 26 == 41)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 42: // lha
    case 43: // lhau
        {
            const uint32_t address = (op >> 26 == 43 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            const uint16_t value = Read16(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = static_cast<uint32_t>(static_cast<int16_t>(value));
            if (op >> 26 == 43)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 44: // sth
    case 45: // sthu
        {
            const uint32_t address = (op >> 26 == 45 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            Write16(address, static_cast<uint16_t>(s.gpr[d]));
            if (op >> 26 == 45 && !stopped_)
            {
                s.gpr[a] = address;
            }

            return !stopped_;
        }

    case 46: // lmw
        {
            uint32_t address = ra0 + static_cast<uint32_t>(Simm(op));
            for (int r = d; r < 32 && !stopped_; r++, address += 4)
            {
                const uint32_t value = Read32(address);
                if (!stopped_)
                {
                    s.gpr[r] = value;
                }
            }

            return !stopped_;
        }

    case 47: // stmw
        {
            uint32_t address = ra0 + static_cast<uint32_t>(Simm(op));
            for (int r = d; r < 32 && !stopped_; r++, address += 4)
            {
                Write32(address, s.gpr[r]);
            }

            return !stopped_;
        }

    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
        return LoadStoreFloat(op);

    case 56:
    case 57:
    case 60:
    case 61:
        return PairedLoadStore(op);

    case 59:
        return Op59(op);

    case 63:
        return Op63(op);

    default:
        return Illegal(op);
    }
}

bool Cpu::Op19(uint32_t op)
{
    State& s = state_;
    const int d = Rd(op);
    const int a = Ra(op);
    const int b = Rb(op);
    const auto cr_op = [&](auto combine)
    {
        const bool value = combine(CrBit(a), CrBit(b));
        const uint32_t bit = 1u << (31 - d);
        s.cr = value ? s.cr | bit : s.cr & ~bit;
    };

    switch (op >> 1 & 0x3ff)
    {
    case 0: // mcrf
        SetCrField(d >> 2, CrField(a >> 2));
        return true;

    case 16:  // bclr
    case 528: // bcctr
        {
            const bool to_ctr = (op >> 1 & 0x3ff) == 528;
            const uint32_t bo = static_cast<uint32_t>(d);
            if (!to_ctr && !(bo & 4))
            {
                s.ctr--;
            }

            const bool ctr_ok = to_ctr || (bo & 4) || ((s.ctr != 0) != ((bo & 2) != 0));
            const bool cond_ok = (bo & 16) || (CrBit(a) == ((bo & 8) != 0));
            const uint32_t target = (to_ctr ? s.ctr : s.lr) & ~3u;
            if (op & 1)
            {
                s.lr = stop_pc_ + 4;
            }
            if (ctr_ok && cond_ok)
            {
                s.pc = target;
            }

            return true;
        }

    case 33: // crnor
        cr_op([](bool x, bool y) { return !(x || y); });
        return true;

    case 50: // rfi
        SetMsr((s.msr & ~kSrr1Msr) | (s.spr[spr::kSrr1] & kSrr1Msr));
        s.pc = s.spr[spr::kSrr0] & ~3u;
        return true;

    case 129: // crandc
        cr_op([](bool x, bool y) { return x && !y; });
        return true;

    case 150: // isync
        return true;

    case 193: // crxor
        cr_op([](bool x, bool y) { return x != y; });
        return true;

    case 225: // crnand
        cr_op([](bool x, bool y) { return !(x && y); });
        return true;

    case 257: // crand
        cr_op([](bool x, bool y) { return x && y; });
        return true;

    case 289: // creqv
        cr_op([](bool x, bool y) { return x == y; });
        return true;

    case 417: // crorc
        cr_op([](bool x, bool y) { return x || !y; });
        return true;

    case 449: // cror
        cr_op([](bool x, bool y) { return x || y; });
        return true;

    default:
        return Illegal(op);
    }
}

bool Cpu::Op31(uint32_t op)
{
    State& s = state_;
    const int d = Rd(op);
    const int a = Ra(op);
    const int b = Rb(op);
    const uint32_t ra0 = a == 0 ? 0 : s.gpr[a];
    const uint32_t xo = op >> 1 & 0x3ff;
    const bool oe = (op & 0x400) != 0;

    // The arithmetic forms: the sum of x, y and a carry in, with the carry and overflow out.
    const auto arith = [&](uint32_t x, uint32_t y, uint32_t carry_in, bool set_carry)
    {
        const uint64_t sum = uint64_t{x} + y + carry_in;
        const auto result = static_cast<uint32_t>(sum);
        if (set_carry)
        {
            s.xer = (sum >> 32) ? s.xer | kXerCa : s.xer & ~kXerCa;
        }
        if (oe)
        {
            const bool overflow = ((x ^ result) & (y ^ result)) >> 31;
            s.xer = overflow ? s.xer | kXerOv | kXerSo : s.xer & ~kXerOv;
        }

        s.gpr[d] = result;
        if (Rc(op))
        {
            SetCr0(result);
        }

        return true;
    };

    const auto logical = [&](uint32_t result)
    {
        s.gpr[a] = result;
        if (Rc(op))
        {
            SetCr0(result);
        }

        return true;
    };

    const auto set_overflow = [&](bool overflow)
    {
        if (oe)
        {
            s.xer = overflow ? s.xer | kXerOv | kXerSo : s.xer & ~kXerOv;
        }
    };

    const uint32_t carry = (s.xer & kXerCa) ? 1 : 0;
    switch (xo)
    {
    case 0: // cmp
        {
            const auto x = static_cast<int32_t>(s.gpr[a]);
            const auto y = static_cast<int32_t>(s.gpr[b]);
            SetCrField(d >> 2, (x < y ? 8 : x > y ? 4 : 2) | ((s.xer & kXerSo) ? 1 : 0));
            return true;
        }

    case 32: // cmpl
        {
            const uint32_t x = s.gpr[a];
            const uint32_t y = s.gpr[b];
            SetCrField(d >> 2, (x < y ? 8 : x > y ? 4 : 2) | ((s.xer & kXerSo) ? 1 : 0));
            return true;
        }

    case 4: // tw
        {
            const auto x = static_cast<int32_t>(s.gpr[a]);
            const auto y = static_cast<int32_t>(s.gpr[b]);
            const uint32_t to = static_cast<uint32_t>(d);
            if (((to & 16) && x < y) || ((to & 8) && x > y) || ((to & 4) && x == y) ||
                ((to & 2) && static_cast<uint32_t>(x) < static_cast<uint32_t>(y)) ||
                ((to & 1) && static_cast<uint32_t>(x) > static_cast<uint32_t>(y)))
            {
                TakeException(Exception::kProgram, stop_pc_, kProgramTrap);
            }

            return true;
        }

    case 8: // subfc
    case 520:
        return arith(~s.gpr[a], s.gpr[b], 1, true);

    case 10: // addc
    case 522:
        return arith(s.gpr[a], s.gpr[b], 0, true);

    case 40: // subf
    case 552:
        return arith(~s.gpr[a], s.gpr[b], 1, false);

    case 104: // neg
    case 616:
        return arith(~s.gpr[a], 0, 1, false);

    case 136: // subfe
    case 648:
        return arith(~s.gpr[a], s.gpr[b], carry, true);

    case 138: // adde
    case 650:
        return arith(s.gpr[a], s.gpr[b], carry, true);

    case 200: // subfze
    case 712:
        return arith(~s.gpr[a], 0, carry, true);

    case 202: // addze
    case 714:
        return arith(s.gpr[a], 0, carry, true);

    case 232: // subfme
    case 744:
        return arith(~s.gpr[a], 0xffffffff, carry, true);

    case 234: // addme
    case 746:
        return arith(s.gpr[a], 0xffffffff, carry, true);

    case 266: // add
    case 778:
        return arith(s.gpr[a], s.gpr[b], 0, false);

    case 11: // mulhwu
        s.gpr[d] = static_cast<uint32_t>((uint64_t{s.gpr[a]} * s.gpr[b]) >> 32);
        if (Rc(op))
        {
            SetCr0(s.gpr[d]);
        }

        return true;

    case 75: // mulhw
        s.gpr[d] = static_cast<uint32_t>(
            static_cast<uint64_t>(int64_t{static_cast<int32_t>(s.gpr[a])} * static_cast<int32_t>(s.gpr[b])) >> 32);
        if (Rc(op))
        {
            SetCr0(s.gpr[d]);
        }

        return true;

    case 235: // mullw
    case 747:
        {
            const int64_t product = int64_t{static_cast<int32_t>(s.gpr[a])} * static_cast<int32_t>(s.gpr[b]);
            s.gpr[d] = static_cast<uint32_t>(product);
            set_overflow(product != static_cast<int32_t>(product));
            if (Rc(op))
            {
                SetCr0(s.gpr[d]);
            }

            return true;
        }

    case 491: // divw
    case 1003:
        {
            // The manual leaves a division by 0, and 0x80000000 / -1, undefined. The result here is rA's sign.
            const auto x = static_cast<int32_t>(s.gpr[a]);
            const auto y = static_cast<int32_t>(s.gpr[b]);
            const bool overflow = y == 0 || (x == INT32_MIN && y == -1);
            s.gpr[d] = overflow ? (x < 0 ? 0xffffffff : 0) : static_cast<uint32_t>(x / y);
            set_overflow(overflow);
            if (Rc(op))
            {
                SetCr0(s.gpr[d]);
            }

            return true;
        }

    case 459: // divwu
    case 971:
        s.gpr[d] = s.gpr[b] == 0 ? 0 : s.gpr[a] / s.gpr[b];
        set_overflow(s.gpr[b] == 0);
        if (Rc(op))
        {
            SetCr0(s.gpr[d]);
        }

        return true;

    case 19: // mfcr
        s.gpr[d] = s.cr;
        return true;

    case 144: // mtcrf
        {
            const uint32_t fields = op >> 12 & 0xff;
            uint32_t mask = 0;
            for (int i = 0; i < 8; i++)
            {
                if (fields & (0x80 >> i))
                {
                    mask |= 0xf0000000u >> (i * 4);
                }
            }

            s.cr = (s.cr & ~mask) | (s.gpr[d] & mask);
            return true;
        }

    case 512: // mcrxr
        SetCrField(d >> 2, s.xer >> 28);
        s.xer &= ~(kXerSo | kXerOv | kXerCa);
        return true;

    case 83: // mfmsr
        s.gpr[d] = s.msr;
        return true;

    case 146: // mtmsr
        SetMsr(s.gpr[d]);
        return true;

    case 339: // mfspr
    case 371: // mftb
        s.gpr[d] = MfSpr(static_cast<int>((op >> 16 & 31) | (op >> 6 & 0x3e0)));
        return true;

    case 467: // mtspr
        return MtSpr(static_cast<int>((op >> 16 & 31) | (op >> 6 & 0x3e0)), s.gpr[d]);

    case 595: // mfsr
        s.gpr[d] = s.sr[a & 15];
        return true;

    case 659: // mfsrin
        s.gpr[d] = s.sr[s.gpr[b] >> 28];
        return true;

    case 210: // mtsr
        s.sr[a & 15] = s.gpr[d];
        return true;

    case 242: // mtsrin
        s.sr[s.gpr[b] >> 28] = s.gpr[d];
        return true;

    case 24: // slw
        {
            const uint32_t amount = s.gpr[b] & 0x3f;
            return logical(amount >= 32 ? 0 : s.gpr[d] << amount);
        }

    case 536: // srw
        {
            const uint32_t amount = s.gpr[b] & 0x3f;
            return logical(amount >= 32 ? 0 : s.gpr[d] >> amount);
        }

    case 792: // sraw
    case 824: // srawi
        {
            // The carry is set when a negative value loses 1 bits.
            const uint32_t amount = xo == 824 ? static_cast<uint32_t>(b) : s.gpr[b] & 0x3f;
            const auto value = static_cast<int32_t>(s.gpr[d]);
            uint32_t result = 0;
            bool carry_out = false;
            if (amount >= 32)
            {
                result = value < 0 ? 0xffffffff : 0;
                carry_out = value < 0;
            }
            else
            {
                result = static_cast<uint32_t>(value >> amount);
                carry_out = value < 0 && amount > 0 && (static_cast<uint32_t>(value) << (32 - amount)) != 0;
            }

            s.xer = carry_out ? s.xer | kXerCa : s.xer & ~kXerCa;
            return logical(result);
        }

    case 26: // cntlzw
        return logical(static_cast<uint32_t>(std::countl_zero(s.gpr[d])));

    case 28: // and
        return logical(s.gpr[d] & s.gpr[b]);

    case 60: // andc
        return logical(s.gpr[d] & ~s.gpr[b]);

    case 124: // nor
        return logical(~(s.gpr[d] | s.gpr[b]));

    case 284: // eqv
        return logical(~(s.gpr[d] ^ s.gpr[b]));

    case 316: // xor
        return logical(s.gpr[d] ^ s.gpr[b]);

    case 412: // orc
        return logical(s.gpr[d] | ~s.gpr[b]);

    case 444: // or
        return logical(s.gpr[d] | s.gpr[b]);

    case 476: // nand
        return logical(~(s.gpr[d] & s.gpr[b]));

    case 922: // extsh
        return logical(static_cast<uint32_t>(static_cast<int16_t>(s.gpr[d])));

    case 954: // extsb
        return logical(static_cast<uint32_t>(static_cast<int8_t>(s.gpr[d])));

    case 20: // lwarx
        {
            const uint32_t address = ra0 + s.gpr[b];
            const uint32_t value = Read32(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            reservation_ = true;
            reservation_address_ = address;
            return true;
        }

    case 150: // stwcx.
        {
            const uint32_t address = ra0 + s.gpr[b];
            const bool stored = reservation_ && reservation_address_ == address;
            if (stored)
            {
                Write32(address, s.gpr[d]);
            }

            reservation_ = false;
            SetCrField(0, (stored ? 2 : 0) | ((s.xer & kXerSo) ? 1 : 0));
            return !stopped_;
        }

    case 23:  // lwzx
    case 55:  // lwzux
    case 87:  // lbzx
    case 119: // lbzux
    case 279: // lhzx
    case 311: // lhzux
    case 343: // lhax
    case 375: // lhaux
    case 534: // lwbrx
    case 790: // lhbrx
        {
            const bool update = xo == 55 || xo == 119 || xo == 311 || xo == 375;
            const uint32_t address = (update ? s.gpr[a] : ra0) + s.gpr[b];
            uint32_t value = 0;
            switch (xo)
            {
            case 23:
            case 55:
                value = Read32(address);
                break;
            case 87:
            case 119:
                value = Read8(address);
                break;
            case 279:
            case 311:
                value = Read16(address);
                break;
            case 343:
            case 375:
                value = static_cast<uint32_t>(static_cast<int16_t>(Read16(address)));
                break;
            case 534:
                value = ByteSwap32(Read32(address));
                break;
            default:
                value = ByteSwap16(Read16(address));
                break;
            }

            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            if (update)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 151: // stwx
    case 183: // stwux
    case 215: // stbx
    case 247: // stbux
    case 407: // sthx
    case 439: // sthux
    case 662: // stwbrx
    case 918: // sthbrx
        {
            const bool update = xo == 183 || xo == 247 || xo == 439;
            const uint32_t address = (update ? s.gpr[a] : ra0) + s.gpr[b];
            switch (xo)
            {
            case 151:
            case 183:
                Write32(address, s.gpr[d]);
                break;
            case 215:
            case 247:
                Write8(address, static_cast<uint8_t>(s.gpr[d]));
                break;
            case 407:
            case 439:
                Write16(address, static_cast<uint16_t>(s.gpr[d]));
                break;
            case 662:
                Write32(address, ByteSwap32(s.gpr[d]));
                break;
            default:
                Write16(address, ByteSwap16(static_cast<uint16_t>(s.gpr[d])));
                break;
            }

            if (update && !stopped_)
            {
                s.gpr[a] = address;
            }

            return !stopped_;
        }

    case 533: // lswx
    case 597: // lswi
        {
            // Bytes go into rD and the following registers, four to a register, most significant first. The registers
            // wrap from r31 to r0.
            uint32_t address = xo == 597 ? ra0 : ra0 + s.gpr[b];
            uint32_t count = xo == 597 ? (b == 0 ? 32 : static_cast<uint32_t>(b)) : s.xer & 0x7f;
            int r = d - 1;
            for (uint32_t i = 0; i < count && !stopped_; i++, address++)
            {
                if (i % 4 == 0)
                {
                    r = (r + 1) % 32;
                    s.gpr[r] = 0;
                }

                s.gpr[r] |= uint32_t{Read8(address)} << (24 - (i % 4) * 8);
            }

            return !stopped_;
        }

    case 661: // stswx
    case 725: // stswi
        {
            uint32_t address = xo == 725 ? ra0 : ra0 + s.gpr[b];
            uint32_t count = xo == 725 ? (b == 0 ? 32 : static_cast<uint32_t>(b)) : s.xer & 0x7f;
            int r = d - 1;
            for (uint32_t i = 0; i < count && !stopped_; i++, address++)
            {
                if (i % 4 == 0)
                {
                    r = (r + 1) % 32;
                }

                Write8(address, static_cast<uint8_t>(s.gpr[r] >> (24 - (i % 4) * 8)));
            }

            return !stopped_;
        }

    case 1014: // dcbz
        {
            const uint32_t address = (ra0 + s.gpr[b]) & ~31u;
            for (uint32_t i = 0; i < 32 && !stopped_; i += 4)
            {
                Write32(address + i, 0);
            }

            return !stopped_;
        }

    case 54:  // dcbst
    case 86:  // dcbf
    case 246: // dcbtst
    case 278: // dcbt
    case 306: // tlbie
    case 470: // dcbi
    case 566: // tlbsync
    case 598: // sync
    case 854: // eieio
    case 982: // icbi
        return true;

    case 535: // lfsx
    case 567: // lfsux
    case 599: // lfdx
    case 631: // lfdux
    case 663: // stfsx
    case 695: // stfsux
    case 727: // stfdx
    case 759: // stfdux
    case 983: // stfiwx
        return LoadStoreFloat(op);

    case 6:  // psq_lx
    case 7:  // psq_stx
    case 38: // psq_lux
    case 39: // psq_stux
        return PairedLoadStore(op);

    default:
        // The paired-single indexed forms decode on 6 bits.
        switch (op >> 1 & 0x3f)
        {
        case 6:
        case 7:
        case 38:
        case 39:
            return PairedLoadStore(op);
        default:
            return Illegal(op);
        }
    }
}

bool Cpu::Illegal(uint32_t op)
{
    stop_opcode_ = op;
    stop_reason_ = StopReason::kIllegal;
    state_.pc = stop_pc_;

    return false;
}

void Cpu::TakeException(Exception exception, uint32_t return_pc, uint32_t srr1_bits)
{
    // The handler runs in real mode, with interrupts, floating point and the rest off. IP selects the vectors' base.
    State& s = state_;
    s.spr[spr::kSrr0] = return_pc;
    s.spr[spr::kSrr1] = (s.msr & kSrr1Msr) | srr1_bits;
    const uint32_t base = (s.msr & msr::kIp) ? 0xfff00000 : 0;
    uint32_t new_msr = s.msr & (msr::kIp | msr::kMe | msr::kIle);
    if (s.msr & msr::kIle)
    {
        new_msr |= msr::kLe;
    }

    SetMsr(new_msr);
    s.pc = base | static_cast<uint32_t>(exception);
}

void Cpu::CheckInterrupts()
{
    if (!(state_.msr & msr::kEe))
    {
        return;
    }

    if (external_)
    {
        TakeException(Exception::kExternal, state_.pc);
    }
    else if (dec_pending_)
    {
        dec_pending_ = false;
        TakeException(Exception::kDecrementer, state_.pc);
    }
}

uint32_t Cpu::MfSpr(int number)
{
    const State& s = state_;
    switch (number)
    {
    case spr::kXer:
        return s.xer;
    case spr::kLr:
        return s.lr;
    case spr::kCtr:
        return s.ctr;
    case spr::kDec:
        return Decrementer();
    case spr::kTblRead:
        return static_cast<uint32_t>(TimeBase());
    case spr::kTbuRead:
        return static_cast<uint32_t>(TimeBase() >> 32);
    default:
        return s.spr[number & 0x3ff];
    }
}

bool Cpu::MtSpr(int number, uint32_t value)
{
    State& s = state_;
    switch (number)
    {
    case spr::kXer:
        s.xer = value;
        return true;
    case spr::kLr:
        s.lr = value;
        return true;
    case spr::kCtr:
        s.ctr = value;
        return true;
    case spr::kDec:
        SetDecrementer(value);
        return true;
    case spr::kTblWrite:
        tb_base_ = (TimeBase() & 0xffffffff00000000) | value;
        tb_cycle_ = cycles_;
        return true;
    case spr::kTbuWrite:
        tb_base_ = (TimeBase() & 0xffffffff) | (uint64_t{value} << 32);
        tb_cycle_ = cycles_;
        return true;
    case spr::kPvr:
        return true;
    default:
        break;
    }

    s.spr[number & 0x3ff] = value;
    if ((number >= spr::kIbat0u && number < spr::kIbat0u + 16) ||
        (number >= spr::kIbat4u && number < spr::kIbat4u + 16) || number == spr::kHid4)
    {
        InvalidateTranslation();
    }

    return true;
}

void Cpu::SetMsr(uint32_t value)
{
    state_.msr = value;

    // A change may enable an interrupt that's waiting.
    next_check_ = 0;
}

uint32_t Cpu::Decrementer() const
{
    return dec_base_ - static_cast<uint32_t>((cycles_ - dec_cycle_) / kCyclesPerTick);
}

void Cpu::SetDecrementer(uint32_t value)
{
    // The exception comes when the count goes from 0 to -1: value + 1 ticks from now for a positive value, and only
    // once it has wrapped for a negative one.
    dec_base_ = value;
    dec_cycle_ = cycles_;
    const uint64_t ticks = uint64_t{value} + 1;
    dec_event_ = cycles_ + ticks * kCyclesPerTick;
    next_check_ = 0;
}

void Cpu::RebuildMap(bool instruction)
{
    // The BATs that are valid in supervisor mode. The lowest numbered comes last. It then wins where two overlap.
    std::vector<uint8_t*>& map = instruction ? fetch_map_ : data_map_;
    std::fill(map.begin(), map.end(), nullptr);
    const int count = (state_.spr[spr::kHid4] & kHid4Sbe) ? 8 : 4;
    for (int i = count - 1; i >= 0; i--)
    {
        const int base = (i < 4 ? spr::kIbat0u + i * 2 : spr::kIbat4u + (i - 4) * 2) + (instruction ? 0 : 8);
        const uint32_t upper = state_.spr[base];
        const uint32_t lower = state_.spr[base + 1];
        if (!(upper & 2))
        {
            continue;
        }

        const uint32_t length = ((upper >> 2 & 0x7ff) + 1) << 17;
        const uint32_t effective = upper & 0xfffe0000 & ~(length - 1);
        const uint32_t physical = lower & 0xfffe0000 & ~(length - 1);
        for (uint32_t offset = 0; offset < length; offset += 0x1000)
        {
            map[(effective + offset) >> 12] = bus_.RamPage(physical + offset);
        }
    }
}

bool Cpu::Translate(uint32_t address, bool instruction, PAddr& physical) const
{
    if (!(state_.msr & (instruction ? msr::kIr : msr::kDr)))
    {
        physical = address;
        return true;
    }

    const int count = (state_.spr[spr::kHid4] & kHid4Sbe) ? 8 : 4;
    for (int i = 0; i < count; i++)
    {
        const int base = (i < 4 ? spr::kIbat0u + i * 2 : spr::kIbat4u + (i - 4) * 2) + (instruction ? 0 : 8);
        const uint32_t upper = state_.spr[base];
        const uint32_t lower = state_.spr[base + 1];
        const uint32_t mask = ~((((upper >> 2 & 0x7ff) + 1) << 17) - 1);
        if ((upper & 2) && (address & mask) == (upper & 0xfffe0000 & mask))
        {
            physical = (lower & 0xfffe0000 & mask) | (address & ~mask);
            return true;
        }
    }

    return false;
}

uint8_t Cpu::Read8(uint32_t address)
{
    if (const uint8_t* page = DataPage(address))
    {
        return page[address & kPageMask];
    }

    return static_cast<uint8_t>(ReadSlow(address, 1));
}

uint16_t Cpu::Read16(uint32_t address)
{
    const uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xffe)
    {
        return LoadBe16(page + (address & kPageMask));
    }

    return static_cast<uint16_t>(ReadSlow(address, 2));
}

uint32_t Cpu::Read32(uint32_t address)
{
    const uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xffc)
    {
        return LoadBe32(page + (address & kPageMask));
    }

    return ReadSlow(address, 4);
}

uint64_t Cpu::Read64(uint32_t address)
{
    const uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xff8)
    {
        return LoadBe64(page + (address & kPageMask));
    }

    const uint64_t high = Read32(address);

    return high << 32 | Read32(address + 4);
}

void Cpu::Write8(uint32_t address, uint8_t value)
{
    if (uint8_t* page = DataPage(address))
    {
        page[address & kPageMask] = value;
        return;
    }

    WriteSlow(address, 1, value);
}

void Cpu::Write16(uint32_t address, uint16_t value)
{
    uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xffe)
    {
        StoreBe16(page + (address & kPageMask), value);
        return;
    }

    WriteSlow(address, 2, value);
}

void Cpu::Write32(uint32_t address, uint32_t value)
{
    uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xffc)
    {
        StoreBe32(page + (address & kPageMask), value);
        return;
    }

    WriteSlow(address, 4, value);
}

void Cpu::Write64(uint32_t address, uint64_t value)
{
    uint8_t* page = DataPage(address);
    if (page && (address & kPageMask) <= 0xff8)
    {
        StoreBe64(page + (address & kPageMask), value);
        return;
    }

    Write32(address, static_cast<uint32_t>(value >> 32));
    Write32(address + 4, static_cast<uint32_t>(value));
}

uint32_t Cpu::ReadSlow(uint32_t address, int size)
{
    // An access that crosses a page goes a byte at a time.
    if ((address & kPageMask) + static_cast<uint32_t>(size) > 0x1000)
    {
        uint32_t value = 0;
        for (int i = 0; i < size; i++)
        {
            value = value << 8 | ReadSlow(address + static_cast<uint32_t>(i), 1);
        }

        return value;
    }

    PAddr physical = 0;
    if (!Translate(address, false, physical))
    {
        Fault(address, false);
        return 0;
    }

    if (const uint8_t* page = bus_.RamPage(physical & ~kPageMask))
    {
        const uint8_t* p = page + (physical & kPageMask);
        return size == 1 ? *p : size == 2 ? LoadBe16(p) : LoadBe32(p);
    }

    uint32_t value = 0;
    if (!bus_.ReadIo(physical, size, value))
    {
        Fault(address, false);
        return 0;
    }

    return value;
}

void Cpu::WriteSlow(uint32_t address, int size, uint32_t value)
{
    if ((address & kPageMask) + static_cast<uint32_t>(size) > 0x1000)
    {
        for (int i = 0; i < size; i++)
        {
            WriteSlow(address + static_cast<uint32_t>(i), 1, value >> ((size - 1 - i) * 8) & 0xff);
        }

        return;
    }

    PAddr physical = 0;
    if (!Translate(address, false, physical))
    {
        Fault(address, true);
        return;
    }

    if (uint8_t* page = bus_.RamPage(physical & ~kPageMask))
    {
        uint8_t* p = page + (physical & kPageMask);
        if (size == 1)
        {
            *p = static_cast<uint8_t>(value);
        }
        else if (size == 2)
        {
            StoreBe16(p, static_cast<uint16_t>(value));
        }
        else
        {
            StoreBe32(p, value);
        }

        return;
    }

    if (!bus_.WriteIo(physical, size, value))
    {
        Fault(address, true);
    }
}

bool Cpu::Fetch(uint32_t address, uint32_t& op)
{
    if (state_.msr & msr::kIr)
    {
        if (const uint8_t* page = fetch_map_[address >> 12])
        {
            op = LoadBe32(page + (address & kPageMask));
            return true;
        }
    }

    PAddr physical = 0;
    const uint8_t* page = nullptr;
    if (!Translate(address, true, physical) || (page = bus_.RamPage(physical & ~kPageMask)) == nullptr)
    {
        stop_pc_ = address;
        fault_address_ = address;
        fault_write_ = false;
        stop_reason_ = StopReason::kFault;
        stopped_ = true;
        return false;
    }

    op = LoadBe32(page + (physical & kPageMask));
    return true;
}

void Cpu::Fault(uint32_t address, bool write)
{
    if (!stopped_)
    {
        fault_address_ = address;
        fault_write_ = write;
        stop_reason_ = StopReason::kFault;
        stopped_ = true;
        state_.pc = stop_pc_;
    }
}

void Cpu::SetCr0(uint32_t result)
{
    const auto value = static_cast<int32_t>(result);
    SetCrField(0, (value < 0 ? 8 : value > 0 ? 4 : 2) | ((state_.xer & kXerSo) ? 1 : 0));
}

void Cpu::SetCrField(int field, uint32_t value)
{
    const int shift = 28 - field * 4;
    state_.cr = (state_.cr & ~(0xfu << shift)) | ((value & 0xf) << shift);
}

} // namespace csf::ppc
