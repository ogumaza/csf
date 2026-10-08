// SPDX-License-Identifier: MIT

#include "nwsnd/voice.h"

#include <array>
#include <cstdint>
#include <iterator>
#include <utility>

#include "nwsnd/tables.h"

namespace csf::nwsnd
{
namespace
{

// The PowerPC's fsel with 0 as the third operand: `x` when it's 0 or more, else 0 (NaN included).
float AtLeastZero(float x)
{
    return x >= 0.0f ? x : 0.0f;
}

// A pan curve's PanInfo (the switch in CalcPreMixVolume, 0x0229C2F4). Curves 0 to 8 are the square root, sine and
// linear curves in turn. Each curve comes plain, then centred at unity, then centred and held to unity. Anything past
// curve 8 is the plain square root.
PanInfo PanInfoOf(uint32_t pan_curve)
{
    static constexpr std::array<PanInfo, 9> kCurves = {{
        {0, false, false, false},
        {0, true, false, false},
        {0, true, true, false},
        {1, false, false, false},
        {1, true, false, false},
        {1, true, true, false},
        {2, false, false, false},
        {2, true, false, false},
        {2, true, true, false},
    }};

    return pan_curve < kCurves.size() ? kCurves[pan_curve] : kCurves[0];
}

// A biquad preset's coefficients for a value from 0 to 1 (GetCoefficients of BiquadFilterLpf, 0x022BB710, and the four
// after it): the table's row for the value, held to the table. The band-passes take the value as v(2 - v). Types 1 to 5
// are the presets. A game registers any other types. Here those types turn the filter off.
bool BiquadPreset(int type, float value, std::array<int16_t, 5>& coefs)
{
    const auto row = [](float scaled, int last)
    {
        const int32_t index = util::ToInt32(scaled);
        return index > last ? last : (index < 0 ? 0 : index);
    };
    const auto take = [&coefs](const int16_t* entry)
    {
        for (int i = 0; i < 5; ++i)
        {
            coefs[i] = entry[i];
        }
    };

    switch (type)
    {
    case 1:
        take(tables::kBiquadLpf[row(111.0f * value, 111)]);
        return true;
    case 2:
        take(tables::kBiquadHpf[row(96.0f * value, 96)]);
        return true;
    case 3:
        take(tables::kBiquadBpf512[row(121.0f * (value * (2.0f - value)), 121)]);
        return true;
    case 4:
        take(tables::kBiquadBpf1024[row(92.0f * (value * (2.0f - value)), 92)]);
        return true;
    case 5:
        take(tables::kBiquadBpf2048[row(92.0f * (value * (2.0f - value)), 92)]);
        return true;
    default:
        return false;
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------------- MultiVoice

bool MultiVoice::Alloc(int channels, uint32_t priority, Callback callback)
{
    const int count = channels > 2 ? 2 : (channels < 1 ? 1 : channels);
    channel_count_ = 0;
    int allocated = 0;
    for (; allocated < count; ++allocated)
    {
        // Voice::AllocVoice (0x022A5970) and Voice::Initialize (0x022A5920): AX's voice, stopped, with the parameters
        // that VoiceParam::Initialize (0x022A254C) sets: PCM16 at 32 kHz. When AX takes the voice away or it's freed,
        // the Voice loses it (AxVoiceDisposeCallback, 0x022A5908).
        Voice& voice = voices_[allocated];
        AxVoice* ax = manager_->ax_->AllocVoice(priority, [&voice](AxVoice&) { voice.ax = nullptr; });
        if (!ax)
        {
            break;
        }
        voice.ax = ax;
        voice.priority = priority;
        voice.state = AxVoice::State::kStop;
        voice.param = VoiceParam{};
        voice.param.tv[0][0] = 1.0f;
        voice.param.tv[0][1] = 1.0f;
        voice.format = SampleFormat::kPcm16;
        voice.sample_rate = 32000;
    }
    if (allocated != count)
    {
        for (int i = 0; i < allocated; ++i)
        {
            FreeVoice(voices_[i]);
        }
        return false;
    }

    channel_count_ = count;
    InitParam(std::move(callback));
    active_ = true;

    return true;
}

void MultiVoice::InitParam(Callback callback)
{
    callback_ = std::move(callback);
    pause_ = false;
    biquad_value_ = 0.0f;
    volume_ = 1.0f;
    pitch_ = 1.0f;
    lpf_freq_ = 1.0f;
    biquad_type_ = 0;
    front_bypass_ = false;
    started_ = false;
    sync_ = 0;
    last_buffer_ = nullptr;
    paused_ = false;
    sdk_started_ = false;
    pan_mode_ = 0;
    pan_curve_ = 0;
    for (OutputParam* param : {&tv_, &drc_[0], &drc_[1]})
    {
        param->volume = 1.0f;
        param->mix_mode = 0;
        param->pan = 0.0f;
        param->span = 0.0f;
        param->main_send = 0.0f;
        param->fx_send = {};
    }
}

void MultiVoice::Start()
{
    started_ = true;
    pause_ = false;
    sync_ |= kSyncStart;
}

void MultiVoice::Stop()
{
    if (sdk_started_)
    {
        StopAllSdkVoice();
        sdk_started_ = false;
    }
    pause_ = false;
    paused_ = false;
    started_ = false;
}

void MultiVoice::Free()
{
    if (!active_)
    {
        return;
    }
    for (int i = 0; i < channel_count_; ++i)
    {
        FreeVoice(voices_[i]);
    }
    channel_count_ = 0;
    manager_->RemoveVoiceList(*this);
    active_ = false;
}

void MultiVoice::Pause(bool pause)
{
    if (pause_ != pause)
    {
        pause_ = pause;
        sync_ |= kSyncPause;
    }
}

void MultiVoice::SetSampleFormat(SampleFormat format)
{
    format_ = format;
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[i].format = format;
    }
}

void MultiVoice::SetSampleRate(uint32_t rate)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[i].sample_rate = rate;
    }
}

void MultiVoice::SetInterpolationType(uint8_t type)
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[i].param.interpolation = type;
    }
}

void MultiVoice::SetAdpcmParam(int channel, const std::array<int16_t, 16>& coefs)
{
    voices_[channel].coefs = coefs;
}

void MultiVoice::AppendWaveBuffer(int channel, WaveBuffer& buffer, bool last)
{
    // Voice::AppendWaveBuffer (0x022A5A8C) queues the buffer on AX's voice. Nothing is queued if the Voice no longer
    // has an AX voice.
    if (AxVoice* ax = voices_[channel].ax)
    {
        ax->AppendWaveBuffer(buffer);
    }
    if (last)
    {
        last_buffer_ = &buffer;
    }
}

void MultiVoice::SetVolume(float volume)
{
    volume = AtLeastZero(volume);
    if (!(volume == volume_))
    {
        volume_ = volume;
        sync_ |= kSyncVe;
    }
}

void MultiVoice::SetPitch(float pitch)
{
    if (!(pitch == pitch_))
    {
        pitch_ = pitch;
        sync_ |= kSyncSrc;
    }
}

void MultiVoice::SetPanMode(uint32_t mode)
{
    if (mode != pan_mode_)
    {
        pan_mode_ = mode;
        sync_ |= kSyncMix;
    }
}

void MultiVoice::SetPanCurve(uint32_t curve)
{
    if (curve != pan_curve_)
    {
        pan_curve_ = curve;
        sync_ |= kSyncMix;
    }
}

void MultiVoice::SetLpfFreq(float lpf)
{
    if (!(lpf == lpf_freq_))
    {
        lpf_freq_ = lpf;
        sync_ |= kSyncLpf;
    }
}

void MultiVoice::SetBiquadFilter(int type, float value)
{
    value = value > 1.0f ? 1.0f : AtLeastZero(value);
    const bool type_changed = static_cast<uint32_t>(type) != biquad_type_;
    if (type_changed)
    {
        biquad_type_ = static_cast<uint8_t>(type);
    }
    if (!(value == biquad_value_))
    {
        biquad_value_ = value;
        sync_ |= kSyncBiquad;
    }
    else if (type_changed)
    {
        sync_ |= kSyncBiquad;
    }
}

void MultiVoice::SetPriority(uint32_t priority)
{
    priority_ = priority;
    manager_->ChangeVoicePriority(*this);
    for (int i = 0; i < channel_count_; ++i)
    {
        // Voice::SetPriority (0x022A5AB0).
        Voice& voice = voices_[i];
        voice.priority = priority_;
        if (voice.ax)
        {
            voice.ax->SetPriority(priority_);
        }
    }
}

void MultiVoice::SetFrontBypass(bool bypass)
{
    // The voices keep it in their parameters too. Only surround output would use it there.
    front_bypass_ = bypass;
}

void MultiVoice::SetOutputLine(uint32_t line)
{
    if (line != output_line_)
    {
        output_line_ = line;
        sync_ |= kSyncMix;
    }
}

void MultiVoice::SetTvParam(const OutputParam& param)
{
    SetOutputParam(param, tv_);
}

void MultiVoice::SetDrcParam(int drc, const OutputParam& param)
{
    SetOutputParam(param, drc_[drc]);
}

void MultiVoice::SetOutputParam(const OutputParam& from, OutputParam& to)
{
    const auto take = [this](float value, float& kept)
    {
        if (!(value == kept))
        {
            kept = value;
            sync_ |= kSyncMix;
        }
    };

    take(AtLeastZero(from.volume), to.volume);
    if (from.mix_mode != to.mix_mode)
    {
        to.mix_mode = from.mix_mode;
        sync_ |= kSyncMix;
    }
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int i = 0; i < 6; ++i)
        {
            take(from.mix_parameter[channel][i], to.mix_parameter[channel][i]);
        }
    }
    take(from.pan, to.pan);
    take(from.span, to.span);
    take(from.main_send, to.main_send);
    for (int bus = 0; bus < 3; ++bus)
    {
        take(from.fx_send[bus], to.fx_send[bus]);
    }
}

void MultiVoice::SetVoiceState(Voice& voice, AxVoice::State state)
{
    // Starting gives AX's voice the wave's format, rate and coefficients. Stopping drops its buffers.
    voice.state = state;
    if (!voice.ax)
    {
        return;
    }
    voice.ax->SetState(state);
    if (state == AxVoice::State::kPlay)
    {
        voice.ax->SetSampleRate(voice.sample_rate);
        voice.ax->SetSampleFormat(voice.format);
        voice.ax->SetAdpcmCoefs(voice.coefs);
    }
    else if (state == AxVoice::State::kStop)
    {
        voice.ax->FreeAllWaveBuffer();
    }
}

void MultiVoice::FreeVoice(Voice& voice)
{
    voice.state = AxVoice::State::kStop;
    if (AxVoice* ax = voice.ax)
    {
        manager_->ax_->Free(*ax);
        voice.ax = nullptr;
    }
}

void MultiVoice::RunAllSdkVoice()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        SetVoiceState(voices_[i], AxVoice::State::kPlay);
    }
}

void MultiVoice::StopAllSdkVoice()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        SetVoiceState(voices_[i], AxVoice::State::kStop);
    }
}

void MultiVoice::PauseAllSdkVoice()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        SetVoiceState(voices_[i], AxVoice::State::kPause);
    }
}

bool MultiVoice::IsPlayFinished() const
{
    if (channel_count_ <= 0 || !last_buffer_)
    {
        return false;
    }

    return last_buffer_->status != WaveBuffer::Status::kWait && last_buffer_->status != WaveBuffer::Status::kPlay;
}

void MultiVoice::UpdateVoiceStatus()
{
    if (!active_ || !sdk_started_)
    {
        return;
    }

    if (IsPlayFinished())
    {
        if (callback_)
        {
            const Callback callback = callback_;
            callback(*this, CallbackStatus::kFinishWave);
        }
        sdk_started_ = false;
        started_ = false;
        return;
    }

    // Voice::IsAvailable (0x0229BC68): AX may have taken a voice away for one of higher priority.
    bool available = true;
    for (int i = 0; i < channel_count_; ++i)
    {
        if (!voices_[i].ax || !voices_[i].ax->IsAvailable())
        {
            available = false;
            break;
        }
    }
    if (available)
    {
        return;
    }

    for (int i = 0; i < channel_count_; ++i)
    {
        FreeVoice(voices_[i]);
    }
    channel_count_ = 0;
    active_ = false;
    manager_->RemoveVoiceList(*this);
    if (callback_)
    {
        const Callback callback = callback_;
        callback(*this, CallbackStatus::kDropDsp);
    }
    started_ = false;
    sdk_started_ = false;
}

void MultiVoice::Calc()
{
    if (!started_)
    {
        return;
    }
    if (sync_ & kSyncSrc)
    {
        CalcSrc();
        sync_ &= ~kSyncSrc;
    }
    if (sync_ & kSyncVe)
    {
        CalcVe();
        sync_ &= ~kSyncVe;
    }
    if (sync_ & kSyncMix)
    {
        CalcMix();
        sync_ &= ~kSyncMix;
    }
    if (sync_ & kSyncLpf)
    {
        CalcLpf();
        sync_ &= ~kSyncLpf;
    }
    if (sync_ & kSyncBiquad)
    {
        CalcBiquadFilter();
        sync_ &= ~kSyncBiquad;
    }
    // The Wii Remotes' filter (0x80, CalcRemoteFilter) isn't modelled.
    sync_ &= ~0x80;
}

void MultiVoice::Update()
{
    if (!active_)
    {
        return;
    }

    enum class Action
    {
        kNone,
        kRun,
        kPause,
    };
    Action action = Action::kNone;
    bool check_pause = false;
    if ((sync_ & kSyncStart) && started_)
    {
        if (!sdk_started_)
        {
            CalcSrc();
            CalcMix();
            CalcVe();
            action = Action::kRun;
            sdk_started_ = true;
            sync_ &= ~(kSyncStart | kSyncSrc | kSyncMix);
        }
        check_pause = true;
    }
    else
    {
        check_pause = sdk_started_;
    }
    if (check_pause && (sync_ & kSyncPause) && started_)
    {
        paused_ = pause_;
        sync_ &= ~kSyncPause;
        action = pause_ ? Action::kPause : Action::kRun;
    }

    // Voice::UpdateParam (0x0229D24C): each voice's parameters reach AX's voice.
    for (int i = 0; i < channel_count_; ++i)
    {
        if (voices_[i].ax)
        {
            voices_[i].ax->Param() = voices_[i].param;
        }
    }

    switch (action)
    {
    case Action::kRun:
        RunAllSdkVoice();
        break;
    case Action::kPause:
        PauseAllSdkVoice();
        break;
    case Action::kNone:
        break;
    }
}

void MultiVoice::CalcSrc()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[i].param.pitch = pitch_;
    }
}

void MultiVoice::CalcVe()
{
    const float volume = volume_ * manager_->hardware_->output_volume;
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[i].param.volume = volume;
    }
}

void MultiVoice::CalcMix()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        voices_[i].param.tv = CalcMixImpl(0, tv_, CalcPreMixVolume(tv_, i, 0));

        // Each GamePad. The voices' parameters have no room for the second GamePad here: AxVoice plays the first
        // GamePad's mix. The Wii Remotes' mix (CalcRemoteMix) isn't modelled.
        voices_[i].param.drc = CalcMixImpl(5, drc_[0], CalcPreMixVolume(drc_[0], i, 1));
    }
}

void MultiVoice::CalcLpf()
{
    const uint16_t frequency = util::CalcLpfFreq(lpf_freq_);
    for (int i = 0; i < channel_count_; ++i)
    {
        // Voice::SetMonoFilter (0x0229CEDC): turning the filter off leaves its frequency.
        if (frequency >= 16000)
        {
            voices_[i].param.lpf_on = false;
        }
        else
        {
            voices_[i].param.lpf_on = true;
            voices_[i].param.lpf_freq = frequency;
        }
    }
}

void MultiVoice::CalcBiquadFilter()
{
    for (int i = 0; i < channel_count_; ++i)
    {
        // Voice::SetBiquadFilter (0x0229CF84): turning the filter off leaves its coefficients.
        std::array<int16_t, 5> coefs{};
        if (biquad_value_ > 0.0f && BiquadPreset(biquad_type_, biquad_value_, coefs))
        {
            voices_[i].param.biquad_on = true;
            voices_[i].param.biquad = coefs;
        }
        else
        {
            voices_[i].param.biquad_on = false;
        }
    }
}

MultiVoice::PreMixVolume MultiVoice::CalcPreMixVolume(const OutputParam& param, int channel, int device) const
{
    const HardwareSettings& hardware = *manager_->hardware_;
    const OutputMode mode = hardware.output_mode[device];
    const OutputMode end_user = hardware.end_user_output_mode[device];

    float front_left = 0.0f, front_right = 0.0f, rear_left = 0.0f, rear_right = 0.0f;
    float front_center = 0.0f, lfe = 0.0f, center = 0.0f;
    if (param.mix_mode != 0)
    {
        const std::array<float, 6>& mix = param.mix_parameter[channel];
        front_left = mix[0];
        front_right = mix[1];
        rear_left = mix[2];
        rear_right = mix[3];
        front_center = mix[4];
        lfe = mix[5];
        center = (((front_left + front_right) + rear_left) + rear_right) * 0.25f;
    }
    else
    {
        const PanInfo info = PanInfoOf(pan_curve_);
        float left = 0.0f, right = 0.0f;
        float front = 0.0f, rear = 0.0f;
        if (mode == OutputMode::kMono)
        {
            left = util::CalcPanRatio(0.0f, info, OutputMode::kMono);
            right = left;
        }
        else if (mode == OutputMode::kStereo || mode == OutputMode::kSurround)
        {
            if (channel_count_ > 1 && pan_mode_ == 1)
            {
                // Balance: each channel keeps its side.
                if (channel == 0)
                {
                    left = util::CalcPanRatio(param.pan, info, mode);
                }
                else if (channel == 1)
                {
                    right = util::CalcPanRatio(-param.pan, info, mode);
                }
            }
            else
            {
                // Dual: a stereo wave's channels start at the sides and move with the pan.
                float pan = param.pan;
                if (channel_count_ == 2)
                {
                    if (channel == 0)
                    {
                        pan = pan - 1.0f;
                    }
                    else if (channel == 1)
                    {
                        pan = pan + 1.0f;
                    }
                }
                left = util::CalcPanRatio(pan, info, mode);
                right = util::CalcPanRatio(-pan, info, mode);
            }
        }

        // Nothing here uses Dolby Pro Logic II output. It gets no gains at all: the game's code leaves them 0.
        if (mode == OutputMode::kSurround)
        {
            front = util::CalcSurroundPanRatio(param.span, info);
            rear = util::CalcSurroundPanRatio(2.0f - param.span, info);
        }
        else if (mode != OutputMode::kDpl2)
        {
            front = util::CalcSurroundPanRatio(0.0f, info);
            rear = util::CalcSurroundPanRatio(2.0f, info);
        }
        center = (left + right) * 0.5f;
        rear_left = rear * left;
        front_right = front * right;
        front_left = front * left;
        rear_right = rear * right;
    }

    // The device's mode as the system reports it: a mono device gets the fronts' centre, a stereo one folds a surround
    // mix's rear channels into the front.
    if (end_user == OutputMode::kMono)
    {
        if (mode == OutputMode::kStereo)
        {
            const float mono = (front_left + front_right) * 0.5f;
            return {mono, mono, rear_left, rear_right, front_center, lfe, center};
        }
        if (mode == OutputMode::kSurround)
        {
            const float mono = ((front_left + rear_left) + (front_right + rear_right)) * 0.5f;
            return {mono, mono, 0.0f, 0.0f, front_center, lfe, center};
        }
    }
    else if (end_user == OutputMode::kStereo && mode == OutputMode::kSurround)
    {
        return {front_left + rear_left, front_right + rear_right, 0.0f, 0.0f, front_center, lfe, center};
    }

    return {front_left, front_right, rear_left, rear_right, front_center, lfe, center};
}

OutputMix MultiVoice::CalcMixImpl(int device_bit, const OutputParam& param, const PreMixVolume& gains) const
{
    const HardwareSettings& hardware = *manager_->hardware_;
    uint32_t devices = 0;
    for (int line = 0; line < 32; ++line)
    {
        if ((output_line_ >> line) & 1)
        {
            devices |= hardware.output_line_devices[line];
        }
    }

    float main = 0.0f;
    std::array<float, 3> fx{};
    if (devices & (1u << device_bit))
    {
        const float send = param.main_send + 1.0f;
        main = send > 1.0f ? 1.0f : AtLeastZero(send);
        for (int bus = 0; bus < 3; ++bus)
        {
            fx[bus] = param.fx_send[bus] > 1.0f ? 1.0f : AtLeastZero(param.fx_send[bus]);
        }
    }

    OutputMix mix{};
    for (int channel = 0; channel < 6; ++channel)
    {
        const float gain = gains[channel] * param.volume;
        mix[0][channel] = main * gain;
        for (int bus = 0; bus < 3; ++bus)
        {
            mix[1 + bus][channel] = fx[bus] * gain;
        }
    }

    return mix;
}

// ----------------------------------------------------------------------------------------------- MultiVoiceManager

MultiVoiceManager::MultiVoiceManager(AxVoices& ax, const HardwareSettings& hardware, int count)
    : ax_(&ax), hardware_(&hardware)
{
    for (int i = 0; i < count; ++i)
    {
        voices_.push_back(std::make_unique<MultiVoice>());
        voices_.back()->manager_ = this;
        free_.push_back(voices_.back().get());
    }
}

MultiVoice* MultiVoiceManager::AllocVoice(int channels, uint32_t priority, MultiVoice::Callback callback)
{
    if (free_.empty() && DropLowestPriorityVoice(priority) == 0)
    {
        return nullptr;
    }

    MultiVoice* voice = free_.front();
    if (!voice->Alloc(channels, priority, std::move(callback)))
    {
        return nullptr;
    }
    voice->priority_ = priority & 0xFF;
    free_.pop_front();
    AppendVoiceList(*voice);

    return voice;
}

int MultiVoiceManager::DropLowestPriorityVoice(uint32_t priority)
{
    if (active_.empty())
    {
        return 0;
    }

    MultiVoice& voice = *active_.front();
    if (static_cast<int32_t>(voice.priority_) > static_cast<int32_t>(priority))
    {
        return 0;
    }

    const MultiVoice::Callback callback = voice.callback_;
    const int channels = voice.channel_count_;
    voice.Stop();
    voice.Free();
    if (callback)
    {
        callback(voice, MultiVoice::CallbackStatus::kDropVoice);
    }

    return channels;
}

void MultiVoiceManager::UpdateAllVoiceStatus()
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        MultiVoice* voice = *it++;
        voice->UpdateVoiceStatus();
    }
}

void MultiVoiceManager::UpdateAllVoices()
{
    for (auto it = active_.begin(); it != active_.end();)
    {
        MultiVoice* voice = *it++;
        voice->Calc();
    }
    for (auto it = active_.begin(); it != active_.end();)
    {
        MultiVoice* voice = *it++;
        voice->Update();
    }
}

void MultiVoiceManager::AppendVoiceList(MultiVoice& voice)
{
    // After every voice of a lower or equal priority.
    auto it = active_.end();
    while (it != active_.begin())
    {
        const auto previous = std::prev(it);
        if (static_cast<int32_t>((*previous)->priority_) <= static_cast<int32_t>(voice.priority_))
        {
            break;
        }
        it = previous;
    }
    active_.insert(it, &voice);
}

void MultiVoiceManager::RemoveVoiceList(MultiVoice& voice)
{
    active_.remove(&voice);
    free_.push_back(&voice);
}

void MultiVoiceManager::ChangeVoicePriority(MultiVoice& voice)
{
    active_.remove(&voice);
    AppendVoiceList(voice);
}

} // namespace csf::nwsnd
