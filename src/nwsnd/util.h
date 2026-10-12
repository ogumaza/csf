// SPDX-License-Identifier: MIT

// nw::snd's arithmetic helpers (nw::snd::internal::Util) in NintendoWare for Cafe: ntdlib's model of them, with the
// Wii U's policy. Each function's comment in ntdlib gives the address of the routine it follows in Animal Crossing:
// amiibo Festival's executable.

#pragma once

#include <cstdint>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"

namespace csf::nwsnd
{

using ntdlib::nwsnd::OutputMode;
using ntdlib::nwsnd::PanInfo;
using ntdlib::nwsnd::Random;

namespace util
{

using ntdlib::nwsnd::CalcPitchRatio;

inline int32_t ToInt32(float x)
{
    return ntdlib::nwsnd::CafePolicy::ToInt32(x);
}

inline float CalcVolumeRatio(float decibel)
{
    return ntdlib::nwsnd::CalcVolumeRatio<ntdlib::nwsnd::CafePolicy>(decibel);
}

inline float CalcPanRatio(float pan, const PanInfo& info, OutputMode mode)
{
    return ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::CafePolicy>(pan, info, mode);
}

inline float CalcSurroundPanRatio(float span, const PanInfo& info)
{
    return ntdlib::nwsnd::CalcSurroundPanRatio<ntdlib::nwsnd::CafePolicy>(span, info);
}

inline uint16_t CalcLpfFreq(float lpf)
{
    return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CafePolicy>(lpf);
}

} // namespace util
} // namespace csf::nwsnd
