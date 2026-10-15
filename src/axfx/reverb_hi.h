// SPDX-License-Identifier: MIT

// AXFXReverbHiExp, the system library's high-quality reverb for an aux bus (0x020145e8 to 0x02015660). Each channel
// runs the same network: early reflections from three taps of an early line, a pre-delay, three combs in parallel, two
// allpasses, a low-pass and a last allpass of the channel's length. The crosstalk then mixes the other two channels
// into each channel's output. NintendoWare's FxReverbHi runs this reverb.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "axfx/axfx.h"

namespace csf::axfx
{

// AXFX_REVERBHI_EXP. A copy of a ReverbHiExp holds the whole state, the delay lines included.
class ReverbHiExp
{
public:
    // AXFX_REVERBHI_EXP's parameters: those of the standard reverb (see ReverbStdExp) and the crosstalk, 0 to 1.
    struct Params
    {
        uint32_t early_mode = 0;
        float pre_delay_time_max = 0.0f;
        float pre_delay_time = 0.0f;
        uint32_t fused_mode = 0;
        float fused_time = 0.0f;
        float coloration = 0.0f;
        float damping = 0.0f;
        float crosstalk = 0.0f;
        float early_gain = 0.0f;
        float fused_gain = 0.0f;
        float out_gain = 0.0f;
        float send_gain = 0.0f;
    };

    // AXFXReverbHiExpGetMemSize (0x020145e8): the bytes of the lines for `params` at the renderer's `rate`.
    static uint32_t GetMemSize(const Params& params, uint32_t rate);

    // AXFXReverbHiExpInit (0x02014e44): allocates the lines at their longest, clears them and sets the reverb up.
    // Returns false for a parameter out of its range. The effect then stays off.
    bool Init(const Params& params, uint32_t rate);

    // AXFXReverbHiExpSettings (0x02015004): Shutdown, then Init. After it succeeds, the next Process leaves its frame
    // as it is.
    bool Settings(const Params& params, uint32_t rate);

    // AXFXReverbHiExpSettingsUpdate (0x020150a8): clears the lines and sets the reverb up for new parameters without
    // allocating the lines again. After it succeeds, the next Process leaves its frame as it is. After it fails, the
    // effect is off.
    bool SettingsUpdate(const Params& params, uint32_t rate);

    // AXFXReverbHiExpShutdown (0x020147e0): frees the lines and turns the effect off.
    void Shutdown();

    // AXFXReverbHiExpCallback (0x0201514c): runs `samples` samples of `frame` through the reverb, in place. A sample of
    // `bus_in`, where it isn't null, adds to the input. `bus_out`, where it isn't null, takes the reverb's output at
    // the send gain. While the effect is off, the frame stays as it is.
    void Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in = {}, const Buffers& bus_out = {});

private:
    // The bits of the library's `active` (see DelayExp).
    static constexpr uint32_t kOff = 1;
    static constexpr uint32_t kSkip = 2;

    static constexpr int kTaps = 3;
    static constexpr int kCombs = 3;
    static constexpr int kAllpasses = 2;

    // One sample of one channel's network, before the crosstalk.
    float ProcessChannel(int channel, float input, float fused_gain, float lowpass_gain);

    // __InitParams (0x02014ac0).
    bool InitParams(uint32_t rate);

    // Clears the lines (0x02014998).
    void ClearLines();

    // Frees the lines (0x020146c8) and turns the effect off.
    void Off();

    Params params_;
    uint32_t active_ = kOff;

    // The early reflections: the line takes each input sample at the last tap's position.
    std::array<std::vector<float>, kChannelCount> early_line_;
    std::array<uint32_t, kTaps> early_pos_{};
    uint32_t early_length_ = 0;
    std::array<float, kTaps> early_coef_{};

    // The pre-delay. A length of 0 leaves the input undelayed.
    std::array<std::vector<float>, kChannelCount> pre_delay_line_;
    uint32_t pre_delay_pos_ = 0;
    uint32_t pre_delay_length_ = 0;

    // The combs, by channel and comb.
    std::array<std::array<std::vector<float>, kCombs>, kChannelCount> comb_line_;
    std::array<uint32_t, kCombs> comb_pos_{};
    std::array<uint32_t, kCombs> comb_length_{};
    std::array<float, kCombs> comb_coef_{};

    // The two allpasses after the combs, by channel and allpass, and each channel's last allpass.
    std::array<std::array<std::vector<float>, kAllpasses>, kChannelCount> allpass_line_;
    std::array<uint32_t, kAllpasses> allpass_pos_{};
    std::array<uint32_t, kAllpasses> allpass_length_{};
    std::array<std::vector<float>, kChannelCount> last_allpass_line_;
    std::array<uint32_t, kChannelCount> last_allpass_pos_{};
    std::array<uint32_t, kChannelCount> last_allpass_length_{};
    float allpass_coef_ = 0.0f;

    // The low-pass between the allpasses: its last output and the weight of it.
    std::array<float, kChannelCount> last_lpf_out_{};
    float lpf_coef_ = 0.0f;
};

} // namespace csf::axfx
