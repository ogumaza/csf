// SPDX-License-Identifier: MIT

// The CSF file format (see format.h and docs/csf.md).

#include "csf/format.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/ascii.h"
#include "common/bytes.h"

namespace csf
{
namespace
{

std::string Hex(uint32_t value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", static_cast<unsigned>(value));

    return buf;
}

// Times past these are taken as malformed. These limits keep the arithmetic far from overflowing. That holds here and
// where the players turn times into sample counts.
constexpr long long kMaxTimeField = 1'000'000'000; // hours or minutes
constexpr double kMaxTimeMs = 1e13;                // about 317 years

// Limits on a set: _lib nesting, files in a set, and the size of one file's program. The limits stop a malformed set
// from making the loader recurse, load or inflate without end.
constexpr int kMaxLibDepth = 10;
constexpr std::size_t kMaxSetFiles = 64;
constexpr std::size_t kMaxProgramSize = std::size_t{1} << 30;

void Put32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; i++)
    {
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    }
}

void Put16(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

// A path in a descriptor: its u16 length, then its bytes.
void PutPath(std::vector<uint8_t>& v, const std::string& path)
{
    Put16(v, static_cast<uint32_t>(path.size()));
    v.insert(v.end(), path.begin(), path.end());
}

void PadTo4(std::vector<uint8_t>& v)
{
    while (v.size() % 4)
    {
        v.push_back(0);
    }
}

// Reads a u16 at `pos` and moves past it, or returns nothing if the data ends first.
std::optional<uint32_t> Read16(const std::vector<uint8_t>& data, std::size_t& pos)
{
    if (pos + 2 > data.size())
    {
        return std::nullopt;
    }

    const uint32_t value = LoadLe16(&data[pos]);
    pos += 2;

    return value;
}

// Reads a path in a descriptor at `pos` and moves past it, or returns nothing if the data ends first.
std::optional<std::string> ReadPath(const std::vector<uint8_t>& data, std::size_t& pos)
{
    const std::optional<uint32_t> length = Read16(data, pos);
    if (!length || pos + *length > data.size())
    {
        return std::nullopt;
    }

    std::string path(reinterpret_cast<const char*>(&data[pos]), *length);
    pos += *length;

    return path;
}

std::string Trim(const std::string& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= 0x20)
    {
        a++;
    }

    while (b > a && static_cast<unsigned char>(s[b - 1]) <= 0x20)
    {
        b--;
    }

    return s.substr(a, b - a);
}

Tags ParseTags(const char* text, std::size_t size)
{
    Tags tags;
    std::string last;
    std::size_t pos = 0;
    while (pos < size)
    {
        std::size_t end = pos;
        while (end < size && text[end] != '\n')
        {
            end++;
        }

        std::string line(text + pos, end - pos);
        pos = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }

        const std::string name = AsciiLower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        if (name.empty())
        {
            continue;
        }

        auto it = tags.find(name);
        if (it != tags.end() && name == last)
        {
            it->second += "\n" + value; // multi-line value
        }
        else
        {
            tags[name] = value;
        }

        last = name;
    }

    return tags;
}

// The directory part of `path`, with its trailing separator. Paths only go to a FileReader. A FileReader may not use
// the file system (foobar2000 passes file:// URLs). So paths stay strings.
std::string DirectoryOf(const std::string& path, std::string_view separators)
{
    const std::size_t separator = path.find_last_of(separators);
    return separator == std::string::npos ? std::string() : path.substr(0, separator + 1);
}

// The checks the two descriptors share: the version, the flags and the output mode.
std::optional<std::string> CheckCommon(const char* name, uint32_t version, uint32_t flags, uint32_t output_mode)
{
    if (version != 1)
    {
        return std::string(name) + " descriptor version " + std::to_string(version) +
               "; this player supports version 1";
    }

    if (flags != 0)
    {
        return std::string("the ") + name + " descriptor's flags are " + Hex(flags) + "; version 1 has none";
    }

    if (output_mode != kOutputStereo)
    {
        return std::string("the ") + name + " descriptor's output mode is " + std::to_string(output_mode) +
               "; version 1 supports only stereo (1)";
    }

    return std::nullopt;
}

} // namespace

std::optional<std::size_t> TagAreaOffset(const std::vector<uint8_t>& data)
{
    if (data.size() < 16 || std::memcmp(data.data(), "PSF", 3) != 0)
    {
        return std::nullopt;
    }

    const uint64_t end = 16ull + LoadLe32(&data[4]) + LoadLe32(&data[8]);
    if (end > data.size())
    {
        return std::nullopt;
    }

    return static_cast<std::size_t>(end);
}

std::optional<std::string> ParsePsf(const std::vector<uint8_t>& data, PsfFile& out, bool with_program)
{
    if (data.size() < 16 || std::memcmp(data.data(), "PSF", 3) != 0)
    {
        return "not a PSF file";
    }

    out.version = data[3];
    const uint32_t reserved_size = LoadLe32(&data[4]);
    const uint32_t program_size = LoadLe32(&data[8]);
    const uint32_t crc = LoadLe32(&data[12]);
    if (16ull + reserved_size + program_size > data.size())
    {
        return "truncated PSF file";
    }

    out.reserved.assign(data.begin() + 16, data.begin() + 16 + reserved_size);
    out.program.clear();
    out.tags.clear();

    if (program_size && with_program)
    {
        const uint8_t* compressed = data.data() + 16 + reserved_size;
        if (crc32(0, compressed, program_size) != crc)
        {
            return "program CRC mismatch";
        }

        z_stream zs{};
        if (inflateInit(&zs) != Z_OK)
        {
            return "zlib init failed";
        }

        zs.next_in = const_cast<uint8_t*>(compressed);
        zs.avail_in = program_size;
        std::vector<uint8_t> buf(1 << 20);
        int ret;
        do
        {
            zs.next_out = buf.data();
            zs.avail_out = static_cast<uInt>(buf.size());
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END)
            {
                inflateEnd(&zs);
                return "corrupt program data";
            }

            const std::size_t produced = buf.size() - zs.avail_out;
            if (produced > kMaxProgramSize - out.program.size())
            {
                inflateEnd(&zs);
                return "program larger than 1 GiB";
            }

            out.program.insert(out.program.end(), buf.data(), buf.data() + produced);
        } while (ret != Z_STREAM_END && (zs.avail_in > 0 || zs.avail_out == 0));

        inflateEnd(&zs);
        if (ret != Z_STREAM_END)
        {
            return "truncated program data";
        }
    }

    const std::size_t tag_start = 16ull + reserved_size + program_size;
    if (data.size() >= tag_start + 5 && std::memcmp(data.data() + tag_start, "[TAG]", 5) == 0)
    {
        out.tags = ParseTags(reinterpret_cast<const char*>(data.data() + tag_start + 5), data.size() - tag_start - 5);
    }

    return std::nullopt;
}

namespace
{

// Writes a PSF file whose program is `pieces`, one after another. The pieces are compressed as they come, straight into
// the file's bytes. From the start, the file's bytes hold room for zlib's bound on the compressed size.
std::vector<uint8_t> WritePsfPieces(uint8_t version, const std::vector<uint8_t>& reserved,
                                    const std::vector<std::span<const uint8_t>>& pieces, const Tags& tags)
{
    uint64_t program_size = 0;
    for (const std::span<const uint8_t> piece : pieces)
    {
        program_size += piece.size();
    }

    const std::string text = FormatTagArea(tags);
    std::vector<uint8_t> out = {'P', 'S', 'F', version};
    Put32(out, static_cast<uint32_t>(reserved.size()));
    Put32(out, 0); // the compressed program's size and CRC, filled in below
    Put32(out, 0);
    out.insert(out.end(), reserved.begin(), reserved.end());
    const std::size_t start = out.size();
    if (program_size > 0)
    {
        z_stream zs{};
        if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK)
        {
            throw std::runtime_error("zlib couldn't compress the program");
        }

        const uLong bound = deflateBound(&zs, static_cast<uLong>(program_size));
        out.reserve(start + bound + text.size());
        out.resize(start + bound);
        zs.next_out = out.data() + start;
        zs.avail_out = static_cast<uInt>(bound);
        bool ok = true;
        for (const std::span<const uint8_t> piece : pieces)
        {
            // zlib counts in 32-bit units.
            for (std::size_t done = 0; ok && done < piece.size();)
            {
                const std::size_t part = std::min<std::size_t>(piece.size() - done, std::size_t{1} << 30);
                zs.next_in = const_cast<uint8_t*>(piece.data() + done);
                zs.avail_in = static_cast<uInt>(part);
                ok = deflate(&zs, Z_NO_FLUSH) == Z_OK && zs.avail_in == 0;
                done += part;
            }
        }

        ok = ok && deflate(&zs, Z_FINISH) == Z_STREAM_END;
        const std::size_t compressed = bound - zs.avail_out;
        deflateEnd(&zs);
        if (!ok)
        {
            throw std::runtime_error("zlib couldn't compress the program");
        }

        out.resize(start + compressed);
        StoreLe32(&out[8], static_cast<uint32_t>(compressed));
        StoreLe32(&out[12], static_cast<uint32_t>(crc32(0, out.data() + start, static_cast<uInt>(compressed))));
    }

    out.insert(out.end(), text.begin(), text.end());

    return out;
}

} // namespace

std::vector<uint8_t> WritePsf(uint8_t version, const std::vector<uint8_t>& reserved,
                              const std::vector<uint8_t>& program, const Tags& tags)
{
    return WritePsfPieces(version, reserved, {program}, tags);
}

std::vector<uint8_t> ProgramBuilder::WritePsf(uint8_t version, const std::vector<uint8_t>& reserved,
                                              const Tags& tags) const
{
    return WritePsfPieces(version, reserved, Pieces(), tags);
}

std::string FormatTagArea(const Tags& tags)
{
    if (tags.empty())
    {
        return {};
    }

    std::string text = "[TAG]";
    for (const auto& [k, v] : tags)
    {
        std::size_t start = 0;
        while (true)
        {
            const std::size_t nl = v.find('\n', start);
            text += k + "=" + v.substr(start, nl == std::string::npos ? std::string::npos : nl - start) + "\n";
            if (nl == std::string::npos)
            {
                break;
            }

            start = nl + 1;
        }
    }

    return text;
}

std::optional<DecimalNumber> ReadDecimal(std::string_view text)
{
    std::size_t at = 0;
    const bool negative = !text.empty() && text[0] == '-';
    if (!text.empty() && (text[0] == '-' || text[0] == '+'))
    {
        at++;
    }

    // The value keeps up to 19 significant digits. They fit in 64 bits. Later digits before the decimal point scale the
    // value, and later digits after it are dropped.
    constexpr int kMaxDigits = 19;
    uint64_t digits = 0;
    int significant = 0;
    int exponent = 0; // of ten
    bool any_digit = false;
    bool point = false;
    for (; at < text.size(); at++)
    {
        const char c = text[at];
        if ((c == '.' || c == ',') && !point)
        {
            point = true;
            continue;
        }

        if (c < '0' || c > '9')
        {
            break;
        }

        any_digit = true;
        if (significant < kMaxDigits)
        {
            significant += digits != 0 || c != '0' ? 1 : 0;
            digits = digits * 10 + static_cast<uint64_t>(c - '0');
            exponent -= point ? 1 : 0;
        }
        else if (!point)
        {
            exponent++;
        }
    }

    if (!any_digit)
    {
        return std::nullopt;
    }

    // An exponent counts only with a digit: "1e" is the number 1 followed by a letter.
    constexpr int kMaxExponent = 100'000;
    std::size_t end = at;
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E'))
    {
        std::size_t e = at + 1;
        const bool negative_exponent = e < text.size() && text[e] == '-';
        if (e < text.size() && (text[e] == '-' || text[e] == '+'))
        {
            e++;
        }

        const std::size_t first = e;
        int value = 0;
        for (; e < text.size() && text[e] >= '0' && text[e] <= '9'; e++)
        {
            value = std::min(value * 10 + (text[e] - '0'), kMaxExponent);
        }

        if (e > first)
        {
            exponent += negative_exponent ? -value : value;
            end = e;
        }
    }

    // While the digits fit in a double's 53 bits and the power of ten is at most 10^22, both are exact. Then one
    // multiplication or division rounds the value once, to what std::strtod gives. Longer numbers and larger exponents
    // round more than once, and can differ from std::strtod in the last bit.
    constexpr std::array<double, 23> kPowersOfTen = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                                     1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                                     1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    constexpr int kLargestPower = static_cast<int>(kPowersOfTen.size()) - 1;
    double value = static_cast<double>(digits);
    for (; exponent > kLargestPower; exponent -= kLargestPower)
    {
        value *= kPowersOfTen[kLargestPower];
    }

    for (; exponent < -kLargestPower; exponent += kLargestPower)
    {
        value /= kPowersOfTen[kLargestPower];
    }

    value = exponent < 0 ? value / kPowersOfTen[-exponent] : value * kPowersOfTen[exponent];

    return DecimalNumber{negative ? -value : value, end};
}

long long ParseTime(const std::string& text)
{
    const std::string time = Trim(text);
    if (time.empty())
    {
        return -1;
    }

    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t colon = time.find(':', start);
        parts.push_back(time.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (colon == std::string::npos)
        {
            break;
        }

        start = colon + 1;
    }

    if (parts.size() > 3)
    {
        return -1;
    }

    long long whole = 0; // hours and minutes, in minutes
    for (std::size_t i = 0; i + 1 < parts.size(); i++)
    {
        char* end = nullptr;
        const long long v = std::strtoll(parts[i].c_str(), &end, 10);
        if (parts[i].empty() || !end || *end || v < 0 || v > kMaxTimeField)
        {
            return -1;
        }

        whole = whole * 60 + v;
    }

    const std::string seconds_text = Trim(parts.back());
    const std::optional<DecimalNumber> seconds = ReadDecimal(seconds_text);
    if (!seconds || seconds->length != seconds_text.size())
    {
        return -1;
    }

    // The comparisons are written so that infinity fails them.
    const double ms = (static_cast<double>(whole) * 60.0 + seconds->value) * 1000.0 + 0.5;
    if (!(seconds->value >= 0) || !(ms <= kMaxTimeMs))
    {
        return -1;
    }

    return static_cast<long long>(ms);
}

std::string FormatTime(long long ms)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);

    return buf;
}

std::vector<uint8_t> ProcessDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    Put32(v, kMagic);
    Put32(v, version);
    Put32(v, flags);
    Put32(v, output_mode);
    PutPath(v, executable_path);
    PutPath(v, archive_path);
    PadTo4(v);
    Put32(v, static_cast<uint32_t>(profile.size()));
    for (const uint32_t word : profile)
    {
        Put32(v, word);
    }

    return v;
}

std::optional<ProcessDescriptor> ProcessDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 16 || LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ProcessDescriptor d;
    d.version = LoadLe32(&data[4]);
    d.flags = LoadLe32(&data[8]);
    d.output_mode = LoadLe32(&data[12]);
    std::size_t pos = 16;
    const std::optional<std::string> executable = ReadPath(data, pos);
    const std::optional<std::string> archive = executable ? ReadPath(data, pos) : std::nullopt;
    pos = (pos + 3) & ~std::size_t{3};
    if (!archive || pos + 4 > data.size())
    {
        return std::nullopt;
    }

    d.executable_path = *executable;
    d.archive_path = *archive;
    const uint32_t words = LoadLe32(&data[pos]);
    pos += 4;
    if (words > (data.size() - pos) / 4)
    {
        return std::nullopt;
    }

    for (uint32_t i = 0; i < words; ++i)
    {
        d.profile.push_back(LoadLe32(&data[pos + 4 * i]));
    }

    return d;
}

std::optional<std::string> ProcessDescriptor::Check() const
{
    if (auto err = CheckCommon("process", version, flags, output_mode))
    {
        return err;
    }

    if (profile.size() != kProfileWords)
    {
        return "the process descriptor's game profile has " + std::to_string(profile.size()) +
               " words; version 1 has " + std::to_string(kProfileWords);
    }

    return std::nullopt;
}

std::vector<uint8_t> ArchiveDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    Put32(v, kMagic);
    Put32(v, version);
    Put32(v, flags);
    Put32(v, output_mode);
    Put32(v, voice_count);
    Put32(v, ax_settings);
    Put32(v, sdk_version);
    PutPath(v, archive_path);
    Put16(v, static_cast<uint32_t>(group_paths.size()));
    for (const std::string& path : group_paths)
    {
        PutPath(v, path);
    }

    PadTo4(v);

    return v;
}

std::optional<ArchiveDescriptor> ArchiveDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 28 || LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ArchiveDescriptor d;
    d.version = LoadLe32(&data[4]);
    d.flags = LoadLe32(&data[8]);
    d.output_mode = LoadLe32(&data[12]);
    d.voice_count = LoadLe32(&data[16]);
    d.ax_settings = LoadLe32(&data[20]);
    d.sdk_version = LoadLe32(&data[24]);
    std::size_t pos = 28;
    const std::optional<std::string> archive = ReadPath(data, pos);
    const std::optional<uint32_t> groups = archive ? Read16(data, pos) : std::nullopt;
    if (!groups)
    {
        return std::nullopt;
    }

    d.archive_path = *archive;
    for (uint32_t i = 0; i < *groups; ++i)
    {
        const std::optional<std::string> path = ReadPath(data, pos);
        if (!path)
        {
            return std::nullopt;
        }

        d.group_paths.push_back(*path);
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    if (auto err = CheckCommon("archive", version, flags, output_mode))
    {
        return err;
    }

    if (voice_count < 1 || voice_count > kDefaultVoices)
    {
        return "the archive descriptor's voice count is " + std::to_string(voice_count) + "; it must be 1 to 96";
    }

    if ((ax_settings & ~(kAxRenderer48k | kAxUpsampleAfterFinalMix)) != 0)
    {
        return "the archive descriptor's AX settings are " + Hex(ax_settings) +
               "; version 1 supports only bits 0 and 1";
    }

    return std::nullopt;
}

namespace
{

// A FILE chunk's header and its path, padded to four bytes. The file's data follows them.
std::vector<uint8_t> FileChunkHead(const std::string& path, std::size_t data_size)
{
    std::vector<uint8_t> path_area;
    PutPath(path_area, path);
    PadTo4(path_area);
    std::vector<uint8_t> head = {'F', 'I', 'L', 'E'};
    Put32(head, static_cast<uint32_t>(path_area.size() + data_size));
    head.insert(head.end(), path_area.begin(), path_area.end());

    return head;
}

} // namespace

void ProgramBuilder::Hold(std::vector<uint8_t> bytes)
{
    if (!bytes.empty())
    {
        pieces_.push_back({std::move(bytes), {}});
    }
}

void ProgramBuilder::AddFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> chunk = FileChunkHead(path, data.size());
    chunk.insert(chunk.end(), data.begin(), data.end());
    PadTo4(chunk);
    Hold(std::move(chunk));
}

void ProgramBuilder::AddFileRef(const std::string& path, std::span<const uint8_t> data)
{
    Hold(FileChunkHead(path, data.size()));
    pieces_.push_back({{}, data});
    Hold(std::vector<uint8_t>((4 - data.size() % 4) % 4, 0));
}

void ProgramBuilder::AddSound(uint32_t sound_id)
{
    std::vector<uint8_t> chunk = {'S', 'N', 'D', ' '};
    Put32(chunk, 4);
    Put32(chunk, sound_id);
    Hold(std::move(chunk));
}

std::vector<std::span<const uint8_t>> ProgramBuilder::Pieces() const
{
    std::vector<std::span<const uint8_t>> pieces;
    for (const Piece& piece : pieces_)
    {
        pieces.push_back(piece.held.empty() ? piece.ref : std::span<const uint8_t>(piece.held));
    }

    return pieces;
}

std::vector<uint8_t> ProgramBuilder::Data() const
{
    std::vector<uint8_t> program;
    for (const std::span<const uint8_t> piece : Pieces())
    {
        program.insert(program.end(), piece.begin(), piece.end());
    }

    return program;
}

namespace
{

// A program's bytes in order, read a piece at a time.
class ProgramSource
{
public:
    virtual ~ProgramSource() = default;

    // Reads up to `n` bytes into `dest` and sets `got` to how many it read. Fewer than `n` are read only at the
    // program's end. Returns an error if the program can't be read.
    virtual std::optional<std::string> Read(uint8_t* dest, std::size_t n, std::size_t& got) = 0;

    // The most bytes the program can have left. A chunk's size can then be checked before its data is read.
    virtual uint64_t MaxLeft() const = 0;
};

// A decompressed program.
class MemorySource final : public ProgramSource
{
public:
    explicit MemorySource(const std::vector<uint8_t>& program) : program_(program)
    {
    }

    std::optional<std::string> Read(uint8_t* dest, std::size_t n, std::size_t& got) override
    {
        got = std::min(n, program_.size() - pos_);
        std::copy_n(program_.begin() + static_cast<std::ptrdiff_t>(pos_), got, dest);
        pos_ += got;

        return std::nullopt;
    }

    uint64_t MaxLeft() const override
    {
        return program_.size() - pos_;
    }

private:
    const std::vector<uint8_t>& program_;
    std::size_t pos_ = 0;
};

// A PSF file's compressed program. The program is inflated as it's read. Then a chunk's data can go straight where it
// belongs.
class InflateSource final : public ProgramSource
{
public:
    InflateSource(const uint8_t* compressed, uint32_t size)
    {
        ok_ = inflateInit(&zs_) == Z_OK;
        zs_.next_in = const_cast<uint8_t*>(compressed);
        zs_.avail_in = size;
    }

    ~InflateSource() override
    {
        if (ok_)
        {
            inflateEnd(&zs_);
        }
    }

    InflateSource(const InflateSource&) = delete;
    InflateSource& operator=(const InflateSource&) = delete;

    std::optional<std::string> Read(uint8_t* dest, std::size_t n, std::size_t& got) override
    {
        got = 0;
        if (!error_ && !ok_)
        {
            error_ = "zlib init failed";
        }

        while (!error_ && got < n && !ended_)
        {
            // zlib counts in 32-bit units.
            const std::size_t want = std::min<std::size_t>(n - got, std::size_t{1} << 30);
            zs_.next_out = dest + got;
            zs_.avail_out = static_cast<uInt>(want);
            const int ret = inflate(&zs_, Z_NO_FLUSH);
            const std::size_t produced = want - zs_.avail_out;
            got += produced;
            produced_ += produced;
            if (produced_ > kMaxProgramSize)
            {
                error_ = "program larger than 1 GiB";
            }
            else if (ret == Z_STREAM_END)
            {
                ended_ = true;
            }
            else if (ret == Z_BUF_ERROR && zs_.avail_in == 0)
            {
                error_ = "truncated program data";
            }
            else if (ret != Z_OK)
            {
                error_ = "corrupt program data";
            }
        }

        return error_;
    }

    // Reads the rest of the stream, and returns its error if it has one.
    std::optional<std::string> Drain()
    {
        std::vector<uint8_t> scratch(0x10000);
        std::size_t got = scratch.size();
        while (got == scratch.size())
        {
            if (auto err = Read(scratch.data(), scratch.size(), got))
            {
                return err;
            }
        }

        return std::nullopt;
    }

    uint64_t MaxLeft() const override
    {
        // Deflate makes at most about 1,032 bytes of each compressed byte. zlib can also hold some output that it
        // hasn't given out yet.
        const uint64_t most = (uint64_t{zs_.avail_in} + 1) * 1032 + 0x10000;
        return ended_ ? 0 : std::min<uint64_t>(most, kMaxProgramSize - produced_);
    }

private:
    z_stream zs_{};
    bool ok_ = false;
    bool ended_ = false;
    uint64_t produced_ = 0;
    std::optional<std::string> error_; // once the stream has failed, every read gives its error
};

// Splits a program into chunks as it reads it (see ParseProgram).
std::optional<std::string> SplitChunks(ProgramSource& source, ProgramChunks& out)
{
    // Reads exactly `n` bytes of a chunk.
    const auto read_all = [&source](uint8_t* dest, std::size_t n) -> std::optional<std::string>
    {
        std::size_t got = 0;
        if (auto err = source.Read(dest, n, got))
        {
            return err;
        }

        return got < n ? std::optional<std::string>("truncated chunk") : std::nullopt;
    };

    // Reads and drops `n` bytes of a chunk.
    std::vector<uint8_t> scratch;
    const auto skip = [&](uint64_t n) -> std::optional<std::string>
    {
        scratch.resize(static_cast<std::size_t>(std::min<uint64_t>(n, 0x10000)));
        while (n > 0)
        {
            const auto part = static_cast<std::size_t>(std::min<uint64_t>(n, scratch.size()));
            if (auto err = read_all(scratch.data(), part))
            {
                return err;
            }

            n -= part;
        }

        return std::nullopt;
    };

    while (true)
    {
        uint8_t header[8];
        std::size_t got = 0;
        if (auto err = source.Read(header, sizeof(header), got))
        {
            return err;
        }

        // The end. Any bytes left over are too few for a chunk's header.
        if (got < sizeof(header))
        {
            return std::nullopt;
        }

        const uint32_t size = LoadLe32(header + 4);
        if (size > source.MaxLeft())
        {
            return "truncated chunk";
        }

        // A chunk with a bad header is read to its end first. That way, a chunk that the program's end cuts short is
        // reported as truncated.
        if (std::memcmp(header, "FILE", 4) == 0)
        {
            if (size < 2)
            {
                return skip(size).value_or("bad FILE chunk");
            }

            uint8_t length[2];
            if (auto err = read_all(length, sizeof(length)))
            {
                return err;
            }

            const uint32_t len = LoadLe16(length);
            const uint32_t data_start = (2 + len + 3) & ~3u;
            if (data_start > size)
            {
                return skip(size - 2).value_or("bad FILE chunk");
            }

            FileChunk file;
            file.path.resize(len);
            file.data.resize(size - data_start);
            if (auto err = read_all(reinterpret_cast<uint8_t*>(file.path.data()), len))
            {
                return err;
            }
            if (auto err = skip(data_start - 2 - len))
            {
                return err;
            }
            if (auto err = read_all(file.data.data(), file.data.size()))
            {
                return err;
            }

            out.files.push_back(std::move(file));
        }
        else if (std::memcmp(header, "SND ", 4) == 0)
        {
            if (size < 4)
            {
                return skip(size).value_or("bad SND chunk");
            }

            uint8_t word[4];
            if (auto err = read_all(word, sizeof(word)))
            {
                return err;
            }
            if (auto err = skip(size - 4))
            {
                return err;
            }

            out.sound = LoadLe32(word);
        }
        else if (auto err = skip(size))
        {
            return err;
        }

        // The padding to a multiple of four bytes. The program's end may cut it short.
        uint8_t padding[3];
        const std::size_t pad = ((size + 3) & ~3u) - size;
        if (auto err = source.Read(padding, pad, got))
        {
            return err;
        }
        if (got < pad)
        {
            return std::nullopt;
        }
    }
}

} // namespace

std::optional<std::string> ParseProgram(const std::vector<uint8_t>& program, ProgramChunks& out)
{
    MemorySource source(program);
    return SplitChunks(source, out);
}

std::optional<std::string> ParsePsfChunks(const std::vector<uint8_t>& data, ProgramChunks& out)
{
    if (data.size() < 16 || std::memcmp(data.data(), "PSF", 3) != 0)
    {
        return "not a PSF file";
    }

    const uint32_t reserved_size = LoadLe32(&data[4]);
    const uint32_t program_size = LoadLe32(&data[8]);
    const uint32_t crc = LoadLe32(&data[12]);
    if (16ull + reserved_size + program_size > data.size())
    {
        return "truncated PSF file";
    }
    if (program_size == 0)
    {
        return std::nullopt;
    }

    const uint8_t* compressed = data.data() + 16 + reserved_size;
    if (crc32(0, compressed, program_size) != crc)
    {
        return "program CRC mismatch";
    }

    // An error in the zlib stream comes first. The same happens when the program is inflated whole before it's split.
    InflateSource source(compressed, program_size);
    const auto err = SplitChunks(source, out);
    if (err)
    {
        if (auto stream_err = source.Drain())
        {
            return stream_err;
        }
    }

    return err;
}

const std::vector<uint8_t>* LoadedSet::File(const std::string& path) const
{
    for (auto it = files.rbegin(); it != files.rend(); ++it)
    {
        if (it->path == path)
        {
            return &it->data;
        }
    }

    return nullptr;
}

std::vector<uint8_t>* LoadedSet::File(const std::string& path)
{
    return const_cast<std::vector<uint8_t>*>(std::as_const(*this).File(path));
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& data)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }

    // One read of the whole file. That's far quicker than copying the file a byte at a time.
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0);
    if (size < 0 || !f)
    {
        // Read streams with unknown sizes, such as pipes, until EOF.
        f.clear();
        data.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }

    data.resize(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    data.resize(static_cast<std::size_t>(f.gcount()));

    return true;
}

namespace
{

std::optional<std::string> LoadRecursive(const std::string& path, const FileReader& reader, std::string_view separators,
                                         LoadedSet& out, std::optional<uint32_t>& sound, int depth, bool top)
{
    if (depth > kMaxLibDepth)
    {
        return "_lib nesting too deep";
    }

    if (out.sources.size() >= kMaxSetFiles)
    {
        return "more than 64 files in the set";
    }

    std::vector<uint8_t> data;
    if (!reader(path, data))
    {
        return "can't read " + path;
    }

    // The program is read later, straight into its chunks.
    PsfFile psf;
    if (auto err = ParsePsf(data, psf, false))
    {
        return path + ": " + *err;
    }

    if (psf.version != kPsfVersionCsf)
    {
        char version[8];
        std::snprintf(version, sizeof(version), "%02x", psf.version);
        return path + ": not a CSF file (PSF version 0x" + version + ")";
    }

    const std::string dir = DirectoryOf(path, separators);
    auto lib = psf.tags.find("_lib");
    if (lib != psf.tags.end() && !lib->second.empty())
    {
        if (auto err = LoadRecursive(dir + lib->second, reader, separators, out, sound, depth + 1, false))
        {
            return err;
        }
    }

    ProgramChunks chunks;
    if (auto err = ParsePsfChunks(data, chunks))
    {
        return path + ": " + *err;
    }

    for (auto& f : chunks.files)
    {
        out.files.push_back(std::move(f));
    }

    if (chunks.sound)
    {
        sound = chunks.sound;
    }

    // The last descriptor in loading order determines the mode.
    if (auto d = ProcessDescriptor::Parse(psf.reserved))
    {
        if (auto err = d->Check())
        {
            return path + ": " + *err;
        }

        out.process = d;
        out.archive.reset();
    }
    else if (auto a = ArchiveDescriptor::Parse(psf.reserved))
    {
        if (auto err = a->Check())
        {
            return path + ": " + *err;
        }

        out.archive = a;
        out.process.reset();
    }

    out.sources.push_back(path);

    for (int n = 2;; n++)
    {
        auto it = psf.tags.find("_lib" + std::to_string(n));
        if (it == psf.tags.end())
        {
            break;
        }

        // An empty one names no library. An empty _lib doesn't either.
        if (it->second.empty())
        {
            continue;
        }

        if (auto err = LoadRecursive(dir + it->second, reader, separators, out, sound, depth + 1, false))
        {
            return err;
        }
    }

    if (top)
    {
        out.tags = psf.tags;
    }

    return std::nullopt;
}

} // namespace

std::optional<std::string> LoadSet(const std::string& path, const FileReader& reader, LoadedSet& out,
                                   std::string_view separators)
{
    out = LoadedSet{};

    std::optional<uint32_t> sound;
    if (auto err = LoadRecursive(path, reader, separators, out, sound, 0, true))
    {
        return err;
    }

    if (!out.process && !out.archive)
    {
        return "no process or archive descriptor in " + path + " or its libraries";
    }

    if (!sound)
    {
        return "no sound selected (SND chunk) in " + path + " or its libraries";
    }

    out.sound = *sound;

    return std::nullopt;
}

} // namespace csf
