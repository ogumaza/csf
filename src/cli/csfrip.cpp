// SPDX-License-Identifier: MIT

// csfrip: builds CSF sets (.csflib + .minicsf files) from Wii U games and sound archives.
//
// kUsage, below, lists the inputs and the options.
//
// Each of a game's sound archives gets a folder of its own in the output folder. The folder has the archive's name.
// Where two of the game's archives have the same name, the folders above each archive are added to it. A JAudio game's
// rip goes straight into the output folder. The game keeps a sound archive's group files outside the archive. The
// ripper finds them by the groups' names in the title, or for a loose archive in its folder and the folders below it.
// Sequences whose data isn't all there are listed and skipped. So are those that make no sound: those whose volume in
// the archive is 0, and those that the length analysis finds never sound. The inputs are ripped one after another. An
// input that fails doesn't stop the rest. Dropping files or folders on the program gives it them as inputs.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/archive_mode.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"
#include "xsflib/sha1.h"

#include "cafe/rpl.h"
#include "csf/format.h"
#include "csf/sar.h"
#include "rip/jaudio.h"
#include "rip/rip.h"
#include "rip/track_states.h"
#include "title/title.h"

namespace
{

namespace fs = std::filesystem;
using csf::FileChunk;
using csf::Tags;
using csf::rip::JAudioJob;
using ntdlib::rip::Job;
using xsflib::rip::InputPath;
using xsflib::rip::ReportOutput;

constexpr char kArchiveModeNotice[] =
    "note: in archive mode, CSF's model of the SDK's sound library plays the sounds instead of the\n"
    "      game's code. For exact rips of a game with a driver profile, give csfrip the game.\n";

constexpr char kUsage[] =
    "usage: csfrip [options] <input> [<input> ...]\n"
    "\n"
    "Rips each input into a .csflib and a .minicsf per sound. The rip goes in a folder next to the\n"
    "input, with the input's name and \"_csf\" added. An input is a Wii U game or a sound archive\n"
    "(.bfsar). A game is a Wii U archive (.wua) or an extracted title (the folder holding code/,\n"
    "content/ and meta/). A disc image (WUD or WUX) or an installable title (title.tmd and .app\n"
    "files) holds the game's files encrypted. Cemu's Title Manager converts either to a .wua file.\n"
    "Files and folders can also be dropped on the program. Sequences that make no sound, and those\n"
    "whose data isn't all there, are listed and skipped.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list a game's sound archives and whether it has a driver profile, or\n"
    "                       what a sound archive holds, then stop\n"
    "  --mode game|archive  game mode runs the game's code and requires a driver profile for the\n"
    "                       game. Game mode is the default when there's a profile. Archive mode\n"
    "                       plays the sound archives with CSF's model of the SDK's sound library.\n"
    "                       Mario Party 10 takes archive mode by default: game mode plays none of\n"
    "                       its effects.\n"
    "  --archive PATH       rip only this sound archive of a game (its path in the title)\n"
    "  --only REGEX         rip only the sounds whose label matches\n"
    "  --bgm                rip only the music: sequences whose labels say BGM, or that play on\n"
    "                       a player named for music (BGM, MUSIC, SONG or FANFARE). In a JAudio\n"
    "                       game, the sounds whose labels start with the game's prefix for music\n"
    "  --no-length          skip length analysis and omit length and fade tags. The analysis also\n"
    "                       finds the sequences that make no sound. Without it, only those at a\n"
    "                       volume of 0 are skipped.\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the name of the game's nw::snd profile, or\n"
    "                       else the game's title)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

using Options = xsflib::cli::RipOptions;

// What a sound archive is ripped with: the archive's file and the group files found for it, by their paths in the title
// (or beside a loose archive). Game mode adds the game's executable and profile. Archive mode adds the descriptor's
// settings. The length analysis plays with those settings in either mode.
struct ArchiveToRip
{
    FileChunk archive;
    std::vector<FileChunk> groups;
    const csf::rip::GameProfile* profile = nullptr; // game mode when set
    FileChunk executable;
    csf::ArchiveDescriptor settings;
    mutable std::string archive_sha1; // the archive's SHA-1, once ArchiveSha1 has worked it out
};

// The SHA-1 of the sound archive of `rip`, in 40 lowercase hex digits. ntdlib's lists of sequence starts find an
// archive by it. The first call works it out. A rip works out the states and the variants of its sounds before it cuts
// the archive.
const std::string& ArchiveSha1(const ArchiveToRip& rip)
{
    if (rip.archive_sha1.empty())
    {
        rip.archive_sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(rip.archive.data));
    }

    return rip.archive_sha1;
}

// Length analysis runs every selected sequence through the model, set up for the rip's settings. Because the sounds are
// independent, the analyses are spread over several threads.
void AnalyzeLengths(std::vector<Job>& jobs, csf::fsar::SoundArchive archive, const csf::ArchiveDescriptor& settings,
                    unsigned threads)
{
    const csf::rip::LengthAnalyzer analyzer(std::move(archive), settings);
    const auto analyze = [&analyzer](Job& job)
    {
        job.timing = analyzer.Analyze(job.index, job.variables, job.track_volumes);

        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze);
}

// The reserved area of a job's mini in archive mode. A job whose tracks aren't all at 1 gets the library's descriptor
// with the job's track volumes (docs/csf.md, "Track volumes"). The set loads that descriptor last. It therefore takes
// the place of the library's. Every other job gets no reserved area.
std::vector<uint8_t> MiniReserved(const ArchiveToRip& to_rip, const Job& job)
{
    if (to_rip.profile || job.track_volumes == ntdlib::rip::kFullTrackVolumes)
    {
        return {};
    }

    csf::ArchiveDescriptor descriptor =
        ntdlib::rip::LibraryDescriptor<csf::rip::RipFormat>(to_rip.archive, to_rip.groups, to_rip.settings);
    descriptor.flags |= csf::ArchiveDescriptor::kFlagTrackVolumes;
    descriptor.track_volumes = {{ntdlib::rip::kSoundItem << 24 | job.index, job.track_volumes}};

    return descriptor.Serialize();
}

// csfrip's archive mode (ntdlib::rip::ArchiveRipTool). A rip with a driver profile is in game mode. It runs the game's
// code, which doesn't load the group files kept outside the archive. Archive mode's library holds only the files that
// the sequences play. Game mode's library holds the archive with zeros in place of the files that the sequences don't
// play (csf::rip::BuildLibrary). The game's code reads each file at the place that the file table gives.
const ntdlib::rip::ArchiveRipTool<csf::rip::RipFormat, ArchiveToRip>& ArchiveTool()
{
    static const auto kTool = []
    {
        ntdlib::rip::ArchiveRipTool<csf::rip::RipFormat, ArchiveToRip> t;
        t.format = "csf";
        t.mini_word = "minicsf";
        t.library_extension = ".csflib";
        t.mini_extension = ".minicsf";
        t.sar_options = csf::kSarOptions;
        t.analyze_lengths = AnalyzeLengths;
        t.variables_tag = csf::kVariablesTag;
        t.format_variables = csf::FormatTagVariables;
        t.variants = [](const ArchiveToRip& rip, const Job& job)
        {
            return csf::rip::VariantsOf(ArchiveSha1(rip), job.label);
        };
        // Some games' code turns the tracks of some pieces up and down. In archive mode, each such piece gets a rip of
        // each state (rip/track_states.h). Game mode's driver starts each sound with its tracks at 1, as the sound
        // library does.
        t.sound_states = [](const ArchiveToRip& rip, const std::vector<Job>& selected)
        {
            return rip.profile ? selected
                               : csf::rip::WithTrackStates(csf::rip::LayeredPieces(ArchiveSha1(rip)), selected);
        };
        t.mini_reserved = MiniReserved;
        t.uses_groups = [](const ArchiveToRip& rip)
        {
            return rip.profile == nullptr;
        };
        t.keeps_sequence_data = [](const ArchiveToRip& rip)
        {
            return rip.profile == nullptr;
        };
        t.mode = [](const ArchiveToRip& rip)
        {
            return rip.profile ? "game" : "archive";
        };
        t.build_library = [](const ArchiveToRip& rip, std::span<const uint32_t> sounds, const Tags& tags)
        {
            return rip.profile
                       ? csf::rip::BuildLibrary(rip.executable, rip.archive, sounds, rip.profile->profile(), tags)
                       : ntdlib::rip::BuildArchiveLibrary<csf::rip::RipFormat>(rip.archive, rip.groups, rip.settings,
                                                                               tags);
        };

        return t;
    }();

    return kTool;
}

// What a sound archive holds, or why it can't be read (ntdlib::rip::DescribeArchive).
std::string DescribeArchive(std::vector<uint8_t> bytes)
{
    return ntdlib::rip::DescribeArchive(std::move(bytes), csf::kSarOptions);
}

// What the messages say about a game (GameText).
enum class GameDetail
{
    kName,          // its product code, name and title ID, and what its files came from
    kVersion,       // those, its version, and whether an update was laid over it
    kVersionAndSdk, // those and its SDK version
};

// How the messages name a game, with the details that `detail` gives. A game whose folder holds another title's XML
// files has no known title. The messages name such a game by its executable.
std::string GameText(const csf::title::GameFiles& game, GameDetail detail)
{
    if (game.foreign_title_id)
    {
        return "the game in " + game.ExecutablePath() + ", whose title isn't known (" + game.source + ")";
    }

    std::string text = game.product_code + ", \"" + game.name + "\" (title " + game.TitleIdText();
    if (detail != GameDetail::kName)
    {
        text += ", version " + std::to_string(game.title_version) + (game.updated ? " with its update" : "");
    }
    if (detail == GameDetail::kVersionAndSdk)
    {
        text += ", SDK " + game.SdkVersionText();
    }

    return text + ", " + game.source + ")";
}

// Lists the sounds of a JAudio game's start-up archive (--list).
void ListJAudioGame(const std::string& input, const csf::title::GameFiles& game, const csf::rip::JAudioGame& jaudio,
                    const std::vector<ntdlib::jaudio::SoundEntry>& sounds)
{
    std::printf("%s: %s\n", input.c_str(), GameText(game, GameDetail::kVersion).c_str());
    std::printf("profile: %s (game mode, JAudio)\n%zu sounds in %s\n", jaudio.release.name, sounds.size(),
                jaudio.startup_archive);
    for (const ntdlib::jaudio::SoundEntry& sound : sounds)
    {
        // A JAudio1 effect plays in the effects sequence. The effect's ID gives its category.
        if (jaudio.driver == csf::GameDriverKind::kJAudio1 &&
            (sound.Id() & ntdlib::jaudio::kAafTypeMask) == ntdlib::jaudio::kAafEffect)
        {
            std::printf("  %08x %s (type %02x, effect in category %u)\n", sound.Id(), sound.name.c_str(), sound.type,
                        ntdlib::jaudio::AafCategory(sound.Id()));
            continue;
        }

        std::printf("  %08x %s (type %02x, %s %u)\n", sound.Id(), sound.name.c_str(), sound.type,
                    sound.source == ntdlib::jaudio::SoundEntry::Source::kArchive ? "file" : "sequence", sound.resource);
    }
}

// Refuses the options that a rip of a JAudio game can't take: archive mode, and --archive. Returns the exit code after
// the error, or nothing for a rip that can go on.
std::optional<int> RefuseJAudioRip(const Options& o)
{
    if (o.mode == "archive")
    {
        std::fprintf(stderr,
                     "error: archive mode plays nw::snd's sequences, and this game's sound library is JAudio. Leave "
                     "out --mode archive.\n");
        return 1;
    }
    if (!o.only_archive.empty())
    {
        std::fprintf(stderr,
                     "error: --archive picks one of a game's sound archives, and this game's sound library is JAudio. "
                     "Leave out --archive.\n");
        return 1;
    }

    return std::nullopt;
}

// Plays the sound of each of `jobs` on the game's code: once to find its wave groups and, without --no-length, once for
// its length (csf::rip::AnalyzeJAudioSound). A sound whose analysis fails gets its error. Running out of memory stops
// the rip.
void AnalyzeJAudioSounds(std::vector<JAudioJob>& jobs, const csf::rip::JAudioGame& jaudio,
                         const csf::rip::JAudioFiles& files, const std::map<std::string, std::vector<uint8_t>>& content,
                         const Options& o)
{
    const csf::cafe::Rpl rpl = csf::cafe::Rpl::Read(files.executable.data);
    const auto analyze = [&](JAudioJob& job)
    {
        try
        {
            csf::rip::JAudioSound found =
                csf::rip::AnalyzeJAudioSound(jaudio, rpl, content, job.id, o.lengths, job.ports, job.section_statuses);
            job.waves = std::move(found.waves);
            job.timing = found.timing;
            job.files_read = std::move(found.files_read);
        }
        catch (const std::bad_alloc&)
        {
            throw;
        }
        catch (const std::exception& e)
        {
            job.error = e.what();
            return false;
        }

        // Without --no-length, a sound left without a timing failed its analysis.
        return o.lengths;
    };
    xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze);
}

// Takes out the sounds whose analysis failed and those that make no sound, with a message for each. Returns the number
// taken out.
int SkipFailedAndSilent(std::vector<JAudioJob>& jobs)
{
    int skipped = 0;
    std::vector<JAudioJob> kept;
    for (JAudioJob& job : jobs)
    {
        if (!job.error.empty())
        {
            ++skipped;
            std::fprintf(stderr, "skipped %s: %s\n", job.label.c_str(), job.error.c_str());
        }
        else if (job.timing && job.timing->silent)
        {
            ++skipped;
            std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());
        }
        else
        {
            kept.push_back(std::move(job));
        }
    }

    jobs = std::move(kept);

    return skipped;
}

// Writes the .csflib of a JAudio game's rip into `out_dir`, under a name from `names`: the executable and the sound
// files. With `read`, each sound file that isn't in it holds zeros (csf::rip::BuildJAudioLibrary). An earlier rip's
// library there stays (see xsflib::rip::FileNames::LibraryIn). Returns the library's name, or nothing after the error.
std::optional<std::string> WriteJAudioLibrary(const fs::path& out_dir, xsflib::rip::FileNames& names,
                                              const csf::rip::JAudioGame& jaudio, const csf::rip::JAudioFiles& files,
                                              const std::set<std::string>* read, const Tags& tags)
{
    const std::vector<uint8_t> lib = csf::rip::BuildJAudioLibrary(jaudio, files.executable, files.sound, tags, read);
    const std::string lib_name = names.LibraryIn(out_dir, fs::path(jaudio.startup_archive).stem().string(), lib);
    fs::create_directories(out_dir);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(lib_name), lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return std::nullopt;
    }
    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    return lib_name;
}

// A JAudio game in game mode (rip/jaudio.h): one .csflib with the executable and the sound files, and a .minicsf for
// each sound of the start-up archive's sound table, with the wave groups that it requires. Throws std::runtime_error if
// one of the rip's files can't be read or if the files that list the sounds are missing or damaged.
int RipJAudioGame(const std::string& input, const fs::path& out_dir, const Options& o,
                  const csf::title::GameFiles& game, const csf::rip::JAudioGame& jaudio,
                  xsflib::cli::FileNameRegistries& registries)
{
    // --list reads only the files that list the sounds. A refused option stops the rip before it reads the rest.
    if (o.list)
    {
        ListJAudioGame(input, game, jaudio,
                       csf::rip::JAudioSounds(jaudio, csf::rip::ReadJAudioSoundLists(jaudio, game)));
        return 0;
    }
    if (const std::optional<int> refused = RefuseJAudioRip(o))
    {
        return *refused;
    }

    const csf::rip::JAudioFiles files = csf::rip::ReadJAudioFiles(jaudio, game);
    const std::map<std::string, std::vector<uint8_t>> content = csf::ContentFiles(files.sound);
    const std::vector<ntdlib::jaudio::SoundEntry> sounds = csf::rip::JAudioSounds(jaudio, content);

    std::fprintf(stderr, "%s\n", GameText(game, GameDetail::kName).c_str());

    std::vector<JAudioJob> jobs = csf::rip::SelectJAudioSounds(sounds, jaudio, o.bgm, o.only);
    if (jobs.empty() && (o.bgm || !o.only_text.empty()))
    {
        return ntdlib::rip::NoSoundMatches(o.only_text, o.bgm);
    }

    AnalyzeJAudioSounds(jobs, jaudio, files, content, o);
    const int skipped = SkipFailedAndSilent(jobs);
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip\n", input.c_str());
        return 0;
    }

    // The game tag defaults to the game title, or to the profile name where the title or the English name isn't
    // known.
    xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, out_dir, ".csflib", ".minicsf");
    const std::string game_name = game.foreign_title_id || game.name.empty() ? jaudio.release.name : game.name;
    const Tags common = xsflib::cli::RipTags(o, o.game_name.empty() ? game_name : o.game_name, "csf", "game");

    // With the length analysis, the second play of each sound loaded its wave groups, as a player does. The library
    // then holds zeros in place of the files that the game read in none of the plays.
    std::set<std::string> read;
    for (const JAudioJob& job : jobs)
    {
        read.insert(job.files_read.begin(), job.files_read.end());
    }

    const std::optional<std::string> lib_name =
        WriteJAudioLibrary(out_dir, names, jaudio, files, o.lengths ? &read : nullptr, common);
    if (!lib_name)
    {
        return 1;
    }

    const auto build_mini = [&lib_name](const JAudioJob& job, const Tags& tags)
    {
        return csf::rip::BuildJAudioMini(job.id, job.waves, *lib_name, tags, job.ports, job.muted_children, job.volumes,
                                         job.section_statuses);
    };
    const int written = xsflib::cli::WriteMinis(out_dir, names, jobs, common, "csf_sound", build_mini);
    if (written < 0)
    {
        return 1;
    }

    std::fprintf(stderr, "%s: %d minicsf files, %d skipped\n", input.c_str(), written, skipped);
    if (written > 0)
    {
        ReportOutput(out_dir);
    }

    return 0;
}

// Lists a game's sound archives with what each holds, and whether the game has a driver profile (--list).
void ListGame(const std::string& input, const csf::title::GameFiles& game, const std::vector<std::string>& archives,
              const csf::rip::GameProfile* profile)
{
    std::printf("%s: %s\n", input.c_str(), GameText(game, GameDetail::kVersionAndSdk).c_str());
    std::printf("%zu sound archive%s\n", archives.size(), archives.size() == 1 ? "" : "s");
    ntdlib::rip::ListArchives(archives, game.read_file, DescribeArchive);
    const char* const modes = !profile ? ""
                              : profile->archive_by_default
                                  ? " (archive mode for its effects; --mode game without them)"
                                  : " (game mode)";
    std::printf("%s\n", profile ? (std::string("profile: ") + profile->release.name + modes).c_str()
                                : "no driver profile for this game: archive mode only");
}

// The names of the games that game mode can drive, for the error about a game without a driver profile.
std::string KnownGameNames()
{
    std::string names;
    for (const csf::rip::GameProfile& p : csf::rip::KnownProfiles())
    {
        names += names.empty() ? "" : ", ";
        names += p.release.name;
    }
    for (const csf::rip::JAudioGame& j : csf::rip::KnownJAudioGames())
    {
        names += names.empty() ? "" : ", ";
        names += j.release.name;
    }

    return names;
}

// The paths of the files of `game`, for ntdlib's searches of a title's files.
auto GamePaths(const csf::title::GameFiles& game)
{
    return std::views::transform(game.files, &csf::title::TitleFile::path);
}

// The mode of a rip of a game with the driver profile `profile` (null for none): game mode if there's a profile that
// doesn't take archive mode by default, and archive mode otherwise. --mode overrides that choice. Nothing after the
// error for game mode without a profile. A rip in archive mode says so first.
std::optional<std::string> RipMode(const Options& o, const csf::rip::GameProfile* profile)
{
    const std::string mode = !o.mode.empty() ? o.mode : profile && !profile->archive_by_default ? "game" : "archive";
    if (mode == "game" && !profile)
    {
        std::fprintf(stderr,
                     "error: no driver profile for this game (known: %s); --mode archive rips its sound archives "
                     "instead\n",
                     KnownGameNames().c_str());
        return std::nullopt;
    }

    if (mode == "archive")
    {
        if (profile && profile->archive_by_default)
        {
            std::fprintf(stderr, "game mode plays none of this game's effects: ripping in archive mode\n");
        }
        else
        {
            if (!profile)
            {
                std::fprintf(stderr, "no driver profile for this game: ripping in archive mode\n");
            }
            std::fprintf(stderr, "%s", kArchiveModeNotice);
        }
    }

    return mode;
}

// What every archive of the game is ripped with in `mode`: the game's executable and profile in game mode, and the
// game's settings for the model. Where the game has a profile, the settings come from the profile. Nothing after the
// error if the executable can't be read.
std::optional<ArchiveToRip> GameRip(const csf::title::GameFiles& game, const csf::rip::GameIdentity& identity,
                                    const csf::rip::GameProfile* profile, const std::string& mode)
{
    ArchiveToRip common;
    common.settings = csf::rip::GameSettings(game, profile);

    // Archive mode plays the effects of a game that has an effect profile. Game mode plays without effects, and its
    // length analysis leaves them out too.
    if (mode == "archive")
    {
        if (const csf::rip::EffectProfile* effects = csf::rip::FindEffectProfile(identity))
        {
            common.settings.effects = effects->effects;
        }
    }

    if (mode == "game")
    {
        common.profile = profile;
        common.executable.path = game.ExecutablePath();
        if (!game.read_file(common.executable.path, common.executable.data))
        {
            std::fprintf(stderr, "error: can't read %s\n", common.executable.path.c_str());
            return std::nullopt;
        }
    }

    return common;
}

// Rips a game's sound archives (or the one --archive names), in the mode that RipMode gives. A JAudio game rips in game
// mode. Each archive's folder takes its registry of file names from `registries` (see NamesIn). Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o,
            xsflib::cli::FileNameRegistries& registries)
{
    csf::title::GameFiles game;
    if (const auto error = csf::title::LoadGame(fs::path(input), game))
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error->c_str());
        return 1;
    }
    if (game.foreign_title_id)
    {
        std::fprintf(stderr,
                     "note: %s holds the code/app.xml, code/cos.xml and meta/meta.xml of another title (%s) in "
                     "place of the game's. csfrip takes %s as the game's executable.\n",
                     input.c_str(), csf::title::TitleIdText(game.foreign_title_id).c_str(),
                     game.ExecutablePath().c_str());
    }

    const csf::rip::GameIdentity identity = csf::rip::IdentityOf(game);
    if (const csf::rip::JAudioGame* jaudio = csf::rip::FindJAudioGame(identity))
    {
        return RipJAudioGame(input, out_dir, o, game, *jaudio, registries);
    }

    const csf::rip::GameProfile* profile = csf::rip::FindProfile(identity);
    const auto archives = game.SoundArchives();
    if (o.list)
    {
        ListGame(input, game, archives, profile);
        return 0;
    }

    std::fprintf(stderr, "%s\n", GameText(game, GameDetail::kName).c_str());

    const std::optional<std::string> mode = RipMode(o, profile);
    if (!mode)
    {
        return 1;
    }

    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    const std::optional<ArchiveToRip> common = GameRip(game, identity, profile, *mode);
    if (!common)
    {
        return 1;
    }

    // A group file anywhere in the title, by its file name in any case. A game can keep group files in the SARC
    // archives beside its sound archive too, under other names.
    const auto find_group = [&game](const std::string& name)
    {
        return ntdlib::rip::FindGroupFile<FileChunk>(GamePaths(game), game.read_file, name);
    };
    const auto find_sarcs = [&game](const std::string& archive_path)
    {
        return ntdlib::rip::SarcsBeside(GamePaths(game), archive_path);
    };

    // The game tag, and the start of each library's name: the profile's name, or else the game's title. A game whose
    // title or English name isn't known is named after the input.
    const std::string title_name =
        game.foreign_title_id || game.name.empty() ? xsflib::rip::NameOfInput(input) : game.name;
    const std::string game_name = o.game_name.empty() ? (profile ? profile->release.name : title_name) : o.game_name;
    return ntdlib::rip::RipArchives(ArchiveTool(), input, out_dir, archives, *common, game.read_file, find_group,
                                    game_name, o, registries, {}, find_sarcs);
}

// Rips a loose sound archive in archive mode into `out_dir`. `names` is the registry of file names for `out_dir`.
// Returns the exit code.
int RipLooseArchive(const std::string& input, const fs::path& out_dir, const Options& o, xsflib::rip::FileNames& names)
{
    if (o.mode == "game")
    {
        std::fprintf(stderr, "error: %s: game mode requires the game itself, not just its sound archive\n",
                     InputPath(input).filename().string().c_str());
        return 2;
    }

    return ntdlib::rip::RipLooseArchive(ArchiveTool(), input, out_dir, o, names, ArchiveToRip{}, DescribeArchive,
                                        kArchiveModeNotice);
}

// Rips one input. The output folders take their registries of file names from `registries`. Returns the exit code.
int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    // The Switch's sound archives are .bfsar files too.
    const std::string head = xsflib::rip::ReadHead(input);
    if (const std::optional<std::string> foreign = csf::rip::ForeignArchive(head))
    {
        std::fprintf(stderr, "error: %s is %s; CSF rips Wii U sound archives (big-endian FSAR, .bfsar)\n",
                     input.c_str(), foreign->c_str());
        return 1;
    }

    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_csf") : fs::path(o.out_dir);
    if (head.starts_with("FSAR"))
    {
        return RipLooseArchive(input, out_dir, o, xsflib::cli::NamesIn(registries, out_dir, ".csflib", ".minicsf"));
    }

    return RipGame(input, out_dir, o, registries);
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<Options> tool;
    tool.name = "csfrip";
    tool.version = CSF_VERSION;
    tool.usage = kUsage;
    tool.mode_error = "--mode is game or archive";
    tool.rip_input = RipInput;

    // Each length analysis of a JAudio game holds the game's memory, with a heap of 224 MiB, and a copy of the game's
    // sound files. A 32-bit process has room for two.
    tool.max_jobs_32bit = 2;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}
