// SPDX-License-Identifier: MIT

#include "nwsnd/envelope.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "common/fused.h"
#include "nwsnd/tables.h"

namespace csf::nwsnd
{
namespace
{

// The level in 0.1 dB of (sustain / 127)^2 (CurveAdshr::CalcDecibelSquare, 0x022BAF48). Archives give 0 to 127.
float DecibelSquare(uint8_t sustain)
{
    return static_cast<float>(tables::kDecibelSquare[std::min<int>(sustain, 127)]);
}

// nw::math's sine of a turn's fraction (CurveSine, 0x022BB420). The turn is in 2^32 steps. The sine is the table's
// entry for the top 8 bits, plus its step to the next entry for the low 24 bits, in one fused step.
float CurveSine(float turn)
{
    const float steps = turn * 4294967296.0f;
    const uint32_t step = steps < 2147483648.0f
                              ? static_cast<uint32_t>(util::ToInt32(steps))
                              : static_cast<uint32_t>(util::ToInt32(steps - 2147483648.0f)) + 0x80000000u;
    const float* entry = tables::kSin[step >> 24];

    return std::fma(entry[1] * static_cast<float>(step & 0xFFFFFF), 0x1p-24f, entry[0]);
}

// Four straight lines through 0, 1, 0, -1 and 0 (CurveTriangle, 0x022BB50C).
float CurveTriangle(float turn)
{
    constexpr std::array<float, 5> kSlopes = {4.0f, -4.0f, -4.0f, 4.0f, 4.0f};
    constexpr std::array<float, 5> kOffsets = {0.0f, 2.0f, 2.0f, -4.0f, 0.0f};
    const float quarters = turn * 4.0f;
    const int32_t line =
        quarters < 2147483648.0f ? util::ToInt32(quarters) : util::ToInt32(quarters - 2147483648.0f) + INT32_MIN;
    const auto i = static_cast<std::size_t>(std::clamp(line, 0, 4));

    return std::fma(kSlopes[i], turn, kOffsets[i]);
}

// 0, then 1 from halfway (CurveSquare, 0x022BB5EC).
float CurveSquare(float turn)
{
    return turn - 0.5f >= 0.0f ? 1.0f : 0.0f;
}

} // namespace

void CurveAdshr::Initialize(float decibel)
{
    SetAttack(127);
    SetHold(0);
    SetDecay(127);
    SetSustain(127);
    SetRelease(127);
    Reset(decibel);
}

void CurveAdshr::SetAttack(int attack)
{
    // Archives give 0 to 127.
    attack_ = tables::kAttack[std::clamp(attack, 0, 127)];
}

void CurveAdshr::SetHold(int hold)
{
    hold_ = static_cast<uint16_t>((hold + 1) * (hold + 1) / 4);
}

void CurveAdshr::SetDecay(int decay)
{
    decay_ = CalcRelease(decay);
}

void CurveAdshr::SetSustain(int sustain)
{
    sustain_ = static_cast<uint8_t>(sustain);
}

void CurveAdshr::SetRelease(int release)
{
    release_ = CalcRelease(release);
}

float CurveAdshr::CalcRelease(int release)
{
    if (release == 127)
    {
        return 65535.0f;
    }
    if (release == 126)
    {
        return 24.0f;
    }
    if (release < 50)
    {
        return static_cast<float>(release * 2 + 1) * 0.0078125f / 5.0f;
    }

    return 60.0f / static_cast<float>(126 - release) / 5.0f;
}

float CurveAdshr::GetValue() const
{
    if (status_ == Status::kAttack && attack_ == 0.0f)
    {
        return 0.0f;
    }

    return value_ / 10.0f;
}

void CurveAdshr::Update(int msec)
{
    switch (status_)
    {
    case Status::kAttack:
        {
            // The level is multiplied by the attack each millisecond. The hold starts once the level is within 1/32 of
            // 0.1 dB of 0.
            float value = value_;
            for (int i = 0; i < msec; ++i)
            {
                value *= attack_;
                if (value > -0.03125f)
                {
                    value_ = 0.0f;
                    hold_counter_ = hold_;
                    status_ = Status::kHold;
                    return;
                }
            }
            value_ = value;
            return;
        }

    case Status::kHold:
        if (msec < hold_counter_)
        {
            hold_counter_ = static_cast<uint16_t>(hold_counter_ - msec);
            return;
        }
        msec -= hold_counter_;
        hold_counter_ = 0;
        status_ = Status::kDecay;
        [[fallthrough]];

    case Status::kDecay:
        {
            // The decay and the release fall by their rate times the milliseconds. The fall is one fused step that's
            // negated after it rounds (fnmsubs). A level that ends at exactly 0 is then -0. The game gives -0 too.
            const float sustain = DecibelSquare(sustain_);
            const float value = NegatedFma(decay_, static_cast<float>(msec), -value_);
            if (value < sustain)
            {
                value_ = sustain;
                status_ = Status::kSustain;
                return;
            }
            value_ = value;
            return;
        }

    case Status::kRelease:
        value_ = NegatedFma(release_, static_cast<float>(msec), -value_);
        return;

    default:
        return;
    }
}

void CurveLfo::Reset()
{
    delay_counter_ = 0;
    counter_ = 0.0f;
    random_value_ = 1.0f;
    started_ = false;
    wrapped_ = false;
}

void CurveLfo::Update(int msec)
{
    if (delay_counter_ < param.delay)
    {
        if (delay_counter_ + static_cast<uint32_t>(msec) <= param.delay)
        {
            delay_counter_ += static_cast<uint32_t>(msec);
            return;
        }
        msec -= static_cast<int>(param.delay - delay_counter_);
        delay_counter_ = param.delay;
    }
    if (!(param.speed > 0.0f))
    {
        return;
    }

    // The cycle starts at the phase, and advances by the speed times the seconds.
    if (!started_)
    {
        started_ = true;
        counter_ = static_cast<float>(param.phase) / 127.0f;
    }
    const float counter = counter_ + param.speed * static_cast<float>(msec) / 1000.0f;
    wrapped_ = !(counter < 1.0f);
    counter_ = counter - static_cast<float>(util::ToInt32(counter));
}

float CurveLfo::GetValue(Random& random)
{
    if (param.depth == 0.0f || delay_counter_ < param.delay)
    {
        return 0.0f;
    }

    float value = 1.0f;
    switch (param.curve)
    {
    case 0:
        value = CurveSine(counter_);
        break;
    case 1:
        value = CurveTriangle(counter_);
        break;
    case 2:
        value = counter_;
        break;
    case 3:
        value = CurveSquare(counter_);
        break;
    case 4:
        // CurveRandom (0x022BB610): a number from 0 to 1. The number is drawn while the cycle has just wrapped.
        if (wrapped_)
        {
            random_value_ = static_cast<float>(random.Next()) / 65535.0f;
        }
        value = random_value_;
        break;
    default:
        break;
    }

    return value * param.depth * static_cast<float>(param.range);
}

} // namespace csf::nwsnd
