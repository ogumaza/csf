// SPDX-License-Identifier: MIT

// Checks CSF's model of nw::snd: its voices on AX, and the engine on a small sound archive. ntdlib's tests check the
// arithmetic helpers, the envelope and the LFO.

#include <cstdint>
#include <vector>

#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "ax/memory.h"
#include "ax/system.h"
#include "csf/sar.h"
#include "nwsnd/ax_voice.h"
#include "nwsnd/engine.h"

namespace fsar_files = ntdlib::testing::sar_files;

using xsflib::testing::Check;

namespace
{

using csf::nwsnd::AxVoice;
using csf::nwsnd::AxVoices;
using csf::nwsnd::OutputMode;
using csf::nwsnd::WaveBuffer;

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
    XSFLIB_CHECK(pb.state == csf::ax::VoiceState::kRun && buffer.status == WaveBuffer::Status::kPlay);
    XSFLIB_CHECK(pb.loop_flag == 1 && pb.current_address == 0x10000000 / 2 && pb.end_address == 0x10000000 / 2 + 99);
    XSFLIB_CHECK(pb.ve.volume == 0x4000 && pb.ve.delta == 0);
    XSFLIB_CHECK(pb.src.ratio == 0x18000 && pb.src_mode == csf::ax::SrcMode::kFourTap && pb.four_tap_filter == 0);
    XSFLIB_CHECK(pb.tv_mix[0][0].volume == 0x8000 && pb.tv_mix[1][0].volume == 0x2000 && pb.tv_mix[2][0].volume == 0);

    // A change of volume ramps across the next frame, from where the last ramp reached.
    voice->Param().volume = 1.0f;

    system.RenderFrame();
    voices.UpdateAllVoices(kStereo);

    XSFLIB_CHECK(pb.ve.volume == 0x4000 && pb.ve.delta == 0x4000 / 96);

    // A pitch change moves the ratio, but the filter chosen at the start stays.
    voice->Param().pitch = 0.5f;

    system.RenderFrame();
    voices.UpdateAllVoices(kStereo);

    XSFLIB_CHECK(pb.src.ratio == 0xC000 && pb.four_tap_filter == 0);

    // Freeing the voice frees its AX voice and finishes its buffer.
    voices.Free(*voice);
    XSFLIB_CHECK(!voice->IsAvailable() && buffer.status == WaveBuffer::Status::kDone);
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
    XSFLIB_CHECK(first.status == WaveBuffer::Status::kDone && second.status == WaveBuffer::Status::kPlay);
    XSFLIB_CHECK(pb.end_address == 0x10001000 / 2 + 299 && pb.loop_flag == 0);
    XSFLIB_CHECK(system.VoiceBuses().tv[0][0][0] == 2000);
}

// The engine plays a sequence from a small archive: at a timebase of 48 and 120 beats a minute, a note of 24 ticks
// (250 ms) on a PCM16 wave of 100 ms, then the end. The note starts in the second frame. The first frame takes the
// start to the sound thread. AX sets the note's voice up at the end of the third frame and plays it from the fourth.
// The sequence ends after its 24 ticks.
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
    const std::vector<uint8_t> kBytecode = {0xB0, 48, 0xE1, 0x00, 120, 60, 127, 24, 0xFF};
    spec.files = {{fsar_files::Sequence(true, kBytecode, {{"Note", 0}}), "", false},
                  {fsar_files::Bank(true, bank), "", false},
                  {fsar_files::WaveArchive(true, {fsar_files::Wave(true, wave)}), "", false}};
    const csf::fsar::SoundArchive archive =
        csf::fsar::SoundArchive::Load(fsar_files::Archive(true, spec), csf::kSarOptions);

    csf::nwsnd::ArchiveModel model(archive);
    csf::nwsnd::Engine& engine = *model.engine;

    XSFLIB_CHECK(engine.StartSequence(0) != nullptr);
    XSFLIB_CHECK(engine.StartSequence(1) == nullptr);

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
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 0);
    engine.RunFrame();
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 1);
    engine.RunFrame();
    XSFLIB_CHECK(!sounds());
    engine.RunFrame();
    XSFLIB_CHECK(sounds());

    // The sequence ends at its 24th tick, about 250 ms after it started. The bank's default release is the fastest. So
    // the note ends with the sequence.
    int frames = 4;
    while (engine.IsBusy() && frames < 1000)
    {
        engine.RunFrame();
        ++frames;
    }

    XSFLIB_CHECK(!engine.IsBusy() && frames > 80 && frames <= 90);
    XSFLIB_CHECK(engine.Channels().ActiveCount() == 0 && engine.Voices().ActiveCount() == 0);
}

} // namespace

int main()
{
    TestAxVoice();
    TestWaveBufferQueue();
    TestEngine();

    return xsflib::testing::Summary("nwsnd");
}
