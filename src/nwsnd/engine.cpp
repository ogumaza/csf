// SPDX-License-Identifier: MIT

#include "nwsnd/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace csf::nwsnd
{

Engine::Engine(ax::System& system, ax::Memory& memory, const fsar::SoundArchive& archive, EngineOptions options)
    : system_(&system),
      memory_(&memory),
      archive_(&archive),
      options_(options),
      ax_voices_(system, memory),
      renderer_(ax_voices_, options_.hardware, options.voice_count),
      voices_(options.voice_count),
      channels_(options.voice_count),
      files_(archive, kSarOptions, [this](ntdlib::nwsnd::LoadedWaveArchive& loaded) { MapWaveArchive(loaded); })
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

void Engine::MapWaveArchive(ntdlib::nwsnd::LoadedWaveArchive& loaded)
{
    loaded.address = next_address_;
    memory_->Map(loaded.address, loaded.file);
    next_address_ += (static_cast<uint32_t>(loaded.file.size()) + 0xFF) & ~0xFFu;
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info)
{
    const std::optional<Files::Note> note = files_.FindNote(banks, slot, info.prg_no, info.key, info.velocity);
    if (!note)
    {
        return nullptr;
    }

    const fsar::VelocityRegion* region = note->region;
    Channel* channel = channels_.AllocChannel(*this, std::min(note->wave.channel_count, 2), info.priority,
                                              info.owner_player, info.owner_track);
    if (!channel)
    {
        return nullptr;
    }

    channel->key = static_cast<uint8_t>(info.key);
    channel->original_key = region->original_key;
    channel->velocity_volume = SequencePolicy::VelocityVolume(info.velocity);
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
    channel->interpolation_type = region->interpolation;
    channel->Start(*this, note->wave, info.length);

    return channel;
}

void Engine::ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus)
{
    players_[static_cast<std::size_t>(player)]->ChannelEvent(*this, track, channel);
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

    const fsar::Sequence sequence = fsar::Sequence::Parse(file, kSarOptions);
    players_.push_back(std::make_unique<SequenceSoundPlayer>(static_cast<int>(players_.size()), info, sequence.data));
    pending_.push_back({players_.back().get(), 1});

    return players_.back().get();
}

void Engine::FrameProcess()
{
    voices_.UpdateAllVoiceStatus(*this);

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
        player->Update(*this);
    }
    channels_.UpdateAllChannel(*this);
    voices_.UpdateAllVoices(*this);
    ax_voices_.UpdatePriorityList();

    // HardwareManager::Update (0x0229EC08) has no fades to move here. The random numbers step once a frame.
    random_.Next();
}

void Engine::RunFrame()
{
    system_->RenderFrame();
    FrameProcess();
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
