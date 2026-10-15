// SPDX-License-Identifier: MIT

#include "axfx/reverb_hi.h"

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
using reverb::kHiEarlyCoefs;
using reverb::kHiEarlySizes;
using reverb::kHiFilterSizes;

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

uint32_t ReverbHiExp::GetMemSize(const Params& params, uint32_t rate)
{
    // Each channel's lines at their longest, and each channel's last allpass. This sum converts the pre-delay with
    // fctiwz, and Init with the library's unsigned conversion.
    const uint32_t* longest = kHiFilterSizes[kFusedModes];
    uint32_t samples = static_cast<uint32_t>(ntdlib::PowerPcToInt32(params.pre_delay_time_max * ToFloat(rate)));
    samples += kHiEarlySizes[kEarlyModes - 1][kTaps - 1];
    for (int i = 0; i < kCombs + kAllpasses; i++)
    {
        samples += longest[i];
    }

    samples *= 3;
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        samples += longest[kCombs + kAllpasses + channel];
    }

    return samples * 4;
}

bool ReverbHiExp::Init(const Params& params, uint32_t rate)
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

    // The library allocates each line with the longest length that the tables give (0x02014834), or no pre-delay line
    // for a longest pre-delay of 0 samples. ClearLines then clears them.
    const uint32_t* longest = kHiFilterSizes[kFusedModes];
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        early_line_[channel].assign(kHiEarlySizes[kEarlyModes - 1][kTaps - 1], 0.0f);
        pre_delay_line_[channel].assign(pre_delay_max_length, 0.0f);
        for (int comb = 0; comb < kCombs; comb++)
        {
            comb_line_[channel][comb].assign(longest[comb], 0.0f);
        }

        for (int allpass = 0; allpass < kAllpasses; allpass++)
        {
            allpass_line_[channel][allpass].assign(longest[kCombs + allpass], 0.0f);
        }

        last_allpass_line_[channel].assign(longest[kCombs + kAllpasses + channel], 0.0f);
    }

    if (!InitParams(rate))
    {
        Off();
        return false;
    }

    active_ &= ~kOff;

    return true;
}

bool ReverbHiExp::Settings(const Params& params, uint32_t rate)
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

bool ReverbHiExp::SettingsUpdate(const Params& params, uint32_t rate)
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

void ReverbHiExp::Shutdown()
{
    Off();
}

void ReverbHiExp::Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in, const Buffers& bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkip;
        return;
    }

    // The gains that the callback computes once a frame.
    const float fused_gain = params_.fused_gain * 0.6f;
    const float lowpass_gain = 1.0f - lpf_coef_;
    const float crosstalk = params_.crosstalk * 0.5f;
    for (uint32_t i = 0; i < samples; i++)
    {
        std::array<float, kChannelCount> out{};
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            uint32_t sample = static_cast<uint32_t>(frame[channel][i]);
            if (bus_in[channel] != nullptr)
            {
                sample += static_cast<uint32_t>(bus_in[channel][i]);
            }

            out[channel] = ProcessChannel(channel, ToFloat(static_cast<int32_t>(sample)), fused_gain, lowpass_gain);
        }

        // The crosstalk mixes the other two channels into each one.
        const float left = Fmadds(out[kRight] + out[kSurround], crosstalk, out[kLeft]);
        const float right = Fmadds(out[kLeft] + out[kSurround], crosstalk, out[kRight]);
        const float surround = Fmadds(out[kLeft] + out[kRight], crosstalk, out[kSurround]);
        frame[kLeft][i] = ntdlib::PowerPcToInt32(left * params_.out_gain);
        frame[kRight][i] = ntdlib::PowerPcToInt32(right * params_.out_gain);
        frame[kSurround][i] = ntdlib::PowerPcToInt32(surround * params_.out_gain);
        if (bus_out[kLeft] != nullptr)
        {
            bus_out[kLeft][i] = ntdlib::PowerPcToInt32(left * params_.send_gain);
        }

        if (bus_out[kRight] != nullptr)
        {
            bus_out[kRight][i] = ntdlib::PowerPcToInt32(right * params_.send_gain);
        }

        if (bus_out[kSurround] != nullptr)
        {
            bus_out[kSurround][i] = ntdlib::PowerPcToInt32(surround * params_.send_gain);
        }

        for (uint32_t& position : early_pos_)
        {
            Step(position, early_length_);
        }

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

float ReverbHiExp::ProcessChannel(int channel, float input, float fused_gain, float lowpass_gain)
{
    // The early reflections: three taps of the line. The input goes into the line at the last tap.
    std::vector<float>& early_line = early_line_[channel];
    const float tap1 = early_line[early_pos_[1]] * early_coef_[1];
    const float taps01 = Fmadds(early_line[early_pos_[0]], early_coef_[0], tap1);
    const float tap2 = early_line[early_pos_[2]];
    early_line[early_pos_[2]] = input;
    const float early = Fmadds(tap2, early_coef_[2], taps01);

    // The pre-delay.
    float delayed = input;
    if (pre_delay_length_ != 0)
    {
        float& sample = pre_delay_line_[channel][pre_delay_pos_];
        delayed = sample;
        sample = input;
    }

    // Three combs in parallel.
    float combs = 0.0f;
    for (int comb = 0; comb < kCombs; comb++)
    {
        float& sample = comb_line_[channel][comb][comb_pos_[comb]];
        const float output = sample;
        sample = Fmadds(output, comb_coef_[comb], delayed);
        combs = combs + output;
    }

    // Two allpasses.
    float passed = combs;
    for (int allpass = 0; allpass < kAllpasses; allpass++)
    {
        float& sample = allpass_line_[channel][allpass][allpass_pos_[allpass]];
        const float delayed_sample = sample;
        sample = Fmadds(delayed_sample, allpass_coef_, passed);
        passed = NegatedFms(sample, allpass_coef_, delayed_sample);
    }

    // The low-pass and the channel's last allpass. The last allpass's position moves on here, a channel at a time.
    const float lowpass = Fmadds(lowpass_gain, passed, lpf_coef_ * last_lpf_out_[channel]);
    last_lpf_out_[channel] = lowpass;
    uint32_t& position = last_allpass_pos_[channel];
    float& sample = last_allpass_line_[channel][position];
    const float delayed_sample = sample;
    sample = Fmadds(delayed_sample, allpass_coef_, lowpass);
    const float output = NegatedFms(sample, allpass_coef_, delayed_sample);
    Step(position, last_allpass_length_[channel]);

    return Fmadds(output, fused_gain, early);
}

bool ReverbHiExp::InitParams(uint32_t rate)
{
    const Params& p = params_;
    if (p.early_mode >= kEarlyModes || p.pre_delay_time < 0.0f || p.pre_delay_time > p.pre_delay_time_max ||
        p.fused_mode >= kFusedModes || p.fused_time < 0.0f || OutOfRange(p.coloration) || OutOfRange(p.damping) ||
        OutOfRange(p.crosstalk) || OutOfRange(p.early_gain) || OutOfRange(p.fused_gain) || OutOfRange(p.out_gain) ||
        OutOfRange(p.send_gain))
    {
        return false;
    }

    // The early line is as long as the last tap's delay. Each tap starts at that length less its delay.
    const uint32_t* early_sizes = kHiEarlySizes[p.early_mode];
    early_length_ = early_sizes[kTaps - 1];
    for (int tap = 0; tap < kTaps; tap++)
    {
        early_pos_[tap] = early_length_ - early_sizes[tap];
        early_coef_[tap] = (kHiEarlyCoefs[p.early_mode][tap] * p.early_gain) * 0.6f;
    }

    // The library's pre-delay can be longer than its line in two cases: a NaN longest pre-delay, and a longer
    // pre_delay_time_max given to SettingsUpdate. The library would then write past the line. The model fails instead.
    const float rate_float = ToFloat(rate);
    pre_delay_pos_ = 0;
    pre_delay_length_ = ToUnsigned(p.pre_delay_time * rate_float);
    if (pre_delay_length_ > pre_delay_line_[0].size())
    {
        return false;
    }

    const uint32_t* sizes = kHiFilterSizes[p.fused_mode];
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

    for (int channel = 0; channel < kChannelCount; channel++)
    {
        last_allpass_pos_[channel] = 0;
        last_allpass_length_[channel] = sizes[kCombs + kAllpasses + channel];
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

void ReverbHiExp::ClearLines()
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

        Clear(last_allpass_line_[channel]);
    }
}

void ReverbHiExp::Off()
{
    active_ |= kOff;
    for (int channel = 0; channel < kChannelCount; channel++)
    {
        early_line_[channel] = {};
        pre_delay_line_[channel] = {};
        comb_line_[channel] = {};
        allpass_line_[channel] = {};
        last_allpass_line_[channel] = {};
    }
}

} // namespace csf::axfx
