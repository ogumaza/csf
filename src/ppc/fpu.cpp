// SPDX-License-Identifier: MIT

// Floating-point and paired-single instructions. Arithmetic runs on the host's IEEE double precision. The host's
// rounding mode follows FPSCR[RN] (this file is built with -frounding-math). Single-precision results are rounded from
// the double result. Conversions between the single and double formats in loads and stores work on the bits. The
// architecture defines them that way. No NaN is quietened on the way.
//
// Not modelled: FPSCR's exception and status bits other than FPCC (Rc forms copy what FPSCR holds into CR1), the 750's
// handling of single-precision operations on operands that aren't single-precision values, and the exact estimates of
// fres, frsqrte, ps_res and ps_rsqrte. Those give the correctly rounded result instead.

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>

#include "common/bytes.h"
#include "common/fused.h"
#include "ppc/cpu.h"

namespace csf::ppc
{
namespace
{

// HID2's paired-single enable.
constexpr uint32_t kHid2Pse = 1u << 29;

int Fd(uint32_t op)
{
    return op >> 21 & 31;
}

int Fa(uint32_t op)
{
    return op >> 16 & 31;
}

int Fb(uint32_t op)
{
    return op >> 11 & 31;
}

int Fc(uint32_t op)
{
    return op >> 6 & 31;
}

// A single-precision value's bits as a double, without changing a NaN (the architecture's load conversion).
double SingleToDouble(uint32_t bits)
{
    const uint64_t sign = uint64_t{bits >> 31} << 63;
    const uint32_t exponent = bits >> 23 & 0xff;
    uint64_t mantissa = bits & 0x7fffff;
    if (exponent == 0xff)
    {
        return std::bit_cast<double>(sign | uint64_t{0x7ff} << 52 | mantissa << 29);
    }
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            return std::bit_cast<double>(sign);
        }

        // A denormal becomes a normal double.
        int64_t e = -126;
        while (!(mantissa & 0x800000))
        {
            mantissa <<= 1;
            e--;
        }

        return std::bit_cast<double>(sign | static_cast<uint64_t>(e + 1023) << 52 | (mantissa & 0x7fffff) << 29);
    }

    return std::bit_cast<double>(sign | uint64_t{exponent - 127 + 1023} << 52 | mantissa << 29);
}

// A double's bits as a single. stfs stores them this way: no rounding, just the bits a single can hold, denormalised
// below the single range (the architecture's store conversion).
uint32_t DoubleToSingle(double value)
{
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    const uint32_t exponent = static_cast<uint32_t>(bits >> 52 & 0x7ff);
    if (exponent > 896 || (bits & 0x7fffffffffffffff) == 0 || exponent == 0x7ff)
    {
        return static_cast<uint32_t>(bits >> 32 & 0xc0000000) | static_cast<uint32_t>(bits >> 29 & 0x3fffffff);
    }
    if (exponent >= 874)
    {
        const uint64_t mantissa = (bits & 0xfffffffffffff) | (uint64_t{1} << 52);
        const int shift = static_cast<int>(897 - exponent);

        return static_cast<uint32_t>(bits >> 32 & 0x80000000) | static_cast<uint32_t>((mantissa >> shift) >> 29);
    }

    return static_cast<uint32_t>(bits >> 32 & 0x80000000);
}

// fctiw and fctiwz: a double as a 32-bit integer, saturating, with NaN as the most negative.
uint32_t ConvertToInt(double value, bool truncate)
{
    if (std::isnan(value))
    {
        return 0x80000000;
    }

    const double rounded = truncate ? std::trunc(value) : std::nearbyint(value);
    if (rounded >= 2147483648.0)
    {
        return 0x7fffffff;
    }
    if (rounded < -2147483648.0)
    {
        return 0x80000000;
    }

    return static_cast<uint32_t>(static_cast<int32_t>(rounded));
}

double Negate(double value)
{
    return std::bit_cast<double>(std::bit_cast<uint64_t>(value) ^ 0x8000000000000000);
}

double Abs(double value)
{
    return std::bit_cast<double>(std::bit_cast<uint64_t>(value) & 0x7fffffffffffffff);
}

// A paired-single quantised value: the type and scale of a GQR's load or store half.
struct Quantization
{
    int type = 0; // 0 float, 4 u8, 5 u16, 6 s8, 7 s16
    int scale = 0;
};

int Size(int type)
{
    return type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
}

double Dequantize(uint32_t raw, const Quantization& q)
{
    double value = 0.0;
    switch (q.type)
    {
    case 4:
        value = static_cast<uint8_t>(raw);
        break;
    case 5:
        value = static_cast<uint16_t>(raw);
        break;
    case 6:
        value = static_cast<int8_t>(raw);
        break;
    case 7:
        value = static_cast<int16_t>(raw);
        break;
    default:
        return SingleToDouble(raw);
    }

    return static_cast<double>(static_cast<float>(std::ldexp(value, -q.scale)));
}

uint32_t Quantize(double value, const Quantization& q)
{
    if (q.type < 4)
    {
        return DoubleToSingle(value);
    }

    const double scaled = std::trunc(std::ldexp(static_cast<double>(static_cast<float>(value)), q.scale));
    const auto clamp = [scaled](double low, double high)
    {
        return std::isnan(scaled) ? 0.0 : scaled < low ? low : scaled > high ? high : scaled;
    };

    switch (q.type)
    {
    case 4:
        return static_cast<uint32_t>(clamp(0, 255));
    case 5:
        return static_cast<uint32_t>(clamp(0, 65535));
    case 6:
        return static_cast<uint32_t>(static_cast<int32_t>(clamp(-128, 127))) & 0xff;
    default:
        return static_cast<uint32_t>(static_cast<int32_t>(clamp(-32768, 32767))) & 0xffff;
    }
}

Quantization LoadQuantization(uint32_t gqr)
{
    const int scale = static_cast<int>(gqr >> 24 & 0x3f);

    return {static_cast<int>(gqr >> 16 & 7), scale >= 32 ? scale - 64 : scale};
}

Quantization StoreQuantization(uint32_t gqr)
{
    const int scale = static_cast<int>(gqr >> 8 & 0x3f);

    return {static_cast<int>(gqr & 7), scale >= 32 ? scale - 64 : scale};
}

} // namespace

bool Cpu::FpEnabled(uint32_t op)
{
    (void)op;
    if (state_.msr & msr::kFp)
    {
        return true;
    }

    TakeException(Exception::kFpUnavailable, stop_pc_);

    return false;
}

void Cpu::SetFprf(double value)
{
    // FPSCR's result flags (C and FPCC): class and sign.
    uint32_t flags = 0;
    switch (std::fpclassify(value))
    {
    case FP_NAN:
        flags = 0x11;
        break;
    case FP_INFINITE:
        flags = std::signbit(value) ? 0x9 : 0x5;
        break;
    case FP_ZERO:
        flags = std::signbit(value) ? 0x12 : 0x2;
        break;
    case FP_SUBNORMAL:
        flags = std::signbit(value) ? 0x18 : 0x14;
        break;
    default:
        flags = std::signbit(value) ? 0x8 : 0x4;
        break;
    }

    state_.fpscr = (state_.fpscr & ~0x1f000u) | flags << 12;
}

void Cpu::FpCompare(int field, double a, double b, bool ordered)
{
    (void)ordered;
    const uint32_t result = std::isnan(a) || std::isnan(b) ? 1 : a < b ? 8 : a > b ? 4 : 2;
    SetCrField(field, result);
    state_.fpscr = (state_.fpscr & ~0xf000u) | result << 12;
}

void Cpu::UpdateRounding()
{
    static constexpr int kModes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    std::fesetround(kModes[state_.fpscr & 3]);
}

double Cpu::RoundToSingle(double value) const
{
    return static_cast<double>(static_cast<float>(value));
}

bool Cpu::LoadStoreFloat(uint32_t op)
{
    if (!FpEnabled(op))
    {
        return true;
    }

    State& s = state_;
    const int d = Fd(op);
    const int a = Fa(op);
    const uint32_t ra0 = a == 0 ? 0 : s.gpr[a];
    uint32_t primary = op >> 26;
    bool update = false;
    uint32_t address = 0;
    if (primary == 31)
    {
        // The indexed forms, mapped onto the immediate ones: lfsx 535 to lfs 48, and so on.
        const uint32_t xo = op >> 1 & 0x3ff;
        if (xo == 983) // stfiwx
        {
            const uint64_t bits = std::bit_cast<uint64_t>(s.fpr[d].ps0);
            Write32(ra0 + s.gpr[Fb(op)], static_cast<uint32_t>(bits));

            return !stopped_;
        }

        primary = 48 + (xo - 535) / 32;
        update = (primary & 1) != 0;
        address = (update ? s.gpr[a] : ra0) + s.gpr[Fb(op)];
    }
    else
    {
        update = (primary & 1) != 0;
        address = (update ? s.gpr[a] : ra0) + static_cast<uint32_t>(static_cast<int16_t>(op & 0xffff));
    }

    switch (primary)
    {
    case 48: // lfs
    case 49:
        {
            const uint32_t bits = Read32(address);
            if (stopped_)
            {
                return false;
            }

            s.fpr[d].ps0 = SingleToDouble(bits);
            s.fpr[d].ps1 = s.fpr[d].ps0;
            break;
        }

    case 50: // lfd
    case 51:
        {
            const uint64_t bits = Read64(address);
            if (stopped_)
            {
                return false;
            }

            s.fpr[d].ps0 = std::bit_cast<double>(bits);
            break;
        }

    case 52: // stfs
    case 53:
        Write32(address, DoubleToSingle(s.fpr[d].ps0));
        break;

    default: // stfd
        Write64(address, std::bit_cast<uint64_t>(s.fpr[d].ps0));
        break;
    }

    if (stopped_)
    {
        return false;
    }
    if (update)
    {
        s.gpr[a] = address;
    }

    return true;
}

bool Cpu::PairedLoadStore(uint32_t op)
{
    State& s = state_;
    if (!(s.spr[spr::kHid2] & kHid2Pse))
    {
        return Illegal(op);
    }
    if (!FpEnabled(op))
    {
        return true;
    }

    // psq_l, psq_lu, psq_st and psq_stu take a 12-bit offset; the indexed forms take rB.
    const int d = Fd(op);
    const int a = Fa(op);
    const uint32_t ra0 = a == 0 ? 0 : s.gpr[a];
    bool load = false;
    bool update = false;
    bool single = false;
    int index = 0;
    uint32_t address = 0;
    if (op >> 26 == 4)
    {
        const uint32_t xo = op >> 1 & 0x3f;
        load = xo == 6 || xo == 38;
        update = xo >= 38;
        single = (op >> 10 & 1) != 0;
        index = static_cast<int>(op >> 7 & 7);
        address = (update ? s.gpr[a] : ra0) + s.gpr[Fb(op)];
    }
    else
    {
        const uint32_t primary = op >> 26;
        load = primary == 56 || primary == 57;
        update = primary == 57 || primary == 61;
        single = (op >> 15 & 1) != 0;
        index = static_cast<int>(op >> 12 & 7);
        int32_t offset = static_cast<int32_t>(op & 0xfff);
        if (offset & 0x800)
        {
            offset -= 0x1000;
        }

        address = (update ? s.gpr[a] : ra0) + static_cast<uint32_t>(offset);
    }

    const uint32_t gqr = s.spr[spr::kGqr0 + index];
    if (load)
    {
        const Quantization q = LoadQuantization(gqr);
        const int size = Size(q.type);
        const auto read = [&](uint32_t at)
        {
            return size == 1 ? uint32_t{Read8(at)} : size == 2 ? uint32_t{Read16(at)} : Read32(at);
        };

        const double ps0 = Dequantize(read(address), q);
        const double ps1 = single ? 1.0 : Dequantize(read(address + static_cast<uint32_t>(size)), q);
        if (stopped_)
        {
            return false;
        }

        s.fpr[d].ps0 = ps0;
        s.fpr[d].ps1 = ps1;
    }
    else
    {
        const Quantization q = StoreQuantization(gqr);
        const int size = Size(q.type);
        const auto write = [&](uint32_t at, double value)
        {
            const uint32_t raw = Quantize(value, q);
            if (size == 1)
            {
                Write8(at, static_cast<uint8_t>(raw));
            }
            else if (size == 2)
            {
                Write16(at, static_cast<uint16_t>(raw));
            }
            else
            {
                Write32(at, raw);
            }
        };

        write(address, s.fpr[d].ps0);
        if (!single)
        {
            write(address + static_cast<uint32_t>(size), s.fpr[d].ps1);
        }
        if (stopped_)
        {
            return false;
        }
    }

    if (update)
    {
        s.gpr[a] = address;
    }

    return true;
}

bool Cpu::Op59(uint32_t op)
{
    if (!FpEnabled(op))
    {
        return true;
    }

    // Single precision: the double result rounded to single, in both slots.
    State& s = state_;
    const double a = s.fpr[Fa(op)].ps0;
    const double b = s.fpr[Fb(op)].ps0;
    const double c = s.fpr[Fc(op)].ps0;
    double result = 0.0;
    switch (op >> 1 & 31)
    {
    case 18:
        result = a / b;
        break;
    case 20:
        result = a - b;
        break;
    case 21:
        result = a + b;
        break;
    case 24: // fres
        result = 1.0 / b;
        break;
    case 25:
        result = a * c;
        break;
    case 28:
        result = std::fma(a, c, -b);
        break;
    case 29:
        result = std::fma(a, c, b);
        break;
    case 30:
        result = NegatedFma(a, c, -b);
        break;
    case 31:
        result = NegatedFma(a, c, b);
        break;
    default:
        return Illegal(op);
    }

    result = RoundToSingle(result);
    s.fpr[Fd(op)].ps0 = result;
    s.fpr[Fd(op)].ps1 = result;
    SetFprf(result);
    if (op & 1)
    {
        SetCrField(1, s.fpscr >> 28);
    }

    return true;
}

bool Cpu::Op63(uint32_t op)
{
    if (!FpEnabled(op))
    {
        return true;
    }

    State& s = state_;
    const int d = Fd(op);
    Fpr& rd = s.fpr[d];
    const double a = s.fpr[Fa(op)].ps0;
    const double b = s.fpr[Fb(op)].ps0;
    const double c = s.fpr[Fc(op)].ps0;
    const auto arithmetic = [&](double result)
    {
        rd.ps0 = result;
        SetFprf(result);
        if (op & 1)
        {
            SetCrField(1, s.fpscr >> 28);
        }

        return true;
    };

    const auto move = [&](double result)
    {
        rd.ps0 = result;
        if (op & 1)
        {
            SetCrField(1, s.fpscr >> 28);
        }

        return true;
    };

    // The A-form arithmetic, by the 5-bit extended opcode.
    switch (op >> 1 & 31)
    {
    case 18:
        return arithmetic(a / b);
    case 20:
        return arithmetic(a - b);
    case 21:
        return arithmetic(a + b);
    case 23: // fsel
        return move(a >= 0.0 ? c : b);
    case 25:
        return arithmetic(a * c);
    case 26: // frsqrte
        return arithmetic(1.0 / std::sqrt(b));
    case 28:
        return arithmetic(std::fma(a, c, -b));
    case 29:
        return arithmetic(std::fma(a, c, b));
    case 30:
        return arithmetic(NegatedFma(a, c, -b));
    case 31:
        return arithmetic(NegatedFma(a, c, b));
    default:
        break;
    }

    switch (op >> 1 & 0x3ff)
    {
    case 0:  // fcmpu
    case 32: // fcmpo
        FpCompare(d >> 2, a, b, (op >> 1 & 0x3ff) == 32);
        return true;

    case 12: // frsp
        {
            const double result = RoundToSingle(b);
            rd.ps1 = result;
            return arithmetic(result);
        }

    case 14: // fctiw
    case 15: // fctiwz
        rd.ps0 = std::bit_cast<double>(0xfff8000000000000 | ConvertToInt(b, (op >> 1 & 0x3ff) == 15));
        if (op & 1)
        {
            SetCrField(1, s.fpscr >> 28);
        }

        return true;

    case 40: // fneg
        return move(Negate(b));
    case 72: // fmr
        return move(b);
    case 136: // fnabs
        return move(Negate(Abs(b)));
    case 264: // fabs
        return move(Abs(b));

    case 38: // mtfsb1
    case 70: // mtfsb0
        {
            const uint32_t bit = 1u << (31 - d);
            s.fpscr = (op >> 1 & 0x3ff) == 38 ? s.fpscr | bit : s.fpscr & ~bit;
            UpdateRounding();
            if (op & 1)
            {
                SetCrField(1, s.fpscr >> 28);
            }

            return true;
        }

    case 64: // mcrfs
        {
            const int source = Fa(op) >> 2;
            SetCrField(d >> 2, s.fpscr >> (28 - source * 4));
            s.fpscr &= ~((0xfu << (28 - source * 4)) & 0x9ff80700u);
            return true;
        }

    case 134: // mtfsfi
        {
            const int shift = 28 - (d >> 2) * 4;
            s.fpscr = (s.fpscr & ~(0xfu << shift)) | ((op >> 12 & 0xf) << shift);
            UpdateRounding();
            if (op & 1)
            {
                SetCrField(1, s.fpscr >> 28);
            }

            return true;
        }

    case 583: // mffs
        rd.ps0 = std::bit_cast<double>(0xfff8000000000000 | s.fpscr);
        if (op & 1)
        {
            SetCrField(1, s.fpscr >> 28);
        }

        return true;

    case 711: // mtfsf
        {
            const uint32_t fields = op >> 17 & 0xff;
            uint32_t mask = 0;
            for (int i = 0; i < 8; i++)
            {
                if (fields & (0x80 >> i))
                {
                    mask |= 0xf0000000u >> (i * 4);
                }
            }

            s.fpscr = (s.fpscr & ~mask) | (static_cast<uint32_t>(std::bit_cast<uint64_t>(b)) & mask);
            UpdateRounding();
            if (op & 1)
            {
                SetCrField(1, s.fpscr >> 28);
            }

            return true;
        }

    default:
        return Illegal(op);
    }
}

bool Cpu::Op4(uint32_t op)
{
    State& s = state_;
    if (!(s.spr[spr::kHid2] & kHid2Pse))
    {
        return Illegal(op);
    }

    // The indexed paired-single loads and stores share the opcode.
    switch (op >> 1 & 0x3f)
    {
    case 6:
    case 7:
    case 38:
    case 39:
        return PairedLoadStore(op);
    default:
        break;
    }

    if (!FpEnabled(op))
    {
        return true;
    }

    const int d = Fd(op);
    const Fpr a = s.fpr[Fa(op)];
    const Fpr b = s.fpr[Fb(op)];
    const Fpr c = s.fpr[Fc(op)];
    const auto set = [&](double ps0, double ps1)
    {
        s.fpr[d].ps0 = RoundToSingle(ps0);
        s.fpr[d].ps1 = RoundToSingle(ps1);
        SetFprf(s.fpr[d].ps0);
        if (op & 1)
        {
            SetCrField(1, s.fpscr >> 28);
        }

        return true;
    };

    const auto move = [&](double ps0, double ps1)
    {
        s.fpr[d].ps0 = ps0;
        s.fpr[d].ps1 = ps1;
        if (op & 1)
        {
            SetCrField(1, s.fpscr >> 28);
        }

        return true;
    };

    // The A-form arithmetic, by the 5-bit extended opcode.
    switch (op >> 1 & 31)
    {
    case 10: // ps_sum0
        return set(a.ps0 + b.ps1, c.ps1);
    case 11: // ps_sum1
        return set(c.ps0, a.ps0 + b.ps1);
    case 12: // ps_muls0
        return set(a.ps0 * c.ps0, a.ps1 * c.ps0);
    case 13: // ps_muls1
        return set(a.ps0 * c.ps1, a.ps1 * c.ps1);
    case 14: // ps_madds0
        return set(std::fma(a.ps0, c.ps0, b.ps0), std::fma(a.ps1, c.ps0, b.ps1));
    case 15: // ps_madds1
        return set(std::fma(a.ps0, c.ps1, b.ps0), std::fma(a.ps1, c.ps1, b.ps1));
    case 18: // ps_div
        return set(a.ps0 / b.ps0, a.ps1 / b.ps1);
    case 20: // ps_sub
        return set(a.ps0 - b.ps0, a.ps1 - b.ps1);
    case 21: // ps_add
        return set(a.ps0 + b.ps0, a.ps1 + b.ps1);
    case 23: // ps_sel
        return move(a.ps0 >= 0.0 ? c.ps0 : b.ps0, a.ps1 >= 0.0 ? c.ps1 : b.ps1);
    case 24: // ps_res
        return set(1.0 / b.ps0, 1.0 / b.ps1);
    case 25: // ps_mul
        return set(a.ps0 * c.ps0, a.ps1 * c.ps1);
    case 26: // ps_rsqrte
        return set(1.0 / std::sqrt(b.ps0), 1.0 / std::sqrt(b.ps1));
    case 28: // ps_msub
        return set(std::fma(a.ps0, c.ps0, -b.ps0), std::fma(a.ps1, c.ps1, -b.ps1));
    case 29: // ps_madd
        return set(std::fma(a.ps0, c.ps0, b.ps0), std::fma(a.ps1, c.ps1, b.ps1));
    case 30: // ps_nmsub
        return set(NegatedFma(a.ps0, c.ps0, -b.ps0), NegatedFma(a.ps1, c.ps1, -b.ps1));
    case 31: // ps_nmadd
        return set(NegatedFma(a.ps0, c.ps0, b.ps0), NegatedFma(a.ps1, c.ps1, b.ps1));
    default:
        break;
    }

    switch (op >> 1 & 0x3ff)
    {
    case 0:  // ps_cmpu0
    case 32: // ps_cmpo0
        FpCompare(d >> 2, a.ps0, b.ps0, (op >> 1 & 0x3ff) == 32);
        return true;
    case 64: // ps_cmpu1
    case 96: // ps_cmpo1
        FpCompare(d >> 2, a.ps1, b.ps1, (op >> 1 & 0x3ff) == 96);
        return true;
    case 40: // ps_neg
        return move(Negate(b.ps0), Negate(b.ps1));
    case 72: // ps_mr
        return move(b.ps0, b.ps1);
    case 136: // ps_nabs
        return move(Negate(Abs(b.ps0)), Negate(Abs(b.ps1)));
    case 264: // ps_abs
        return move(Abs(b.ps0), Abs(b.ps1));
    case 528: // ps_merge00
        return move(a.ps0, b.ps0);
    case 560: // ps_merge01
        return move(a.ps0, b.ps1);
    case 592: // ps_merge10
        return move(a.ps1, b.ps0);
    case 624: // ps_merge11
        return move(a.ps1, b.ps1);
    case 1014: // dcbz_l
        {
            const uint32_t address = ((Fa(op) == 0 ? 0 : s.gpr[Fa(op)]) + s.gpr[Fb(op)]) & ~31u;
            for (uint32_t i = 0; i < 32 && !stopped_; i += 4)
            {
                Write32(address + i, 0);
            }

            return !stopped_;
        }
    default:
        return Illegal(op);
    }
}

} // namespace csf::ppc
