// SPDX-License-Identifier: MIT

// AXFXReverbStdExp, the system library's standard reverb for an aux bus (0x020169c0 to 0x0201780c). Each channel runs
// the same network: an early line that feeds back into itself, a pre-delay, two combs in parallel, an allpass, a
// low-pass and a second allpass. The channels don't mix. NintendoWare's FxReverbStd runs this reverb.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "axfx/axfx.h"

namespace csf::axfx
{

// AXFX_REVERBSTD_EXP. A copy of a ReverbStdExp holds the whole state, the delay lines included.
class ReverbStdExp
{
public:
    // AXFX_REVERBSTD_EXP's parameters: the early mode (0 to 7), the longest pre-delay and the pre-delay in seconds,
    // the fused mode (0 to 5) and the fused time in seconds, the coloration (the allpasses' gain), the damping, and
    // the gains of the early reflections, the fused sound, the output and the send to busOut. The gains run from 0 to
    // 1.
    struct Params
    {
        uint32_t early_mode = 0;
        float pre_delay_time_max = 0.0f;
        float pre_delay_time = 0.0f;
        uint32_t fused_mode = 0;
        float fused_time = 0.0f;
        float coloration = 0.0f;
        float damping = 0.0f;
        float early_gain = 0.0f;
        float fused_gain = 0.0f;
        float out_gain = 0.0f;
        float send_gain = 0.0f;
    };

    // AXFXReverbStdExpGetMemSize (0x020169c0): the bytes of the lines for `params` at the renderer's `rate`.
    static uint32_t GetMemSize(const Params& params, uint32_t rate);

    // AXFXReverbStdExpInit (0x0201714c): allocates the lines at their longest, clears them and sets the reverb up.
    // Returns false for a parameter out of its range. The effect then stays off.
    bool Init(const Params& params, uint32_t rate);

    // AXFXReverbStdExpSettings (0x020172e8): Shutdown, then Init. After it succeeds, the next Process leaves its frame
    // as it is.
    bool Settings(const Params& params, uint32_t rate);

    // AXFXReverbStdExpSettingsUpdate (0x0201738c): clears the lines and sets the reverb up for new parameters without
    // allocating the lines again. After it succeeds, the next Process leaves its frame as it is. After it fails, the
    // effect is off.
    bool SettingsUpdate(const Params& params, uint32_t rate);

    // AXFXReverbStdExpShutdown (0x02016b70): frees the lines and turns the effect off.
    void Shutdown();

    // AXFXReverbStdExpCallback (0x02017430): runs `samples` samples of `frame` through the reverb, in place. A sample
    // of `bus_in`, where it isn't null, adds to the input. `bus_out`, where it isn't null, takes the reverb's output at
    // the send gain. While the effect is off, the frame stays as it is.
    void Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in = {}, const Buffers& bus_out = {});

private:
    // The bits of the library's `active` (see DelayExp).
    static constexpr uint32_t kOff = 1;
    static constexpr uint32_t kSkip = 2;

    static constexpr int kCombs = 2;
    static constexpr int kAllpasses = 2;

    // __InitParams (0x02016e00).
    bool InitParams(uint32_t rate);

    // Clears the lines (0x02016cfc).
    void ClearLines();

    // Frees the lines (0x02016a7c) and turns the effect off.
    void Off();

    Params params_;
    uint32_t active_ = kOff;

    // The early line feeds back into itself.
    std::array<std::vector<float>, kChannelCount> early_line_;
    uint32_t early_pos_ = 0;
    uint32_t early_length_ = 0;
    float early_coef_ = 0.0f;

    // The pre-delay. A length of 0 leaves the input undelayed.
    std::array<std::vector<float>, kChannelCount> pre_delay_line_;
    uint32_t pre_delay_pos_ = 0;
    uint32_t pre_delay_length_ = 0;

    // The combs, by channel and comb.
    std::array<std::array<std::vector<float>, kCombs>, kChannelCount> comb_line_;
    std::array<uint32_t, kCombs> comb_pos_{};
    std::array<uint32_t, kCombs> comb_length_{};
    std::array<float, kCombs> comb_coef_{};

    // The allpasses, by channel and allpass.
    std::array<std::array<std::vector<float>, kAllpasses>, kChannelCount> allpass_line_;
    std::array<uint32_t, kAllpasses> allpass_pos_{};
    std::array<uint32_t, kAllpasses> allpass_length_{};
    float allpass_coef_ = 0.0f;

    // The low-pass between the allpasses: its last output and the weight of it.
    std::array<float, kChannelCount> last_lpf_out_{};
    float lpf_coef_ = 0.0f;
};

} // namespace csf::axfx
