// SPDX-License-Identifier: MIT

// nw::snd's envelope (CurveAdshr) and LFO (CurveLfo) in NintendoWare for Cafe: ntdlib's model of them, with the Wii U's
// policy.

#pragma once

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/envelope.h"

namespace csf::nwsnd
{

using CurveAdshr = ntdlib::nwsnd::CurveAdshr<ntdlib::nwsnd::CafePolicy>;
using CurveLfo = ntdlib::nwsnd::CurveLfo<ntdlib::nwsnd::CafePolicy>;

} // namespace csf::nwsnd
