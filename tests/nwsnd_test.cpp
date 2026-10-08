// SPDX-License-Identifier: MIT

// Checks CSF's model of nw::snd: its arithmetic helpers against the tables they read and against what the library's
// code computes for chosen inputs, and the engine on a small sound archive.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ax/memory.h"
#include "ax/system.h"
#include "check.h"
#include "fsar/formats.h"
#include "fsar_files.h"
#include "nwsnd/ax_voice.h"
#include "nwsnd/engine.h"
#include "nwsnd/envelope.h"
#include "nwsnd/tables.h"
#include "nwsnd/util.h"

namespace
{

using csf::nwsnd::AxVoice;
using csf::nwsnd::AxVoices;
using csf::nwsnd::CurveAdshr;
using csf::nwsnd::CurveLfo;
using csf::nwsnd::OutputMode;
using csf::nwsnd::PanInfo;
using csf::nwsnd::WaveBuffer;
namespace tables = csf::nwsnd::tables;
namespace util = csf::nwsnd::util;

void TestTables()
{
    // The tables hold what their names say at the points where that's exact.
    CSF_CHECK(tables::kSemitone[0] == 1.0f && tables::kFine[0] == 1.0f && tables::kDecibel[904] == 1.0f);
    CSF_CHECK(tables::kSin[64][0] == 1.0f && tables::kSin[0][2] == 1.0f && tables::kSin[256][0] == tables::kSin[0][0]);
    CSF_CHECK(tables::kLpfFreq[0] == 80 && tables::kLpfFreq[23] == 16000);
}

void TestPitch()
{
    // Octaves double and halve exactly, and semitones and 1/256 semitones come from the tables.
    CSF_CHECK(util::CalcPitchRatio(0) == 1.0f);
    CSF_CHECK(util::CalcPitchRatio(3072) == 2.0f);
    CSF_CHECK(util::CalcPitchRatio(-3 * 3072) == 0.125f);
    CSF_CHECK(util::CalcPitchRatio(7 * 256 + 5) == tables::kSemitone[7] * tables::kFine[5]);
    CSF_CHECK(util::CalcPitchRatio(3072 + 256) == 2.0f * tables::kSemitone[1]);
    CSF_CHECK(util::CalcPitchRatio(-1) == 0.5f * tables::kSemitone[11] * tables::kFine[255]);
}

void TestVolume()
{
    // Steps of 0.1 dB from -90.4 dB, truncated, and held at both ends.
    CSF_CHECK(util::CalcVolumeRatio(0.0f) == 1.0f);
    CSF_CHECK(util::CalcVolumeRatio(-6.05f) == tables::kDecibel[904 - 60]);
    CSF_CHECK(util::CalcVolumeRatio(100.0f) == tables::kDecibel[964]);
    CSF_CHECK(util::CalcVolumeRatio(-200.0f) == tables::kDecibel[0]);
}

void TestPan()
{
    // The square-root curve's centre is sqrt(1/2), or unity when the curve is scaled to its centre.
    PanInfo info;
    CSF_CHECK(util::CalcPanRatio(0.0f, info, OutputMode::kStereo) == tables::kStereoPan[0][128]);
    info.center_zero_db = true;
    CSF_CHECK(util::CalcPanRatio(0.0f, info, OutputMode::kStereo) == 1.0f);

    // A pan beyond either end is held there.
    info = {};
    CSF_CHECK(util::CalcPanRatio(-5.0f, info, OutputMode::kStereo) == tables::kStereoPan[0][0]);
    CSF_CHECK(util::CalcPanRatio(5.0f, info, OutputMode::kStereo) == tables::kStereoPan[0][256]);

    // Surround output takes the surround curves unless the pan bypasses them.
    info.curve = 1;
    CSF_CHECK(util::CalcPanRatio(0.25f, info, OutputMode::kSurround) == tables::kSurroundPan[1][160]);
    info.front_bypass = true;
    CSF_CHECK(util::CalcPanRatio(0.25f, info, OutputMode::kSurround) == tables::kStereoPan[1][160]);

    // A surround pan runs from the front at 0 to the rear at 2, on the stereo curves.
    info = {};
    CSF_CHECK(util::CalcSurroundPanRatio(1.0f, info) == tables::kStereoPan[0][128]);
    CSF_CHECK(util::CalcSurroundPanRatio(3.0f, info) == tables::kStereoPan[0][256]);
}

void TestLpfAndRandom()
{
    // Low-pass values step by a thirtieth from 0.13561438 to 0.9.
    CSF_CHECK(util::CalcLpfFreq(1.0f) == 16000 && util::CalcLpfFreq(2.0f) == 16000);
    CSF_CHECK(util::CalcLpfFreq(0.0f) == 80 && util::CalcLpfFreq(-1.0f) == 80);
    CSF_CHECK(util::CalcLpfFreq(0.5f) == tables::kLpfFreq[10]);

    // The random numbers are a linear congruential generator's top 16 bits.
    csf::nwsnd::Random random;
    const uint32_t first = 0x12345678u * 0x19660Du + 0x3C6EF35Fu;
    const uint32_t second = first * 0x19660Du + 0x3C6EF35Fu;
    CSF_CHECK(random.Next() == first >> 16);
    CSF_CHECK(random.Next() == second >> 16);
}

void TestEnvelope()
{
    // A new envelope has the fastest attack, and reads 0 dB at once.
    CurveAdshr envelope;
    CSF_CHECK(envelope.GetValue() == 0.0f);

    // An attack multiplies the level by its entry each millisecond until it's within 1/32 of 0.1 dB of 0.
    envelope.SetAttack(100);
    envelope.SetHold(3);
    envelope.Reset(CurveAdshr::kInitialDecibel);
    envelope.Update(1);
    const float first = CurveAdshr::kInitialDecibel * 10.0f * tables::kAttack[100];
    CSF_CHECK(envelope.GetValue() == first / 10.0f);
    envelope.Update(10000);
    CSF_CHECK(envelope.GetStatus() == CurveAdshr::Status::kHold && envelope.GetValue() == 0.0f);

    // The hold lasts (3 + 1)^2 / 4 ms. Then the decay falls by its rate each millisecond, in one fused step.
    envelope.SetDecay(10);
    envelope.SetSustain(64);
    envelope.Update(3);
    CSF_CHECK(envelope.GetStatus() == CurveAdshr::Status::kHold);
    envelope.Update(3);
    const float rate = 21.0f * 0.0078125f / 5.0f;
    CSF_CHECK(envelope.GetStatus() == CurveAdshr::Status::kDecay &&
              envelope.GetValue() == std::fma(-rate, 2.0f, 0.0f) / 10.0f);

    // The decay stops at the sustain level.
    envelope.SetDecay(126);
    envelope.Update(1000);
    CSF_CHECK(envelope.GetStatus() == CurveAdshr::Status::kSustain &&
              envelope.GetValue() == static_cast<float>(tables::kDecibelSquare[64]) / 10.0f);

    // A release falls the same way, from wherever the level is.
    const float sustain = static_cast<float>(tables::kDecibelSquare[64]);
    envelope.SetRelease(100);
    envelope.SetStatus(CurveAdshr::Status::kRelease);
    envelope.Update(10);
    CSF_CHECK(envelope.GetValue() == std::fma(-(60.0f / 26.0f / 5.0f), 10.0f, sustain) / 10.0f);
}

void TestLfo()
{
    // An LFO without depth gives nothing.
    csf::nwsnd::Random random;
    CurveLfo lfo;
    lfo.Reset();
    lfo.Update(100);
    CSF_CHECK(lfo.GetValue(random) == 0.0f);

    // A sine at one cycle a second peaks a quarter of a second in, after its delay.
    lfo.param.depth = 0.5f;
    lfo.param.range = 2;
    lfo.param.speed = 1.0f;
    lfo.param.delay = 100;
    lfo.Reset();
    lfo.Update(60);
    CSF_CHECK(lfo.GetValue(random) == 0.0f);
    lfo.Update(290);
    CSF_CHECK(lfo.GetValue(random) == 1.0f);

    // The other curves at the same place, and the triangle's other lines.
    lfo.param.curve = 1;
    CSF_CHECK(lfo.GetValue(random) == 1.0f);
    lfo.param.curve = 2;
    CSF_CHECK(lfo.GetValue(random) == 0.25f);
    lfo.param.curve = 3;
    CSF_CHECK(lfo.GetValue(random) == 0.0f);
    lfo.param.curve = 1;
    lfo.Update(500);
    CSF_CHECK(lfo.GetValue(random) == -1.0f);
    lfo.param.curve = 3;
    CSF_CHECK(lfo.GetValue(random) == 1.0f);
    lfo.param.curve = 9;
    CSF_CHECK(lfo.GetValue(random) == 1.0f);

    // The random curve draws while the cycle has just wrapped, and holds the number until the next wrap.
    csf::nwsnd::Random expected;
    lfo.param.curve = 4;
    lfo.param.delay = 0;
    lfo.Reset();
    lfo.Update(500);
    CSF_CHECK(lfo.GetValue(random) == 1.0f);
    lfo.Update(500);
    const float drawn = static_cast<float>(expected.Next()) / 65535.0f;
    CSF_CHECK(lfo.GetValue(random) == drawn);
    lfo.Update(100);
    CSF_CHECK(lfo.GetValue(random) == drawn);

    // A phase starts the cycle part of the way through.
    lfo.param.curve = 2;
    lfo.param.phase = 127;
    lfo.Reset();
    lfo.Update(250);
    CSF_CHECK(lfo.GetValue(random) == 0.25f);
}

// A constant PCM16 wave of `count` samples.
std::vector<uint8_t> ConstantWave(int count, int16_t value)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < count; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8));
        bytes.push_back(static_cast<uint8_t>(value));
    }

    return bytes;
}

void TestAxVoice()
{
    // A voice set up to play a buffer starts its AX voice with the buffer's offsets, the volume, the TV's mix and the
    // ratio. The ratio chooses the 4-tap filter.
    const std::vector<uint8_t> wave = ConstantWave(100, 1000);
    csf::ax::Memory memory;
    memory.Map(0x10000000, wave);
    csf::ax::System system(memory);
    AxVoices voices(system, memory);
    constexpr std::array<OutputMode, 2> kStereo = {OutputMode::kStereo, OutputMode::kStereo};
    AxVoice* voice = voices.AllocVoice(64, {});
    if (!Check(voice != nullptr, "an AX voice is acquired"))
    {
        return;
    }
    WaveBuffer buffer;
    buffer.address = 0x10000000;
    buffer.sample_length = 100;
    buffer.loop = true;
    voice->SetSampleFormat(csf::nwsnd::SampleFormat::kPcm16);
    voice->SetSampleRate(48000);
    voice->Param().volume = 0.5f;
    voice->Param().tv[0][0] = 1.0f;
    voice->Param().tv[0][1] = 0.25f;
    voice->AppendWaveBuffer(buffer);
    voice->SetState(AxVoice::State::kPlay);

    voices.UpdateAllVoices(kStereo);

    const csf::ax::Pb& pb = voice->SdkVoice()->game;
    CSF_CHECK(pb.state == csf::ax::VoiceState::kRun && buffer.status == WaveBuffer::Status::kPlay);
    CSF_CHECK(pb.loop_flag == 1 && pb.current_address == 0x10000000 / 2 && pb.end_address == 0x10000000 / 2 + 99);
    CSF_CHECK(pb.ve.volume == 0x4000 && pb.ve.delta == 0);
    CSF_CHECK(pb.src.ratio == 0x18000 && pb.src_mode == csf::ax::SrcMode::kFourTap && pb.four_tap_filter == 0);
    CSF_CHECK(pb.tv_mix[0][0].volume == 0x8000 && pb.tv_mix[1][0].volume == 0x2000 && pb.tv_mix[2][0].volume == 0);

    // A change of volume ramps across the next frame, from where the last ramp reached.
    voice->Param().volume = 1.0f;
    system.RenderFrame();
    voices.UpdateAllVoices(kStereo);
    CSF_CHECK(pb.ve.volume == 0x4000 && pb.ve.delta == 0x4000 / 96);

    // A pitch change moves the ratio, but the filter chosen at the start stays.
    voice->Param().pitch = 0.5f;
    system.RenderFrame();
    voices.UpdateAllVoices(kStereo);
    CSF_CHECK(pb.src.ratio == 0xC000 && pb.four_tap_filter == 0);

    // Freeing the voice frees its AX voice and finishes its buffer.
    voices.Free(*voice);
    CSF_CHECK(!voice->IsAvailable() && buffer.status == WaveBuffer::Status::kDone);
}

void TestWaveBufferQueue()
{
    // A voice moves on to the buffer queued behind the one it's playing. The queued buffer lies past the first buffer's
    // end. The voice moves on when the loop count shows that it has reached the queued buffer. It then plays that
    // buffer to its end.
    const std::vector<uint8_t> first_wave = ConstantWave(100, 1000);
    const std::vector<uint8_t> second_wave = ConstantWave(300, 2000);
    csf::ax::Memory memory;
    memory.Map(0x10000000, first_wave);
    memory.Map(0x10001000, second_wave);
    csf::ax::System system(memory);
    AxVoices voices(system, memory);
    constexpr std::array<OutputMode, 2> kStereo = {OutputMode::kStereo, OutputMode::kStereo};
    AxVoice* voice = voices.AllocVoice(64, {});
    if (!Check(voice != nullptr, "an AX voice is acquired"))
    {
        return;
    }
    WaveBuffer first;
    first.address = 0x10000000;
    first.sample_length = 100;
    WaveBuffer second;
    second.address = 0x10001000;
    second.sample_length = 300;
    voice->Param().tv[0][0] = 1.0f;
    voice->Param().interpolation = 1; // linear: it passes a constant through unchanged
    voice->AppendWaveBuffer(first);
    voice->AppendWaveBuffer(second);
    voice->SetState(AxVoice::State::kPlay);

    for (int frame = 0; frame < 4; ++frame)
    {
        voices.UpdateAllVoices(kStereo);
        system.RenderFrame();
    }

    const csf::ax::Pb& pb = voice->SdkVoice()->game;
    CSF_CHECK(first.status == WaveBuffer::Status::kDone && second.status == WaveBuffer::Status::kPlay);
    CSF_CHECK(pb.end_address == 0x10001000 / 2 + 299 && pb.loop_flag == 0);
    CSF_CHECK(system.VoiceBuses().tv[0][0][0] == 2000);
}

// The engine plays a sequence from a small archive: at a timebase of 48 and 120 beats a minute, a note of 24 ticks (250
// ms) on a PCM16 wave of 100 ms, then the end. The note starts in the second frame. The first frame takes the start to
// the sound thread. AX sets the note's voice up at the end of the third frame and plays it from the fourth. The
// sequence ends after its 24 ticks.
void TestEngine()
{
    std::vector<uint8_t> samples(2 * 3200);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        samples[i] = 0x10; // 0x1000, big-endian
    }
    fsar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.loop_end = 3200;
    wave.channels = {samples};

    fsar_files::Region region;
    region.original_key = 60;
    fsar_files::VelocityTable velocities;
    velocities.entries = {{0, region}};
    fsar_files::KeyTable keys;
    keys.entries = {{0, velocities}};
    fsar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};
    bank.instruments = {keys};

    fsar_files::SoundSpec sound;
    sound.name = "SEQ_NOTE";
    sound.volume = 127;
    sound.banks = {0};
    fsar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_NOTE", 1}};
    spec.wave_archives = {{"WARC_NOTE", 2}};
    spec.players = {{"PLAYER_NOTE", 1}};
    const std::vector<uint8_t> bytecode = {0xB0, 48, 0xE1, 0x00, 120, 60, 127, 24, 0xFF};
    spec.files = {{fsar_files::Sequence(true, bytecode, {{"Note", 0}}), "", false},
                  {fsar_files::Bank(true, bank), "", false},
                  {fsar_files::WaveArchive(true, {fsar_files::Wave(true, wave)}), "", false}};
    const csf::fsar::SoundArchive archive = csf::fsar::SoundArchive::Load(fsar_files::Archive(true, spec));

    csf::nwsnd::ArchiveModel model(archive);
    csf::nwsnd::Engine& engine = *model.engine;
    CSF_CHECK(engine.StartSequence(0) != nullptr);
    CSF_CHECK(engine.StartSequence(1) == nullptr);

    const auto sounds = [&model]
    {
        for (const int16_t sample : model.system.Output())
        {
            if (sample != 0)
            {
                return true;
            }
        }
        return false;
    };
    engine.RunFrame();
    CSF_CHECK(engine.Channels().ActiveCount() == 0);
    engine.RunFrame();
    CSF_CHECK(engine.Channels().ActiveCount() == 1);
    engine.RunFrame();
    CSF_CHECK(!sounds());
    engine.RunFrame();
    CSF_CHECK(sounds());

    // The sequence ends at its 24th tick, about 250 ms after it started. The bank's default release is the fastest. So
    // the note ends with the sequence.
    int frames = 4;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }
    CSF_CHECK(!engine.IsBusy() && frames > 80 && frames <= 90);
    CSF_CHECK(engine.Channels().ActiveCount() == 0 && engine.Voices().ActiveCount() == 0);
}

} // namespace

int main()
{
    TestTables();
    TestPitch();
    TestVolume();
    TestPan();
    TestLpfAndRandom();
    TestEnvelope();
    TestLfo();
    TestAxVoice();
    TestWaveBufferQueue();
    TestEngine();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
