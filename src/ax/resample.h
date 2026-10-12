// SPDX-License-Identifier: MIT

// Sample-rate conversion (AXPBSRC): how the renderer makes each output sample from a voice's last four samples and its
// position between them. AXSetVoiceSrcType's types 1 to 4 select linear interpolation or one of three 4-tap filters,
// for 8, 12 and 16 kHz. Type 0 takes the samples as they are, and doesn't come here. Both run as in the microcode (see
// render.h). The filters' coefficients are in the DSP's coefficient ROM (ntdlib/dsp/coef_rom.h).

#pragma once

#include <array>
#include <cstdint>

#include "ax/pb.h"

namespace csf::ax
{

// The output sample for a position `frac` (0.16) among the voice's last four samples, `last`, oldest first. Linear
// interpolation (0x0683) goes from the oldest sample to the second oldest. The 4-tap filter `filter` (0x0630; 0, 1 or
// 2: 8, 12 or 16 kHz, when `mode` is kFourTap) centres on the middle two.
int16_t Resample(SrcMode mode, uint16_t filter, const std::array<int16_t, 4>& last, uint32_t frac);

} // namespace csf::ax
