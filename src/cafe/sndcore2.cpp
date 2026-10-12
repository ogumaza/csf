// SPDX-License-Identifier: MIT

#include "cafe/sndcore2.h"

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace csf::cafe
{
namespace
{

// A voice's record, and what sndcore2 keeps in it.
constexpr uint32_t kRecordSize = 0x80;
constexpr uint32_t kRecordState = 0x04;
constexpr uint32_t kRecordContext = 0x24;
constexpr uint32_t kRecordCallback = 0x48;

int16_t Signed(uint32_t value)
{
    return static_cast<int16_t>(static_cast<uint16_t>(value));
}

} // namespace

Sndcore2::Sndcore2(Machine& machine) : machine_(machine)
{
    // A game links snd_core or sndcore2. The two libraries' functions take the same arguments. The voices' records keep
    // sndcore2's layout for both (see kRecordSize).
    const auto reg = [&machine](const char* name, Machine::Handler handler)
    {
        machine.Register("sndcore2", name, handler);
        machine.Register("snd_core", name, std::move(handler));
    };

    // Start-up: AXInitWithParams(params) and AXInit start the library that the game links. Under sndcore2, the first
    // word of `params` selects the 48 kHz renderer (1) or the 32 kHz one (0). snd_core has only the 32 kHz renderer.
    for (const auto& [module, library] :
         {std::pair{"sndcore2", ax::System::Library::kSndCore2}, std::pair{"snd_core", ax::System::Library::kSndCore}})
    {
        const bool sndcore2 = library == ax::System::Library::kSndCore2;
        const auto ax_init_with_params = [this, library, sndcore2](Machine& m)
        {
            Init(library, sndcore2 && m.memory.Read32(m.Arg(0)) == 1);
            m.Return(0);
        };
        machine.Register(module, "AXInitWithParams", ax_init_with_params);

        const auto ax_init = [this, library](Machine& m)
        {
            Init(library, false);
            m.Return(0);
        };
        machine.Register(module, "AXInit", ax_init);
    }

    // AXQuit stops AX as the game closes, after the sounds that CSF plays.
    reg("AXQuit", [](Machine&) {});
    reg("AXSetDefaultMixerSelect", [](Machine&) {});

    const auto ax_set_device_upsample_stage = [this](Machine& m)
    {
        system_.SetDeviceUpsampleStage(static_cast<ax::System::Device>(m.Arg(0)), m.Arg(1) != 0);
        m.Return(0);
    };
    reg("AXSetDeviceUpsampleStage", ax_set_device_upsample_stage);

    const auto ax_set_device_linear_upsampler = [this](Machine& m)
    {
        m.Return(static_cast<uint32_t>(
            system_.SetDeviceLinearUpsampler(static_cast<ax::System::Device>(m.Arg(0)), m.Arg(1), m.Arg(2) != 0)));
    };
    reg("AXSetDeviceLinearUpsampler", ax_set_device_linear_upsampler);

    reg("AXInitProfile", [](Machine&) {});
    reg("AXGetSwapProfile", [](Machine& m) { m.Return(0); });

    // Voices: AXAcquireVoiceEx(priority, callback, context). The callback receives the record, the context and the
    // reason.
    const auto ax_acquire_voice_ex = [this](Machine& m)
    {
        const uint32_t callback = m.Arg(1);
        const auto drop = [this, callback](ax::Voice& dropped, uint32_t context, uint32_t reason)
        {
            if (callback != 0)
            {
                machine_.Call(callback, {RecordOf(dropped), context, reason});
            }
        };
        ax::Voice* const voice = system_.AcquireVoice(m.Arg(0), drop, m.Arg(2));
        if (voice == nullptr)
        {
            m.Return(0);
            return;
        }

        voices_[voice->index] = voice;
        const uint32_t record = RecordOf(*voice);
        m.memory.Write32(record + kRecordContext, m.Arg(2));
        m.memory.Write32(record + kRecordCallback, callback);
        WriteState(*voice);
        m.Return(record);
    };
    reg("AXAcquireVoiceEx", ax_acquire_voice_ex);

    const auto ax_free_voice = [this](Machine& m)
    {
        ax::Voice& voice = VoiceAt(m.Arg(0));
        system_.FreeVoice(voice);
        WriteState(voice);
    };
    reg("AXFreeVoice", ax_free_voice);

    reg("AXSetVoicePriority", [this](Machine& m) { system_.SetVoicePriority(VoiceAt(m.Arg(0)), m.Arg(1)); });

    const auto ax_set_voice_state = [this](Machine& m)
    {
        ax::Voice& voice = VoiceAt(m.Arg(0));
        system_.SetVoiceState(voice, static_cast<ax::VoiceState>(m.Arg(1)));
        WriteState(voice);
    };
    reg("AXSetVoiceState", ax_set_voice_state);

    reg("AXSetVoiceType",
        [this](Machine& m) { system_.SetVoiceType(VoiceAt(m.Arg(0)), static_cast<ax::VoiceType>(m.Arg(1))); });
    reg("AXIsVoiceRunning", [this](Machine& m) { m.Return(system_.IsVoiceRunning(VoiceAt(m.Arg(0))) ? 1 : 0); });
    reg("AXSetVoiceMixerSelect", [](Machine& m) { m.Return(0); });

    // AXPBOFFSET: the format and loop flag (u16 each), then the loop, end and current offsets and the samples' address.
    const auto read_offsets = [](Machine& m, uint32_t address)
    {
        ax::Offsets offsets;
        offsets.format = static_cast<ax::SampleFormat>(m.memory.Read16(address));
        offsets.loop_flag = m.memory.Read16(address + 2);
        offsets.loop = m.memory.Read32(address + 4);
        offsets.end = m.memory.Read32(address + 8);
        offsets.current = m.memory.Read32(address + 12);
        offsets.samples = m.memory.Read32(address + 16);

        return offsets;
    };
    const auto write_offsets = [](Machine& m, uint32_t address, const ax::Offsets& offsets)
    {
        m.memory.Write16(address, static_cast<uint16_t>(offsets.format));
        m.memory.Write16(address + 2, offsets.loop_flag);
        m.memory.Write32(address + 4, offsets.loop);
        m.memory.Write32(address + 8, offsets.end);
        m.memory.Write32(address + 12, offsets.current);
        m.memory.Write32(address + 16, offsets.samples);
    };

    reg("AXSetVoiceOffsets",
        [this, read_offsets](Machine& m) { system_.SetVoiceOffsets(VoiceAt(m.Arg(0)), read_offsets(m, m.Arg(1))); });
    reg("AXGetVoiceOffsets",
        [this, write_offsets](Machine& m) { write_offsets(m, m.Arg(1), system_.GetVoiceOffsets(VoiceAt(m.Arg(0)))); });
    reg("AXCheckVoiceOffsets", [](Machine& m) { m.Return(1); });
    reg("AXSetVoiceLoop", [this](Machine& m) { system_.SetVoiceLoop(VoiceAt(m.Arg(0)), m.Arg(1) != 0); });
    reg("AXSetVoiceLoopOffset", [this](Machine& m) { system_.SetVoiceLoopOffset(VoiceAt(m.Arg(0)), m.Arg(1)); });
    reg("AXSetVoiceEndOffset", [this](Machine& m) { system_.SetVoiceEndOffset(VoiceAt(m.Arg(0)), m.Arg(1)); });
    reg("AXSetVoiceCurrentOffset", [this](Machine& m) { system_.SetVoiceCurrentOffset(VoiceAt(m.Arg(0)), m.Arg(1)); });

    const auto ax_set_voice_loop_offset_ex = [this](Machine& m)
    {
        system_.SetVoiceLoopOffsetEx(VoiceAt(m.Arg(0)), m.Arg(1), m.Arg(2));
        m.Return(0);
    };
    reg("AXSetVoiceLoopOffsetEx", ax_set_voice_loop_offset_ex);

    const auto ax_set_voice_end_offset_ex = [this](Machine& m)
    {
        system_.SetVoiceEndOffsetEx(VoiceAt(m.Arg(0)), m.Arg(1), m.Arg(2));
        m.Return(0);
    };
    reg("AXSetVoiceEndOffsetEx", ax_set_voice_end_offset_ex);

    reg("AXGetVoiceCurrentOffsetEx",
        [this](Machine& m) { m.Return(system_.GetVoiceOffsetsEx(VoiceAt(m.Arg(0)), m.Arg(1)).current); });
    reg("AXGetVoiceLoopCount", [this](Machine& m) { m.Return(system_.GetVoiceLoopCount(VoiceAt(m.Arg(0)))); });

    // AXPBADPCM: 16 coefficients, the gain, the predictor and scale, and yn1 and yn2, all 16-bit; AXPBADPCMLOOP: the
    // last three.
    const auto ax_set_voice_adpcm = [this](Machine& m)
    {
        ax::Adpcm adpcm;
        const uint32_t address = m.Arg(1);
        for (int i = 0; i < 16; ++i)
        {
            adpcm.coefs[i] = Signed(m.memory.Read16(address + 2 * i));
        }
        adpcm.gain = m.memory.Read16(address + 0x20);
        adpcm.predictor_scale = m.memory.Read16(address + 0x22);
        adpcm.yn1 = Signed(m.memory.Read16(address + 0x24));
        adpcm.yn2 = Signed(m.memory.Read16(address + 0x26));
        system_.SetVoiceAdpcm(VoiceAt(m.Arg(0)), adpcm);
    };
    reg("AXSetVoiceAdpcm", ax_set_voice_adpcm);

    const auto ax_set_voice_adpcm_loop = [this](Machine& m)
    {
        const uint32_t address = m.Arg(1);
        system_.SetVoiceAdpcmLoop(VoiceAt(m.Arg(0)), {m.memory.Read16(address), Signed(m.memory.Read16(address + 2)),
                                                      Signed(m.memory.Read16(address + 4))});
    };
    reg("AXSetVoiceAdpcmLoop", ax_set_voice_adpcm_loop);

    // AXPBSRC: the ratio's high and low halves, the fraction, and the last four samples, all 16-bit.
    const auto ax_set_voice_src = [this](Machine& m)
    {
        const uint32_t address = m.Arg(1);
        ax::Src src;
        src.ratio = m.memory.Read32(address);
        src.frac = m.memory.Read16(address + 4);
        for (int i = 0; i < 4; ++i)
        {
            src.last[i] = Signed(m.memory.Read16(address + 6 + 2 * i));
        }
        system_.SetVoiceSrc(VoiceAt(m.Arg(0)), src);
    };
    reg("AXSetVoiceSrc", ax_set_voice_src);

    const auto ax_set_voice_src_ratio = [this](Machine& m)
    {
        system_.SetVoiceSrcRatio(VoiceAt(m.Arg(0)), static_cast<float>(m.FloatArg(0)));
        m.Return(0);
    };
    reg("AXSetVoiceSrcRatio", ax_set_voice_src_ratio);

    reg("AXSetVoiceSrcType", [this](Machine& m) { system_.SetVoiceSrcType(VoiceAt(m.Arg(0)), m.Arg(1)); });

    // AXPBVE: the volume and its delta.
    const auto ax_set_voice_ve = [this](Machine& m)
    {
        system_.SetVoiceVe(VoiceAt(m.Arg(0)), {m.memory.Read16(m.Arg(1)), Signed(m.memory.Read16(m.Arg(1) + 2))});
    };
    reg("AXSetVoiceVe", ax_set_voice_ve);

    // AXSetVoiceDeviceMix(voice, device, index, mix): for each of the device's channels, a volume and delta (16-bit
    // each) for each of the four buses.
    const auto ax_set_voice_device_mix = [this](Machine& m)
    {
        const auto device = static_cast<ax::System::Device>(m.Arg(1));
        const int channels = device == ax::System::Device::kTv ? ax::kTvChannels : ax::kDrcChannels;
        std::vector<ax::System::ChannelMix> mix(static_cast<size_t>(channels));
        for (int c = 0; c < channels; ++c)
        {
            for (int bus = 0; bus < ax::kBusCount; ++bus)
            {
                const uint32_t at = m.Arg(3) + static_cast<uint32_t>(c * 16 + bus * 4);
                mix[c][bus] = {m.memory.Read16(at), Signed(m.memory.Read16(at + 2))};
            }
        }
        m.Return(static_cast<uint32_t>(system_.SetVoiceDeviceMix(VoiceAt(m.Arg(0)), device, m.Arg(2), mix)));
    };
    reg("AXSetVoiceDeviceMix", ax_set_voice_device_mix);

    // The filters. AXPBLPF: on, yn1, a0, b0; AXPBBIQUAD: on, xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2.
    const auto ax_set_voice_lpf = [this](Machine& m)
    {
        const uint32_t a = m.Arg(1);
        system_.SetVoiceLpf(VoiceAt(m.Arg(0)), {m.memory.Read16(a), Signed(m.memory.Read16(a + 2)),
                                                Signed(m.memory.Read16(a + 4)), Signed(m.memory.Read16(a + 6))});
    };
    reg("AXSetVoiceLpf", ax_set_voice_lpf);

    reg("AXSetVoiceLpfCoefs",
        [this](Machine& m) { system_.SetVoiceLpfCoefs(VoiceAt(m.Arg(0)), Signed(m.Arg(1)), Signed(m.Arg(2))); });

    const auto ax_compute_lpf_coefs = [this](Machine& m)
    {
        const auto [a0, b0] = system_.ComputeLpfCoefs(m.Arg(0));
        m.memory.Write16(m.Arg(1), a0);
        m.memory.Write16(m.Arg(2), b0);
    };
    reg("AXComputeLpfCoefs", ax_compute_lpf_coefs);

    const auto ax_set_voice_biquad = [this](Machine& m)
    {
        const uint32_t a = m.Arg(1);
        ax::Biquad biquad;
        biquad.on = m.memory.Read16(a);
        int16_t* const fields[] = {&biquad.xn1, &biquad.xn2, &biquad.yn1, &biquad.yn2, &biquad.b0,
                                   &biquad.b1,  &biquad.b2,  &biquad.a1,  &biquad.a2};
        for (int i = 0; i < 9; ++i)
        {
            *fields[i] = Signed(m.memory.Read16(a + 2 + 2 * static_cast<uint32_t>(i)));
        }
        system_.SetVoiceBiquad(VoiceAt(m.Arg(0)), biquad);
    };
    reg("AXSetVoiceBiquad", ax_set_voice_biquad);

    const auto ax_set_voice_biquad_coefs = [this](Machine& m)
    {
        system_.SetVoiceBiquadCoefs(VoiceAt(m.Arg(0)), Signed(m.Arg(1)), Signed(m.Arg(2)), Signed(m.Arg(3)),
                                    Signed(m.Arg(4)), Signed(m.Arg(5)));
    };
    reg("AXSetVoiceBiquadCoefs", ax_set_voice_biquad_coefs);

    // Protection.
    reg("AXUserBegin", [this](Machine& m) { m.Return(static_cast<uint32_t>(system_.UserBegin())); });
    reg("AXUserEnd", [this](Machine& m) { m.Return(static_cast<uint32_t>(system_.UserEnd())); });
    reg("AXUserIsProtected", [this](Machine& m) { m.Return(system_.UserIsProtected() ? 1 : 0); });
    reg("AXVoiceBegin", [this](Machine& m) { m.Return(static_cast<uint32_t>(system_.VoiceBegin(VoiceAt(m.Arg(0))))); });
    reg("AXVoiceEnd", [this](Machine& m) { m.Return(static_cast<uint32_t>(system_.VoiceEnd(VoiceAt(m.Arg(0))))); });

    // The frame callbacks that the game registers. They run as each frame starts.
    const auto ax_register_app_frame_callback = [this](Machine& m)
    {
        frame_callbacks_.push_back(m.Arg(0));
        m.Return(0);
    };
    reg("AXRegisterAppFrameCallback", ax_register_app_frame_callback);

    const auto ax_deregister_app_frame_callback = [this](Machine& m)
    {
        std::erase(frame_callbacks_, m.Arg(0));
        m.Return(0);
    };
    reg("AXDeregisterAppFrameCallback", ax_deregister_app_frame_callback);

    // The aux buses: AXRegisterAuxCallback(device, index, bus, callback, context) and AXGetAuxCallback(device, index,
    // bus, callback out, context out). Their callbacks run a game's effects. Game mode doesn't run them yet.
    const auto ax_register_aux_callback = [this](Machine& m)
    {
        aux_[{m.Arg(0), m.Arg(1), m.Arg(2)}] = {m.Arg(3), m.Arg(4)};
        m.Return(0);
    };
    reg("AXRegisterAuxCallback", ax_register_aux_callback);

    const auto ax_get_aux_callback = [this](Machine& m)
    {
        const auto [callback, context] = aux_[{m.Arg(0), m.Arg(1), m.Arg(2)}];
        m.memory.Write32(m.Arg(3), callback);
        m.memory.Write32(m.Arg(4), context);
        m.Return(0);
    };
    reg("AXGetAuxCallback", ax_get_aux_callback);

    const auto ax_set_aux_return_volume = [this](Machine& m)
    {
        system_.SetAuxReturnVolume(static_cast<ax::System::Device>(m.Arg(0)), m.Arg(1), static_cast<int>(m.Arg(2)),
                                   static_cast<uint16_t>(m.Arg(3)));
        m.Return(0);
    };
    reg("AXSetAuxReturnVolume", ax_set_aux_return_volume);

    // The devices' final-mix callbacks: AXRegisterDeviceFinalMixCallback(device, callback) and
    // AXGetDeviceFinalMixCallback(device, callback out). sndcore2 takes them for the TV (0) and the GamePads (1), and
    // refuses other devices with -1 (0x0200B95C). Each frame, AX calls them with their devices' mixes (see
    // RunFinalMix).
    const auto ax_register_device_final_mix_callback = [this](Machine& m)
    {
        const uint32_t device = m.Arg(0);
        const uint32_t callback = m.Arg(1);
        if (device > 1)
        {
            m.Return(static_cast<uint32_t>(-1));
            return;
        }

        final_mix_[device] = callback;
        ax::System::FinalMixCallback run;
        if (callback != 0)
        {
            run = [this, device, callback](ax::System::FinalMix& mix)
            {
                RunFinalMix(device, callback, mix);
            };
        }
        system_.RegisterDeviceFinalMixCallback(device == 0 ? ax::System::Device::kTv : ax::System::Device::kDrc,
                                               std::move(run));
        m.Return(0);
    };
    reg("AXRegisterDeviceFinalMixCallback", ax_register_device_final_mix_callback);

    const auto ax_get_device_final_mix_callback = [this](Machine& m)
    {
        m.memory.Write32(m.Arg(1), final_mix_[m.Arg(0)]);
        m.Return(0);
    };
    reg("AXGetDeviceFinalMixCallback", ax_get_device_final_mix_callback);

    // AXGetDeviceMode(device, mode out): stereo for every device. Game mode renders stereo. Stereo is mode 0. The Wii's
    // AX numbers its modes the same way: 1 is surround and 2 is Dolby Pro Logic II. AX starts the TV's mode at 0.
    const auto ax_get_device_mode = [](Machine& m)
    {
        if (m.Arg(0) >= 3)
        {
            m.Return(static_cast<uint32_t>(-1));
            return;
        }

        m.memory.Write32(m.Arg(1), 0);
        m.Return(0);
    };
    reg("AXGetDeviceMode", ax_get_device_mode);

    // The Wii Remotes' speakers (padscore) aren't modelled. Each call returns 0.
    for (const char* name : {"WENCGetEncodeData", "WPADCanSendStreamData", "WPADControlSpeaker", "WPADSendStreamData"})
    {
        machine.Register("padscore", name, [](Machine& m) { m.Return(0); });
    }

    // snduser2's effects keep the memory hooks that a game gives them. Here, the effects don't require those hooks.
    machine.Register("snduser2", "AXFXSetHooks", [](Machine&) {});
    machine.Register("snduser2", "AXFXGetHooks", [](Machine&) {});

    // The GamePad's virtual surround, the devices' compressors and the Wii Remotes' speakers aren't modelled (see
    // ax::System). DRCVS_Process leaves the GamePad's mix as it is.
    for (const char* name : {"AXSetDRCVSMode", "AXSetDRCVSDownmixBalance", "AXSetDRCVSLC", "AXSetDRCVSOutputGain",
                             "AXSetDRCVSSpeakerPosition", "AXSetDRCVSSurroundDepth", "AXSetDRCVSSurroundLevelGain",
                             "DRCVS_SetOutputMode", "DRCVS_Process", "AXSetDeviceCompressor"})
    {
        reg(name, [](Machine& m) { m.Return(0); });
    }
    for (const char* name : {"AXSetVoiceRmtOn", "AXSetVoiceRmtIIR", "AXSetVoiceRmtIIRCoefs"})
    {
        reg(name, [](Machine&) {});
    }
    for (const char* name : {"AXRmtGetSamples", "AXRmtGetSamplesLeft", "AXRmtAdvancePtr"})
    {
        reg(name, [](Machine& m) { m.Return(0); });
    }
}

void Sndcore2::Start()
{
    records_ = machine_.Allocate(kRecordSize * ax::kVoiceCount, 64);
    for (uint32_t i = 0; i < ax::kVoiceCount; ++i)
    {
        machine_.memory.Write32(records_ + i * kRecordSize, i);
    }

    const std::span<uint8_t> data = machine_.memory.RegionAt(Machine::kDataBase);
    memory_.Map(Machine::kDataBase, data);
}

void Sndcore2::Init(ax::System::Library library, bool renderer_48k)
{
    system_.Init(library, renderer_48k);
    voices_ = {};

    const auto frame_callback = [this]
    {
        // The records take their voices' states as the frame left them. sndcore2 updates them the same way.
        for (ax::Voice* const voice : voices_)
        {
            if (voice != nullptr)
            {
                WriteState(*voice);
            }
        }

        const std::vector<uint32_t> callbacks = frame_callbacks_;
        for (const uint32_t callback : callbacks)
        {
            machine_.Interrupt(callback);
        }
    };
    system_.RegisterFrameCallback(frame_callback);
}

void Sndcore2::RunFinalMix(uint32_t device, uint32_t callback, ax::System::FinalMix& mix)
{
    // The device's AXDeviceFinalMixData, laid out as sndcore2 lays it out (0x020083D8): the address of the channels'
    // buffer addresses, then the channels, the samples, the devices and the output channels, a halfword each. The
    // buffers hold 32-bit samples in 16-bit scale, back to back, one frame's samples apart. A game's mixer may write
    // the channels from the first buffer's address on (JAudio1 does). AX calls the callback with the data's address
    // (0x02008400), and takes the output channels back from the data afterwards.
    Memory& memory = machine_.memory;
    uint32_t& data = final_mix_data_[device];
    if (data == 0)
    {
        data = machine_.Allocate(kFinalMixDataBytes);
    }
    const uint32_t pointers = data + 16;
    const uint32_t buffers = pointers + kFinalMixChannels * 4;
    memory.Write32(data, pointers);
    memory.Write16(data + 4, mix.channels_in);
    memory.Write16(data + 6, mix.samples);
    memory.Write16(data + 8, mix.devices);
    memory.Write16(data + 10, mix.channels_out);

    for (std::size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto buffer = static_cast<uint32_t>(buffers + c * mix.samples * 4);
        memory.Write32(static_cast<uint32_t>(pointers + c * 4), buffer);
        for (uint32_t i = 0; i < mix.samples; ++i)
        {
            memory.Write32(buffer + i * 4, static_cast<uint32_t>(mix.channels[c][i]));
        }
    }

    machine_.Interrupt(callback, {data});

    for (std::size_t c = 0; c < mix.channels.size(); ++c)
    {
        const auto buffer = static_cast<uint32_t>(buffers + c * mix.samples * 4);
        for (uint32_t i = 0; i < mix.samples; ++i)
        {
            mix.channels[c][i] = static_cast<int32_t>(memory.Read32(buffer + i * 4));
        }
    }
    mix.channels_out = memory.Read16(data + 10);
}

ax::Voice& Sndcore2::VoiceAt(uint32_t record)
{
    const uint32_t index = (record - records_) / kRecordSize;
    if (record < records_ || index >= ax::kVoiceCount || (record - records_) % kRecordSize != 0 ||
        voices_[index] == nullptr)
    {
        throw std::runtime_error("the guest named an AX voice that game mode doesn't have");
    }

    return *voices_[index];
}

void Sndcore2::WriteState(const ax::Voice& voice)
{
    machine_.memory.Write32(RecordOf(voice) + kRecordState, static_cast<uint32_t>(voice.state));
}

uint32_t Sndcore2::RecordOf(const ax::Voice& voice) const
{
    return records_ + voice.index * kRecordSize;
}

} // namespace csf::cafe
