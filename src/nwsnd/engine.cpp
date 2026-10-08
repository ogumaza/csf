// SPDX-License-Identifier: MIT

#include "nwsnd/engine.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace csf::nwsnd
{

Engine::Engine(ax::System& system, ax::Memory& memory, const fsar::SoundArchive& archive, EngineOptions options)
    : system_(&system),
      memory_(&memory),
      archive_(&archive),
      options_(options),
      ax_voices_(system, memory),
      voices_(ax_voices_, options_.hardware, options.voice_count),
      channels_(voices_, options.voice_count)
{
    global_vars_.fill(-1);

    // HwCallbackProc counts the frame, updates the AX voices, and wakes the sound thread. RunFrame stands in for the
    // sound thread.
    system_->RegisterFrameCallback([this] { ax_voices_.UpdateAllVoices(options_.hardware.output_mode); });
}

Engine::~Engine()
{
    system_->RegisterFrameCallback({});
}

const fsar::Bank* Engine::GetBank(uint32_t bank_item)
{
    if (const auto it = banks_.find(bank_item); it != banks_.end())
    {
        return it->second.get();
    }

    const uint32_t index = bank_item & 0xFFFFFF;
    if ((bank_item >> 24) != 0x03 || index >= archive_->Banks().size())
    {
        return nullptr;
    }
    const std::span<const uint8_t> file = archive_->FileData(archive_->Banks()[index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    return banks_.emplace(bank_item, std::make_unique<const fsar::Bank>(fsar::Bank::Parse(file))).first->second.get();
}

const Engine::LoadedWaveArchive* Engine::GetWaveArchive(uint32_t wave_archive_item)
{
    if (const auto it = wave_archives_.find(wave_archive_item); it != wave_archives_.end())
    {
        return it->second.get();
    }

    const uint32_t index = wave_archive_item & 0xFFFFFF;
    if ((wave_archive_item >> 24) != 0x05 || index >= archive_->WaveArchives().size())
    {
        return nullptr;
    }
    const std::span<const uint8_t> file = archive_->FileData(archive_->WaveArchives()[index].file_id);
    if (file.empty())
    {
        return nullptr;
    }

    // The whole file goes into AX's memory. The game loads the file into its sound heap the same way. The waves then
    // lie where they lie in the file.
    auto loaded = std::make_unique<LoadedWaveArchive>();
    loaded->file = file;
    loaded->address = next_address_;
    loaded->archive = fsar::WaveArchive::Parse(file);
    memory_->Map(loaded->address, file);
    next_address_ += (static_cast<uint32_t>(file.size()) + 0xFF) & ~0xFFu;

    return wave_archives_.emplace(wave_archive_item, std::move(loaded)).first->second.get();
}

std::optional<WaveInfo> Engine::GetWave(uint32_t wave_archive_item, uint32_t index)
{
    const auto key = std::make_pair(wave_archive_item, index);
    if (const auto it = waves_.find(key); it != waves_.end())
    {
        return it->second;
    }

    // WaveFileReader::ReadWaveInfo (0x022B85B0): PCM8 and PCM16 by their codes, and any other code as DSP-ADPCM.
    std::optional<WaveInfo> result;
    const LoadedWaveArchive* loaded = GetWaveArchive(wave_archive_item);
    if (loaded && index < loaded->archive.waves.size())
    {
        const fsar::Wave wave = fsar::Wave::Parse(loaded->archive.waves[index]);
        if (!wave.channels.empty())
        {
            WaveInfo info;
            switch (wave.encoding)
            {
            case fsar::WaveEncoding::kPcm8:
                info.format = SampleFormat::kPcm8;
                break;
            case fsar::WaveEncoding::kPcm16:
                info.format = SampleFormat::kPcm16;
                break;
            default:
                info.format = SampleFormat::kDspAdpcm;
                break;
            }
            info.loop = wave.loop;
            info.channel_count = static_cast<int>(wave.channels.size());
            info.sample_rate = wave.sample_rate;
            info.loop_start = wave.loop_start;
            info.loop_end = wave.loop_end;
            info.original_loop_start = wave.loop_start;
            for (std::size_t c = 0; c < std::min<std::size_t>(wave.channels.size(), 2); ++c)
            {
                const fsar::WaveChannel& from = wave.channels[c];
                WaveInfo::Channel& to = info.channels[c];
                to.address = loaded->address + static_cast<uint32_t>(from.data.data() - loaded->file.data());
                if (from.adpcm)
                {
                    to.coefs = from.adpcm->coefs;
                    to.context = {from.adpcm->pred_scale, from.adpcm->yn1, from.adpcm->yn2};
                    to.loop_context = {from.adpcm->loop_pred_scale, from.adpcm->loop_yn1, from.adpcm->loop_yn2};
                }
            }
            result = info;
        }
    }

    waves_.emplace(key, result);

    return result;
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info)
{
    if (slot < 0 || static_cast<std::size_t>(slot) >= banks.size())
    {
        return nullptr;
    }
    const fsar::Bank* bank = GetBank(banks[slot]);
    if (!bank || info.prg_no >= bank->instruments.size() || !bank->instruments[info.prg_no])
    {
        return nullptr;
    }
    const fsar::VelocityRegion* region = bank->instruments[info.prg_no]->Find(info.key, info.velocity);
    if (!region || region->wave_id_index >= bank->waves.size())
    {
        return nullptr;
    }
    const fsar::WaveId& id = bank->waves[region->wave_id_index];
    const std::optional<WaveInfo> wave = GetWave(id.wave_archive_item, id.index);
    if (!wave)
    {
        return nullptr;
    }

    Channel* channel = channels_.AllocChannel(std::min(wave->channel_count, 2), info.priority, info.callback);
    if (!channel)
    {
        return nullptr;
    }

    channel->key = static_cast<uint8_t>(info.key);
    channel->original_key = region->original_key;
    channel->velocity_volume = static_cast<float>(static_cast<uint8_t>(info.velocity)) * (1.0f / 127.0f);
    channel->init_volume = static_cast<float>(region->volume) * (1.0f / 127.0f);
    channel->tune = region->pitch;
    channel->envelope.SetAttack(region->adshr.attack);
    channel->envelope.SetHold(region->adshr.hold);
    channel->envelope.SetDecay(region->adshr.decay);
    channel->envelope.SetSustain(region->adshr.sustain);
    channel->envelope.SetRelease(region->adshr.release);
    channel->init_pan = static_cast<float>(info.init_pan + static_cast<int>(region->pan) - 0x40) / 63.0f;
    channel->key_group = region->key_group;
    channel->ignore_note_off = region->ignore_note_off;
    channel->interpolation_type = region->interpolation_type;
    channel->Start(*wave, info.length, 0);

    return channel;
}

SequenceSoundPlayer* Engine::StartSequence(uint32_t index)
{
    if (index >= archive_->Sounds().size())
    {
        return nullptr;
    }
    const fsar::SoundInfo& info = archive_->Sounds()[index];
    if (info.type != fsar::SoundType::kSequence)
    {
        return nullptr;
    }
    const std::span<const uint8_t> file = archive_->FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    const fsar::Sequence sequence = fsar::Sequence::Parse(file);
    players_.push_back(std::make_unique<SequenceSoundPlayer>(*this, info, sequence.data));
    pending_.push_back({players_.back().get(), 1});

    return players_.back().get();
}

void Engine::FrameProcess()
{
    voices_.UpdateAllVoiceStatus();

    // The sound thread's commands. They start the sounds that are due.
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (it->frames > 0)
        {
            --it->frames;
            ++it;
            continue;
        }
        it->player->Start();
        it = pending_.erase(it);
    }

    // The players' frames (OnUpdateFrameSoundThread), the channels and the voices.
    for (const std::unique_ptr<SequenceSoundPlayer>& player : players_)
    {
        player->Update();
    }
    channels_.UpdateAllChannel(random_);
    voices_.UpdateAllVoices();
    ax_voices_.UpdatePriorityList();

    // HardwareManager::Update (0x0229EC08) has no fades to move here. The random numbers step once a frame.
    random_.Next();
}

void Engine::RunFrame()
{
    system_->RenderFrame();
    FrameProcess();
    ++frame_count_;
}

bool Engine::IsBusy() const
{
    for (const std::unique_ptr<SequenceSoundPlayer>& player : players_)
    {
        if (!player->IsFinished())
        {
            return true;
        }
    }

    return channels_.ActiveCount() > 0;
}

ArchiveModel::ArchiveModel(const fsar::SoundArchive& archive, EngineOptions options)
{
    system.Init(ax::System::Library::kSndCore2, options.renderer_48k);
    system.SetDeviceUpsampleStage(ax::System::Device::kTv, options.upsample_after_final_mix);
    system.SetDeviceUpsampleStage(ax::System::Device::kDrc, options.upsample_after_final_mix);
    engine = std::make_unique<Engine>(system, memory, archive, options);
}

} // namespace csf::nwsnd
