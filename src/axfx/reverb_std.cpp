// SPDX-License-Identifier: MIT

#include "axfx/reverb_std.h"

#include <algorithm>
#include <cmath>

#include "ntdlib/fused.h"

#include "axfx/reverb_tables.h"
#include "axfx/util.h"

namespace csf::axfx
{
namespace
{

using reverb::kEarlyModes;
using reverb::kFusedModes;
using reverb::kStdEarlySizes;
using reverb::kStdFilterSizes;

// Whether a gain is below 0 or above 1. A NaN gain passes the library's tests.
bool OutOfRange(float value)
{
    return value < 0.0f || value > 1.0f;
}

void Clear(std::vector<float>& line)
{
    std::fill(line.begin(), line.end(), 0.0f);
}

} // namespace

uint32_t ReverbStdExp::GetMemSize(const Params& params, uint32_t rate)
{
    // Each channel's lines at their longest. This sum converts the pre-delay with fctiwz, and Init with the library's
    // unsigned conversion.
    const uint32_t* longest = kStdFilterSizes[kFusedModes];
    uint32_t samples = static_cast<uint32_t>(ntdlib::PowerPcToInt32(params.pre_delay_time_max * ToFloat(rate)));
    samples += kStdEarlySizes[kEarlyModes - 1];
    for (int i = 0; i < kCombs + kAllpasses; i++)
    {
        samples += longest[i];
    }

    return samples * 12;
}

bool ReverbStdExp::Init(const Params& params, uint32_t rate)
{
    params_ = params;
    active_ = kOff;
    if (params.pre_delay_time_max < 0.0f)
    {
        Off();
        return false;
    }

    const uint32_t pre_delay_max_length = ToUnsigned(params.pre_delay_time_max * ToFloat(rate));
    if (pre_delay_max_length > reverb::kMaxPreDelayLength)
    {
        Off();
        return false;
    }

    // The library allocates each line with the longest length that the tables give (0x02016bc4), or no pre-delay line
    // for a longest pre-delay of 0 samples. ClearLines then clears them.
    const uint32_t* longest = kStdFilterSizes[kFusedModes];
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        early_line_[channel].assign(kStdEarlySizes[kEarlyModes - 1], 0.0f);
        pre_delay_line_[channel].assign(pre_delay_max_length, 0.0f);
        for (int comb = 0; comb < kCombs; comb++)
        {
            comb_line_[channel][comb].assign(longest[comb], 0.0f);
        }

        for (int allpass = 0; allpass < kAllpasses; allpass++)
        {
            allpass_line_[channel][allpass].assign(longest[kCombs + allpass], 0.0f);
        }
    }

    if (!InitParams(rate))
    {
        Off();
        return false;
    }

    active_ &= ~kOff;

    return true;
}

bool ReverbStdExp::Settings(const Params& params, uint32_t rate)
{
    Off();
    if (!Init(params, rate))
    {
        Off();
        return false;
    }

    active_ = (active_ | kSkip) & ~kOff;

    return true;
}

bool ReverbStdExp::SettingsUpdate(const Params& params, uint32_t rate)
{
    // After a Shutdown or a failed Init, the library would set the reverb up on lines it has freed, and its callback
    // would then run on null lines. The model fails instead.
    params_ = params;
    active_ |= kOff;
    if (early_line_[0].empty())
    {
        Off();
        return false;
    }

    ClearLines();
    if (!InitParams(rate))
    {
        Off();
        return false;
    }

    active_ = (active_ | kSkip) & ~kOff;

    return true;
}

void ReverbStdExp::Shutdown()
{
    Off();
}

void ReverbStdExp::Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in, const Buffers& bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkip;
        return;
    }

    // The gains that the callback computes once a frame.
    const float early_gain = params_.early_gain * 0.6f;
    const float lowpass_gain = 1.0f - lpf_coef_;
    const float fused_gain = params_.fused_gain * 0.6f;
    for (uint32_t i = 0; i < samples; i++)
    {
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            uint32_t sample = static_cast<uint32_t>(frame[channel][i]);
            if (bus_in[channel] != nullptr)
            {
                sample += static_cast<uint32_t>(bus_in[channel][i]);
            }

            const float input = ToFloat(static_cast<int32_t>(sample));

            // The early line.
            float& early_sample = early_line_[channel][early_pos_];
            const float early = early_sample;
            early_sample = Fmadds(early, early_coef_, input);

            // The pre-delay.
            float delayed = input;
            if (pre_delay_length_ != 0)
            {
                float& delay_sample = pre_delay_line_[channel][pre_delay_pos_];
                delayed = delay_sample;
                delay_sample = input;
            }

            // Two combs in parallel.
            float& comb0 = comb_line_[channel][0][comb_pos_[0]];
            const float out0 = comb0;
            comb0 = Fmadds(out0, comb_coef_[0], delayed);
            float& comb1 = comb_line_[channel][1][comb_pos_[1]];
            const float out1 = comb1;
            comb1 = Fmadds(out1, comb_coef_[1], delayed);
            const float combs = out0 + out1;

            // An allpass, the low-pass and a second allpass.
            float& allpass0 = allpass_line_[channel][0][allpass_pos_[0]];
            const float delayed0 = allpass0;
            allpass0 = Fmadds(delayed0, allpass_coef_, combs);
            const float passed0 = NegatedFms(allpass0, allpass_coef_, delayed0);
            const float lowpass = Fmadds(lowpass_gain, passed0, lpf_coef_ * last_lpf_out_[channel]);
            last_lpf_out_[channel] = lowpass;
            float& allpass1 = allpass_line_[channel][1][allpass_pos_[1]];
            const float delayed1 = allpass1;
            allpass1 = Fmadds(delayed1, allpass_coef_, lowpass);
            const float passed1 = NegatedFms(allpass1, allpass_coef_, delayed1);

            const float output = Fmadds(early, early_gain, passed1 * fused_gain);
            frame[channel][i] = ntdlib::PowerPcToInt32(output * params_.out_gain);
            if (bus_out[channel] != nullptr)
            {
                bus_out[channel][i] = ntdlib::PowerPcToInt32(output * params_.send_gain);
            }
        }

        Step(early_pos_, early_length_);
        if (pre_delay_length_ != 0)
        {
            Step(pre_delay_pos_, pre_delay_length_);
        }

        for (int comb = 0; comb < kCombs; comb++)
        {
            Step(comb_pos_[comb], comb_length_[comb]);
        }

        for (int allpass = 0; allpass < kAllpasses; allpass++)
        {
            Step(allpass_pos_[allpass], allpass_length_[allpass]);
        }
    }
}

bool ReverbStdExp::InitParams(uint32_t rate)
{
    const Params& p = params_;
    if (p.early_mode >= kEarlyModes || p.pre_delay_time < 0.0f || p.pre_delay_time > p.pre_delay_time_max ||
        p.fused_mode >= kFusedModes || p.fused_time < 0.0f || OutOfRange(p.coloration) || OutOfRange(p.damping) ||
        OutOfRange(p.early_gain) || OutOfRange(p.fused_gain) || OutOfRange(p.out_gain) || OutOfRange(p.send_gain))
    {
        return false;
    }

    // Early modes 0 to 3 feed the early line back negated.
    early_pos_ = 0;
    early_length_ = kStdEarlySizes[p.early_mode];
    early_coef_ = p.early_mode <= 3 ? -0.33f : 0.33f;

    // The library's pre-delay can be longer than its line in two cases: a NaN longest pre-delay, and a longer
    // pre_delay_time_max given to SettingsUpdate. The library would then write past the line. The model fails instead.
    const float rate_float = ToFloat(rate);
    pre_delay_pos_ = 0;
    pre_delay_length_ = ToUnsigned(p.pre_delay_time * rate_float);
    if (pre_delay_length_ > pre_delay_line_[0].size())
    {
        return false;
    }

    const uint32_t* sizes = kStdFilterSizes[p.fused_mode];
    for (int comb = 0; comb < kCombs; comb++)
    {
        comb_pos_[comb] = 0;
        comb_length_[comb] = sizes[comb];
        comb_coef_[comb] = reverb::CombCoef(sizes[comb], p.fused_time, rate_float);
    }

    for (int allpass = 0; allpass < kAllpasses; allpass++)
    {
        allpass_pos_[allpass] = 0;
        allpass_length_[allpass] = sizes[kCombs + allpass];
    }

    // The coloration is the allpasses' gain. The low-pass weighs its last output by 1 - damping, at most 0.95.
    allpass_coef_ = p.coloration;
    lpf_coef_ = 1.0f - p.damping;
    if (lpf_coef_ > 0.95f)
    {
        lpf_coef_ = 0.95f;
    }

    last_lpf_out_.fill(0.0f);

    return true;
}

void ReverbStdExp::ClearLines()
{
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        Clear(early_line_[channel]);
        Clear(pre_delay_line_[channel]);
        for (std::vector<float>& line : comb_line_[channel])
        {
            Clear(line);
        }

        for (std::vector<float>& line : allpass_line_[channel])
        {
            Clear(line);
        }
    }
}

void ReverbStdExp::Off()
{
    active_ |= kOff;
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        early_line_[channel] = {};
        pre_delay_line_[channel] = {};
        comb_line_[channel] = {};
        allpass_line_[channel] = {};
    }
}

} // namespace csf::axfx
