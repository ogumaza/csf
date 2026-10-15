// SPDX-License-Identifier: MIT

// csfrip: builds CSF sets (.csflib + .minicsf files) from Wii U games and sound archives.
//
// usage: csfrip [options] <input> [<input> ...]
//   <input>          a Wii U game or a sound archive (.bfsar). A game is a Wii U archive (.wua) or an extracted title
//                    (a folder holding code/, content/ and meta/).
//   -o, --output DIR the folder the rips go in (default: a folder next to each input, with the input's name and "_csf"
//                    added)
//   --list           list each game's sound archives and whether it has a driver profile, or what an archive holds,
//                    then stop
//   --mode game|archive
//                    game mode runs the game's code and requires a driver profile for the game (src/rip/rip.cpp, and
//                    src/rip/jaudio.cpp for JAudio games). Game mode is the default when there's a profile. Archive
//                    mode plays the sound archives with CSF's model of the SDK's sound library instead. A JAudio game
//                    has no such archives, and rips in game mode only.
//   --archive PATH   rip only this sound archive of a game (its path in the title)
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            rip only the music (ntdlib::rip::IsMusic): sequences whose labels say BGM, or that play on a music
//                    player. In a JAudio game, the sounds whose labels start with the game's prefix for music.
//   --no-length      don't analyze lengths (no length/fade tags, and only the sequences at a volume of 0 are found to
//                    make no sound)
//   --jobs N         length analyses run in parallel (default: the number of CPU threads)
//   --game NAME      game tag (default: the name of the game's nw::snd profile, or else the game's title, or the
//                    archive's name)
//   --artist NAME, --year YEAR, --copyright TEXT, --by NAME (csfby)
//   --version        print the version
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
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/jaudio/archive.h"
#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/archive_mode.h"
#include "xsflib/cli/rip.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"

#include "csf/format.h"
#include "csf/sar.h"
#include "rip/jaudio.h"
#include "rip/rip.h"
#include "title/title.h"

namespace
{

namespace fs = std::filesystem;
using csf::FileChunk;
using csf::Tags;
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
};

// Length analysis runs every selected sequence through the model. Because the sounds are independent, the analyses are
// spread over several threads.
void AnalyzeLengths(std::vector<Job>& jobs, csf::fsar::SoundArchive archive, const csf::ArchiveDescriptor& settings,
                    unsigned threads)
{
    csf::nwsnd::EngineOptions options;
    options.voice_count = static_cast<int>(settings.voice_count);
    options.renderer_48k = (settings.ax_settings & csf::kAxRenderer48k) != 0;
    options.upsample_after_final_mix = (settings.ax_settings & csf::kAxUpsampleAfterFinalMix) != 0;

    const csf::rip::LengthAnalyzer analyzer(std::move(archive), options, settings.effects);
    const auto analyze = [&analyzer](Job& job)
    {
        job.timing = analyzer.Analyze(job.index);
        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze);
}

// csfrip's archive mode (ntdlib::rip::ArchiveRipTool). A rip with a driver profile is in game mode. It runs the game's
// code, which doesn't load the group files kept outside the archive.
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
        t.uses_groups = [](const ArchiveToRip& rip)
        {
            return rip.profile == nullptr;
        };
        t.mode = [](const ArchiveToRip& rip)
        {
            return rip.profile ? "game" : "archive";
        };
        t.build_library = [](const ArchiveToRip& rip, const Tags& tags)
        {
            return rip.profile ? csf::rip::BuildLibrary(rip.executable, rip.archive, rip.profile->profile(), tags)
                               : ntdlib::rip::BuildArchiveLibrary<csf::rip::RipFormat>(rip.archive, rip.groups,
                                                                                       rip.settings, tags);
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

// A sound of a JAudio game to rip: its name, its place in the sound table, its ID, and what the ripper found.
struct JAudioJob
{
    std::string label;
    uint32_t index = 0;
    uint32_t id = 0;
    std::vector<uint32_t> waves;
    std::optional<csf::rip::Timing> timing;
    std::string error;
};

// A JAudio game in game mode (rip/jaudio.h): one .csflib with the executable and the sound files, and a .minicsf for
// each sound of the start-up archive's sound table, with the wave groups that it requires.
int RipJAudioGame(const std::string& input, const fs::path& out_dir, const Options& o,
                  const csf::title::GameFiles& game, const csf::rip::JAudioGame& jaudio,
                  xsflib::cli::FileNameRegistries& registries)
{
    std::string error;
    std::optional<csf::rip::JAudioFiles> files = csf::rip::ReadJAudioFiles(jaudio, game, error);
    if (!files)
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error.c_str());
        return 1;
    }

    // JAudioSounds throws if the game has no start-up archive.
    const std::map<std::string, std::vector<uint8_t>> content = csf::rip::ContentFiles(files->sound);
    std::vector<ntdlib::jaudio::SoundEntry> sounds;
    try
    {
        sounds = csf::rip::JAudioSounds(jaudio, content);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), e.what());
        return 1;
    }

    if (o.list)
    {
        std::printf("%s: %s, \"%s\" (title %s, version %u%s, %s)\n", input.c_str(), game.product_code.c_str(),
                    game.name.c_str(), game.TitleIdText().c_str(), game.title_version,
                    game.updated ? " with its update" : "", game.source.c_str());
        std::printf("profile: %s (game mode, JAudio)\n%zu sounds in %s\n", jaudio.name, sounds.size(),
                    jaudio.startup_archive);
        for (const ntdlib::jaudio::SoundEntry& sound : sounds)
        {
            std::printf("  %08x %s (type %02x, %s %u)\n", sound.Id(), sound.name.c_str(), sound.type,
                        sound.source == ntdlib::jaudio::SoundEntry::Source::kArchive ? "file" : "sequence",
                        sound.resource);
        }
        return 0;
    }

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

    std::fprintf(stderr, "%s, \"%s\" (title %s, %s)\n", game.product_code.c_str(), game.name.c_str(),
                 game.TitleIdText().c_str(), game.source.c_str());

    std::vector<JAudioJob> jobs;
    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        const ntdlib::jaudio::SoundEntry& sound = sounds[i];
        const std::string label = sound.name.empty() ? xsflib::Hex(sound.Id(), 8) : sound.name;
        if ((o.bgm && !label.starts_with(jaudio.music_prefix)) || !std::regex_search(label, o.only))
        {
            continue;
        }

        JAudioJob job;
        job.label = label;
        job.index = static_cast<uint32_t>(i);
        job.id = sound.Id();
        jobs.push_back(std::move(job));
    }
    if (jobs.empty() && (o.bgm || !o.only_text.empty()))
    {
        return ntdlib::rip::NoSoundMatches(o.only_text, o.bgm);
    }

    // Each sound plays on the game's code: once to find its wave groups, and once for its length. A sound that fails is
    // skipped. Running out of memory stops the rip.
    const csf::cafe::Rpl rpl = csf::cafe::Rpl::Read(files->executable.data);
    const auto analyze = [&](JAudioJob& job)
    {
        try
        {
            csf::rip::JAudioSound found = csf::rip::AnalyzeJAudioSound(jaudio, rpl, content, job.id, o.lengths);
            job.waves = std::move(found.waves);
            job.timing = found.timing;
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

    std::vector<JAudioJob> complete;
    int skipped = 0;
    for (JAudioJob& job : jobs)
    {
        if (!job.error.empty())
        {
            skipped++;
            std::fprintf(stderr, "skipped %s: %s\n", job.label.c_str(), job.error.c_str());
        }
        else if (job.timing && job.timing->silent)
        {
            skipped++;
            std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());
        }
        else
        {
            complete.push_back(std::move(job));
        }
    }
    if (complete.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip\n", input.c_str());
        return 0;
    }

    xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, out_dir, ".csflib", ".minicsf");
    const xsflib::psf::Tags common =
        xsflib::cli::RipTags(o, o.game_name.empty() ? game.name : o.game_name, "csf", "game");

    const std::string lib_name = names.Library(fs::path(jaudio.startup_archive).stem().string());
    const std::vector<uint8_t> lib = csf::rip::BuildJAudioLibrary(jaudio, files->executable, files->sound, common);
    fs::create_directories(out_dir);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(lib_name), lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    const auto build_mini = [&lib_name](const JAudioJob& job, const xsflib::psf::Tags& tags)
    {
        return csf::rip::BuildJAudioMini(job.id, job.waves, lib_name, tags);
    };
    const int written = xsflib::cli::WriteMinis(out_dir, names, complete, common, "csf_sound", build_mini);
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

// Rips a game's sound archives (or the one --archive names). The mode is game mode if a driver profile exists, and
// archive mode otherwise. --mode overrides that choice. A JAudio game rips in game mode. Each archive's folder takes
// its registry of file names from `registries` (see NamesIn). Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o,
            xsflib::cli::FileNameRegistries& registries)
{
    csf::title::GameFiles game;
    if (const auto error = csf::title::LoadGame(fs::path(input), game))
    {
        std::fprintf(stderr, "error: %s: %s\n", input.c_str(), error->c_str());
        return 1;
    }

    if (const csf::rip::JAudioGame* jaudio = csf::rip::FindJAudioGame(game))
    {
        return RipJAudioGame(input, out_dir, o, game, *jaudio, registries);
    }

    const csf::rip::GameProfile* profile = csf::rip::FindProfile(game);
    const auto archives = game.SoundArchives();
    if (o.list)
    {
        std::printf("%s: %s, \"%s\" (title %s, version %u%s, SDK %s, %s)\n", input.c_str(), game.product_code.c_str(),
                    game.name.c_str(), game.TitleIdText().c_str(), game.title_version,
                    game.updated ? " with its update" : "", game.SdkVersionText().c_str(), game.source.c_str());
        std::printf("%zu sound archive%s\n", archives.size(), archives.size() == 1 ? "" : "s");
        for (const std::string& path : archives)
        {
            std::vector<uint8_t> bytes;
            const bool read = game.read_file(path, bytes);
            const size_t size = bytes.size();
            const std::string held = read ? DescribeArchive(std::move(bytes)) : "can't be read";
            std::printf("  %s: %zu bytes, %s\n", path.c_str(), size, held.c_str());
        }
        std::printf("%s\n", profile ? (std::string("profile: ") + profile->name + " (game mode)").c_str()
                                    : "no driver profile for this game: archive mode only");
        return 0;
    }

    std::fprintf(stderr, "%s, \"%s\" (title %s, %s)\n", game.product_code.c_str(), game.name.c_str(),
                 game.TitleIdText().c_str(), game.source.c_str());

    const std::string mode = o.mode.empty() ? (profile ? "game" : "archive") : o.mode;
    if (mode == "game" && !profile)
    {
        std::vector<const char*> known;
        for (const csf::rip::GameProfile& p : csf::rip::KnownProfiles())
        {
            known.push_back(p.name);
        }
        for (const csf::rip::JAudioGame& j : csf::rip::KnownJAudioGames())
        {
            known.push_back(j.name);
        }

        std::fprintf(stderr, "error: no driver profile for this game (known: ");
        for (std::size_t i = 0; i < known.size(); i++)
        {
            std::fprintf(stderr, "%s%s", i ? ", " : "", known[i]);
        }
        std::fprintf(stderr, "); --mode archive rips its sound archives instead\n");
        return 1;
    }

    if (mode == "archive")
    {
        if (!profile)
        {
            std::fprintf(stderr, "no driver profile for this game: ripping in archive mode\n");
        }
        std::fprintf(stderr, "%s", kArchiveModeNotice);
    }

    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    // What every archive of the game is ripped with: the game's executable and profile in game mode, and the game's
    // settings for the model. Where the game has a profile, the settings come from the profile.
    ArchiveToRip common;
    common.settings.sdk_version = game.sdk_version;
    if (profile)
    {
        const csf::game::Profile& driver = profile->profile();
        common.settings.voice_count = profile->voice_count;
        common.settings.ax_settings = (driver.renderer_48k ? csf::kAxRenderer48k : 0) |
                                      (driver.upsample_after_final_mix ? csf::kAxUpsampleAfterFinalMix : 0);
    }

    // Archive mode plays the effects of a game that has an effect profile. Game mode plays without effects, and its
    // length analysis leaves them out too.
    if (mode == "archive")
    {
        if (const csf::rip::EffectProfile* effects = csf::rip::FindEffectProfile(game))
        {
            common.settings.effects = effects->effects;
        }
    }

    if (mode == "game")
    {
        common.profile = profile;
        common.executable.path = "code/" + game.executable;
        if (!game.read_file(common.executable.path, common.executable.data))
        {
            std::fprintf(stderr, "error: can't read %s\n", common.executable.path.c_str());
            return 1;
        }
    }

    // A group file is found by its name anywhere in the title.
    const auto find_group = [&game](const std::string& name) -> std::optional<FileChunk>
    {
        for (const csf::title::TitleFile& f : game.files)
        {
            FileChunk group{f.path, {}};
            if (ntdlib::rip::IsGroupFileName(fs::path(f.path).filename().string(), name) &&
                game.read_file(f.path, group.data))
            {
                return group;
            }
        }

        return std::nullopt;
    };

    // A game can also keep group files in SARC archives in an archive's folder, under other names. They're searched in
    // the order of their paths' UTF-8 bytes.
    const auto find_sarcs = [&game](const std::string& archive_path)
    {
        const std::string folder = archive_path.substr(0, archive_path.rfind('/') + 1);
        std::vector<std::string> sarcs;
        for (const csf::title::TitleFile& f : game.files)
        {
            if (f.path.starts_with(folder) && f.path.find('/', folder.size()) == std::string::npos &&
                f.path.ends_with(".sarc"))
            {
                sarcs.push_back(f.path);
            }
        }
        std::sort(sarcs.begin(), sarcs.end());

        return sarcs;
    };

    const std::string game_name = o.game_name.empty() ? (profile ? profile->name : game.name) : o.game_name;
    return ntdlib::rip::RipArchives(ArchiveTool(), input, out_dir, archives, common, game.read_file, find_group,
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
