// SPDX-License-Identifier: MIT

// AXFXDelayExp, the system library's delay for an aux bus (0x02010978 to 0x020113fc). Each channel runs through a
// one-pole low-pass into a delay line with feedback. The frame takes the line's output at the output gain. The
// arithmetic is in integers, with the gains in 128ths. NintendoWare's FxDelay runs this delay.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "axfx/axfx.h"

namespace csf::axfx
{

// AXFX_DELAY_EXP. A copy of a DelayExp holds the whole state, the delay lines included.
class DelayExp
{
public:
    // AXFX_DELAY_EXP's parameters: the longest delay that the lines hold and the delay, in ms; the feedback, below 1;
    // the low-pass's cutoff, 0 to 1, where 1 passes everything; and the gains of the output and of the send to busOut,
    // 0 to 1.
    struct Params
    {
        float max_delay = 0.0f;
        float delay = 0.0f;
        float feedback = 0.0f;
        float lpf = 0.0f;
        float out_gain = 0.0f;
        float send_gain = 0.0f;
    };

    // AXFXDelayExpGetMemSize (0x02010978): the bytes of the three lines for `params` at the renderer's `rate`.
    static uint32_t GetMemSize(const Params& params, uint32_t rate);

    // AXFXDelayExpInit (0x02010e24): allocates the lines for the longest delay, clears them and sets the delay up.
    // Returns false for a parameter out of its range. The effect then stays off. After a successful Init, the next
    // Process leaves its frame as it is.
    bool Init(const Params& params, uint32_t rate);

    // AXFXDelayExpSettings (0x02010f78): Shutdown, then Init.
    bool Settings(const Params& params, uint32_t rate);

    // AXFXDelayExpSettingsUpdate (0x0201101c): sets the delay up for new parameters in the lines that Init allocated,
    // and clears them (__InitParams). After it succeeds, the next Process leaves its frame as it is. After it fails,
    // the effect is off.
    bool SettingsUpdate(const Params& params, uint32_t rate);

    // AXFXDelayExpShutdown (0x02010a88): frees the lines and turns the effect off.
    void Shutdown();

    // AXFXDelayExpCallback (0x020110b8): runs `samples` samples of `frame` through the delay, in place. A sample of
    // `bus_in`, where it isn't null, adds to the input. `bus_out`, where it isn't null, takes the line's output at the
    // send gain. While the effect is off, the frame stays as it is.
    void Process(const Buffers& frame, uint32_t samples, const Buffers& bus_in = {}, const Buffers& bus_out = {});

    // The words of AXFX_DELAY_EXP from +0x0C to +0x34, in order, for checks against the library's structure.
    struct State
    {
        uint32_t position = 0;
        uint32_t length = 0;
        uint32_t max_length = 0;
        int32_t feedback_gain = 0;
        std::array<int32_t, kChannelCount> last{};
        int32_t lpf_coef = 0;
        int32_t out_gain = 0;
        int32_t send_gain = 0;
        uint32_t active = 0;
    };

    State GetState() const
    {
        return {position_, length_, max_length_, feedback_gain_, last_, lpf_coef_, out_gain_, send_gain_, active_};
    }

private:
    // The bits of the library's `active`: kOff while the effect is off. Init sets it, and a successful Init clears it.
    // kSkip: the next Process leaves its frame as it is and clears the bit.
    static constexpr uint32_t kOff = 1;
    static constexpr uint32_t kSkip = 2;

    // __InitParams (0x02010b90).
    bool InitParams(uint32_t rate);

    // Frees the lines (0x020109f4) and turns the effect off.
    void Off();

    Params params_;
    std::array<std::vector<int32_t>, kChannelCount> line_;
    uint32_t position_ = 0;
    uint32_t length_ = 0;     // the delay in samples: the position wraps there
    uint32_t max_length_ = 0; // the lines' length
    int32_t feedback_gain_ = 0;
    std::array<int32_t, kChannelCount> last_{}; // the low-pass's last output
    int32_t lpf_coef_ = 0;
    int32_t out_gain_ = 0;
    int32_t send_gain_ = 0;
    uint32_t active_ = kOff;
};

} // namespace csf::axfx
