// SPDX-License-Identifier: MIT

#include "nwsnd/util.h"

#include <cmath>
#include <cstdint>

#include "nwsnd/tables.h"

namespace csf::nwsnd::util
{

int32_t ToInt32(float x)
{
    if (std::isnan(x) || x <= -2147483648.0f)
    {
        return INT32_MIN;
    }
    if (x >= 2147483648.0f)
    {
        return INT32_MAX;
    }

    return static_cast<int32_t>(x);
}

uint64_t ToUint64(float x)
{
    if (!(x >= 1.0f))
    {
        return 0;
    }
    if (x >= 18446744073709551616.0f)
    {
        return UINT64_MAX;
    }

    return static_cast<uint64_t>(x);
}

float CalcPitchRatio(int pitch)
{
    // Whole octaves first, then semitones and 1/256 semitones from the tables. Doubling and halving are exact.
    int octave = 0;
    if (pitch < 0)
    {
        const int down = (-1 - pitch) / 3072 + 1;
        octave = -down;
        pitch += down * 3072;
    }
    if (pitch >= 3072)
    {
        const int up = (pitch - 3072) / 3072 + 1;
        octave += up;
        pitch -= up * 3072;
    }

    const int semitone = pitch / 256;
    const int fine = pitch - semitone * 256;
    float ratio = 1.0f;
    for (int i = 0; i < octave; ++i)
    {
        ratio += ratio;
    }
    for (int i = 0; i > octave; --i)
    {
        ratio *= 0.5f;
    }
    if (semitone != 0)
    {
        ratio *= tables::kSemitone[semitone];
    }
    if (fine != 0)
    {
        ratio *= tables::kFine[fine];
    }

    return ratio;
}

float CalcVolumeRatio(float decibel)
{
    // fsel holds anything not above -90.4, NaN included, to -90.4.
    float level = 6.0f;
    if (!(decibel > 6.0f))
    {
        level = decibel - -90.4f >= 0.0f ? decibel : -90.4f;
    }

    return tables::kDecibel[static_cast<int>(level * 10.0f) + 904];
}

float CalcPanRatio(float pan, const PanInfo& info, OutputMode mode)
{
    float x = 1.0f;
    if (!(pan > 1.0f))
    {
        const float held = pan - -1.0f >= 0.0f ? pan : -1.0f;
        x = (held + 1.0f) * 0.5f;
    }

    // A sound's pan curve comes from its archive. Archives give 0 to 2. The game doesn't check the value.
    const bool surround = !info.front_bypass && mode == OutputMode::kSurround;
    const float* curve = surround ? tables::kSurroundPan[info.curve % 3] : tables::kStereoPan[info.curve % 3];
    float ratio = curve[static_cast<int>(x * 256.0f + 0.5f)];
    if (info.center_zero_db)
    {
        ratio /= curve[128];
    }

    const float limit = info.clamp_to_unity ? 1.0f : 2.0f;
    if (ratio > limit)
    {
        return limit;
    }

    return ratio >= 0.0f ? ratio : 0.0f;
}

float CalcSurroundPanRatio(float span, const PanInfo& info)
{
    float x = 1.0f;
    if (!(span > 2.0f))
    {
        x = (span >= 0.0f ? span : 0.0f) * 0.5f;
    }

    const float ratio = tables::kStereoPan[info.curve % 3][static_cast<int>(x * 256.0f + 0.5f)];
    if (ratio > 2.0f)
    {
        return 2.0f;
    }

    return ratio >= 0.0f ? ratio : 0.0f;
}

uint16_t CalcLpfFreq(float lpf)
{
    // The steps are a thirtieth apart from 0.13561438. The code divides by 1/30 rather than multiplying by 30.
    constexpr float kFirstStep = 0x1.15bcfep-3f; // 0.13561438
    constexpr float kStep = 0x1.111112p-5f;      // 1/30
    float value = 1.0f;
    if (!(lpf > 1.0f))
    {
        value = lpf >= 0.0f ? lpf : 0.0f;
    }
    if (value < kFirstStep)
    {
        return 80;
    }
    if (value >= 0.9f)
    {
        return 16000;
    }

    return tables::kLpfFreq[static_cast<int>((value - kFirstStep) / kStep)];
}

} // namespace csf::nwsnd::util
