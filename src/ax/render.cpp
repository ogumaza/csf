// SPDX-License-Identifier: MIT

#include "ax/render.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ntdlib/dsp/accelerator.h"
#include "ntdlib/dsp/accumulator.h"

#include "ax/resample.h"

namespace csf::ax
{
namespace
{

// The microcode's results wrap to the 40 bits of the DSP's accumulators (Wrap40). In 40-bit mode, the microcode reads
// an accumulator's middle word saturated to 16 bits (MiddleWord).
using ntdlib::dsp::MiddleWord;
using ntdlib::dsp::Wrap40;

// An accumulator stored as a 32-bit sample: the middle word as MiddleWord reads it, then the low word.
int32_t Store32(int64_t accumulator)
{
    const auto high = static_cast<uint16_t>(MiddleWord(accumulator));
    return static_cast<int32_t>(uint32_t{high} << 16 | static_cast<uint16_t>(accumulator));
}

// How the microcode uses the DSP's accelerator for a voice (ntdlib's model of the accelerator). The accelerator reads
// the voice's samples in order and decodes them: ADPCM, or PCM scaled by the gain. At the end address, the accelerator
// goes back to the loop address. The microcode loads the accelerator's registers from the voice's block at the start of
// the frame (0x0739). At the end, the microcode stores the position and the decoder's state back in the block.
class Accelerator
{
public:
    Accelerator(Pb& pb, const Memory& memory) : pb_(pb), memory_(memory)
    {
        accelerator_.format = static_cast<uint16_t>(pb.format);
        accelerator_.start = pb.loop_address & 0x3FFFFFFF;
        accelerator_.end = pb.end_address & 0x3FFFFFFF;
        accelerator_.current = pb.current_address & 0xBFFFFFFF;
        accelerator_.gain = pb.adpcm.gain;
        accelerator_.pred_scale = pb.adpcm.predictor_scale & 0x7F;
        accelerator_.yn1 = static_cast<uint16_t>(pb.adpcm.yn1);
        accelerator_.yn2 = static_cast<uint16_t>(pb.adpcm.yn2);
    }

    // The next sample. Reaching the end address raises an exception, and its handler runs (AtEnd). The formats that
    // read the accelerator's input register read 0.
    int16_t Next()
    {
        return static_cast<int16_t>(ntdlib::dsp::AcceleratorNext(
            accelerator_, pb_.adpcm.coefs, [this](uint32_t address) { return Byte(address); }, [this] { AtEnd(); }));
    }

    // Stores the position and the decoder's state in the block. The block keeps 7 bits of an ADPCM frame's header.
    void Store()
    {
        pb_.current_address = accelerator_.current;
        pb_.adpcm.predictor_scale = accelerator_.pred_scale & 0x7F;
        pb_.adpcm.yn1 = static_cast<int16_t>(accelerator_.yn1);
        pb_.adpcm.yn2 = static_cast<int16_t>(accelerator_.yn2);
    }

private:
    // A byte of sample data. The extension is the address's top three bits.
    uint8_t Byte(uint32_t address) const
    {
        return memory_.Read8(static_cast<uint32_t>(pb_.extension & 7) << 29 | (address & 0x1FFFFFFF));
    }

    // The exception's handler (0x087E). It stops a voice that doesn't loop. The rest of the frame's reads then give 0.
    // For a voice that loops, the handler loads the decoder's state at the loop start. A stream's history continues
    // instead. The stream's buffer has been refilled.
    void AtEnd()
    {
        if (pb_.loop_flag != 1)
        {
            pb_.state = VoiceState::kStop;
            accelerator_.reads_stopped = true;
        }
        else
        {
            accelerator_.pred_scale = pb_.adpcm_loop.predictor_scale & 0x7F;
            if (pb_.type != VoiceType::kStream)
            {
                accelerator_.yn1 = static_cast<uint16_t>(pb_.adpcm_loop.yn1);
                accelerator_.yn2 = static_cast<uint16_t>(pb_.adpcm_loop.yn2);
            }
        }
    }

    Pb& pb_;
    const Memory& memory_;
    ntdlib::dsp::Accelerator accelerator_;
};

// The frame's samples at the renderer's rate. The ratio advances a 16.16 position. Each output sample comes from the
// last four samples (see resample.h). The block keeps them oldest first.
void Fetch(Pb& pb, Accelerator& accelerator, std::span<int16_t> out)
{
    if (pb.src_mode == SrcMode::kNone)
    {
        for (int16_t& x : out)
        {
            x = accelerator.Next();
        }
        for (std::size_t i = 0; i < 4; ++i)
        {
            pb.src.last[i] = out[out.size() - 4 + i];
        }

        return;
    }

    std::array<int16_t, 4> last = pb.src.last;
    std::size_t oldest = 0;
    uint32_t position = pb.src.frac;
    for (int16_t& x : out)
    {
        position += pb.src.ratio;
        for (uint32_t n = position >> 16; n > 0; --n)
        {
            last[oldest] = accelerator.Next();
            oldest = (oldest + 1) & 3;
        }
        position &= 0xFFFF;

        const std::array<int16_t, 4> ordered = {last[oldest], last[(oldest + 1) & 3], last[(oldest + 2) & 3],
                                                last[(oldest + 3) & 3]};
        x = Resample(pb.src_mode, pb.four_tap_filter, ordered, position);
    }

    pb.src.frac = static_cast<uint16_t>(position);
    for (std::size_t i = 0; i < 4; ++i)
    {
        pb.src.last[i] = last[(oldest + i) & 3];
    }
}

// The volume envelope (0x0269): a 1.15 gain that starts at the block's volume with the first sample and moves by the
// delta each sample. The microcode steps each pair of samples by twice the delta. That step is a 16-bit signed value.
// The product saturates to 16 bits.
void ApplyEnvelope(VolumeRamp& ve, std::span<int16_t> samples)
{
    const auto delta = static_cast<uint16_t>(ve.delta);
    const auto step = static_cast<int16_t>(static_cast<uint16_t>(2 * delta));
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const uint32_t start = i % 2 == 0 ? ve.volume : ve.volume + uint32_t{delta};
        const auto gain = static_cast<uint16_t>(start + static_cast<uint32_t>(static_cast<int32_t>(i / 2) * step));
        samples[i] = MiddleWord(2 * int64_t{samples[i]} * gain);
    }

    ve.volume =
        static_cast<uint16_t>(ve.volume + static_cast<uint32_t>(static_cast<int32_t>(samples.size() / 2) * step));
}

// The one-pole low-pass filter (0x05AF), y = a0 x + b0 y1, with a0 taken as unsigned. Each output saturates to 16 bits
// and feeds back that way.
void ApplyLpf(Lpf& lpf, std::span<int16_t> samples)
{
    if (lpf.on == 0)
    {
        return;
    }

    const auto a0 = static_cast<uint16_t>(lpf.a0);
    int16_t y = lpf.yn1;
    for (int16_t& x : samples)
    {
        y = MiddleWord(2 * int64_t{x} * a0 + 2 * int64_t{y} * lpf.b0);
        x = y;
    }

    lpf.yn1 = y;
}

// The biquad filter (0x05CC), with coefficients in 2.14. The sum is shifted left two places, rounded to a whole sample
// (ties to even) and saturated to 16 bits. Each output feeds back that way.
void ApplyBiquad(Biquad& biquad, std::span<int16_t> samples)
{
    if (biquad.on == 0)
    {
        return;
    }

    for (int16_t& x : samples)
    {
        const int64_t sum = int64_t{biquad.b0} * x + int64_t{biquad.b1} * biquad.xn1 + int64_t{biquad.b2} * biquad.xn2 +
                            int64_t{biquad.a1} * biquad.yn1 + int64_t{biquad.a2} * biquad.yn2;
        const int64_t shifted = Wrap40(sum * 4);
        const int64_t rounded = Wrap40(((shifted & 0x10000) ? shifted + 0x8000 : shifted + 0x7FFF) & ~int64_t{0xFFFF});
        const int16_t y = MiddleWord(rounded);

        biquad.xn2 = biquad.xn1;
        biquad.xn1 = x;
        biquad.yn2 = biquad.yn1;
        biquad.yn1 = y;
        x = y;
    }
}

// Adds the samples into one bus channel (0x0796 and 0x07B1). Each product is rounded down to a whole sample. The volume
// is constant, or it starts at the block's volume with the first sample and moves by the delta each sample.
void Mix(std::span<const int16_t> samples, VolumeRamp& mix, bool ramp, Buses::Channel& bus)
{
    const auto delta = static_cast<uint16_t>(mix.delta);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto gain = ramp ? static_cast<uint16_t>(mix.volume + i * delta) : mix.volume;
        bus[i] += static_cast<int32_t>((2 * int64_t{samples[i]} * gain) >> 16);
    }

    if (ramp)
    {
        mix.volume = static_cast<uint16_t>(mix.volume + samples.size() * delta);
    }
}

// A volume that goes from `from` to `to` across a frame: one gain for each sample (0x04C5). Each step adds 683 65536ths
// of the change, or 682 for every third step. The first sample already has one step. The last sample gets `to`. The
// 48 kHz microcode steps by 455 65536ths each time.
std::array<uint16_t, kMaxSamplesPerFrame> Ramp(uint16_t from, uint16_t to, std::size_t samples)
{
    std::array<uint16_t, kMaxSamplesPerFrame> gains{};
    const auto change = static_cast<int16_t>(static_cast<uint16_t>(to - from));
    int64_t sum = int64_t{from} * 0x10000;
    for (std::size_t i = 0; i + 1 < samples; ++i)
    {
        const int step = samples == kSamplesPerFrame48k ? 455 : i % 3 == 2 ? 682 : 683;
        sum += int64_t{change} * step;
        gains[i] = static_cast<uint16_t>(sum >> 16);
    }
    gains[samples - 1] = to;

    return gains;
}

} // namespace

void RenderVoice(Pb& pb, const Memory& memory, int samples, Buses& buses)
{
    if (pb.state != VoiceState::kRun)
    {
        return;
    }

    std::array<int16_t, kMaxSamplesPerFrame> buffer;
    const std::span<int16_t> x(buffer.data(), static_cast<std::size_t>(samples));
    Accelerator accelerator(pb, memory);
    Fetch(pb, accelerator, x);
    accelerator.Store();
    ApplyEnvelope(pb.ve, x);
    ApplyLpf(pb.lpf, x);
    ApplyBiquad(pb.biquad, x);

    // Into each bus and channel the voice's masks name: two bits each, both set for a volume that moves.
    for (int bus = 0; bus < kBusCount; ++bus)
    {
        for (int channel = 0; channel < kTvChannels; ++channel)
        {
            const int bits = (pb.tv_mask[bus] >> (2 * channel)) & 3;
            if (bits != 0)
            {
                Mix(x, pb.tv_mix[channel][bus], bits == 3, buses.tv[bus][channel]);
            }
        }
    }
    for (int drc = 0; drc < kDrcCount; ++drc)
    {
        for (int bus = 0; bus < kBusCount; ++bus)
        {
            for (int channel = 0; channel < kDrcChannels; ++channel)
            {
                const int bits = (pb.drc_mask[drc][bus] >> (2 * channel)) & 3;
                if (bits != 0)
                {
                    Mix(x, pb.drc_mix[drc][channel][bus], bits == 3, buses.drc[drc][bus][channel]);
                }
            }
        }
    }
}

void ReturnAux(std::span<const int32_t> returned, uint16_t from, uint16_t to, std::span<int32_t> main)
{
    const auto gains = Ramp(from, to, main.size());
    for (std::size_t i = 0; i < main.size(); ++i)
    {
        main[i] = Store32(int64_t{main[i]} + ((int64_t{returned[i]} * gains[i]) >> 15));
    }
}

void OutputBus(std::span<const int32_t> bus, uint16_t from, uint16_t to, std::span<int32_t> out)
{
    const auto gains = Ramp(from, to, out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = Store32(Wrap40(((int64_t{bus[i]} * gains[i]) >> 15) * 256));
    }
}

} // namespace csf::ax
