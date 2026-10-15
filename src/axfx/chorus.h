// SPDX-License-Identifier: MIT

// AXFXChorusExp, the system library's chorus for an aux bus (0x0200bd14 to 0x0200c954). Each channel's output reads a
// line of 100 ms behind the input, by a time that a sine LFO varies, through a 4-tap interpolator. The line takes the
// input plus the output at the feedback's gain. NintendoWare's FxChorus runs this chorus.
//
// The lines' length and the delay follow the renderer's rate. The LFO's step and depth are computed for 32 kHz at any
// rate. At 48 kHz the LFO then runs 1.5 times as fast as its rate parameter gives.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "axfx/axfx.h"

namespace csf::axfx
{

// AXFX_CHORUS_EXP. A copy of a ChorusExp holds the whole state, the delay lines included.
class ChorusExp
{
public:
    // AXFX_CHORUS_EXP's parameters: the delay in ms (0.1 to 50), the LFO's swing as a share of the delay (0 to 1),
    // the LFO's rate in Hz (0.1 to 2), the feedback (below 1), and the gains of the output and of the send to busOut
    // (0 to 1).
    struct Params
    {
        float delay_time = 0.0f;
        float depth = 0.0f;
        float rate = 0.0f;
        float feedback = 0.0f;
        float out_gain = 0.0f;
        float send_gain = 0.0f;
    };

    // AXFXChorusExpGetMemSize (0x0200bd14): the bytes of the three lines at the renderer's `rate`.
    static uint32_t GetMemSize(uint32_t rate);

    // AXFXChorusExpInit (0x0200c234): allocates and clears the lines, sets the positions and starts the LFO. Returns
    // false for a parameter out of its range. The effect then stays off.
    bool Init(const Params& params, uint32_t rate);

    // AXFXChorusExpSettings (0x0200c2e8): Shutdown, then Init. After it succeeds, the next Process leaves its frame as
    // it is.
    bool Settings(const Params& params, uint32_t rate);

    // AXFXChorusExpSettingsUpdate (0x0200c398): clears the lines, sets the positions and starts the LFO again for new
    // parameters, without allocating the lines again. After it succeeds, the next Process leaves its frame as it is.
    // After it fails, the effect is off.
    bool SettingsUpdate(const Params& params, uint32_t rate);

    // AXFXChorusExpShutdown (0x0200be3c): frees the lines and turns the effect off.
    void Shutdown();

    // AXFXChorusExpCallback (0x0200c5a8): runs `samples` samples of `frame` through the chorus, in place. A sample of
    // `bus_in`, where it isn't null, adds to the input. `bus_out`, where it isn't null, takes the output at the send
    // gain. While the effect is off, the frame stays as it is.
    void Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in = {}, const Buffers& bus_out = {});

private:
    // AXFX_CHORUS_EXP_LFO: the LFO's step, depth, phase and interpolation, in 16.16 fixed point. The library also
    // stores the table's address, and a step in samples that nothing reads.
    struct Lfo
    {
        int32_t phase_add = 0;
        int32_t depth_samp = 0;
        uint32_t phase = 0;
        uint32_t sign = 0;
        uint32_t last_num = 0;
        int32_t last_value = 0;
        int32_t grad = 0;
        int32_t grad_factor = 0;
    };

    // The bits of the library's `active` (see DelayExp).
    static constexpr uint32_t kOff = 1;
    static constexpr uint32_t kSkip = 2;

    // Clears the lines and sets the positions for the delay (0x0200be90).
    bool ResetLines(uint32_t rate);

    // __InitParams (0x0200bfb4).
    bool InitParams(uint32_t rate);

    // Frees the lines (0x0200bdcc) and turns the effect off.
    void Off();

    // __CalcLFO (0x0200c450): the LFO's offset of the read position for the next sample.
    int32_t NextLfo();

    Params params_;
    std::array<std::vector<float>, kChannelCount> line_;
    uint32_t in_pos_ = 0;
    uint32_t out_pos_ = 0;  // 16.16
    uint32_t last_pos_ = 0; // 16.16
    uint32_t size_fp_ = 0;  // 16.16
    uint32_t size_ = 0;
    Lfo lfo_;
    std::array<std::array<float, 4>, kChannelCount> history_{};
    uint32_t hist_index_ = 0;
    uint32_t active_ = kOff;
};

} // namespace csf::axfx
