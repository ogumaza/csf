// SPDX-License-Identifier: MIT

#include "nwsnd/sequence.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "nwsnd/engine.h"
#include "nwsnd/util.h"

namespace csf::nwsnd
{
namespace
{

// A tick's commands may run no further than this (ParseNextTick, 0x022B6F14).
constexpr int kParseLimit = 10000;

// A sound frame of 3 ms, in 1/65536 ms (UpdateTick, 0x022B7368).
constexpr uint64_t kFrameUnits = 0x30000;

// A held note, for the ripper's length analysis: one of this many ticks or more. That's 200 beats at the default
// timebase, and minutes at any usual tempo. A sound that plays until the game stops it gives a note this length. The
// game then stops the sound before the note ends. Music's notes are far shorter.
constexpr int32_t kHeldTicks = 9600;

// The PowerPC's fsel with 0 as the third operand: `x` when it's 0 or more, else 0.
float AtLeastZero(float x)
{
    return x >= 0.0f ? x : 0.0f;
}

} // namespace

// ----------------------------------------------------------------------------------------------------- SequenceTrack

SequenceTrack::SequenceTrack(SequenceSoundPlayer& player, int index) : player_(&player), index_(index)
{
    InitParam();
}

SequenceTrack::~SequenceTrack()
{
    for (Channel* channel = channel_list_; channel; channel = channel->next_in_track)
    {
        channel->callback = nullptr;
    }
}

void SequenceTrack::InitParam()
{
    volume_ = 1.0f;
    pitch_ = 1.0f;
    pan_range_ = 1.0f;
    tv_ = OutputParam{};
    drc_ = {};
    mute_ = false;
    bank_index_ = 0;
    note_finish_wait_ = false;
    note_wait_ = true;
    wait_ = 0;
    cmp_flag_ = true;
    data_ = {};
    has_data_ = false;
    porta_ = false;
    silence_ = false;
    mono_ = false;
    prg_no_ = 0;
    damper_ = false;
    tie_ = false;
    call_depth_ = 0;
    current_ = 0;
    lfo_params_ = {};
    lfo_types_ = {};
    vol_.Init(127);
    volume2_.Init(127);
    pan_.Init(0);
    span_.Init(0);
    bend_.Init(0);
    porta_key_ = 60;
    velocity_range_ = 127;
    bend_range_ = 2;
    hold_ = 0xFF;
    attack_ = decay_ = sustain_ = release_ = 0xFF;
    main_send_ = 127;
    init_pan_ = 0;
    transpose_ = 0;
    sweep_pitch_ = 0.0f;
    porta_time_ = 0;
    priority_ = 64;
    fx_send_ = {};
    lpf_ = 0.0f;
    biquad_value_ = 0.0f;
    biquad_type_ = 0;
    output_line_ = -1;
    vars_.fill(-1);
}

void SequenceTrack::SetSeqData(std::span<const uint8_t> data, uint32_t offset)
{
    data_ = data;
    current_ = offset;
    has_data_ = true;
}

void SequenceTrack::Open()
{
    wait_ = 0;
    open_ = true;
    call_depth_ = 0;
    note_finish_wait_ = false;
}

void SequenceTrack::Close()
{
    ReleaseAllChannel(-1);
    FreeAllChannel();
    open_ = false;
}

void SequenceTrack::ReleaseAllChannel(int release)
{
    UpdateChannelParam();
    for (Channel* channel = channel_list_; channel; channel = channel->next_in_track)
    {
        if (channel->active)
        {
            if (release >= 0)
            {
                channel->envelope.SetRelease(static_cast<uint8_t>(release));
            }
            channel->Release();
        }
    }
}

void SequenceTrack::FreeAllChannel()
{
    // Channel::DetachChannel (0x022B6104) for each.
    for (Channel* channel = channel_list_; channel; channel = channel->next_in_track)
    {
        channel->callback = nullptr;
    }
    channel_list_ = nullptr;
}

void SequenceTrack::StopAllChannel()
{
    for (Channel* channel = channel_list_; channel;)
    {
        Channel* next = channel->next_in_track;
        channel->Stop();
        channel->CallChannelCallback(Channel::CallbackStatus::kStopped);
        player_->GetEngine().Channels().Free(*channel);
        channel = next;
    }
    channel_list_ = nullptr;
}

void SequenceTrack::SetMute(int mode)
{
    switch (mode)
    {
    case 0:
        mute_ = false;
        break;
    case 1:
        mute_ = true;
        break;
    case 2:
        ReleaseAllChannel(-1);
        FreeAllChannel();
        mute_ = true;
        break;
    case 3:
        StopAllChannel();
        mute_ = true;
        break;
    default:
        break;
    }
}

void SequenceTrack::UpdateChannelLength()
{
    if (!open_)
    {
        return;
    }

    for (Channel* channel = channel_list_; channel; channel = channel->next_in_track)
    {
        if (channel->length > 0)
        {
            --channel->length;
        }

        // UpdateChannelRelease (0x022B6E3C).
        if (channel->length == 0 && channel->envelope.GetStatus() != CurveAdshr::Status::kRelease && !damper_)
        {
            channel->NoteOff();
        }

        if (!channel->auto_sweep)
        {
            channel->UpdateSweep(1);
        }
    }
}

int SequenceTrack::ParseNextTick(bool do_note_on)
{
    if (!open_)
    {
        return 0;
    }

    vol_.Update();
    volume2_.Update();
    pan_.Update();
    span_.Update();
    bend_.Update();

    if (note_finish_wait_)
    {
        if (channel_list_)
        {
            return 1;
        }
        note_finish_wait_ = false;
    }

    if (wait_ > 0)
    {
        --wait_;
        if (wait_ > 0)
        {
            return 1;
        }
    }

    if (!has_data_)
    {
        return 1;
    }

    int count = 0;
    while (wait_ == 0 && !note_finish_wait_)
    {
        if (++count > kParseLimit)
        {
            break;
        }
        if (RunCommand(do_note_on) == 1)
        {
            return -1;
        }
    }

    return 1;
}

uint8_t SequenceTrack::ReadByte()
{
    if (current_ >= data_.size())
    {
        // Past the bytecode's end: as if it ended there.
        current_ = static_cast<uint32_t>(data_.size()) + 1;
        return 0xFF;
    }

    return data_[current_++];
}

int16_t* SequenceTrack::Variable(int index)
{
    if (index < 0x20)
    {
        return player_->Variable(index);
    }
    if (index < 0x30)
    {
        return &vars_[index - 0x20];
    }

    return nullptr;
}

int32_t SequenceTrack::ReadArg(int type)
{
    switch (type)
    {
    case kArgU8:
        return ReadByte();

    case kArgS16:
        {
            const uint32_t high = ReadByte();
            const uint32_t low = ReadByte();
            return static_cast<int32_t>(((high << 8) & 0xFFFF) | low);
        }

    case kArgVmidi:
        {
            uint32_t value = 0;
            uint8_t byte;
            do
            {
                byte = ReadByte();
                value = (byte & 0x7F) | (value << 7);
            } while ((byte & 0x80) && current_ <= data_.size());
            return static_cast<int32_t>(value);
        }

    case kArgRandom:
        {
            const uint32_t high1 = ReadByte();
            const uint32_t low1 = ReadByte();
            const int32_t low = static_cast<int16_t>(((high1 << 8) & 0xFFFF) | low1);
            const uint32_t high2 = ReadByte();
            const uint32_t low2 = ReadByte();
            const int32_t high = static_cast<int16_t>(((high2 << 8) & 0xFFFF) | low2);
            const uint32_t random = player_->GetEngine().Rng().Next();
            drew_at_ = player_->commands_run_;
            const int32_t product = static_cast<int32_t>(static_cast<uint32_t>((high - low) + 1) * random);
            return low + (product >> 16);
        }

    case kArgVariable:
        {
            const int index = ReadByte();
            MarkRead(index);
            const int16_t* variable = Variable(index);
            return variable ? *variable : 0;
        }

    default:
        return 0;
    }
}

SequenceTrack::SoundState SequenceTrack::State() const
{
    SoundState state{};
    state.prg_no = prg_no_;
    state.bank_index = bank_index_;
    state.transpose = transpose_;
    state.velocity_range = velocity_range_;
    state.bend_range = bend_range_;
    state.priority = priority_;
    state.porta_key = porta_key_;
    state.porta_time = porta_time_;
    state.main_send = main_send_;
    state.init_pan = init_pan_;
    state.biquad_type = biquad_type_;
    state.envelope = {attack_, decay_, sustain_, release_};
    state.hold = hold_;
    state.fx_send = fx_send_;
    state.lpf = lpf_;
    state.biquad_value = biquad_value_;
    state.sweep_pitch = sweep_pitch_;
    state.output_line = output_line_;
    state.lfo_params = lfo_params_;
    state.lfo_types = lfo_types_;
    state.mute = mute_;
    state.silence = silence_;
    state.tie = tie_;
    state.mono = mono_;
    state.porta = porta_;
    state.damper = damper_;
    state.front_bypass = front_bypass_;
    state.note_wait = note_wait_;

    std::size_t at = 0;
    const auto ramp = [&state, &at](const auto& value)
    {
        state.ramps[at++] = value.Get();
        state.ramps[at++] = value.target;
        state.ramps[at++] = value.duration - value.counter;
    };
    ramp(vol_);
    ramp(volume2_);
    ramp(pan_);
    ramp(span_);
    ramp(bend_);
    ramp(player_->main_volume_);

    state.newest = channel_list_;
    state.timebase = player_->timebase_;
    state.tempo = player_->tempo_;

    return state;
}

int SequenceTrack::RunCommand(bool do_note_on)
{
    if (!player_->on_loop)
    {
        return Parse(do_note_on);
    }

    const uint64_t now = ++player_->commands_run_;
    run_.resize(data_.size() + 1);
    run_[std::min<std::size_t>(current_, data_.size())] = now;
    const SoundState before = State();
    const int result = Parse(do_note_on);
    if (State() != before)
    {
        MarkChange();
    }

    return result;
}

int SequenceTrack::Parse(bool do_note_on)
{
    // Returns 1 at the bytecode's end (FF).
    int arg_type = kArgNone;
    int time_type = kArgNone;
    bool condition = true;
    held_wait_ = false;
    uint32_t command = ReadByte();
    conditional_ = command == 0xA2;
    if (conditional_) // if
    {
        conditional_at_ = player_->commands_run_;
        command = ReadByte();
        condition = cmp_flag_;
    }

    if (command == 0xA3) // time
    {
        time_type = kArgS16;
        command = ReadByte();
    }
    else if (command == 0xA4) // time random
    {
        time_type = kArgRandom;
        command = ReadByte();
    }
    else if (command == 0xA5) // time variable
    {
        time_type = kArgVariable;
        command = ReadByte();
    }

    if (command == 0xA0) // random
    {
        command = ReadByte();
        arg_type = kArgRandom;
    }
    else if (command == 0xA1) // variable
    {
        command = ReadByte();
        arg_type = kArgVariable;
    }

    // A random or variable prefix gives the argument's type. Without one, each command reads the type it takes.
    const auto read_arg = [&](int type)
    {
        return ReadArg(arg_type != kArgNone ? arg_type : type);
    };

    if ((command & 0x80) == 0)
    {
        // A note: the command is the key, and the velocity and the length follow.
        const uint8_t velocity = ReadByte();
        const int32_t length = read_arg(kArgVmidi);
        if (!condition)
        {
            return 0;
        }

        const int key = std::clamp(static_cast<int>(command) + static_cast<int>(transpose_), 0, 127);
        noted_at_ = player_->commands_run_;
        holds_ = length >= kHeldTicks;
        if (!mute_ && do_note_on && NoteOn(key, velocity, length > 0 ? length : -1, tie_))
        {
            MarkChange();
        }
        if (note_wait_)
        {
            wait_ = length;
            held_wait_ = holds_;
            if (length == 0)
            {
                note_finish_wait_ = true;
            }
        }
        return 0;
    }

    switch (command & 0xF0)
    {
    case 0x80:
        switch (command)
        {
        case 0x80: // wait
            {
                const int32_t wait = read_arg(kArgVmidi);
                if (condition)
                {
                    wait_ = wait;
                }
                return 0;
            }

        case 0x81: // program
            {
                const int32_t program = read_arg(kArgVmidi);
                if (condition)
                {
                    CommandProc(command, program, 0);
                }
                return 0;
            }

        case 0x88: // open a track
            {
                const uint32_t track = ReadByte();
                uint32_t offset = ReadByte();
                offset = (offset << 8) | ReadByte();
                offset = (offset << 8) | ReadByte();
                if (condition)
                {
                    CommandProc(command, static_cast<int32_t>(track), static_cast<int32_t>(offset));
                }
                return 0;
            }

        case 0x89: // jump
        case 0x8A: // call
            {
                uint32_t offset = ReadByte();
                offset = (offset << 8) | ReadByte();
                offset = (offset << 8) | ReadByte();
                if (condition)
                {
                    CommandProc(command, static_cast<int32_t>(offset), 0);
                }
                return 0;
            }

        default:
            return 0;
        }

    case 0x90:
        if (condition)
        {
            CommandProc(command, 0, 0);
        }
        return 0;

    case 0xB0:
    case 0xC0:
    case 0xD0:
        {
            const uint32_t value = static_cast<uint32_t>(read_arg(kArgU8)) & 0xFF;
            int32_t time = 0;
            if (time_type != kArgNone)
            {
                time = ReadArg(time_type);
            }
            if (!condition)
            {
                return 0;
            }
            const int32_t arg =
                command == 0xC3 || command == 0xC4 ? static_cast<int8_t>(value) : static_cast<int32_t>(value);
            CommandProc(command, arg, time);
            return 0;
        }

    case 0xE0:
        {
            const int32_t value = static_cast<int16_t>(read_arg(kArgS16));
            if (condition)
            {
                CommandProc(command, value, 0);
            }
            return 0;
        }

    case 0xF0:
        if (command == 0xF0)
        {
            const uint32_t sub = ReadByte();
            switch (sub & 0xF0)
            {
            case 0x80:
            case 0x90:
                {
                    const uint32_t variable = ReadByte();
                    const int32_t value = static_cast<int16_t>(read_arg(kArgS16));
                    if (condition)
                    {
                        CommandProc(0xF000 + sub, static_cast<int32_t>(variable), value);
                    }
                    return 0;
                }

            case 0xA0:
            case 0xB0:
                {
                    // The extra LFOs' settings: a plain byte. No prefix changes it.
                    const uint32_t value = ReadByte();
                    if (condition)
                    {
                        CommandProc(0xF000 + sub, static_cast<int32_t>(value), 0);
                    }
                    return 0;
                }

            case 0xE0:
                {
                    const int32_t value = static_cast<uint16_t>(read_arg(kArgS16));
                    if (condition)
                    {
                        CommandProc(0xF000 + sub, value, 0);
                    }
                    return 0;
                }

            default:
                return 0;
            }
        }

        if (command == 0xFE) // the tracks to allocate: the sound's setup reads them
        {
            ReadByte();
            ReadByte();
            return 0;
        }

        if (command == 0xFF) // the end
        {
            return condition ? 1 : 0;
        }

        if (condition)
        {
            CommandProc(command, 0, 0);
        }
        return 0;

    default:
        return 0;
    }
}

void SequenceTrack::CommandProc(uint32_t command, int32_t arg1, int32_t arg2)
{
    if (command > 0xFF)
    {
        const uint32_t op = command & 0xFF;
        int16_t* variable = nullptr;
        if ((op & 0xF0) == 0x80 || (op & 0xF0) == 0x90)
        {
            variable = Variable(arg1);
            if (!variable)
            {
                return;
            }
        }

        const int32_t value = arg2;
        const int32_t current = variable ? *variable : 0;
        if ((op & 0xF0) == 0x90)
        {
            MarkRead(arg1);
        }
        const auto set = [variable](int32_t result)
        {
            *variable = static_cast<int16_t>(result);
        };
        const auto lfo_byte = [](int32_t arg)
        {
            return static_cast<uint32_t>(arg) & 0xFF;
        };
        const auto lfo_period = [](int32_t arg)
        {
            const int16_t period = static_cast<int16_t>(arg);
            return period == 0 ? 0.0f : 100.0f / static_cast<float>(period);
        };
        switch (op)
        {
        case 0x80: // set
            set(value);
            break;
        case 0x81: // add
            set(current + value);
            break;
        case 0x82: // subtract
            set(current - value);
            break;
        case 0x83: // multiply
            set(static_cast<int32_t>(static_cast<uint32_t>(current) * static_cast<uint32_t>(value)));
            break;
        case 0x84: // divide
            if (value != 0)
            {
                set(current / static_cast<int16_t>(value));
            }
            break;
        case 0x85: // shift: PowerPC's shifts take the amount's low six bits, and 32 or more gives 0 or the sign
            if (value >= 0)
            {
                const uint32_t shift = static_cast<uint32_t>(value) & 0x3F;
                set(shift >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(current) << shift));
            }
            else
            {
                const uint32_t shift = static_cast<uint32_t>(-value) & 0x3F;
                set(shift >= 32 ? (current < 0 ? -1 : 0) : current >> shift);
            }
            break;
        case 0x86: // random
            {
                int32_t range = value;
                const bool negative = range < 0;
                if (negative)
                {
                    range = static_cast<int16_t>(-range);
                }
                const uint32_t random = player_->GetEngine().Rng().Next();
                drew_at_ = player_->commands_run_;
                int32_t result = static_cast<int32_t>(random * static_cast<uint32_t>(range + 1)) >> 16;
                if (negative)
                {
                    result = -result;
                }
                set(result);
                break;
            }
        case 0x87: // and
            set(current & value);
            break;
        case 0x88: // or
            set(current | value);
            break;
        case 0x89: // exclusive or
            set(current ^ value);
            break;
        case 0x8A: // not
            set(~value);
            break;
        case 0x8B: // modulo
            if (value != 0)
            {
                set(current - (current / value) * value);
            }
            break;
        case 0x90:
            cmp_flag_ = current == value;
            break;
        case 0x91:
            cmp_flag_ = current >= value;
            break;
        case 0x92:
            cmp_flag_ = current > value;
            break;
        case 0x93:
            cmp_flag_ = current <= value;
            break;
        case 0x94:
            cmp_flag_ = current < value;
            break;
        case 0x95:
            cmp_flag_ = current != value;
            break;

        // LFOs 1 to 3: curve, phase, depth, speed, type and range, then the delay and the period.
        case 0xA0:
            lfo_params_[1].curve = static_cast<uint8_t>(arg1);
            break;
        case 0xA1:
            lfo_params_[1].phase = static_cast<uint8_t>(arg1);
            break;
        case 0xA2:
            lfo_params_[1].depth = static_cast<float>(lfo_byte(arg1)) * 0.0078125f;
            break;
        case 0xA3:
            lfo_params_[1].speed = static_cast<float>(lfo_byte(arg1)) * 0.390625f;
            break;
        case 0xA4:
            lfo_types_[1] = static_cast<uint8_t>(arg1);
            break;
        case 0xA5:
            lfo_params_[1].range = static_cast<uint8_t>(arg1);
            break;
        case 0xA6:
            lfo_params_[2].curve = static_cast<uint8_t>(arg1);
            break;
        case 0xA7:
            lfo_params_[2].phase = static_cast<uint8_t>(arg1);
            break;
        case 0xA8:
            lfo_params_[2].depth = static_cast<float>(lfo_byte(arg1)) * 0.0078125f;
            break;
        case 0xA9:
            lfo_params_[2].speed = static_cast<float>(lfo_byte(arg1)) * 0.390625f;
            break;
        case 0xAA:
            lfo_types_[2] = static_cast<uint8_t>(arg1);
            break;
        case 0xAB:
            lfo_params_[2].range = static_cast<uint8_t>(arg1);
            break;
        case 0xAC:
            lfo_params_[3].curve = static_cast<uint8_t>(arg1);
            break;
        case 0xAD:
            lfo_params_[3].phase = static_cast<uint8_t>(arg1);
            break;
        case 0xAE:
            lfo_params_[3].depth = static_cast<float>(lfo_byte(arg1)) * 0.0078125f;
            break;
        case 0xAF:
            lfo_params_[3].speed = static_cast<float>(lfo_byte(arg1)) * 0.390625f;
            break;
        case 0xB0:
            lfo_types_[3] = static_cast<uint8_t>(arg1);
            break;
        case 0xB1:
            lfo_params_[3].range = static_cast<uint8_t>(arg1);
            break;
        case 0xE1:
            lfo_params_[1].delay = static_cast<uint32_t>(arg1 * 5);
            break;
        case 0xE2:
            lfo_params_[1].speed = lfo_period(arg1);
            break;
        case 0xE3:
            lfo_params_[2].delay = static_cast<uint32_t>(arg1 * 5);
            break;
        case 0xE4:
            lfo_params_[2].speed = lfo_period(arg1);
            break;
        case 0xE5:
            lfo_params_[3].delay = static_cast<uint32_t>(arg1 * 5);
            break;
        case 0xE6:
            lfo_params_[3].speed = lfo_period(arg1);
            break;

        case 0xE0: // the game's procedure: archive mode has none
        default:
            break;
        }
        return;
    }

    const uint8_t byte = static_cast<uint8_t>(arg1);
    switch (command)
    {
    case 0x81: // program
        if (arg1 < 0x10000)
        {
            prg_no_ = static_cast<uint16_t>(arg1);
        }
        break;

    case 0x88: // open a track
        {
            SequenceTrack* track = player_->GetTrack(arg1);
            if (!track || track == this)
            {
                break;
            }
            track->Close();
            track->SetSeqData(data_, static_cast<uint32_t>(arg2));
            track->Open();
            MarkChange(); // the new track's sound is this pass's change
            break;
        }

    case 0x89: // jump
        {
            // A jump back to a command that the track has run is a loop, outside a subroutine. Inside a subroutine,
            // such as one that draws a random value again until it differs from the last, the jump isn't a loop. An
            // unconditional jump inside a subroutine is still a loop when the track holds a note that sounds until the
            // game stops it. A subroutine that bends a held note's pitch for ever is an example. It's a loop too when
            // the track has run no conditional command since the jump's target. Nothing can then leave the loop. A
            // melody that steps through its notes this way and leaves through a conditional command isn't a loop.
            const auto target = static_cast<uint32_t>(arg1);
            if (target < current_ && target < run_.size() && run_[target] != 0)
            {
                const auto call = [](const CallStackEntry& entry)
                {
                    return !entry.is_loop;
                };

                const bool in_call = std::any_of(call_stack_.begin(), call_stack_.begin() + call_depth_, call);
                if (!in_call || (!conditional_ && (HoldsSustainedNote() || conditional_at_ < run_[target])))
                {
                    CountLoop(target);
                }
            }
            current_ = target;
            break;
        }

    case 0x8A: // call
        if (call_depth_ >= kCallStackDepth)
        {
            break;
        }
        call_stack_[call_depth_].address = current_;
        call_stack_[call_depth_].is_loop = false;
        ++call_depth_;
        current_ = static_cast<uint32_t>(arg1);
        break;

    case 0xB0:
        player_->timebase_ = byte;
        break;

    case 0xB1:
        hold_ = byte;
        break;

    case 0xB2: // monophonic
        mono_ = arg1 != 0;
        if (mono_)
        {
            ReleaseAllChannel(-1);
            FreeAllChannel();
        }
        break;

    case 0xB3:
        velocity_range_ = byte;
        break;

    case 0xB4:
        biquad_type_ = static_cast<int8_t>(byte);
        break;

    case 0xB5:
        biquad_value_ = static_cast<float>(arg1) / 127.0f;
        break;

    case 0xB6:
        bank_index_ = byte;
        break;

    case 0xBD:
        lfo_params_[0].phase = byte;
        break;

    case 0xBE:
        lfo_params_[0].curve = byte;
        break;

    case 0xBF: // front bypass
        front_bypass_ = arg1 != 0;
        break;

    case 0xC0:
        pan_.SetTarget(static_cast<int8_t>(arg1 - 0x40), static_cast<int16_t>(arg2));
        break;

    case 0xC1:
        vol_.SetTarget(byte, static_cast<int16_t>(arg2));
        break;

    case 0xC2:
        player_->main_volume_.SetTarget(byte, static_cast<int16_t>(arg2));
        break;

    case 0xC3:
        transpose_ = static_cast<int8_t>(arg1);
        break;

    case 0xC4:
        bend_.SetTarget(static_cast<int8_t>(arg1), static_cast<int16_t>(arg2));
        break;

    case 0xC5:
        bend_range_ = byte;
        break;

    case 0xC6:
        priority_ = byte;
        break;

    case 0xC7:
        note_wait_ = arg1 != 0;
        break;

    case 0xC8: // tie
        tie_ = arg1 != 0;
        ReleaseAllChannel(-1);
        FreeAllChannel();
        break;

    case 0xC9: // portamento from a key
        porta_ = true;
        porta_key_ = static_cast<uint8_t>(arg1 + transpose_);
        break;

    case 0xCA:
        lfo_params_[0].depth = static_cast<float>(static_cast<uint32_t>(byte)) * 0.0078125f;
        break;

    case 0xCB:
        lfo_params_[0].speed = static_cast<float>(static_cast<uint32_t>(byte)) * 0.390625f;
        break;

    case 0xCC:
        lfo_types_[0] = byte;
        break;

    case 0xCD:
        lfo_params_[0].range = byte;
        break;

    case 0xCE:
        porta_ = arg1 != 0;
        break;

    case 0xCF:
        porta_time_ = byte;
        break;

    case 0xD0:
        attack_ = byte;
        break;

    case 0xD1:
        decay_ = byte;
        break;

    case 0xD2:
        sustain_ = byte;
        break;

    case 0xD3:
        release_ = byte;
        break;

    case 0xD4: // loop start
        if (call_depth_ < kCallStackDepth)
        {
            call_stack_[call_depth_].address = current_;
            call_stack_[call_depth_].is_loop = true;
            call_stack_[call_depth_].loop_count = byte;
            ++call_depth_;
        }
        break;

    case 0xD5:
        volume2_.SetTarget(byte, static_cast<int16_t>(arg2));
        break;

    case 0xD6: // print a variable: only a debug flag in the game shows it
        break;

    case 0xD7:
        span_.SetTarget(static_cast<int8_t>(arg1), static_cast<int16_t>(arg2));
        break;

    case 0xD8:
        lpf_ = static_cast<float>(arg1 - 0x40) * 0.015625f;
        break;

    case 0xD9:
        fx_send_[0] = byte;
        break;

    case 0xDA:
        fx_send_[1] = byte;
        break;

    case 0xDB:
        main_send_ = byte;
        break;

    case 0xDC:
        init_pan_ = static_cast<int8_t>(arg1 - 0x40);
        break;

    case 0xDD:
        SetMute(byte);
        break;

    case 0xDE:
        fx_send_[2] = byte;
        break;

    case 0xDF:
        damper_ = byte >= 0x40;
        break;

    case 0xE0:
        lfo_params_[0].delay = static_cast<uint32_t>(arg1 * 5);
        break;

    case 0xE1:
        player_->tempo_ = static_cast<uint16_t>(std::clamp(arg1, 0, 0x3FF));
        break;

    case 0xE3:
        sweep_pitch_ = static_cast<float>(arg1) * 0.015625f;
        break;

    case 0xE4: // LFO 0's period, in 10 ms
        {
            const int16_t period = static_cast<int16_t>(arg1);
            lfo_params_[0].speed = period == 0 ? 0.0f : 100.0f / static_cast<float>(period);
            break;
        }

    case 0xFB: // the envelope back to the bank's
        attack_ = decay_ = sustain_ = release_ = 0xFF;
        hold_ = 0xFF;
        break;

    case 0xFC: // loop end
        {
            if (call_depth_ == 0)
            {
                break;
            }
            CallStackEntry& entry = call_stack_[call_depth_ - 1];
            if (!entry.is_loop)
            {
                break;
            }
            uint8_t count = entry.loop_count;
            if (count != 0)
            {
                --count;
                if (count == 0)
                {
                    --call_depth_;
                    break;
                }
            }
            else
            {
                // A count of 0 loops for ever. On the main track, that's a loop of the sequence.
                CountLoop(entry.address);
            }
            entry.loop_count = count;
            current_ = entry.address;
            break;
        }

    case 0xFD: // return
        while (call_depth_ != 0)
        {
            --call_depth_;
            if (!call_stack_[call_depth_].is_loop)
            {
                current_ = call_stack_[call_depth_].address;
                break;
            }
        }
        break;

    default:
        break;
    }
}

void SequenceTrack::CountLoop(uint32_t target)
{
    // The pass began when the track last ran `target`. A still pass also left the variables that tracks read with the
    // values that the pass before left. The first pass compares them with 0. A variable that no track reads doesn't
    // count. The game reads some, such as the beats that a timing track marks.
    const uint64_t begun = target < run_.size() ? run_[target] : 0;
    std::array<int16_t, 48> variables{};
    for (int i = 0; i < static_cast<int>(variables.size()); ++i)
    {
        const int16_t* variable = IsRead(i) ? Variable(i) : nullptr;
        variables[static_cast<std::size_t>(i)] = variable ? *variable : 0;
    }
    idle_ = changed_at_ < begun;
    still_ = idle_ && drew_at_ < begun && variables == loop_variables_;
    loop_variables_ = variables;
    looped_at_ = player_->commands_run_;

    // The pass played a note if the track has played one since the pass began. A loop inside the pass doesn't hide the
    // notes before that loop.
    const bool noted = noted_at_ != 0 && noted_at_ >= begun;
    const bool alone = HoldsSustainedNote() && player_->OthersRest(index_);
    if (player_->on_loop && !idle_ && (index_ == 0 || (noted && player_->IsMainTrack(index_)) || alone))
    {
        player_->on_loop(index_, target);
    }
    quiet_ = !noted;
}

void SequenceTrack::MarkChange()
{
    changed_at_ = player_->commands_run_;
    player_->changed_at_ = changed_at_;
}

void SequenceTrack::MarkRead(int index)
{
    if (index >= 0 && index < 32)
    {
        player_->variables_read_ |= 1u << index;
    }
    else if (index >= 32 && index < 48)
    {
        variables_read_ = static_cast<uint16_t>(variables_read_ | 1u << (index - 32));
    }
}

bool SequenceTrack::IsRead(int index) const
{
    if (index >= 0 && index < 32)
    {
        return (player_->variables_read_ >> index & 1) != 0;
    }
    if (index >= 32 && index < 48)
    {
        return (variables_read_ >> (index - 32) & 1) != 0;
    }

    return false;
}

bool SequenceTrack::WaitsOnEndlessNote() const
{
    if (!note_finish_wait_)
    {
        return false;
    }

    for (const Channel* channel = channel_list_; channel; channel = channel->next_in_track)
    {
        if (channel->length < 0 && channel->WaveLoops())
        {
            return true;
        }
    }

    return false;
}

bool SequenceTrack::HasEndingNote() const
{
    if (holds_ || damper_)
    {
        return false;
    }

    for (const Channel* channel = channel_list_; channel; channel = channel->next_in_track)
    {
        if (channel->length > 0)
        {
            return true;
        }
    }

    return false;
}

bool SequenceTrack::HoldsSustainedNote() const
{
    for (const Channel* channel = channel_list_; channel; channel = channel->next_in_track)
    {
        const bool endless = holds_ || channel->length < 0;
        if (endless && channel->envelope.GetStatus() == CurveAdshr::Status::kSustain && channel->WaveLoops())
        {
            return true;
        }
    }

    return false;
}

Channel::Callback SequenceTrack::MakeCallback()
{
    return [this](Channel& channel, Channel::CallbackStatus)
    {
        OnChannelEvent(channel);
    };
}

void SequenceTrack::OnChannelEvent(Channel& channel)
{
    // SequenceSoundPlayer::ChannelCallback (0x022CE614) does nothing. The channel leaves the track's list.
    if (channel_list_ == &channel)
    {
        channel_list_ = channel.next_in_track;
        return;
    }
    for (Channel* previous = channel_list_; previous; previous = previous->next_in_track)
    {
        if (previous->next_in_track == &channel)
        {
            previous->next_in_track = channel.next_in_track;
            return;
        }
    }
}

bool SequenceTrack::NoteOn(int key, int velocity, int32_t length, bool tie)
{
    const int scaled = velocity * velocity_range_ / 127;
    const auto velocity_volume = [scaled]
    {
        // Bank::CalcChannelVelocityVolume (0x022C49D4).
        return static_cast<float>(static_cast<uint8_t>(scaled)) * (1.0f / 127.0f);
    };

    const Channel* newest = channel_list_;
    const bool same = newest && newest->key == key && newest->velocity_volume == velocity_volume();
    Channel* channel = nullptr;
    if (tie)
    {
        channel = channel_list_;
        if (channel)
        {
            channel->key = static_cast<uint8_t>(key);
            channel->velocity_volume = velocity_volume();
        }
    }

    if (mono_)
    {
        channel = channel_list_;
        if (channel)
        {
            if (channel->envelope.GetStatus() == CurveAdshr::Status::kRelease)
            {
                channel->Stop();
                channel->CallChannelCallback(Channel::CallbackStatus::kStopped);
                player_->GetEngine().Channels().Free(*channel);
                channel = nullptr;
            }
            else
            {
                channel->key = static_cast<uint8_t>(key);
                channel->velocity_volume = velocity_volume();
                channel->length = length;
            }
        }
    }

    if (!channel)
    {
        NoteOnInfo info;
        info.prg_no = prg_no_;
        info.key = key;
        info.velocity = scaled;
        info.length = tie ? -1 : length;
        info.init_pan = init_pan_;
        info.priority = static_cast<uint32_t>(player_->channel_priority_) + priority_;
        info.callback = MakeCallback();
        channel = player_->GetEngine().NoteOn(player_->bank_items_, bank_index_, info);
        if (!channel)
        {
            return false;
        }

        if (channel->key_group != 0)
        {
            for (Channel* other = channel_list_; other; other = other->next_in_track)
            {
                if (other->key_group == channel->key_group)
                {
                    other->envelope.SetRelease(0x7E);
                    other->Release();
                }
            }
        }

        // AddChannel (0x022C9A3C).
        channel->next_in_track = channel_list_;
        channel_list_ = channel;
    }

    if (attack_ <= 127)
    {
        channel->envelope.SetAttack(attack_);
    }
    if (decay_ <= 127)
    {
        channel->envelope.SetDecay(decay_);
    }
    if (sustain_ <= 127)
    {
        channel->envelope.SetSustain(sustain_);
    }
    if (release_ <= 127)
    {
        channel->envelope.SetRelease(release_);
    }
    if (hold_ <= 127)
    {
        channel->envelope.SetHold(hold_);
    }

    float sweep = sweep_pitch_;
    if (porta_)
    {
        sweep = sweep + static_cast<float>(static_cast<int>(porta_key_) - key);
    }
    if (porta_time_ == 0)
    {
        channel->SetSweepParam(sweep, length, false);
    }
    else
    {
        const int squared = porta_time_ * porta_time_;
        const float magnitude = sweep >= 0.0f ? sweep : -sweep;
        const int time = util::ToInt32(static_cast<float>(squared) * magnitude) >> 5;
        channel->SetSweepParam(sweep, time * 5, true);
    }
    porta_key_ = static_cast<uint8_t>(key);

    // The silence ramp starts over from where it stands. The new target is the track's silence.
    const uint8_t silence = silence_ ? 0 : 255;
    if (channel->silence_counter >= channel->silence_duration)
    {
        channel->silence_duration = 0;
        channel->silence_from = channel->silence_to;
        channel->silence_to = silence;
        channel->silence_counter = 0;
    }
    else
    {
        const int32_t change = (static_cast<int32_t>(channel->silence_to) - channel->silence_from) *
                               channel->silence_counter / channel->silence_duration;
        channel->silence_duration = 0;
        channel->silence_counter = 0;
        channel->silence_from = static_cast<uint8_t>(channel->silence_from + change);
        channel->silence_to = silence;
    }

    channel->release_priority_fix = player_->release_priority_fix_;
    channel->pan_mode = player_->pan_mode_;
    channel->pan_curve = player_->pan_curve_;
    if (channel->voice)
    {
        channel->voice->SetFrontBypass(front_bypass_);
    }

    return channel != newest || !same || sweep != 0.0f;
}

void SequenceTrack::UpdateChannelParam()
{
    if (!open_ || !channel_list_)
    {
        return;
    }

    const SequenceSoundPlayer& player = *player_;
    const uint32_t level = static_cast<uint32_t>(vol_.Get()) * volume2_.Get() * player.main_volume_.Get();
    float volume = static_cast<float>(static_cast<int32_t>(level)) / 2048383.0f;
    volume = volume * volume;
    volume = volume * volume_;
    volume = volume * player.volume_;

    const float bend = (static_cast<float>(bend_.Get()) * 0.0078125f) * static_cast<float>(bend_range_);
    const float pitch = player.pitch_ * pitch_;
    const float lpf = lpf_ + player.lpf_;
    int biquad_type = biquad_type_;
    float biquad_value = biquad_value_;
    if (player.biquad_type_ != -1)
    {
        biquad_type = player.biquad_type_;
        biquad_value = player.biquad_value_;
    }
    const uint32_t output_line = output_line_ != -1 ? static_cast<uint32_t>(output_line_) : player.output_line_;

    float pan = static_cast<float>(pan_.Get()) / 63.0f;
    pan = pan > 1.0f ? 1.0f : (pan - -1.0f >= 0.0f ? pan : -1.0f);
    pan = pan * pan_range_;
    pan = pan * player.pan_range_;
    float span = static_cast<float>(span_.Get()) / 63.0f;
    span = span > 2.0f ? 2.0f : AtLeastZero(span);
    const float main_send = static_cast<float>(main_send_) / 127.0f - 1.0f;
    std::array<float, 3> fx_send;
    for (int bus = 0; bus < 3; ++bus)
    {
        fx_send[bus] = static_cast<float>(fx_send_[bus]) / 127.0f;
    }

    // Each device's output parameters combine the player's parameters, the track's parameters, and the bytecode's pan,
    // surround pan and sends.
    const auto combine = [&](const OutputParam& from_player, const OutputParam& from_track)
    {
        OutputParam param = from_player;
        param.volume = from_player.volume * from_track.volume;
        for (int channel = 0; channel < 2; ++channel)
        {
            for (int i = 0; i < 6; ++i)
            {
                param.mix_parameter[channel][i] =
                    from_player.mix_parameter[channel][i] * from_track.mix_parameter[channel][i];
            }
        }
        param.pan = (from_player.pan + pan) + from_track.pan;
        param.span = (from_player.span + span) + from_track.span;
        param.main_send = (from_player.main_send + main_send) + from_track.main_send;
        for (int bus = 0; bus < 3; ++bus)
        {
            param.fx_send[bus] = (from_player.fx_send[bus] + fx_send[bus]) + from_track.fx_send[bus];
        }

        return param;
    };
    const OutputParam tv = combine(player.tv_, tv_);
    const std::array<OutputParam, 2> drc = {combine(player.drc_[0], drc_[0]), combine(player.drc_[1], drc_[1])};

    for (Channel* channel = channel_list_; channel; channel = channel->next_in_track)
    {
        channel->bend = bend;
        channel->track_volume = volume;
        channel->user_pitch = pitch;
        channel->lpf = lpf;
        channel->biquad_type = static_cast<uint8_t>(biquad_type);
        channel->biquad_value = biquad_value;
        channel->output_line = output_line;
        for (int i = 0; i < 4; ++i)
        {
            channel->lfos[i].param = lfo_params_[i];
            channel->lfo_types[i] = lfo_types_[i];
        }
        channel->tv = tv;
        channel->drc = drc;
    }
}

// ----------------------------------------------------------------------------------------------- SequenceSoundPlayer

SequenceSoundPlayer::SequenceSoundPlayer(Engine& engine, const fsar::SoundInfo& info, std::span<const uint8_t> data)
    : engine_(&engine), data_(data), allocate_track_flags_(info.allocate_track_flags), start_offset_(info.start_offset)
{
    // What the archive player gives a sound from its info (SoundArchivePlayer::SetCommonSoundParam, 0x02295C98, and the
    // sequence's setup): the volume, a division by 127, the pan mode and curve, and the channels' priority.
    volume_ = AtLeastZero(static_cast<float>(info.volume) / 127.0f);
    pan_mode_ = info.pan_mode;
    pan_curve_ = info.pan_curve;
    channel_priority_ = info.channel_priority;
    release_priority_fix_ = info.release_priority_fix;
    bank_items_ = info.banks;
    main_volume_.Init(127);
    local_vars_.fill(-1);
}

SequenceSoundPlayer::~SequenceSoundPlayer() = default;

int16_t* SequenceSoundPlayer::Variable(int index)
{
    if (index < 0)
    {
        return nullptr;
    }
    if (index < 16)
    {
        return &local_vars_[index];
    }
    if (index < 32)
    {
        return &engine_->GlobalVariables()[index - 16];
    }

    return nullptr;
}

void SequenceSoundPlayer::Start()
{
    for (int i = 0; i < 16; ++i)
    {
        if (allocate_track_flags_ & (1u << i))
        {
            tracks_[i] = std::make_unique<SequenceTrack>(*this, i);
        }
    }
    if (tracks_[0])
    {
        tracks_[0]->SetSeqData(data_, start_offset_);
        tracks_[0]->Open();
    }
    started_ = true;
}

void SequenceSoundPlayer::Update()
{
    if (!started_ || finished_)
    {
        return;
    }

    UpdateTick();
    if (finished_)
    {
        return;
    }
    for (int i = 0; i < 16; ++i)
    {
        if (SequenceTrack* track = GetTrack(i))
        {
            track->UpdateChannelParam();
        }
    }
}

bool SequenceSoundPlayer::IsSettled() const
{
    const auto settled = [](const SequenceTrack& track)
    {
        return track.WaitsForNotes() || (track.IsStill() && !track.HasEndingNote());
    };

    return tempo_ == 0 || timebase_ == 0 || EveryOpenTrack(settled);
}

bool SequenceSoundPlayer::IsStuck() const
{
    const auto stuck = [](const SequenceTrack& track)
    {
        return track.WaitsForEver() || (track.IsStill() && track.IsQuiet());
    };

    return tempo_ == 0 || timebase_ == 0 || EveryOpenTrack(stuck);
}

bool SequenceSoundPlayer::IsMoving() const
{
    const auto still = [](const SequenceTrack& track)
    {
        return !track.IsMoving();
    };

    return main_volume_.Moving() || !EveryOpenTrack(still);
}

bool SequenceSoundPlayer::IsMainTrack(int index) const
{
    for (int i = 0; i < index; ++i)
    {
        const SequenceTrack* track = tracks_[i].get();
        if (track && track->IsOpen() && !track->IsIdle() && !track->WaitsForEver() && (i == 0 || !track->IsQuiet()))
        {
            return false;
        }
    }

    return true;
}

bool SequenceSoundPlayer::OthersRest(int index) const
{
    for (int i = 0; i < 16; ++i)
    {
        const SequenceTrack* track = tracks_[i].get();
        if (i != index && track && track->IsOpen() && !track->IsIdle() && !track->WaitsForEver())
        {
            return false;
        }
    }

    return true;
}

bool SequenceSoundPlayer::EveryOpenTrack(const std::function<bool(const SequenceTrack& track)>& test) const
{
    for (const std::unique_ptr<SequenceTrack>& track : tracks_)
    {
        if (track && track->IsOpen() && !test(*track))
        {
            return false;
        }
    }

    return true;
}

bool SequenceSoundPlayer::ParseNextTick(bool do_note_on)
{
    main_volume_.Update();

    bool any_open = false;
    for (int i = 0; i < 16; ++i)
    {
        SequenceTrack* track = GetTrack(i);
        if (!track)
        {
            continue;
        }
        track->UpdateChannelLength();
        if (track->ParseNextTick(do_note_on) < 0)
        {
            CloseTrack(i);
            continue; // the track was freed, and reads as closed
        }
        if (track->IsOpen())
        {
            any_open = true;
        }
    }

    return !any_open;
}

void SequenceSoundPlayer::UpdateTick()
{
    const auto ticks_per_ms = [this]
    {
        const float rate = static_cast<float>(static_cast<int32_t>(timebase_ * tempo_)) * tempo_ratio_;
        return rate / 60000.0f;
    };

    float rate = ticks_per_ms();
    if (rate == 0.0f)
    {
        return;
    }

    uint64_t rest = kFrameUnits;
    uint64_t next = util::ToUint64((65536.0f * tick_fraction_) / rate);
    while (next < rest)
    {
        rest -= next;
        if (ParseNextTick(true))
        {
            FinishPlayer();
            return;
        }
        rate = ticks_per_ms();
        ++tick_count_;
        if (rate == 0.0f)
        {
            return;
        }
        next = util::ToUint64(65536.0f / rate);
    }

    tick_fraction_ = (static_cast<float>(next - rest) * rate) * (1.0f / 65536.0f);
}

void SequenceSoundPlayer::FinishPlayer()
{
    started_ = false;
    for (int i = 0; i < 16; ++i)
    {
        CloseTrack(i);
    }
    finished_ = true;
}

void SequenceSoundPlayer::CloseTrack(int index)
{
    if (SequenceTrack* track = GetTrack(index))
    {
        track->Close();
        tracks_[index].reset();
    }
}

} // namespace csf::nwsnd
