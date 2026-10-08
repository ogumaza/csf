// SPDX-License-Identifier: MIT

// Archive mode's model of nw::snd for one sound archive. The model runs the sound thread's frame
// (SoundThread::FrameProcess) and AX's frame callback (HwCallbackProc) around src/ax. The model lays the archive's
// banks and wave archives out in AX's memory, the same way a game's sound heap would hold them. The model plays the
// archive's sequence sounds. Each function's comment gives the address of the routine it follows in Animal Crossing:
// amiibo Festival's executable.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ax/memory.h"
#include "ax/system.h"
#include "fsar/formats.h"
#include "nwsnd/ax_voice.h"
#include "nwsnd/channel.h"
#include "nwsnd/sequence.h"
#include "nwsnd/util.h"
#include "nwsnd/voice.h"

namespace csf::nwsnd
{

struct EngineOptions
{
    // nw::snd's voice and channel count, the .data word at 0x101AA478. It's 96 unless the game changes it before its
    // sound system starts.
    int voice_count = 96;
    HardwareSettings hardware;

    // How the game starts AX (ArchiveModel): the 48 kHz renderer rather than the 32 kHz one, and each device's output
    // reaching 48 kHz after its final mix stage rather than before it. The reference game's settings are the defaults.
    bool renderer_48k = false;
    bool upsample_after_final_mix = true;
};

class Engine
{
public:
    // Runs on `system`, with the archive's waves mapped into `memory`. `system` must have been started. AX reads
    // samples from `memory`. The archive must outlive the engine.
    Engine(ax::System& system, ax::Memory& memory, const fsar::SoundArchive& archive, EngineOptions options = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts sequence sound `index`, by its index in the archive's sound table. The game's sound archive player does
    // the same when a sound is started before a frame. The sound starts in the second sound frame after that (the first
    // frame takes the start to the sound thread). Returns the player, or nullptr if there's no such sequence or its
    // files aren't in the archive.
    SequenceSoundPlayer* StartSequence(uint32_t index);

    // A frame of 3 ms. AX renders, and AX's frame callback updates nw::snd's AX voices (HwCallbackProc, 0x022A9FD8).
    // Then the sound thread's frame runs (FrameProcess, 0x0229F228).
    void RunFrame();

    // Whether a sound still plays or a channel still sounds.
    bool IsBusy() const;

    uint64_t FrameCount() const
    {
        return frame_count_;
    }

    Random& Rng()
    {
        return random_;
    }

    // The sequences' global variables. Every player shares them (InitSequenceSoundPlayer, 0x022C8504, sets them to -1).
    std::array<int16_t, 16>& GlobalVariables()
    {
        return global_vars_;
    }

    ChannelManager& Channels()
    {
        return channels_;
    }

    MultiVoiceManager& Voices()
    {
        return voices_;
    }

    // A note on the bank in a sequence's bank slot (Bank::NoteOn, 0x022C4A0C): the velocity region for the key and
    // velocity, its wave, and a channel that starts it. Returns nullptr if the slot, the program, the region or the
    // wave isn't there, or no channel is free.
    Channel* NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info);

private:
    struct LoadedWaveArchive
    {
        std::span<const uint8_t> file;
        uint32_t address = 0; // where the whole file lies in AX's memory
        fsar::WaveArchive archive;
    };

    struct PendingStart
    {
        SequenceSoundPlayer* player;
        int frames; // sound frames before it starts
    };

    void FrameProcess();
    const fsar::Bank* GetBank(uint32_t bank_item);
    const LoadedWaveArchive* GetWaveArchive(uint32_t wave_archive_item);
    std::optional<WaveInfo> GetWave(uint32_t wave_archive_item, uint32_t index);

    ax::System* system_;
    ax::Memory* memory_;
    const fsar::SoundArchive* archive_;
    EngineOptions options_;
    AxVoices ax_voices_;
    MultiVoiceManager voices_;
    ChannelManager channels_;
    Random random_;
    std::array<int16_t, 16> global_vars_;

    // Declared after the channels. The players are then destroyed first, and their tracks' hold on channels goes with
    // them.
    std::vector<std::unique_ptr<SequenceSoundPlayer>> players_;
    std::vector<PendingStart> pending_;

    std::map<uint32_t, std::unique_ptr<const fsar::Bank>> banks_;
    std::map<uint32_t, std::unique_ptr<const LoadedWaveArchive>> wave_archives_;
    std::map<std::pair<uint32_t, uint32_t>, std::optional<WaveInfo>> waves_;
    uint32_t next_address_ = 0x10000000;
    uint64_t frame_count_ = 0;
};

// Archive mode's whole stack for one sound archive: AX's memory, AX (started as the options specify) and the engine.
struct ArchiveModel
{
    explicit ArchiveModel(const fsar::SoundArchive& archive, EngineOptions options = {});

    ax::Memory memory;
    ax::System system{memory};
    std::unique_ptr<Engine> engine;
};

} // namespace csf::nwsnd
