// SPDX-License-Identifier: MIT

// The single-precision pow that snduser2 links (0x02023754) and the log, exp and exp2 that it calls. The reverbs take
// their combs' gains from it. Each function follows the library's code operation for operation: polynomials and a
// rational function in single precision, some steps fused, and the result's exponent set in its bits. The results then
// differ from a correctly rounded pow in many last bits. The library sets errno on a range or domain error. The models
// leave errno alone.

#pragma once

namespace csf::axfx
{

// powf(x, y) (0x02023754).
float Pow(float x, float y);

// logf(x) (0x02023940), the natural logarithm.
float Log(float x);

// expf(x) (0x02023ba8).
float Exp(float x);

// exp2f(x) (0x02023a38). Below 2^-126 the result is 2 to the power of x truncated, without its fraction.
float Exp2(float x);

} // namespace csf::axfx
