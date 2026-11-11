// SPDX-License-Identifier: MIT

#include "rip/jaudio.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
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
#include "ntdlib/jaudio/wind_waker_ports.h"
#include "ntdlib/jaudio/z2_starts.h"
#include "ntdlib/jaudio/z2_states.h"
#include "ntdlib/ppc/cpu.h"
#include "ntdlib/rip/archive.h"
#include "xsflib/psf/psf.h"
#include "xsflib/text.h"

#include "cafe/memory.h"
#include "cafe/rpl.h"
#include "csf/format.h"
#include "csf/jaudio2_files.h"
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

std::vector<uint32_t> WindWakerHdJapanWords()
{
    return game::ProfileWords(game::WindWakerHdJapan());
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

// The length analysis's view of a JAudio driver (ntdlib/jaudio/game_lengths.h): AX's frames of 144 stereo pairs at 48
// kHz, handed out in the blocks that the analysis asks for, and the game's memory and CPU.
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

// Plays `sound` with `ports` on a new driver of the game with `waves` loaded, and runs the analysis on it. The driver
// is the one that the rip's descriptor names, with the rip's profile. With `found`, the analysis finds the waves that
// the notes look up, and each lookup counts as a sound. The files of `files` that the game read go into `read`.
std::optional<ntdlib::rip::Timing> Play(const JAudioGame& jaudio, const cafe::Rpl& executable,
                                        const std::map<std::string, std::vector<uint8_t>>& files, uint32_t sound,
                                        const xsflib::psf::Variables& ports,
                                        std::span<const SectionStatus> section_statuses,
                                        std::span<const uint32_t> waves, std::set<ntdlib::jaudio::WaveUse>* found,
                                        std::set<std::string>& read)
{
    std::string error;
    const std::unique_ptr<game::GameDriver> driver =
        game::MakeGameDriver(static_cast<uint32_t>(jaudio.driver), jaudio.profile_words(), executable, files, error);
    if (!driver)
    {
        throw std::runtime_error(std::string(jaudio.release.name) + ": " + error);
    }

    driver->SetTagValues(ports);
    driver->SetSectionStatuses(std::vector<SectionStatus>(section_statuses.begin(), section_statuses.end()));
    driver->Start(ContentPath(jaudio.startup_archive).value(), sound, waves);

    DriverMachine machine(*driver);
    const std::optional<ntdlib::rip::Timing> timing = ntdlib::jaudio::AnalyzeGameSound(
        machine, driver->HandleAddress(), jaudio.watch, Rules(), found, found != nullptr, sound);

    const std::set<std::string> game_read = driver->FilesRead();
    read.insert(game_read.begin(), game_read.end());

    return timing;
}

// The Japanese release of The Wind Waker HD, from the USA release's entry. The sound files are in the JP folder.
// JaiInit.aaf, Seqs/JaiSeq.bdt and Banks/shop_0.aw differ from the USA release's, and the other 67 are the same, byte
// for byte. Of the 93 sequences, only the effects sequence differs. The code is the USA release's in another layout
// (game::WindWakerHdJapan). The watch's addresses move with JAudio1's code and variables. bgmMute's table (0x1018dcd8)
// holds the USA release's bytes, and the starters' code reads it the same way. The ports and the music's states are
// therefore the USA release's.
JAudioGame WindWakerHdJapanGame(JAudioGame game)
{
    game.release = {"The Legend of Zelda: The Wind Waker HD (Japan)", 0x0005000010143400,
                    "e78659505b50d2dbb8be6240cf633a0a4c1afa45"};
    game.profile_words = WindWakerHdJapanWords;
    game.startup_archive = "content/Cafe/JP/AudioRes/JAudioRes/JaiInit.aaf";
    game.sound_folder = "content/Cafe/JP/AudioRes/JAudioRes/";
    game.name_file = "content/Cafe/JP/AudioRes/JAudioRes/JaiInfo.asn";
    game.watch.parse_command = 0x02846eb8;
    game.watch.jump_handler = 0x02847484;
    game.watch.wave_handle_functions[0] = 0x02845670;
    game.watch.wave_handle_functions[1] = 0x02845c80;
    game.watch.wave_bank_table = 0x104b54c0;
    game.watch.effects_sequence = 0x104b5160;

    return game;
}

// Twilight Princess HD (USA).
JAudioGame TwilightPrincessHdGame()
{
    return {
        .release = {"The Legend of Zelda: Twilight Princess HD (USA)", 0x000500001019E500,
                    "51e067cc026a6ebfc037e8071c966fe05e8f8ff7"},
        .driver = GameDriverKind::kJAudio2,
        .profile_words = TwilightPrincessHdWords,
        .startup_archive = "content/Audiores/Z2Sound.baa",
        .sound_folder = "content/Audiores/",
        .left_out = {"content/Audiores/Stream/"},
        .music_prefix = "Z2BGM_",
        // JASSeqParser::parse is slot 2 of its vtable (0x100c1424). The jump (0xC7) is entry 0xC7 of the command table
        // at 0x100bfc30. The conditional jump (0xC8) is entry 0xC8. It takes its jump at 0x02baebc8, where r30 holds
        // the track and r31 the jump's arguments. The sequence's and the sound effect's vtables come from their RTTI,
        // and the JAISe that a handle holds is its JAISound part, at +0x10. The wave handles are slot 2 of
        // JASBasicWaveBank's and JASSimpleWaveBank's vtables. The wave banks are JAUSectionHeap's table, at +0xfc of
        // its instance.
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
        // As the game starts five pieces, Z2SeqMgr::bgmStart (0x02c24d44) mutes some children of their track 0. The
        // scenes and the battles later set the tracks of more pieces through changeBgmStatus (0x02c2286c) and
        // changeSubBgmStatus (0x02c214e4). changeBgmStatus also writes the status to port 9 through
        // Z2SoundStarter::setPortData (0x02c2e138).
        .muted_starts = ntdlib::jaudio::kTwilightPrincessStarts,
        .music_states = ntdlib::jaudio::kTwilightPrincessHdStates,
    };
}

// The Wind Waker HD (USA).
JAudioGame WindWakerHdGame()
{
    return {
        .release = {"The Legend of Zelda: The Wind Waker HD (USA)", 0x0005000010143500,
                    "bdcbc4f815ae4c9b0a1f4a16238f96c197b38239"},
        .driver = GameDriverKind::kJAudio1,
        .profile_words = WindWakerHdWords,
        .startup_archive = "content/Cafe/US/AudioRes/JAudioRes/JaiInit.aaf",
        .sound_folder = "content/Cafe/US/AudioRes/JAudioRes/",
        .left_out = {},
        .music_prefix = "JA_BGM_",
        // parseSeq's loop over a track's commands starts at 0x028463b4. At 0x028463d8, r31 holds the track. The jump is
        // entry 0xC8 of the command table at 0x1017a30c. The parser takes the jump at 0x028469a4. There, r29 holds the
        // track and r9 the jump's offset. A track's reader is at +0xc. The wave handles are entry 2 of TBasicWaveBank's
        // and TSimpleWaveBank's vtables (0x1017a574, 0x1017a5dc). The wave banks are WaveBankMgr's table. A sequence's
        // JAISound holds its parameters at +0x38 (getSeqParameter, 0x0280b05c). seHandle holds the effects sequence's
        // JAISound. An effect's JAISound holds its slot in its first byte.
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
        // The sounds that read a port, and their values. The GameCube release uses the same table.
        .port_effects = ntdlib::jaudio::WindWakerPortEffects(),
        // bgmStart (0x0202204c) maps each ID from 0x80000105 to a piece and a column of bgmMute's table (0x1018dcb8):
        // the tracks that play. A state that plays the same tracks as the piece's first is left out:
        // JA_BGM_MJ_TOWER_BATTLE's 0x80000115. JA_BGM_SEA and JA_BGM_SEA_STORM play every track while the boat sails
        // (setShipSailState, 0x020299d0), and JA_BGM_SEA_ENEMY other tracks while Link is on board (setLinkOnBoard,
        // 0x02027cf4). The battle music and the minibosses' music bring in tracks while an enemy is within 30 units
        // (bgmNowBattle, 0x0202879c, and mbossBgmNearByProcess, 0x02025940) and while Link's sword is drawn
        // (bgmSetSwordUsing, 0x02028fe0).
        .music_variants =
            {
                {0x80000004, 0x80000004, "NEAR", {{kNearState, 1}}}, // JA_BGM_BATTLE_NORM
                {0x80000004, 0x80000004, "SWORD", {{kSwordState, 1}}},
                {0x80000004, 0x80000004, "NEAR_SWORD", {{kSwordState, 1}, {kNearState, 1}}},
                {0x80000005, 0x80000105, "80000105", {}},            // JA_BGM_KINDAN_BOSS
                {0x80000014, 0x80000110, "80000110", {}},            // JA_BGM_DK_BATTLE
                {0x80000019, 0x80000019, "NEAR", {{kNearState, 1}}}, // JA_BGM_MBOSS
                {0x80000019, 0x80000019, "SWORD", {{kSwordState, 1}}},
                {0x80000019, 0x80000019, "NEAR_SWORD", {{kSwordState, 1}, {kNearState, 1}}},
                {0x8000001a, 0x8000001a, "NEAR", {{kNearState, 1}}}, // JA_BGM_MBOSS_S
                {0x8000001a, 0x8000001a, "SWORD", {{kSwordState, 1}}},
                {0x8000001a, 0x8000001a, "NEAR_SWORD", {{kSwordState, 1}, {kNearState, 1}}},
                {0x8000001c, 0x8000001c, "ON_BOARD", {{kOnBoardState, 1}}}, // JA_BGM_SEA_ENEMY
                {0x80000023, 0x80000120, "80000120", {}},                   // JA_BGM_BST_BATTLE
                {0x80000023, 0x80000121, "80000121", {}},
                {0x80000023, 0x80000122, "80000122", {}},
                {0x80000023, 0x80000123, "80000123", {}}, // column 3 of the row before
                {0x80000028, 0x80000125, "80000125", {}}, // JA_BGM_KAMI_TOWER, at 1.3143 times the tempo
                {0x80000028, 0x80000126, "80000126", {}},
                {0x8000002e, 0x8000002e, "SAILING", {{kSailState, 1}}}, // JA_BGM_SEA
                {0x8000003c, 0x8000003c, "SAILING", {{kSailState, 1}}}, // JA_BGM_SEA_STORM
                {0x80000042, 0x80000130, "80000130", {}},               // JA_BGM_D_GANON_1
                {0x80000042, 0x80000131, "80000131", {}},
                {0x80000042, 0x80000132, "80000132", {}},
                {0x80000042, 0x80000133, "80000133", {}}, // column 3 of the row after
                {0x80000048, 0x80000140, "80000140", {}}, // JA_BGM_BIG_POW
                {0x80000049, 0x80000150, "80000150", {}}, // JA_BGM_PAST_BIG_POW
                {0x8000004a, 0x80000151, "80000151", {}}, // JA_BGM_PAST_BKM
            },
    };
}

std::vector<JAudioGame> MakeKnownJAudioGames()
{
    const JAudioGame wind_waker = WindWakerHdGame();

    return {TwilightPrincessHdGame(), wind_waker, WindWakerHdJapanGame(wind_waker)};
}

} // namespace

const std::vector<JAudioGame>& KnownJAudioGames()
{
    static const std::vector<JAudioGame> kGames = MakeKnownJAudioGames();
    return kGames;
}

namespace
{

// The job of `job`'s sound in `state`, a state of the game's music.
JAudioJob StateJob(const JAudioJob& job, const ntdlib::jaudio::MusicState& state)
{
    // The children at 0 go in the muted file, and the others in the volumes file.
    JAudioJob variant = job;
    variant.ports.clear();
    variant.muted_children = 0;
    variant.volumes.clear();
    variant.section_statuses.clear();
    for (const ntdlib::jaudio::SectionStatus& s : state.section_statuses)
    {
        if (s.status != 0)
        {
            variant.section_statuses.push_back({s.section, s.status});
        }
    }

    for (const ntdlib::jaudio::ChildGroup& group : state.groups)
    {
        if (group.children == 0)
        {
            continue;
        }

        if (group.volume == 0.0f)
        {
            variant.muted_children |= group.children;
        }
        else
        {
            variant.volumes.push_back({group.children, group.volume});
        }
    }

    if (state.status_port >= 0)
    {
        variant.ports = {{kStatusPort, static_cast<int16_t>(state.status_port)}};
    }

    return variant;
}

// The other starts of `job`'s sound: its muted starts with a suffix, then its states of the game's music with a
// suffix. A muted start or a state without a suffix changes `job` itself.
std::vector<JAudioJob> JAudioStarts(JAudioJob& job, const JAudioGame& jaudio)
{
    std::vector<JAudioJob> starts;
    for (const ntdlib::jaudio::MutedStart& start : jaudio.muted_starts)
    {
        if (start.sound != job.id)
        {
            continue;
        }

        if (start.suffix.empty())
        {
            job.muted_children = start.muted_children;
            continue;
        }

        JAudioJob variant = job;
        variant.label += start.suffix;
        variant.muted_children = start.muted_children;
        starts.push_back(std::move(variant));
    }

    for (const ntdlib::jaudio::MusicState& state : jaudio.music_states)
    {
        if (state.sound != job.id)
        {
            continue;
        }

        // The JAudio2 driver writes a state's ports with the start. Many sequences clear port 9 in their first tick. A
        // status that the game writes once the piece plays (status_later) would then be lost, and the state would rip
        // as the piece's start. CSF leaves such states out until the JAudio2 driver can write them later (R-A42).
        if (state.status_later)
        {
            continue;
        }

        JAudioJob variant = StateJob(job, state);
        if (state.suffix.empty())
        {
            job = std::move(variant);
            continue;
        }

        variant.label += state.suffix;
        starts.push_back(std::move(variant));
    }

    return starts;
}

// The variants of `job`'s sound: a piece's other states (MusicVariant), then the variants of a sound that reads a
// port (ntdlib::jaudio::PortEffect).
std::vector<JAudioJob> JAudioVariants(const JAudioJob& job, const JAudioGame& jaudio)
{
    std::vector<JAudioJob> variants;
    for (const MusicVariant& variant : jaudio.music_variants)
    {
        if (variant.piece == job.id)
        {
            JAudioJob other = job;
            other.id = variant.id;
            other.label += std::string("_") + variant.suffix;
            other.ports = variant.values;
            variants.push_back(std::move(other));
        }
    }

    const auto effect =
        std::find_if(jaudio.port_effects.begin(), jaudio.port_effects.end(),
                     [&job](const ntdlib::jaudio::PortEffect& port_effect) { return port_effect.id == job.id; });
    if (effect == jaudio.port_effects.end())
    {
        return variants;
    }

    for (const int16_t value : effect->values)
    {
        JAudioJob variant = job;
        variant.ports = {{effect->port, value}};
        variant.label += " " + FormatTagVariables(variant.ports);
        variants.push_back(std::move(variant));
    }

    return variants;
}

// The file at `path` in the title. Throws std::runtime_error if it can't be read.
FileChunk ReadTitleFile(const title::GameFiles& game, const std::string& path)
{
    FileChunk chunk{path, {}};
    if (!game.read_file(path, chunk.data))
    {
        throw std::runtime_error("can't read " + path);
    }

    return chunk;
}

} // namespace

std::vector<JAudioJob> SelectJAudioSounds(const std::vector<ntdlib::jaudio::SoundEntry>& sounds,
                                          const JAudioGame& jaudio, bool music_only, const std::regex& labels)
{
    std::vector<JAudioJob> jobs;
    const auto pick = [&](JAudioJob job)
    {
        if ((!music_only || job.label.starts_with(jaudio.music_prefix)) && std::regex_search(job.label, labels))
        {
            jobs.push_back(std::move(job));
        }
    };

    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        const ntdlib::jaudio::SoundEntry& sound = sounds[i];
        JAudioJob job;
        job.label = sound.name.empty() ? xsflib::Hex(sound.Id(), 8) : sound.name;
        job.index = static_cast<uint32_t>(i);
        job.id = sound.Id();
        std::vector<JAudioJob> starts = JAudioStarts(job, jaudio);

        pick(job);
        for (JAudioJob& start : starts)
        {
            pick(std::move(start));
        }
        for (JAudioJob& variant : JAudioVariants(job, jaudio))
        {
            pick(std::move(variant));
        }
    }

    return jobs;
}

const JAudioGame* FindJAudioGame(const GameIdentity& game)
{
    return FindKnownGame(KnownJAudioGames(), game);
}

JAudioFiles ReadJAudioFiles(const JAudioGame& jaudio, const title::GameFiles& game)
{
    JAudioFiles files;
    files.executable = ReadTitleFile(game, game.ExecutablePath());

    for (const title::TitleFile& file : game.files)
    {
        const std::string& path = file.path;
        if (!path.starts_with(jaudio.sound_folder) ||
            std::any_of(jaudio.left_out.begin(), jaudio.left_out.end(),
                        [&path](const char* folder) { return path.starts_with(folder); }))
        {
            continue;
        }

        files.sound.push_back(ReadTitleFile(game, path));
    }

    return files;
}

std::map<std::string, std::vector<uint8_t>> ReadJAudioSoundLists(const JAudioGame& jaudio, const title::GameFiles& game)
{
    std::vector<FileChunk> lists;
    for (const title::TitleFile& file : game.files)
    {
        const bool names = jaudio.name_file != nullptr && file.path == jaudio.name_file;
        if (file.path != jaudio.startup_archive && !names)
        {
            continue;
        }

        lists.push_back(ReadTitleFile(game, file.path));
    }

    return ContentFiles(lists);
}

uint32_t AsnNameKey(uint32_t id)
{
    constexpr uint32_t kPlayBits = ntdlib::jaudio::kAafToEnd | ntdlib::jaudio::kAafToEnd >> 1;
    return (id & ntdlib::jaudio::kAafTypeMask) == ntdlib::jaudio::kAafEffect ? id & ~kPlayBits : id;
}

std::vector<ntdlib::jaudio::SoundEntry> JAudioSounds(const JAudioGame& jaudio,
                                                     const std::map<std::string, std::vector<uint8_t>>& content)
{
    const std::vector<uint8_t>& startup = ContentFile(content, jaudio.startup_archive);
    if (jaudio.driver == GameDriverKind::kJAudio2)
    {
        return ntdlib::jaudio::ReadSounds(startup);
    }

    if (!jaudio.name_file)
    {
        throw std::runtime_error(std::string(jaudio.release.name) + " names no file of sound names");
    }

    // The names that the name file gives the sounds, by their keys.
    std::map<uint32_t, std::string> names;
    for (ntdlib::jaudio::AsnSound& named : ntdlib::jaudio::ReadAsnSounds(ContentFile(content, jaudio.name_file)))
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

JAudioSound AnalyzeJAudioSound(const JAudioGame& jaudio, const cafe::Rpl& executable,
                               const std::map<std::string, std::vector<uint8_t>>& files, uint32_t sound, bool lengths,
                               const xsflib::psf::Variables& ports, std::span<const SectionStatus> section_statuses)
{
    // The wave systems, from the start-up archive: a JAudio2 game's BAA or a JAudio1 game's AAF.
    const std::vector<uint8_t>& startup = ContentFile(files, jaudio.startup_archive);
    const std::vector<ntdlib::jaudio::WaveSystem> systems = jaudio.driver == GameDriverKind::kJAudio2
                                                                ? ntdlib::jaudio::ReadWaveSystems(startup)
                                                                : ntdlib::jaudio::ReadAafWaveSystems(startup);

    // Wave discovery plays the sound with the game's common groups only.
    std::set<ntdlib::jaudio::WaveUse> found;
    JAudioSound result;
    Play(jaudio, executable, files, sound, ports, section_statuses, {}, &found, result.files_read);

    result.waves =
        ntdlib::jaudio::WaveGroups(systems, std::vector<ntdlib::jaudio::WaveUse>(found.begin(), found.end()));

    if (lengths)
    {
        result.timing =
            Play(jaudio, executable, files, sound, ports, section_statuses, result.waves, nullptr, result.files_read);
    }

    return result;
}

std::vector<uint8_t> BuildJAudioLibrary(const JAudioGame& jaudio, const FileChunk& executable,
                                        const std::vector<FileChunk>& sound_files, const Tags& tags,
                                        const std::set<std::string>* read)
{
    ProcessDescriptor descriptor;
    descriptor.version = ProcessDescriptor::kVersion2;
    descriptor.driver = static_cast<uint32_t>(jaudio.driver);
    descriptor.executable_path = executable.path;
    descriptor.archive_path = jaudio.startup_archive;
    descriptor.profile = jaudio.profile_words();

    // Each file that the game didn't read refers to the start of one block of zeros, as long as the longest of them.
    const auto unread = [read](const FileChunk& file)
    {
        const std::optional<std::string> path = ContentPath(file.path);
        return read != nullptr && path && !read->contains(*path);
    };

    std::size_t zero_bytes = 0;
    for (const FileChunk& file : sound_files)
    {
        if (unread(file))
        {
            zero_bytes = std::max(zero_bytes, file.data.size());
        }
    }
    const std::vector<uint8_t> zeros(zero_bytes);

    ProgramBuilder program;
    program.AddFileRef(executable.path, executable.data);
    for (const FileChunk& file : sound_files)
    {
        program.AddFileRef(file.path, unread(file) ? std::span(zeros.data(), file.data.size()) : std::span(file.data));
    }

    return program.WritePsf(kPsfVersionCsf, descriptor.Serialize(), tags);
}

std::vector<uint8_t> BuildJAudioMini(uint32_t sound, const std::vector<uint32_t>& waves, const std::string& lib_name,
                                     Tags tags, const xsflib::psf::Variables& ports, uint32_t muted_children,
                                     std::span<const ChildVolume> volumes,
                                     std::span<const SectionStatus> section_statuses)
{
    ProgramBuilder program;
    program.AddSound(sound);

    if (!waves.empty())
    {
        program.AddFile(kWavesPath, WavesFile(waves));
    }

    if (muted_children != 0)
    {
        program.AddFile(kMutedPath, MutedFile(muted_children));
    }

    if (!volumes.empty())
    {
        program.AddFile(kVolumesPath, VolumesFile(volumes));
    }

    if (!section_statuses.empty())
    {
        program.AddFile(kSectionsPath, SectionsFile(section_statuses));
    }

    tags["_lib"] = lib_name;
    if (!ports.empty())
    {
        tags[kVariablesTag] = FormatTagVariables(ports);
    }

    return program.WritePsf(kPsfVersionCsf, {}, tags);
}

} // namespace csf::rip
