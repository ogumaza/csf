// SPDX-License-Identifier: MIT

#include "rip/jaudio.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/ppc/cpu.h"
#include "ntdlib/rip/archive.h"
#include "xsflib/bytes.h"

#include "cafe/memory.h"
#include "cafe/rpl.h"
#include "csf/format.h"
#include "csf/ntdlib_cafe.h"
#include "game/game_driver.h"
#include "game/jaudio1_driver.h"
#include "game/jaudio2_driver.h"
#include "rip/rip.h"
#include "title/title.h"

namespace csf::rip
{
namespace
{

std::vector<uint32_t> TwilightPrincessHdWords()
{
    return game::ProfileWords(game::TwilightPrincessHd());
}

std::vector<uint32_t> WindWakerHdWords()
{
    return game::ProfileWords(game::WindWakerHd());
}

// A file of the title's content by its path in the title. Throws std::runtime_error if the game lacks it.
const std::vector<uint8_t>& ContentFile(const std::map<std::string, std::vector<uint8_t>>& content,
                                        const std::string& path)
{
    const std::optional<std::string> under = ContentPath(path);
    const auto found = under ? content.find(*under) : content.end();
    if (found == content.end())
    {
        throw std::runtime_error("the game has no " + path);
    }

    return found->second;
}

// The length analysis's view of a JAudio driver (ntdlib/jaudio/game_lengths.h): AX's frames of 144 stereo pairs at
// 48 kHz, handed out in the blocks that the analysis asks for, and the game's memory and CPU.
class DriverMachine
{
public:
    explicit DriverMachine(game::GameDriver& driver) : driver_(driver)
    {
    }

    std::size_t Render(int16_t* out, std::size_t frames)
    {
        std::size_t done = 0;
        while (done < frames)
        {
            if (next_ == pending_.size())
            {
                const std::span<const int16_t> frame = driver_.Frame();
                pending_.assign(frame.begin(), frame.end());
                next_ = 0;
            }
            const std::size_t take = std::min((pending_.size() - next_) / 2, frames - done);
            std::copy_n(pending_.begin() + static_cast<std::ptrdiff_t>(next_), take * 2, out + done * 2);
            next_ += take * 2;
            done += take;
        }

        return done;
    }

    uint32_t Word(uint32_t address)
    {
        cafe::Memory& memory = driver_.Machine().memory;
        return memory.RegionAt(address).size() >= 4 ? memory.Read32(address) : 0;
    }

    std::optional<uint8_t> Byte(uint32_t address)
    {
        cafe::Memory& memory = driver_.Machine().memory;
        if (memory.RegionAt(address).empty())
        {
            return std::nullopt;
        }

        return memory.Read8(address);
    }

    ntdlib::ppc::Cpu& Cpu()
    {
        return driver_.Machine().cpu;
    }

private:
    game::GameDriver& driver_;
    std::vector<int16_t> pending_;
    std::size_t next_ = 0;
};

// The analysis's rules for game mode's output: 48 kHz, an AX frame at a time.
ntdlib::jaudio::GameLengthRules Rules()
{
    ntdlib::jaudio::GameLengthRules rules;
    rules.sample_rate = kPlayerSampleRate;
    rules.block_frames = ax::kOutputSamplesPerFrame;

    return rules;
}

// Plays `sound` on a new driver of the game with `waves` loaded, and runs the analysis on it. The driver is the one
// that the rip's descriptor names, with the rip's profile. With `found`, the analysis finds the waves that the notes
// look up, and each lookup counts as a sound.
std::optional<ntdlib::rip::Timing> Play(const JAudioGame& game, const cafe::Rpl& executable,
                                        const std::map<std::string, std::vector<uint8_t>>& files, uint32_t sound,
                                        std::span<const uint32_t> waves, std::set<ntdlib::jaudio::WaveUse>* found)
{
    std::string error;
    const std::unique_ptr<game::GameDriver> driver =
        game::MakeGameDriver(static_cast<uint32_t>(game.driver), game.profile_words(), executable, files, error);
    if (!driver)
    {
        throw std::runtime_error(std::string(game.release.name) + ": " + error);
    }

    driver->Start(ContentPath(game.startup_archive).value(), sound, waves);

    DriverMachine machine(*driver);
    return ntdlib::jaudio::AnalyzeGameSound(machine, driver->HandleAddress(), game.watch, Rules(), found,
                                            found != nullptr, sound);
}

} // namespace

const std::vector<JAudioGame>& KnownJAudioGames()
{
    static const std::vector<JAudioGame> kGames = {
        {
            .release = {"The Legend of Zelda: Twilight Princess HD (USA)", 0x000500001019E500,
                        "51e067cc026a6ebfc037e8071c966fe05e8f8ff7"},
            .driver = GameDriverKind::kJAudio2,
            .profile_words = TwilightPrincessHdWords,
            .startup_archive = "content/Audiores/Z2Sound.baa",
            .sound_folder = "content/Audiores/",
            .left_out = {"content/Audiores/Stream/"},
            .music_prefix = "Z2BGM_",
            // JASSeqParser::parse is slot 2 of its vtable (0x100c1424). The jump (0xC7) is entry 0xC7 of the command
            // table at 0x100bfc30. The conditional jump (0xC8) is entry 0xC8. It takes its jump at 0x02baebc8, where
            // r30 holds the track and r31 the jump's arguments. The sequence's and the sound effect's vtables come from
            // their RTTI, and the JAISe that a handle holds is its JAISound part, at +0x10. The wave handles are slot 2
            // of JASBasicWaveBank's and JASSimpleWaveBank's vtables. The wave banks are JAUSectionHeap's table, at
            // +0xfc of its instance.
            .watch =
                {
                    .parse_command = 0x02bae840,
                    .jump_handler = 0x02baeb84,
                    .command_track_register = 4,
                    .conditional_jump = 0x02baebc8,
                    .conditional_track_register = 30,
                    .conditional_arguments_register = 31,
                    .depth_offset = 8,
                    .vtable_offset = 0x94,
                    .sequence_vtable = 0x100bfee8,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x100bfde4,
                    .effect_track_offset = 0xb4,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x02c4ff80, 0x02c506ec},
                    .wave_bank_table = 0x1012dbf4,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
        },
        {
            .release = {"The Legend of Zelda: The Wind Waker HD (USA)", 0x0005000010143500,
                        "bdcbc4f815ae4c9b0a1f4a16238f96c197b38239"},
            .driver = GameDriverKind::kJAudio1,
            .profile_words = WindWakerHdWords,
            .startup_archive = "content/Cafe/US/AudioRes/JAudioRes/JaiInit.aaf",
            .sound_folder = "content/Cafe/US/AudioRes/JAudioRes/",
            .left_out = {},
            .music_prefix = "JA_BGM_",
            // parseSeq's loop over a track's commands starts at 0x028463b4. At 0x028463d8, r31 holds the track. The
            // jump is entry 0xC8 of the command table at 0x1017a30c. The parser takes the jump at 0x028469a4. There,
            // r29 holds the track and r9 the jump's offset. A track's reader is at +0xc. The wave handles are entry 2
            // of TBasicWaveBank's and TSimpleWaveBank's vtables (0x1017a574, 0x1017a5dc). The wave banks are
            // WaveBankMgr's table. A sequence's JAISound holds its parameters at +0x38 (getSeqParameter, 0x0280b05c).
            // seHandle holds the effects sequence's JAISound. An effect's JAISound holds its slot in its first byte.
            .watch =
                {
                    .parse_command = 0x028463d8,
                    .jump_handler = 0x028469a4,
                    .command_track_register = 31,
                    .jump_track_register = 29,
                    .jump_target_register = 9,
                    .reader_offset = 0xc,
                    .depth_offset = 0x18,
                    .wait_offset = 0x14,
                    .wave_handle_functions = {0x02844b90, 0x028451a0},
                    .wave_bank_table = 0x104b5378,
                    .jaudio1 = true,
                    .parameters_offset = 0x38,
                    .parameters_track_offset = 0x1360,
                    .effects_sequence = 0x104b5018,
                    .effect_slot_offset = 0,
                    .parent_offset = 0x2f8,
                    .children_offset = 0x2fc,
                },
            .name_file = "content/Cafe/US/AudioRes/JAudioRes/JaiInfo.asn",
        },
    };
    return kGames;
}

const JAudioGame* FindJAudioGame(const title::GameFiles& game)
{
    return FindKnownGame(KnownJAudioGames(), game);
}

JAudioFiles ReadJAudioFiles(const JAudioGame& jaudio, const title::GameFiles& game)
{
    JAudioFiles files;
    files.executable.path = game.ExecutablePath();
    if (!game.read_file(files.executable.path, files.executable.data))
    {
        throw std::runtime_error("can't read " + files.executable.path);
    }

    for (const title::TitleFile& file : game.files)
    {
        const std::string& path = file.path;
        if (!path.starts_with(jaudio.sound_folder) ||
            std::any_of(jaudio.left_out.begin(), jaudio.left_out.end(),
                        [&path](const char* folder) { return path.starts_with(folder); }))
        {
            continue;
        }

        FileChunk chunk{path, {}};
        if (!game.read_file(path, chunk.data))
        {
            throw std::runtime_error("can't read " + path);
        }
        files.sound.push_back(std::move(chunk));
    }

    return files;
}

uint32_t AsnNameKey(uint32_t id)
{
    constexpr uint32_t kPlayBits = ntdlib::jaudio::kAafToEnd | ntdlib::jaudio::kAafToEnd >> 1;
    return (id & ntdlib::jaudio::kAafTypeMask) == ntdlib::jaudio::kAafEffect ? id & ~kPlayBits : id;
}

std::vector<AsnSound> ReadAsnSounds(std::span<const uint8_t> asn)
{
    constexpr std::size_t kHeaderBytes = 16;
    constexpr std::size_t kRecordBytes = 32;
    constexpr std::size_t kNameBytes = 28;
    constexpr std::size_t kCategories = 18;
    constexpr std::size_t kFirst = kHeaderBytes + kCategories * kRecordBytes;

    const uint32_t count = asn.size() >= kHeaderBytes ? xsflib::LoadBe32(asn.data() + 12) : 0;
    if (asn.size() < kFirst || (asn.size() - kFirst) / kRecordBytes < count)
    {
        throw std::runtime_error("JaiInfo.asn is too short for its sounds");
    }

    std::vector<AsnSound> sounds;
    for (std::size_t i = 0; i < count; ++i)
    {
        const uint8_t* record = asn.data() + kFirst + i * kRecordBytes;
        const uint8_t* end = std::find(record, record + kNameBytes, uint8_t{0});
        sounds.push_back({xsflib::LoadBe32(record + kNameBytes), std::string(record, end)});
    }

    return sounds;
}

std::vector<ntdlib::jaudio::SoundEntry> JAudioSounds(const JAudioGame& game,
                                                     const std::map<std::string, std::vector<uint8_t>>& content)
{
    const std::vector<uint8_t>& startup = ContentFile(content, game.startup_archive);
    if (game.driver == GameDriverKind::kJAudio2)
    {
        return ntdlib::jaudio::ReadSounds(startup);
    }

    if (!game.name_file)
    {
        throw std::runtime_error(std::string(game.release.name) + " names no file of sound names");
    }

    // The names that the name file gives the sounds, by their keys.
    std::map<uint32_t, std::string> names;
    for (AsnSound& named : ReadAsnSounds(ContentFile(content, game.name_file)))
    {
        names.emplace(AsnNameKey(named.id), std::move(named.name));
    }

    // The start-up archive's sound table, with those names: the sequences first, then the effects.
    std::vector<ntdlib::jaudio::SoundEntry> sounds;
    std::vector<ntdlib::jaudio::SoundEntry> effects;
    for (ntdlib::jaudio::SoundEntry& sound : ntdlib::jaudio::ReadAafSounds(startup))
    {
        if (const auto name = names.find(AsnNameKey(sound.Id())); name != names.end())
        {
            sound.name = name->second;
        }

        const bool sequence = (sound.Id() & ntdlib::jaudio::kAafTypeMask) == ntdlib::jaudio::kAafSequence;
        (sequence ? sounds : effects).push_back(std::move(sound));
    }

    sounds.insert(sounds.end(), std::make_move_iterator(effects.begin()), std::make_move_iterator(effects.end()));

    return sounds;
}

JAudioSound AnalyzeJAudioSound(const JAudioGame& game, const cafe::Rpl& executable,
                               const std::map<std::string, std::vector<uint8_t>>& files, uint32_t sound, bool lengths)
{
    // The wave systems, from the start-up archive: a JAudio2 game's BAA or a JAudio1 game's AAF.
    const std::vector<uint8_t>& startup = ContentFile(files, game.startup_archive);
    const std::vector<ntdlib::jaudio::WaveSystem> systems = game.driver == GameDriverKind::kJAudio2
                                                                ? ntdlib::jaudio::ReadWaveSystems(startup)
                                                                : ntdlib::jaudio::ReadAafWaveSystems(startup);

    // Wave discovery plays the sound with the game's common groups only.
    std::set<ntdlib::jaudio::WaveUse> found;
    Play(game, executable, files, sound, {}, &found);

    JAudioSound result;
    result.waves =
        ntdlib::jaudio::WaveGroups(systems, std::vector<ntdlib::jaudio::WaveUse>(found.begin(), found.end()));
    if (lengths)
    {
        result.timing = Play(game, executable, files, sound, result.waves, nullptr);
    }

    return result;
}

std::vector<uint8_t> BuildJAudioLibrary(const JAudioGame& game, const FileChunk& executable,
                                        const std::vector<FileChunk>& sound_files, const Tags& tags)
{
    ProcessDescriptor descriptor;
    descriptor.version = ProcessDescriptor::kVersion2;
    descriptor.driver = static_cast<uint32_t>(game.driver);
    descriptor.executable_path = executable.path;
    descriptor.archive_path = game.startup_archive;
    descriptor.profile = game.profile_words();

    ProgramBuilder program;
    program.AddFileRef(executable.path, executable.data);
    for (const FileChunk& file : sound_files)
    {
        program.AddFileRef(file.path, file.data);
    }

    return program.WritePsf(kPsfVersionCsf, descriptor.Serialize(), tags);
}

std::vector<uint8_t> BuildJAudioMini(uint32_t sound, const std::vector<uint32_t>& waves, const std::string& lib_name,
                                     Tags tags)
{
    ProgramBuilder program;
    program.AddSound(sound);
    if (!waves.empty())
    {
        program.AddFile(kWavesPath, WavesFile(waves));
    }

    tags["_lib"] = lib_name;

    return program.WritePsf(kPsfVersionCsf, {}, tags);
}

} // namespace csf::rip
