// SPDX-License-Identifier: MIT

// A voice's parameter block (AXPB): what a game's calls set, and what the DSP reads, updates and mixes from. AX keeps
// two copies of each voice's block: the game's copy and the renderer's copy. Once a frame, AX copies the changes
// between them (see System). CSF keeps the fields as values rather than the console's 0x2C0 big-endian bytes. No game
// reads the block.

#pragma once

#include <array>
#include <cstdint>

namespace csf::ax
{

// AX has 96 voices and 32 priorities: 0 marks a free voice, and 31 is the highest.
constexpr int kVoiceCount = 96;
constexpr uint32_t kPriorityFree = 0;
constexpr uint32_t kPriorityMax = 31;

// A frame is 3 ms: 96 samples with the 32 kHz renderer and 144 with the 48 kHz one. The devices always run at 48 kHz.
constexpr int kSamplesPerFrame32k = 96;
constexpr int kSamplesPerFrame48k = 144;
constexpr int kMaxSamplesPerFrame = 144;
constexpr int kOutputSamplesPerFrame = 144;

// The devices and their channels. Each device has a main bus and three aux buses, A, B and C.
constexpr int kBusCount = 4;
constexpr int kTvChannels = 6;  // left, right, left surround, right surround, centre, LFE
constexpr int kDrcCount = 2;    // the GamePads
constexpr int kDrcChannels = 4; // left, right, left surround, right surround

// A gain of 1.0 in a 1.15 volume.
constexpr uint16_t kUnityVolume = 0x8000;

enum class SampleFormat : uint16_t
{
    kAdpcm = 0x00,
    kPcm16 = 0x0A,
    kPcm8 = 0x19,
};

// How the renderer converts the sample rate. AXSetVoiceSrcType's types 0 to 4 are none, linear and the three 4-tap
// filters.
enum class SrcMode : uint16_t
{
    kFourTap = 0,
    kLinear = 1,
    kNone = 2,
};

enum class VoiceState : uint16_t
{
    kStop = 0,
    kRun = 1,
};

// A streaming voice's buffer is refilled as it plays. Its ADPCM history isn't reloaded at its loop start.
enum class VoiceType : uint16_t
{
    kNormal = 0,
    kStream = 1,
};

// The sync word's bits: which parts of the game's copy changed since the last frame. Those parts are copied to the
// renderer's copy at the next frame.
namespace sync
{

constexpr uint32_t kSrcType = 0x00000001;
constexpr uint32_t kMixMasks = 0x00000002;
constexpr uint32_t kState = 0x00000004;
constexpr uint32_t kType = 0x00000008;
constexpr uint32_t kMix = 0x00000010;
constexpr uint32_t kItd = 0x00000020;
constexpr uint32_t kItdTarget = 0x00000040;
constexpr uint32_t kVe = 0x00000100;
constexpr uint32_t kVeDelta = 0x00000200;
constexpr uint32_t kAddresses = 0x00000400;
constexpr uint32_t kLoopFlag = 0x00000800;
constexpr uint32_t kLoopAddress = 0x00001000;
constexpr uint32_t kEndAddress = 0x00002000;
constexpr uint32_t kCurrentAddress = 0x00004000;
constexpr uint32_t kAdpcm = 0x00008000;
constexpr uint32_t kSrc = 0x00010000;
constexpr uint32_t kSrcRatio = 0x00020000;
constexpr uint32_t kAdpcmLoop = 0x00040000;
constexpr uint32_t kLpf = 0x00080000;
constexpr uint32_t kLpfCoefs = 0x00100000;
constexpr uint32_t kBiquad = 0x00200000;
constexpr uint32_t kBiquadCoefs = 0x00400000;
constexpr uint32_t kRmtOn = 0x00800000;
constexpr uint32_t kRmtSrc = 0x08000000;
constexpr uint32_t kRmtIir = 0x10000000;

} // namespace sync

// A volume and its change per sample, both 1.15 (AXPBVE, and each entry of a device mix).
struct VolumeRamp
{
    uint16_t volume = 0;
    int16_t delta = 0;
};

// The ADPCM decoder's state (AXPBADPCM): eight coefficient pairs in 4.11, the gain, the predictor and scale of the
// current frame (bits 4-6 and 0-3) and the last two samples decoded.
struct Adpcm
{
    std::array<int16_t, 16> coefs{};
    uint16_t gain = 0;
    uint16_t predictor_scale = 0;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
};

// The decoder's state at the loop start (AXPBADPCMLOOP).
struct AdpcmLoop
{
    uint16_t predictor_scale = 0;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
};

// Sample-rate conversion (AXPBSRC): the ratio in 16.16 (input samples per output sample), the fractional position in
// 0.16 and the last four input samples.
struct Src
{
    uint32_t ratio = 0;
    uint16_t frac = 0;
    std::array<int16_t, 4> last{};
};

// The one-pole low-pass filter (AXPBLPF), y = a0 x + b0 y1 with coefficients in 1.15.
struct Lpf
{
    uint16_t on = 0;
    int16_t yn1 = 0;
    int16_t a0 = 0;
    int16_t b0 = 0;
};

// The biquad filter (AXPBBIQUAD), with coefficients in 2.14. a1 and a2 are added. They're the negatives of the textbook
// feedback coefficients.
struct Biquad
{
    uint16_t on = 0;
    int16_t xn1 = 0;
    int16_t xn2 = 0;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
    int16_t b0 = 0;
    int16_t b1 = 0;
    int16_t b2 = 0;
    int16_t a1 = 0;
    int16_t a2 = 0;
};

struct Pb
{
    SrcMode src_mode = SrcMode::kFourTap;
    uint16_t four_tap_filter = 0; // 0, 1, 2: the 8, 12 and 16 kHz filters
    uint16_t mixer_select = 0;
    VoiceType type = VoiceType::kNormal;

    // Each bus's mix mask has two bits per channel, channel c at bit 2c: 0 off, 1 the volume, 3 the volume and its
    // delta. The mixes are indexed [channel][bus].
    std::array<uint16_t, kBusCount> tv_mask{};
    std::array<std::array<uint16_t, kBusCount>, kDrcCount> drc_mask{};
    std::array<std::array<VolumeRamp, kBusCount>, kTvChannels> tv_mix{};
    std::array<std::array<std::array<VolumeRamp, kBusCount>, kDrcChannels>, kDrcCount> drc_mix{};
    uint16_t rmt_on = 0;

    VoiceState state = VoiceState::kStop;
    VolumeRamp ve;

    // The addresses count in the format's units: bytes for PCM8, 16-bit words for PCM16 and nibbles for ADPCM. ADPCM's
    // 8-byte frames are a header byte and 14 samples. The addresses are absolute, without the byte address's top three
    // bits. Those bits are the extension. The end address is the last sample played.
    uint16_t loop_flag = 0;
    SampleFormat format = SampleFormat::kAdpcm;
    uint16_t extension = 0;
    uint32_t loop_address = 0;
    uint32_t end_address = 0;
    uint32_t current_address = 0;

    Adpcm adpcm;
    Src src;
    AdpcmLoop adpcm_loop;
    Lpf lpf;
    Biquad biquad;
};

} // namespace csf::ax
