// SPDX-License-Identifier: MIT

#include "axfx/delay.h"

#include <algorithm>

#include "ntdlib/fused.h"

#include "axfx/util.h"

namespace csf::axfx
{
namespace
{

// The longest line that the model allocates: 2^24 samples, 64 MiB for each channel. The game's allocation would fail
// long before. The model's Init fails there too.
constexpr uint32_t kMaxLength = 1u << 24;

// A time in ms as a count of samples at the renderer's rate. The library multiplies the time by the samples in a ms in
// single precision and converts the product to an unsigned integer. A result of 0 becomes 1.
uint32_t Samples(float ms, uint32_t rate)
{
    const uint32_t samples = ToUnsigned(ms * ToFloat(static_cast<int32_t>(SamplesPerMs(rate))));

    return samples == 0 ? 1 : samples;
}

// A gain of 0 to 1 in 128ths: in single precision, then truncated (fctiwz).
int32_t Gain(float gain)
{
    return ntdlib::PowerPcToInt32(128.0f * gain);
}

// The callback's product of a sample and a gain in 128ths: the low 32 bits of the product (mullw), then a shift right
// by 7 that keeps the sign (srawi).
int32_t Scale(int32_t sample, int32_t gain)
{
    return static_cast<int32_t>(static_cast<uint32_t>(sample) * static_cast<uint32_t>(gain)) >> 7;
}

} // namespace

uint32_t DelayExp::GetMemSize(const Params& params, uint32_t rate)
{
    const float ms = ToFloat(static_cast<int32_t>(SamplesPerMs(rate)));

    return static_cast<uint32_t>(ntdlib::PowerPcToInt32(params.max_delay * ms)) * 12;
}

bool DelayExp::Init(const Params& params, uint32_t rate)
{
    params_ = params;
    active_ = kOff;
    if (!(params.max_delay > 0.0f))
    {
        Off();
        return false;
    }

    max_length_ = Samples(params.max_delay, rate);
    if (max_length_ > kMaxLength)
    {
        Off();
        return false;
    }

    // The library allocates the lines through the hooks (0x02010adc). __InitParams clears them.
    for (std::vector<int32_t>& line : line_)
    {
        line.assign(max_length_, 0);
    }

    if (!InitParams(rate))
    {
        Off();
        return false;
    }

    active_ = (active_ | kSkip) & ~kOff;

    return true;
}

bool DelayExp::Settings(const Params& params, uint32_t rate)
{
    Off();

    return Init(params, rate);
}

bool DelayExp::SettingsUpdate(const Params& params, uint32_t rate)
{
    params_ = params;
    active_ |= kOff;
    if (!InitParams(rate))
    {
        Off();
        return false;
    }

    active_ = (active_ | kSkip) & ~kOff;

    return true;
}

void DelayExp::Shutdown()
{
    Off();
}

void DelayExp::Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in, const Buffers& bus_out)
{
    if (active_ != 0)
    {
        active_ &= ~kSkip;
        return;
    }

    // The low-pass weighs its last output by lpf_coef_ and the input by the rest of 128.
    const int32_t input_gain = 128 - lpf_coef_;
    for (uint32_t i = 0; i < samples; i++)
    {
        std::array<int32_t, kChannelCount> delayed{};
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            delayed[channel] = line_[channel][position_];
            uint32_t input = static_cast<uint32_t>(frame[channel][i]);
            if (bus_in[channel] != nullptr)
            {
                input += static_cast<uint32_t>(bus_in[channel][i]);
            }

            const uint32_t sum = static_cast<uint32_t>(input_gain) * input +
                                 static_cast<uint32_t>(lpf_coef_) * static_cast<uint32_t>(last_[channel]);
            last_[channel] = static_cast<int32_t>(sum) >> 7;
            const uint32_t feedback = static_cast<uint32_t>(Scale(delayed[channel], feedback_gain_));
            line_[channel][position_] = static_cast<int32_t>(static_cast<uint32_t>(last_[channel]) + feedback);
        }

        Step(position_, length_);
        for (int channel = 0; channel < kChannelCount; channel++)
        {
            frame[channel][i] = Scale(delayed[channel], out_gain_);
            if (bus_out[channel] != nullptr)
            {
                bus_out[channel][i] = Scale(delayed[channel], send_gain_);
            }
        }
    }
}

bool DelayExp::InitParams(uint32_t rate)
{
    const Params& p = params_;
    if (p.delay > p.max_delay || p.feedback < 0.0f || !(p.feedback < 1.0f) || p.lpf < 0.0f || p.lpf > 1.0f ||
        p.out_gain < 0.0f || p.out_gain > 1.0f || p.send_gain < 0.0f || p.send_gain > 1.0f || line_[0].empty())
    {
        return false;
    }

    for (std::vector<int32_t>& line : line_)
    {
        std::fill(line.begin(), line.end(), 0);
    }

    // The library doesn't test the delay against 0. A negative delay gives a length beyond the lines, and the library's
    // position would then leave them. The model fails instead.
    length_ = Samples(p.delay, rate);
    if (length_ > max_length_)
    {
        return false;
    }

    feedback_gain_ = Gain(p.feedback);
    position_ = 0;

    // The low-pass's weight of its last output: 1 - lpf, at most 0.95.
    float lpf_coef = 1.0f - p.lpf;
    if (lpf_coef > 0.95f)
    {
        lpf_coef = 0.95f;
    }

    lpf_coef_ = Gain(lpf_coef);
    last_.fill(0);
    out_gain_ = Gain(p.out_gain);
    send_gain_ = Gain(p.send_gain);

    return true;
}

void DelayExp::Off()
{
    active_ |= kOff;
    for (std::vector<int32_t>& line : line_)
    {
        line = std::vector<int32_t>();
    }
}

} // namespace csf::axfx
