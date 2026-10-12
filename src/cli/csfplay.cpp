// SPDX-License-Identifier: MIT

// csfplay: renders .minicsf files to WAV or FLAC at the output's 48000 Hz, with xsflib's frame of the play tools.
//
// usage: csfplay [options] <file> [<file> ...]
//   -o, --output FILE  the output file, WAV or FLAC by its extension (default: the input's name with .wav, next to
//                      it). It takes a single input.
//   --length T         override the length tag (seconds, m:s or h:m:s)
//   --fade T           override the fade tag
//   --default T        length when there's no length tag (default and fade: PlaybackOptions)
//   --info             print the tags and loaded files, and don't render
//   --version          print the version
//
// The files are rendered one after another. A file that fails doesn't stop the rest. A .csflib among them is skipped.
// It's a library that the others load. Dropping files on the program gives it the files as inputs.

#include <cstdio>
#include <string>

#include "xsflib/cli/play.h"

#include "csf/format.h"
#include "csf/player.h"

namespace
{

// The help's description of what the tool does.
constexpr char kDescription[] =
    "Renders each .minicsf file at the output's 48000 Hz to a WAV file next to it. The render\n"
    "lasts for the file's length and fade tags. Libraries (.csflib) are skipped: the .minicsf\n"
    "files load them. You can also drop files on the program.\n";

// What the set holds, for the summary line of --info.
std::string DescribeSet(csf::Playback& playback)
{
    const csf::LoadedSet& set = playback.GetPlayer().GetLoadedSet();
    char text[80];
    std::snprintf(text, sizeof(text), "%s mode, %zu files, sound 0x%08x", set.descriptor ? "game" : "archive",
                  set.files.size(), static_cast<unsigned>(*set.sound));

    return text;
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::PlayTool<csf::Player> tool;
    tool.name = "csfplay";
    tool.version = CSF_VERSION;
    tool.description = kDescription;
    tool.library_extension = ".csflib";
    tool.mini_extension = ".minicsf";
    tool.a_file = "a CSF file";
    tool.set_up = "setting up";
    tool.describe_set = DescribeSet;

    return xsflib::cli::RunPlayTool(tool, argc, argv);
}
