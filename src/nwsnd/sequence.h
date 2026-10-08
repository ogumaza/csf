// SPDX-License-Identifier: MIT

// nw::snd's sequence playback in NintendoWare for Cafe (driver::SequenceTrack with its MmlParser, and
// driver::SequenceSoundPlayer). The comment on a method that follows one of the game's routines gives the routine's
// address in Animal Crossing: amiibo Festival's executable. A field comment gives the field's offset in the game's
// object.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "fsar/formats.h"
#include "nwsnd/channel.h"
#include "nwsnd/envelope.h"
#include "nwsnd/voice.h"

namespace csf::nwsnd
{

class Engine;
class SequenceSoundPlayer;

// MoveValue<T, s16>: a linear ramp to a target over a number of sequence ticks.
template <typename T>
struct MoveValue
{
    void Init(T value)
    {
        start = value;
        target = value;
        duration = 0;
        counter = 0;
    }

    void Update()
    {
        if (counter < duration)
        {
            ++counter;
        }
    }

    T Get() const
    {
        if (counter >= duration)
        {
            return target;
        }

        return static_cast<T>(static_cast<int>(start) +
                              (static_cast<int>(target) - static_cast<int>(start)) * counter / duration);
    }

    void SetTarget(T value, int16_t ticks)
    {
        start = Get();
        duration = ticks;
        counter = 0;
        target = value;
    }

    bool Moving() const
    {
        return counter < duration;
    }

    T start{};
    T target{};
    int16_t duration = 0;
    int16_t counter = 0;
};

class SequenceTrack
{
public:
    // Track `index`, 0 to 15, of `player`.
    SequenceTrack(SequenceSoundPlayer& player, int index);

    SequenceTrack(const SequenceTrack&) = delete;
    SequenceTrack& operator=(const SequenceTrack&) = delete;

    // Clears the callbacks of the channels that still point at the track.
    ~SequenceTrack();

    void SetSeqData(std::span<const uint8_t> data, uint32_t offset); // 0x022B69A8
    void Open();                                                     // 0x022B69B8
    void Close();                                                    // 0x022B6158

    bool IsOpen() const
    {
        return open_;
    }

    // The channels the track plays, the newest first. Each channel points at the next with next_in_track.
    const Channel* ChannelList() const
    {
        return channel_list_;
    }

    // Whether the track won't read on while its notes sound: it waits for them to end (after a note of length 0 with
    // note wait on) and some still sound, or it waits for ever. A track whose notes have all ended reads on at its next
    // tick.
    bool WaitsForNotes() const
    {
        return (note_finish_wait_ && channel_list_ != nullptr) || WaitsForEver();
    }

    // Whether the track will never read on, or not before the game stops the sound. Either the track waits for a
    // negative number of ticks, or it waits out a held note (see kHeldTicks). A note or wait whose length comes from a
    // variable left at its default of -1 makes a negative wait.
    bool WaitsForEver() const
    {
        return wait_ < 0 || held_wait_;
    }

    // Whether the track's last pass of a loop changed nothing in the sound, and nothing has changed since (for the
    // ripper's length analysis, while the player has on_loop set). A track that polls or counts a variable once a tick
    // makes such idle passes. See CountLoop.
    bool IsIdle() const
    {
        return idle_ && changed_at_ < looped_at_;
    }

    // Whether the track is idle, its last pass left the variables that tracks read as the pass before did, and it has
    // drawn no random number since that pass began. Such a track does the same on every pass until the game changes a
    // variable.
    bool IsStill() const
    {
        return IsIdle() && still_ && drew_at_ < looped_at_;
    }

    // Whether the track's volume, pan or pitch bend is moving to a new value.
    bool IsMoving() const
    {
        return vol_.Moving() || volume2_.Moving() || pan_.Moving() || span_.Moving() || bend_.Moving();
    }

    // A tick: the ramps advance and the commands up to the next wait run. Returns -1 when the track reaches its end, 1
    // otherwise, or 0 if it's closed (0x022B6F14).
    int ParseNextTick(bool do_note_on);

    void UpdateChannelLength(); // 0x022B6E88
    void UpdateChannelParam();  // 0x022B55B4

private:
    enum ArgType
    {
        kArgNone = 0,
        kArgU8 = 1,
        kArgS16 = 2,
        kArgVmidi = 3,
        kArgRandom = 4,
        kArgVariable = 5,
    };

    // A call or a loop (8 bytes at +0x10C): where it returns to, and the loop's count.
    struct CallStackEntry
    {
        bool is_loop = false;
        uint8_t loop_count = 0;
        uint32_t address = 0;
    };

    static constexpr int kCallStackDepth = 10;

    // What a command can change in the track's sound, for the ripper's length analysis: the track's settings, its
    // ramps' places and targets, its newest channel, and the player's tempo and main volume.
    struct SoundState
    {
        uint32_t prg_no;
        uint8_t bank_index;
        int8_t transpose;
        uint8_t velocity_range, bend_range, priority, porta_key, porta_time, main_send;
        int8_t init_pan, biquad_type;
        std::array<uint8_t, 4> envelope;
        int16_t hold;
        std::array<uint8_t, 3> fx_send;
        float lpf, biquad_value, sweep_pitch;
        int32_t output_line;
        std::array<CurveLfoParam, 4> lfo_params;
        std::array<uint8_t, 4> lfo_types;
        bool mute, silence, tie, mono, porta, damper, front_bypass, note_wait;
        std::array<int32_t, 18> ramps; // the value, target and ticks to go of the track's five ramps and main volume
        const Channel* newest;
        uint8_t timebase;
        uint16_t tempo;

        bool operator==(const SoundState&) const = default;
    };

    SoundState State() const;

    // Parse, with what the ripper's length analysis follows while the player has on_loop set: each command's place in
    // the order the player's tracks run them, and whether the command changes the track's sound.
    int RunCommand(bool do_note_on);

    void InitParam();                                               // 0x022CABAC
    int Parse(bool do_note_on);                                     // MmlParser::Parse, 0x022CB7C8
    int32_t ReadArg(int type);                                      // MmlParser::ReadArg, 0x022CB67C
    void CommandProc(uint32_t command, int32_t arg1, int32_t arg2); // MmlParser::CommandProc, 0x022CBF1C
    void ReleaseAllChannel(int release);                            // 0x022B608C
    void FreeAllChannel();                                          // 0x022B6114
    void StopAllChannel();                                          // 0x022CAAA4
    void SetMute(int mode);                                         // 0x022CB34C
    void OnChannelEvent(Channel& channel);                          // ChannelCallbackFunc, 0x022CB06C
    uint8_t ReadByte();
    Channel::Callback MakeCallback();

    // 0x022C9A4C. Returns whether the note changes the sound (for the ripper's length analysis). A note changes it
    // unless it carries on the newest note at the same key and velocity, with no sweep. Only a tie or a monophonic note
    // carries on the newest note.
    bool NoteOn(int key, int velocity, int32_t length, bool tie);

    // The track went back to `target`, the start of a loop. The pass that ended there began at the track's last run of
    // `target`. A pass that changed nothing in the sound is idle, such as a pass that only polls or counts a variable.
    // An idle pass is never a loop of the sequence (SequenceSoundPlayer::on_loop). Any other pass is one if the track
    // is track 0, or if it's the main track (SequenceSoundPlayer::IsMainTrack) and played a note on the way round.
    void CountLoop(uint32_t target);

    // The command being run changed the track's sound.
    void MarkChange();

    // The track read variable `index` in a command's argument or in a comparison. Whether a track has read variable
    // `index` so far: any track for the player's and the global variables, this one for the track's variables.
    void MarkRead(int index);
    bool IsRead(int index) const;

    // Whether the track holds a held note (see kHeldTicks) on a looping wave, settled at its envelope's sustain level.
    bool HoldsSustainedNote() const;

    // A variable by its command's index (MmlParser::GetVariablePtr, 0x022CB600): 0 to 15 the player's variables, 16 to
    // 31 the global ones, 32 to 47 the track's variables.
    int16_t* Variable(int index);

    SequenceSoundPlayer* player_;
    int index_;
    bool open_ = false; // +0x01

    // SetTrackVolume, SetTrackPitch and the rest: 1 and the defaults. The game calls them, and archive mode doesn't.
    float volume_ = 1.0f;            // +0x04
    float pitch_ = 1.0f;             // +0x08
    float pan_range_ = 1.0f;         // +0x0C
    OutputParam tv_;                 // +0x10
    std::array<OutputParam, 2> drc_; // +0x60

    std::span<const uint8_t> data_; // +0x100
    uint32_t current_ = 0;          // +0x104
    bool has_data_ = false;
    bool cmp_flag_ = true;                                                   // +0x108
    bool note_wait_ = true;                                                  // +0x109
    bool tie_ = false;                                                       // +0x10A
    bool mono_ = false;                                                      // +0x10B
    std::array<CallStackEntry, kCallStackDepth> call_stack_;                 // +0x10C
    uint8_t call_depth_ = 0;                                                 // +0x15C
    bool front_bypass_ = false;                                              // +0x15D (InitParam leaves it)
    bool mute_ = false;                                                      // +0x15E
    bool silence_ = false;                                                   // +0x15F
    int32_t wait_ = 0;                                                       // +0x160
    bool note_finish_wait_ = false;                                          // +0x164
    bool porta_ = false;                                                     // +0x165
    bool damper_ = false;                                                    // +0x166
    uint8_t bank_index_ = 0;                                                 // +0x167
    uint32_t prg_no_ = 0;                                                    // +0x168
    float sweep_pitch_ = 0.0f;                                               // +0x16C
    MoveValue<uint8_t> vol_;                                                 // +0x170
    MoveValue<uint8_t> volume2_;                                             // +0x176
    MoveValue<int8_t> pan_;                                                  // +0x17C
    MoveValue<int8_t> span_;                                                 // +0x182
    MoveValue<int8_t> bend_;                                                 // +0x188
    std::array<CurveLfoParam, 4> lfo_params_;                                // +0x190
    std::array<uint8_t, 4> lfo_types_{};                                     // +0x1D0
    uint8_t velocity_range_ = 127;                                           // +0x1D4
    uint8_t bend_range_ = 2;                                                 // +0x1D5
    int8_t init_pan_ = 0;                                                    // +0x1D6
    int8_t transpose_ = 0;                                                   // +0x1D8
    uint8_t priority_ = 64;                                                  // +0x1D9
    uint8_t porta_key_ = 60;                                                 // +0x1DA
    uint8_t porta_time_ = 0;                                                 // +0x1DB
    uint8_t attack_ = 0xFF, decay_ = 0xFF, sustain_ = 0xFF, release_ = 0xFF; // +0x1DC
    int16_t hold_ = 0xFF;                                                    // +0x1E0
    int8_t biquad_type_ = 0;                                                 // +0x1E2
    uint8_t main_send_ = 127;                                                // +0x1E3
    std::array<uint8_t, 3> fx_send_{};                                       // +0x1E4
    float lpf_ = 0.0f;                                                       // +0x1E8
    float biquad_value_ = 0.0f;                                              // +0x1EC
    int32_t output_line_ = -1;                                               // +0x1F0: the player's when -1
    std::array<int16_t, 16> vars_{};                                         // +0x1F4
    Channel* channel_list_ = nullptr;                                        // +0x218

    // What the ripper's length analysis follows while the player has on_loop set.
    // Times are counts of the commands that the player's tracks have run (SequenceSoundPlayer::commands_run_).
    std::vector<uint64_t> run_;                // when the track last ran each command, or 0 if it hasn't
    uint64_t changed_at_ = 0;                  // when the track last changed the sound
    uint64_t drew_at_ = 0;                     // when the track last drew a random number
    uint64_t looped_at_ = 0;                   // when the track last went back to the start of a loop
    uint16_t variables_read_ = 0;              // the track's variables it has read, a bit each
    std::array<int16_t, 48> loop_variables_{}; // the variables that tracks read, at the last loop (0 for the others)
    bool idle_ = false;                        // whether the track's last pass of a loop changed nothing in the sound
    bool still_ = false; // whether that pass was idle, drew no random number and left the variables as the one before
    bool noted_ = false; // whether the track has played a note since its last loop
    bool conditional_ = false; // whether the command being run is conditional
    bool holds_ = false;       // whether the track's last note is a held one (see kHeldTicks)
    bool held_wait_ = false;   // whether the track waits out a held note
};

// What a player gets from its sound: the sound's info in the archive, its sequence's bytecode, and its banks by slot.
// Each bank comes with the waves of the wave archive it plays.
class SequenceSoundPlayer
{
public:
    SequenceSoundPlayer(Engine& engine, const fsar::SoundInfo& info, std::span<const uint8_t> data);

    SequenceSoundPlayer(const SequenceSoundPlayer&) = delete;
    SequenceSoundPlayer& operator=(const SequenceSoundPlayer&) = delete;

    ~SequenceSoundPlayer();

    // Opens the tracks that the sound's allocation flags name. The first track opens at the sound's start (Setup,
    // 0x022C8654, and Start, 0x022C8924).
    void Start();

    // Each sound frame: the ticks that come due, then each track's parameters reach its channels (0x022B75FC).
    void Update();

    bool IsFinished() const
    {
        return finished_;
    }

    // The ticks so far (+0x228).
    uint32_t TickCount() const
    {
        return tick_count_;
    }

    // Track `index`, 0 to 15, or nullptr if it isn't allocated.
    const SequenceTrack* Track(int index) const
    {
        return index >= 0 && index < 16 ? tracks_[index].get() : nullptr;
    }

    // Whether the sequence has started and hasn't ended.
    bool IsActive() const
    {
        return started_ && !finished_;
    }

    // Whether no open track will change the sound while the notes sound, or the tempo is 0 (for the ripper's length
    // analysis). Each open track waits for its notes to end, waits for ever, or only makes still passes
    // (SequenceTrack::IsStill). Then the sequence plays on unchanged while the notes sound.
    bool IsSettled() const;

    // Whether no open track will change the sound again, or the tempo is 0. Each open track waits for ever or only
    // makes still passes.
    bool IsStuck() const;

    // Whether the main volume, or any open track's volume, pan or pitch bend, is moving to a new value.
    bool IsMoving() const;

    // When a track last changed the sound, counted in the commands that the tracks have run while on_loop is set (for
    // the ripper's length analysis). A track changes the sound when it plays a note, changes a setting or a ramp, or
    // opens a track. 0 if no track has.
    uint64_t ChangedAt() const
    {
        return changed_at_;
    }

    // A pass of track `track` that changed the sound ended with a jump back to `target`, a command the track has
    // already run, or at the end of a loop that repeats for ever and starts at `target`: a loop of the sequence (for
    // the ripper's length analysis). The track is track 0, or the main track if it played a note on the way round (see
    // IsMainTrack). A jump back into code that the track hasn't run isn't a loop. Sequences that share their start make
    // such jumps.
    std::function<void(int track, uint32_t target)> on_loop;

private:
    friend class SequenceTrack;

    Engine& GetEngine()
    {
        return *engine_;
    }

    SequenceTrack* GetTrack(int index) // 0x022B5588
    {
        return index >= 0 && index < 16 ? tracks_[index].get() : nullptr;
    }

    int16_t* Variable(int index); // 0x022C8A58

    // Whether track `index` is the main track: every track before it is closed or idle (SequenceTrack::IsIdle). The
    // main track's loops are the sequence's loops.
    bool IsMainTrack(int index) const;

    // Whether `test` holds for every open track (and true when none is open).
    bool EveryOpenTrack(const std::function<bool(const SequenceTrack& track)>& test) const;

    // A tick of every track. Returns whether none is open any more (0x022B70AC).
    bool ParseNextTick(bool do_note_on);
    void UpdateTick();          // 0x022B7368
    void FinishPlayer();        // 0x022B620C
    void CloseTrack(int index); // 0x022B6198

    Engine* engine_;
    std::span<const uint8_t> data_;
    uint32_t allocate_track_flags_;
    uint32_t start_offset_;
    std::vector<uint32_t> bank_items_;                      // the bank in each slot, by item ID
    std::array<std::unique_ptr<SequenceTrack>, 16> tracks_; // +0x1C8
    std::array<int16_t, 16> local_vars_;                    // +0x208
    bool started_ = false;                                  // +0x25
    bool finished_ = false;                                 // +0x27

    // What BasicSound gives the player from the sound's info and what the game sets (BasicSoundPlayer's parameters).
    float volume_ = 1.0f;            // +0x2C
    float pitch_ = 1.0f;             // +0x30
    float lpf_ = 0.0f;               // +0x34
    float biquad_value_ = 0.0f;      // +0x38
    int8_t biquad_type_ = -1;        // +0x3C: the tracks' unless 0 or more
    uint32_t pan_mode_ = 0;          // +0x40
    uint32_t pan_curve_ = 0;         // +0x44
    uint32_t output_line_ = 1;       // +0x48
    OutputParam tv_;                 // +0x50
    std::array<OutputParam, 2> drc_; // +0xA0

    bool release_priority_fix_ = false; // +0x190
    float pan_range_ = 1.0f;            // +0x194
    float tempo_ratio_ = 1.0f;          // +0x198
    float tick_fraction_ = 0.0f;        // +0x19C
    uint8_t channel_priority_ = 64;     // +0x1AC
    uint8_t timebase_ = 48;             // +0x1AD
    uint16_t tempo_ = 120;              // +0x1AE
    MoveValue<uint8_t> main_volume_;    // +0x1B0
    uint32_t tick_count_ = 0;           // +0x228

    // What the ripper's length analysis follows while on_loop is set: the commands that the tracks have run, when a
    // track last changed the sound (ChangedAt), and the player's and the global variables that a track has read, a bit
    // each.
    uint64_t commands_run_ = 0;
    uint64_t changed_at_ = 0;
    uint32_t variables_read_ = 0;
};

} // namespace csf::nwsnd
