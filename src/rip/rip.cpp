// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ax/system.h"
#include "common/ascii.h"
#include "common/sha1.h"
#include "fsar/formats.h"
#include "nwsnd/engine.h"

namespace csf::rip
{
namespace
{

// The item ID's type in its top byte: a sound, a bank, a player and a wave archive.
constexpr uint32_t kSoundItem = 0x01;
constexpr uint32_t kBankItem = 0x03;
constexpr uint32_t kPlayerItem = 0x04;
constexpr uint32_t kWaveArchiveItem = 0x05;

// The index of item `id` of type `type`, or nothing if it's of another type or past `count`.
std::optional<std::size_t> ItemIndex(uint32_t id, uint32_t type, std::size_t count)
{
    const std::size_t index = id & 0xFFFFFF;
    if ((id >> 24) != type || index >= count)
    {
        return std::nullopt;
    }

    return index;
}

// ASCII letters, digits, '_', '-' and '.' are kept, and everything else, spaces included, becomes '_'. Unlike
// std::isalnum, this doesn't depend on the C locale.
std::string SafeName(const std::string& s)
{
    std::string out;
    for (const char c : s)
    {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '-' || c == '.';
        out += keep ? c : '_';
    }

    return out;
}

// Windows reserves these device names, whatever follows the first dot of a file name.
bool ReservedOnWindows(const std::string& stem)
{
    const std::string name = AsciiLower(stem);
    const bool numbered =
        name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '0' && name[3] <= '9';

    return name == "con" || name == "prn" || name == "aux" || name == "nul" || numbered;
}

// A looping sequence plays this many loops, and then fades out over kFadeSeconds.
constexpr int kLoops = 2;
constexpr double kFadeSeconds = 10.0;

// A frame of AX and nw::snd.
constexpr double kFrameMs = 3.0;

// The longest a sequence plays.
constexpr double kMaxSeconds = 600.0;
constexpr uint64_t kMaxFrames = static_cast<uint64_t>(kMaxSeconds * 1000.0 / kFrameMs);

// A sequence that stays busy without a sound for this long has ended where it fell silent.
constexpr double kSilentSeconds = 180.0;
constexpr uint64_t kSilentFrames = static_cast<uint64_t>(kSilentSeconds * 1000.0 / kFrameMs);

// Sound is a sample louder than this (-72 dB). A note held at a level of 1 isn't sound.
constexpr int kSilenceLevel = 8;

// The silence after the last sound of a sequence that ends.
constexpr double kTailMs = 500.0;

Timing EndAt(uint64_t frame)
{
    return {static_cast<long long>(static_cast<double>(frame) * kFrameMs + kTailMs + 0.5), 0};
}

Timing LoopAt(uint64_t frame)
{
    return {static_cast<long long>(static_cast<double>(frame) * kFrameMs + 0.5),
            static_cast<long long>(kFadeSeconds * 1000.0)};
}

// Measures a sequence on the model: it plays until the sequence loops kLoops times, ends, or falls silent for good, or
// for kMaxSeconds. Frames count the model's output: a frame's commands are heard from the next frame on.
std::optional<Timing> MeasureSequence(const fsar::SoundArchive& archive, const nwsnd::EngineOptions& options,
                                      uint32_t sound_index)
{
    auto model = std::make_unique<nwsnd::ArchiveModel>(archive, options);
    nwsnd::Engine& engine = *model->engine;
    nwsnd::SequenceSoundPlayer* player = engine.StartSequence(sound_index);
    if (!player)
    {
        return std::nullopt;
    }

    // Whether it ends or loops, a sequence that never sounds is silent and gets the tail alone. Such a sequence may
    // still play notes that make no sound.
    bool sounded = false;
    const auto heard = [&sounded](Timing timing)
    {
        if (!sounded)
        {
            timing = EndAt(0);
            timing.silent = true;
        }

        return timing;
    };

    // Loops that go round before the first sound don't count. An example is a track that waits or sets values while a
    // note fades in or another track starts. A loop of another track, or one that starts elsewhere, starts the count
    // again. The loops counted so far don't repeat what plays now. That happens when track 0 polls a variable while a
    // later track plays the music, or when a track moves on to the loop it plays for good.
    int loops_seen = 0;
    std::pair<int, uint32_t> loop{-1, 0};
    uint64_t frame = 0;
    uint64_t first_loop_frame = 0;
    uint64_t loop_frame = 0;
    player->on_loop = [&](int track, uint32_t target)
    {
        if (!sounded)
        {
            return;
        }

        if (std::pair{track, target} != loop)
        {
            loops_seen = 0;
            loop = {track, target};
        }
        ++loops_seen;
        if (loops_seen == 1)
        {
            first_loop_frame = frame + 1;
        }
        if (loops_seen == kLoops)
        {
            loop_frame = frame + 1;
        }
    };

    // Some sequences reach a point where every open track waits. A track may wait for notes that never end (held notes
    // on looping waves), or never read on again, or not before the game stops the sound. A track may also poll a
    // variable once a tick and change nothing (SequenceTrack::IsStill). Such a sequence is steady once nothing else
    // changes. No note's envelope is in its attack, hold, decay or release, and no volume, pan or pitch bend is moving.
    // (A released note ends by itself, and that can let its track read on.) A steady sequence plays on unchanged but
    // for the waves' positions. So it loops when they do. Each running voice's passes back to its wave's loop start are
    // followed throughout. A steady sequence has looped when every voice has passed kLoops times since the sound last
    // changed. A track's change, a moving envelope or ramp, and a voice that starts or stops all change the sound. A
    // track that polls may only show that it's still after a long pass, and the waves' passes during it count.
    std::array<uint32_t, ax::kVoiceCount> positions{};
    std::array<bool, ax::kVoiceCount> running{};
    // The frames of each voice's latest passes, the newest first.
    std::array<std::array<uint64_t, kLoops>, ax::kVoiceCount> passes{};
    uint64_t changed = 0;      // the frame of the sound's last change
    uint64_t change_order = 0; // ChangedAt in that frame
    const auto follow_sound = [&]
    {
        for (const ax::Voice& voice : model->system.Voices())
        {
            const ax::Pb& pb = voice.renderer;
            const bool runs = pb.state == ax::VoiceState::kRun;
            if (runs != running[voice.index])
            {
                changed = frame;
            }
            else if (runs && pb.current_address < positions[voice.index])
            {
                std::array<uint64_t, kLoops>& latest = passes[voice.index];
                std::copy_backward(latest.begin(), latest.end() - 1, latest.end());
                latest[0] = frame;
            }
            running[voice.index] = runs;
            positions[voice.index] = pb.current_address;
        }

        if (engine.Channels().AnyChanging() || player->IsMoving() || player->ChangedAt() != change_order)
        {
            changed = frame;
        }
        change_order = player->ChangedAt();
    };
    const auto waves_looped = [&]
    {
        bool any = false;
        for (const ax::Voice& voice : model->system.Voices())
        {
            const ax::Pb& pb = voice.renderer;
            if (pb.state != ax::VoiceState::kRun)
            {
                continue;
            }

            if (pb.loop_flag == 0 || passes[voice.index].back() <= changed)
            {
                return false;
            }
            any = true;
        }

        return any;
    };

    uint64_t idle = 0;
    uint64_t last_sound = 0;
    for (; frame < kMaxFrames; ++frame)
    {
        engine.RunFrame();

        const auto audible = [](int16_t sample)
        {
            return sample > kSilenceLevel || sample < -kSilenceLevel;
        };

        const std::span<const int16_t> output = model->system.Output();
        if (std::any_of(output.begin(), output.end(), audible))
        {
            sounded = true;
            last_sound = frame;
        }

        // A loop that went round without a sound, such as a wait that a track repeats once its notes have ended, isn't
        // music: the sequence ended at its last sound.
        if (loops_seen >= kLoops)
        {
            return heard(last_sound < first_loop_frame ? EndAt(last_sound + 1) : LoopAt(loop_frame));
        }

        follow_sound();
        const bool steady = player->IsActive() && player->IsSettled() && changed < frame;
        if (steady && waves_looped())
        {
            // A sequence that has made no sound since its sound last changed, such as a held note at a volume of 0,
            // ended at its last sound.
            return heard(last_sound < changed ? EndAt(last_sound + 1) : LoopAt(frame + 1));
        }

        // A sequence that stays busy without a sound for kSilentSeconds, such as one whose music has ended while a
        // track that plays no notes runs on, ended where it fell silent.
        const bool busy = engine.IsBusy();
        if (busy && frame - last_sound >= kSilentFrames)
        {
            idle = frame - last_sound;
            break;
        }

        // A sequence that will never change again, or not before the game stops it, has ended when its last note has.
        // Its tracks may wait for ever or poll a variable that only the game sets.
        const bool stuck = player->IsActive() && player->IsStuck() && engine.Channels().ActiveCount() == 0;
        if (!busy || stuck)
        {
            if (++idle > 10)
            {
                break;
            }
        }
        else
        {
            idle = 0;
        }
    }
    if (frame >= kMaxFrames)
    {
        return heard(LoopAt(kMaxFrames));
    }

    // Ended: the length runs to the end of the last frame with sound and the tail, with no fade. Only silence follows,
    // however long the sequence takes to end (a track may hold a note at a volume of 0 for minutes). The idle frames
    // began at `frame + 1 - idle`.
    return heard(EndAt(std::min(frame + 1 - idle, last_sound + 1)));
}

} // namespace

const std::vector<GameProfile>& KnownProfiles()
{
    static const std::vector<GameProfile> kProfiles = {
        // Animal Crossing: amiibo Festival (USA). Its NdSysAudio gives nw::snd 78 voices.
        {"Animal Crossing: amiibo Festival", 0x00050000101C6400, "4686cf8a1253f8d58996cf665731729c8c83e368",
         &game::AmiiboFestival, 78},
    };

    return kProfiles;
}

const GameProfile* FindProfile(const title::GameFiles& game)
{
    std::vector<uint8_t> executable;
    if (!game.read_file || !game.read_file("code/" + game.executable, executable))
    {
        return nullptr;
    }

    const std::string sha1 = Sha1::Hex(Sha1::Of(executable));
    for (const GameProfile& profile : KnownProfiles())
    {
        if (game.title_id == profile.title_id && sha1 == profile.executable_sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

std::vector<std::string> ExternalGroups(const fsar::SoundArchive& archive)
{
    std::vector<std::string> names;
    for (const fsar::GroupInfo& group : archive.Groups())
    {
        if (group.file_id == 0xFFFFFFFF && !group.name.empty())
        {
            names.push_back(group.name);
        }
    }

    return names;
}

std::optional<std::string> ForeignArchive(std::string_view head)
{
    if (head.starts_with("CSAR"))
    {
        return "a 3DS sound archive (CSAR)";
    }
    if (head.starts_with("RSAR"))
    {
        return "a Wii sound archive (RSAR)";
    }
    if (head.starts_with("FSAR\xFF\xFE"))
    {
        return "a Switch sound archive (little-endian FSAR)";
    }

    return std::nullopt;
}

std::vector<uint8_t> BuildLibrary(const FileChunk& executable, const FileChunk& archive, const game::Profile& profile,
                                  const Tags& tags)
{
    ProcessDescriptor descriptor;
    descriptor.executable_path = executable.path;
    descriptor.archive_path = archive.path;
    descriptor.profile = game::ProfileWords(profile);

    ProgramBuilder program;
    program.AddFileRef(executable.path, executable.data);
    program.AddFileRef(archive.path, archive.data);

    return program.WritePsf(kPsfVersionCsf, descriptor.Serialize(), tags);
}

std::vector<uint8_t> BuildArchiveLibrary(const FileChunk& archive, const std::vector<FileChunk>& groups,
                                         ArchiveDescriptor settings, const Tags& tags)
{
    settings.archive_path = archive.path;
    settings.group_paths.clear();
    ProgramBuilder program;
    program.AddFileRef(archive.path, archive.data);
    for (const FileChunk& group : groups)
    {
        settings.group_paths.push_back(group.path);
        program.AddFileRef(group.path, group.data);
    }

    return program.WritePsf(kPsfVersionCsf, settings.Serialize(), tags);
}

std::vector<uint8_t> BuildMini(uint32_t index, const std::string& lib_name, Tags tags)
{
    ProgramBuilder program;
    program.AddSound(kSoundItem << 24 | index);
    tags["_lib"] = lib_name;

    return program.WritePsf(kPsfVersionCsf, {}, tags);
}

std::vector<std::string> MissingData(const fsar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size())
    {
        return {"the sound itself"};
    }

    const fsar::SoundInfo& sound = archive.Sounds()[index];
    std::vector<std::string> missing;
    if (archive.FileData(sound.file_id).empty())
    {
        missing.push_back("its sequence data");
    }

    // Each bank, and the wave archives its waves are in. An empty slot requires nothing.
    std::set<uint32_t> wave_archives;
    for (const uint32_t item : sound.banks)
    {
        if (item == 0xFFFFFFFF)
        {
            continue;
        }

        const std::optional<std::size_t> bank = ItemIndex(item, kBankItem, archive.Banks().size());
        if (!bank)
        {
            missing.push_back("a bank the archive doesn't list");
            continue;
        }

        const fsar::BankInfo& info = archive.Banks()[*bank];
        const std::span<const uint8_t> file = archive.FileData(info.file_id);
        const std::string name = info.name.empty() ? "#" + std::to_string(*bank) : info.name;
        if (file.empty())
        {
            missing.push_back("bank " + name);
            continue;
        }

        try
        {
            for (const fsar::WaveId& wave : fsar::Bank::Parse(file).waves)
            {
                wave_archives.insert(wave.wave_archive_item);
            }
        }
        catch (const std::exception&)
        {
            missing.push_back("bank " + name + " (can't be read)");
        }
    }

    for (const uint32_t item : wave_archives)
    {
        const std::optional<std::size_t> wave_archive =
            ItemIndex(item, kWaveArchiveItem, archive.WaveArchives().size());
        if (!wave_archive)
        {
            missing.push_back("a wave archive the archive doesn't list");
        }
        else if (archive.FileData(archive.WaveArchives()[*wave_archive].file_id).empty())
        {
            const std::string& name = archive.WaveArchives()[*wave_archive].name;
            missing.push_back("wave archive " + (name.empty() ? "#" + std::to_string(*wave_archive) : name));
        }
    }

    return missing;
}

bool IsMusic(const fsar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size() || archive.Sounds()[index].type != fsar::SoundType::kSequence)
    {
        return false;
    }

    const fsar::SoundInfo& sound = archive.Sounds()[index];
    if (AsciiLower(sound.name).find("bgm") != std::string::npos)
    {
        return true;
    }

    const std::optional<std::size_t> player = ItemIndex(sound.player_id, kPlayerItem, archive.Players().size());
    if (!player)
    {
        return false;
    }

    // The player's name, word by word, each without the digits it ends in (PLAYER_BGM1).
    bool music = false;
    bool effects = false;
    const std::string name = AsciiLower(archive.Players()[*player].name);
    for (std::size_t start = 0; start <= name.size();)
    {
        const std::size_t end = std::min(name.find('_', start), name.size());
        std::string word = name.substr(start, end - start);
        word.erase(word.find_last_not_of("0123456789") + 1);
        music = music || word == "bgm" || word == "music" || word == "song" || word == "fanfare";
        effects = effects || word == "se";
        start = end + 1;
    }

    return music && !effects;
}

std::string FileNames::Library(const std::string& base)
{
    std::string name = SafeName(base);
    const std::size_t stem_length = std::min(name.find('.'), name.size());
    if (ReservedOnWindows(name.substr(0, stem_length)))
    {
        name.insert(stem_length, "_");
    }

    return Unique(name, ".csflib");
}

std::string FileNames::Mini(uint32_t index, const std::string& label)
{
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%04u ", static_cast<unsigned>(index));

    return Unique(prefix + SafeName(label), ".minicsf");
}

std::string FileNames::Unique(const std::string& stem, const std::string& extension)
{
    std::string name = stem + extension;
    for (int n = 2; !taken_.insert(AsciiLower(name)).second; n++)
    {
        name = stem + "_" + std::to_string(n) + extension;
    }

    return name;
}

struct LengthAnalyzer::Impl
{
    fsar::SoundArchive archive;
    nwsnd::EngineOptions options;
};

LengthAnalyzer::LengthAnalyzer(fsar::SoundArchive archive, nwsnd::EngineOptions options)
    : impl_(new Impl{std::move(archive), options})
{
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    // A sequence the model can't play, with a damaged bank for instance, gets no length. Running out of memory isn't
    // the sequence's fault. std::bad_alloc goes to the caller instead.
    try
    {
        return MeasureSequence(impl_->archive, impl_->options, sound_index);
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

} // namespace csf::rip
