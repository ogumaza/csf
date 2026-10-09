// SPDX-License-Identifier: MIT

// The CSF file format (docs/csf.md): PSF container, program chunks (FILE and SND), process and archive descriptors,
// tags.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace csf
{

// PSF version byte of CSF. See docs/csf.md ("Version byte") before changing it.
constexpr uint8_t kPsfVersionCsf = 0x28;

// The output's sample rate: 48,000 Hz exactly. Players report it, and the `length` and `fade` tags count at it
// (docs/csf.md, "Tags").
constexpr int kPlayerSampleRate = 48'000;

// Output modes: 0 mono, 1 stereo, 2 surround. Version 1 plays stereo only.
constexpr uint32_t kOutputStereo = 1;

// AX's settings, in the archive descriptor and the game profile: the 48 kHz renderer rather than the 32 kHz one, and
// each device's output reaching 48 kHz after the final mix stage rather than before it.
constexpr uint32_t kAxRenderer48k = 1;
constexpr uint32_t kAxUpsampleAfterFinalMix = 2;

// The number of words in a version 1 game profile (docs/csf.md, "Driver").
constexpr uint32_t kProfileWords = 32;

// Tags from a PSF file's [TAG] section. Names are lower-cased. Multi-line values are joined with '\n'.
using Tags = std::map<std::string, std::string>;

// One parsed PSF file.
struct PsfFile
{
    uint8_t version = 0;
    std::vector<uint8_t> reserved;
    std::vector<uint8_t> program; // decompressed
    Tags tags;
};

// Parses a PSF file. Returns an error message on failure. Without `with_program`, the program isn't decompressed or
// checked. That's enough to read the version, reserved area and tags.
std::optional<std::string> ParsePsf(const std::vector<uint8_t>& data, PsfFile& out, bool with_program = true);

// Offset of the tag area (the end of the program) of a PSF file, or nullopt if the header is invalid.
std::optional<std::size_t> TagAreaOffset(const std::vector<uint8_t>& data);

// The tag area for `tags`: "[TAG]" and name=value lines (a multi-line value becomes one line per line, all with the
// same name). Empty when there are no tags.
std::string FormatTagArea(const Tags& tags);

// Writes a PSF file. zlib compresses the program at its highest level. Throws std::runtime_error if zlib fails. Only
// running out of memory can make zlib fail.
std::vector<uint8_t> WritePsf(uint8_t version, const std::vector<uint8_t>& reserved,
                              const std::vector<uint8_t>& program, const Tags& tags);

// A number read from the start of a tag's value, and the number of characters it took.
struct DecimalNumber
{
    double value;
    std::size_t length;
};

// Reads a decimal number from the start of `text` the same way in every locale: an optional sign, digits with an
// optional '.' or ',' before decimals, and an optional exponent ("1.5e3"). Returns nullopt if `text` doesn't start
// with one. Unlike std::strtod, it reads no white space, hex, infinity or NaN.
std::optional<DecimalNumber> ReadDecimal(std::string_view text);

// Parses a PSF length or fade tag ("seconds", "m:s" or "h:m:s", with '.' or ',' before decimals) into milliseconds.
// Returns -1 if the tag is empty or malformed.
long long ParseTime(const std::string& text);

// Formats milliseconds as a PSF time ("m:ss.fff").
std::string FormatTime(long long ms);

// The process descriptor: the reserved area of a game-mode set (docs/csf.md, "Process descriptor").
struct ProcessDescriptor
{
    static constexpr uint32_t kMagic = 0x50465343; // "CSFP"
    static constexpr uint32_t kVersion = 1;

    static std::optional<ProcessDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t flags = 0;
    uint32_t output_mode = kOutputStereo;
    std::string executable_path; // FILE chunk holding the game's executable (an RPX file)
    std::string archive_path;    // FILE chunk holding the sound archive (.bfsar)
    std::vector<uint32_t> profile;
};

// Descriptor of an archive-mode set: the sound archive is played by CSF's model of the SDK's sound library (nw::snd and
// AX) instead of by the game's code (docs/csf.md, "Archive mode").
struct ArchiveDescriptor
{
    static constexpr uint32_t kMagic = 0x41465343; // "CSFA"
    static constexpr uint32_t kVersion = 1;

    // nw::snd's voice count when the game doesn't change it, and the most AX has.
    static constexpr uint32_t kDefaultVoices = 96;

    static std::optional<ArchiveDescriptor> Parse(const std::vector<uint8_t>& data);

    // Returns an error message for an unsupported descriptor, or no value if it is supported.
    std::optional<std::string> Check() const;

    std::vector<uint8_t> Serialize() const;

    uint32_t version = kVersion;
    uint32_t flags = 0;
    uint32_t output_mode = kOutputStereo;
    uint32_t voice_count = kDefaultVoices;
    uint32_t ax_settings = kAxUpsampleAfterFinalMix;
    uint32_t sdk_version = 0;             // major * 10000 + minor * 100 + micro, or 0 if unknown
    std::string archive_path;             // FILE chunk holding the sound archive (.bfsar)
    std::vector<std::string> group_paths; // FILE chunks holding group files (.bfgrp) the game keeps outside it
};

// A FILE chunk: a file of the game's title, by its path in the title.
struct FileChunk
{
    std::string path;
    std::vector<uint8_t> data;
};

// Builds a program (docs/csf.md, "Program: chunks"): the chunks in the order they're added. Each chunk is padded to a
// multiple of four bytes.
class ProgramBuilder
{
public:
    void AddFile(const std::string& path, const std::vector<uint8_t>& data);

    // Like AddFile, but the program refers to `data` instead of copying it. A large file is then held once. `data` must
    // outlive the builder.
    void AddFileRef(const std::string& path, std::span<const uint8_t> data);

    // Selects the sound to play (`SND ` chunk): its nw::snd item id.
    void AddSound(uint32_t sound_id);

    // The program's pieces, in order.
    std::vector<std::span<const uint8_t>> Pieces() const;

    // The whole program.
    std::vector<uint8_t> Data() const;

    // A PSF file holding the program: the bytes that WritePsf makes of Data(). The difference is that this compresses
    // the pieces as they come. The files that the program refers to aren't copied into a whole program first.
    std::vector<uint8_t> WritePsf(uint8_t version, const std::vector<uint8_t>& reserved, const Tags& tags) const;

private:
    // Bytes the builder holds (chunk headers, paths, padding and the files AddFile copies), or bytes it refers to.
    struct Piece
    {
        std::vector<uint8_t> held;
        std::span<const uint8_t> ref;
    };

    void Hold(std::vector<uint8_t> bytes);

    std::vector<Piece> pieces_;
};

// A decompressed program split into chunks.
struct ProgramChunks
{
    std::vector<FileChunk> files;
    std::optional<uint32_t> sound; // the last `SND ` chunk
};

// Splits a decompressed program into chunks (unknown types are skipped).
std::optional<std::string> ParseProgram(const std::vector<uint8_t>& program, ProgramChunks& out);

// Reads a PSF file's program straight into its chunks: ParsePsf followed by ParseProgram, but without holding the
// decompressed program. A FILE chunk's data is inflated into its vector with no copy of the whole program.
std::optional<std::string> ParsePsfChunks(const std::vector<uint8_t>& data, ProgramChunks& out);

// A complete set: the files of a file and its libraries in loading order, the effective descriptor (exactly one of
// `process` and `archive`), the sound, and the tags of the top-level file.
struct LoadedSet
{
    std::vector<FileChunk> files;
    std::optional<ProcessDescriptor> process; // game mode
    std::optional<ArchiveDescriptor> archive; // archive mode
    uint32_t sound = 0;
    Tags tags;
    std::vector<std::string> sources; // files loaded, in order (for diagnostics)

    // The data of the file at `path`, the last one loaded with that path, or nullptr if none was.
    const std::vector<uint8_t>* File(const std::string& path) const;
    std::vector<uint8_t>* File(const std::string& path);
};

// Reads a file by path, and returns false if it can't be read.
using FileReader = std::function<bool(const std::string& path, std::vector<uint8_t>& data)>;

// Loads a CSF/miniCSF file with its _lib/_libN chain. Paths in _lib tags are relative to the directory of the file that
// names them: its path up to the last of `separators`. Returns an error for a set a version 1 player can't play: one
// without a descriptor or a sound, or whose descriptor fails its Check().
std::optional<std::string> LoadSet(const std::string& path, const FileReader& reader, LoadedSet& out,
                                   std::string_view separators = "/\\");

// Reads a whole file from disk.
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& data);

} // namespace csf
