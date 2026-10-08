// SPDX-License-Identifier: MIT

// The PowerPC's negated fused multiply-add (fnmadd and fnmsub, in their double, single and paired-single forms): a * c
// + b, rounded once, then negated. Compilers turn a negated fma into one instruction that negates before it rounds
// instead (Arm's FNMADD or x86's VFNMSUB, for example). Those instructions give +0 for an exact zero of operands with
// opposite signs. The PowerPC gives -0. The sum passes through a volatile. The two steps then stay apart, and every
// build gives the PowerPC's result.

#pragma once

#include <cmath>

namespace csf
{

inline double NegatedFma(double a, double c, double b)
{
    volatile double sum = std::fma(a, c, b);
    return -sum;
}

inline float NegatedFma(float a, float c, float b)
{
    volatile float sum = std::fma(a, c, b);
    return -sum;
}

} // namespace csf
