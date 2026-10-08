// SPDX-License-Identifier: MIT

// foo_input_csf: foobar2000 input component for CSF (.csf and .minicsf). Modelled on kode54's PSF-family inputs
// (foo_input_vio2sf, foo_input_ncsf). PSF tags are shown as metadata (game as album, year as date). Length and fade
// come from the tags (or the defaults in Advanced preferences > Decoding > CSF decoder). The tags can be edited.
//
// The player core (src/csf) plays the sound: with the game's sound code on an emulated Espresso in game mode, or with
// CSF's model of the SDK's sound library in archive mode. CSF's model of AX does the mixing either way. While playing,
// the component renders ahead on a worker thread and buffers the audio (see src/csf/render_ahead.h). Seeks within the
// buffer are instant. Seeks further ahead wait for rendering to reach the target. Seeks back before the buffered audio
// render from the start of the track. With rendering ahead turned off, decode_run renders each block itself.
//
// Build: see plugins/foobar2000/README.md (clang-cl or MSVC on Windows, or Apple Clang on macOS, with the foobar2000
// SDK).

#include <SDK/foobar2000.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/ascii.h"
#include "csf/format.h"
#include "csf/playback.h"
#include "csf/render_ahead.h"

namespace
{

using csf::Playback;
using csf::PlaybackOptions;
using csf::Player;
using csf::RenderAhead;
using csf::Tags;

// {055C07E3-B971-4C58-942D-7DA2A3F7DCF4}
constexpr GUID kInputGuid = {0x055c07e3, 0xb971, 0x4c58, {0x94, 0x2d, 0x7d, 0xa2, 0xa3, 0xf7, 0xdc, 0xf4}};

// Advanced preferences entries, under Decoding > CSF decoder.
constexpr GUID kBranchGuid = {0x93350f79, 0xc605, 0x4fd4, {0x9f, 0xf8, 0xb6, 0xd9, 0x38, 0xe5, 0x11, 0x15}};
constexpr GUID kLengthGuid = {0x8651c6b1, 0x9efb, 0x4bfe, {0xab, 0x8c, 0x4a, 0xa5, 0x42, 0x45, 0xd4, 0xb1}};
constexpr GUID kFadeGuid = {0x24193d95, 0x3830, 0x4255, {0xa9, 0x67, 0x31, 0xbb, 0x28, 0xed, 0xf2, 0x5c}};
constexpr GUID kEndlessGuid = {0x74f274d2, 0xdc82, 0x411a, {0xaa, 0x61, 0x77, 0x26, 0xe0, 0x76, 0x70, 0xff}};
constexpr GUID kRenderAheadGuid = {0xe4096534, 0x4c76, 0x4744, {0x85, 0x8a, 0xf8, 0x2c, 0xa4, 0x71, 0xeb, 0xd7}};

advconfig_branch_factory cfg_branch("CSF decoder", kBranchGuid, advconfig_entry::guid_branch_decoding, 0);
advconfig_integer_factory cfg_default_length("Default length for files without a length tag (seconds)", kLengthGuid,
                                             kBranchGuid, 0, PlaybackOptions::kDefaultLengthMs / 1000, 1, 24 * 3600);
advconfig_integer_factory cfg_default_fade("Default fade for files without a length tag (seconds)", kFadeGuid,
                                           kBranchGuid, 1, PlaybackOptions::kDefaultFadeMs / 1000, 0, 3600);
advconfig_checkbox_factory cfg_endless("Play endlessly (ignore lengths during playback only)", kEndlessGuid,
                                       kBranchGuid, 2, false);
advconfig_checkbox_factory cfg_render_ahead("Render ahead during playback (quicker seeking; uses more CPU and memory)",
                                            kRenderAheadGuid, kBranchGuid, 3, true);

constexpr std::size_t kBlockFrames = 1024; // frames decoded per decode_run call

// The largest file read into memory: as large as a program may be. A .csflib holds a whole sound archive, and the
// largest archives are hundreds of megabytes.
constexpr t_filesize kMaxFileSize = t_filesize{1} << 30;

// PSF tags that aren't metadata. Editing the tags keeps them as they are.
bool IsSystemTag(const std::string& name)
{
    return name.starts_with("_lib") || name == "length" || name == "fade" || name == "volume" || name == "utf8";
}

// The foobar2000 field name for a PSF tag name.
std::string FieldFromTag(const std::string& tag)
{
    if (tag == "game")
    {
        return "album";
    }
    if (tag == "year")
    {
        return "date";
    }

    return tag;
}

// The PSF tag name for a foobar2000 field name.
std::string TagFromField(const std::string& field)
{
    if (field == "album")
    {
        return "game";
    }
    if (field == "date")
    {
        return "year";
    }

    return field;
}

// PSF tags are UTF-8 when the file has a utf8 tag. Without one, text that isn't valid UTF-8 is taken to be in the
// system code page on Windows, and in Windows-1252 elsewhere. That's how pfc converts "ANSI" text off Windows.
pfc::string8 TagText(const std::string& value, bool utf8)
{
    if (utf8 || pfc::is_valid_utf8(value.c_str()))
    {
        return pfc::string8(value.c_str());
    }

    return pfc::string8(pfc::stringcvt::string_utf8_from_ansi(value.c_str()));
}

void ReadWhole(file::ptr& f, std::vector<uint8_t>& out, abort_callback& abort)
{
    f->reopen(abort);
    const t_filesize size = f->get_size_ex(abort);
    if (size > kMaxFileSize)
    {
        pfc::throw_exception_with_message<exception_io_data>("CSF file too large");
    }

    out.resize(static_cast<std::size_t>(size));
    if (!out.empty())
    {
        f->read_object(out.data(), out.size(), abort);
    }
}

class InputCsf : public input_stubs
{
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort)
    {
        path_ = p_path;
        file_ = p_filehint;
        input_open_file_helper(file_, p_path, p_reason, p_abort);
        ReadWhole(file_, data_, p_abort);

        csf::PsfFile psf;
        if (auto err = csf::ParsePsf(data_, psf, false))
        {
            throw exception_io_unsupported_format();
        }

        if (psf.version != csf::kPsfVersionCsf)
        {
            throw exception_io_unsupported_format();
        }

        tags_ = std::move(psf.tags);
    }

    void get_info(file_info& p_info, abort_callback& /*p_abort*/)
    {
        const bool utf8 = tags_.count("utf8") != 0;
        replaygain_info rg = p_info.get_replaygain();
        bool have_rg = false;
        for (const auto& [name, value] : tags_)
        {
            if (IsSystemTag(name))
            {
                continue;
            }

            if (rg.set_from_meta(name.c_str(), value.c_str()))
            {
                have_rg = true;
                continue;
            }

            const std::string field = FieldFromTag(name);

            // A multi-line value (repeated tag lines) becomes one field value per line.
            std::size_t start = 0;
            while (true)
            {
                const std::size_t nl = value.find('\n', start);
                const std::string line = value.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                p_info.meta_add(field.c_str(), TagText(line, utf8));
                if (nl == std::string::npos)
                {
                    break;
                }

                start = nl + 1;
            }
        }
        if (have_rg)
        {
            p_info.set_replaygain(rg);
        }

        const csf::TrackLength length = GetLength();
        p_info.set_length(static_cast<double>(length.play_ms + length.fade_ms) / 1000.0);
        p_info.info_set_int("samplerate", Playback::kSampleRate);
        p_info.info_set_int("channels", 2);
        p_info.info_set_int("bitspersample", 16);
        p_info.info_set("encoding", "synthesized");
        p_info.info_set("codec", "CSF");
        if (auto it = tags_.find("volume"); it != tags_.end())
        {
            p_info.info_set("csf_volume", it->second.c_str());
        }
    }

    t_filestats2 get_stats2(uint32_t f, abort_callback& a)
    {
        return file_->get_stats2_(f, a);
    }

    void decode_initialize(unsigned p_flags, abort_callback& p_abort)
    {
        // foobar2000 may restart decoding with the same input. Stop the previous worker thread first.
        ahead_.reset();
        playback_.reset();

        // The file and its libraries, read through foobar2000's file system.
        const csf::FileReader reader = [&](const std::string& path, std::vector<uint8_t>& out) -> bool
        {
            if (path == path_)
            {
                out = data_;
                return true;
            }

            try
            {
                file::ptr f;
                filesystem::g_open_read(f, path.c_str(), p_abort);
                ReadWhole(f, out, p_abort);
                return true;
            }
            catch (const exception_aborted&)
            {
                throw;
            }
            catch (const std::exception&)
            {
                return false;
            }
        };
        // A file in an archive has a path like unpack://zip|<length>|<the archive's path>|<its path in the archive>. So
        // its libraries are found after the last | too.
        const bool in_archive = std::string_view(path_).starts_with("unpack://");
        csf::LoadedSet set;
        if (auto err = csf::LoadSet(path_, reader, set, in_archive ? "/\\|" : "/\\"))
        {
            pfc::throw_exception_with_message<exception_io_data>(err->c_str());
        }

        // Enable endless playback and rendering ahead only during playback. Conversion and scanning require a finite
        // length and never seek.
        const bool playing = (p_flags & input_flag_playback) != 0;
        PlaybackOptions options = DefaultOptions();
        options.endless = cfg_endless.get() && playing && !(p_flags & input_flag_no_looping);

        playback_ = std::make_unique<Playback>();
        if (!playback_->Open(std::move(set), options))
        {
            pfc::throw_exception_with_message<exception_io_data>(playback_->Error().c_str());
        }

        if (!playback_->Start())
        {
            pfc::throw_exception_with_message<exception_io_data>(playback_->Error().c_str());
        }

        if (playing && cfg_render_ahead.get())
        {
            ahead_ = std::make_unique<RenderAhead>(std::move(playback_));
        }

        buffer_.resize(2 * kBlockFrames);
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort)
    {
        p_abort.check();
        const std::size_t n = ahead_ ? ahead_->Read(buffer_.data(), kBlockFrames, [&] { return p_abort.is_aborting(); })
                                     : playback_->Render(buffer_.data(), kBlockFrames);
        if (n == 0)
        {
            p_abort.check();
            std::string error;
            if (ahead_)
            {
                error = ahead_->Error();
            }
            else if (playback_->GetPlayer().GetState() == Player::State::kError)
            {
                error = playback_->Error();
            }

            if (!error.empty())
            {
                pfc::throw_exception_with_message<exception_io_data>(error.c_str());
            }

            return false;
        }

        p_chunk.set_data_fixedpoint(buffer_.data(), n * 2 * sizeof(int16_t), Playback::kSampleRate, 2, 16,
                                    audio_chunk::channel_config_stereo);

        return true;
    }

    void decode_seek(double p_seconds, abort_callback& p_abort)
    {
        const t_uint64 target = audio_math::time_to_samples(p_seconds, Playback::kSampleRate);
        if (ahead_)
        {
            ahead_->Seek(target); // decode_run waits for the audio there
            return;
        }

        if (!playback_->Seek(target, [&] { return p_abort.is_aborting(); }))
        {
            p_abort.check();
            pfc::throw_exception_with_message<exception_io_data>(playback_->Error().c_str());
        }
    }

    bool decode_can_seek()
    {
        return true;
    }

    bool decode_get_dynamic_info(file_info& /*p_out*/, double& /*p_timestamp_delta*/)
    {
        return false;
    }

    bool decode_get_dynamic_info_track(file_info& /*p_out*/, double& /*p_timestamp_delta*/)
    {
        return false;
    }

    void decode_on_idle(abort_callback& p_abort)
    {
        file_->on_idle(p_abort);
    }

    // Rewrites the tag area with the edited tags. The program and the system tags (_lib, length, fade and volume) stay
    // as they are.
    void retag(const file_info& p_info, abort_callback& p_abort)
    {
        Tags tags;
        for (const auto& [name, value] : tags_)
        {
            if (IsSystemTag(name))
            {
                tags[name] = value;
            }
        }

        for (t_size i = 0; i < p_info.meta_get_count(); i++)
        {
            const std::string tag = TagFromField(csf::AsciiLower(p_info.meta_enum_name(i)));
            if (tag.empty() || IsSystemTag(tag) || tag.find('=') != std::string::npos)
            {
                continue;
            }

            std::string value;
            for (t_size j = 0; j < p_info.meta_enum_value_count(i); j++)
            {
                if (j)
                {
                    value += '\n';
                }
                value += p_info.meta_enum_value(i, j);
            }

            tags[tag] = value;
        }

        const replaygain_info rg = p_info.get_replaygain();
        replaygain_info::t_text_buffer buf;
        if (rg.format_album_gain(buf))
        {
            tags["replaygain_album_gain"] = buf;
        }

        if (rg.format_album_peak(buf))
        {
            tags["replaygain_album_peak"] = buf;
        }

        if (rg.format_track_gain(buf))
        {
            tags["replaygain_track_gain"] = buf;
        }

        if (rg.format_track_peak(buf))
        {
            tags["replaygain_track_peak"] = buf;
        }

        tags["utf8"] = "1";
        WriteTags(tags, p_abort);
    }

    void remove_tags(abort_callback& p_abort)
    {
        Tags tags;
        for (const auto& [name, value] : tags_)
        {
            if (IsSystemTag(name))
            {
                tags[name] = value;
            }
        }

        WriteTags(tags, p_abort);
    }

    static bool g_is_our_content_type(const char* /*p_content_type*/)
    {
        return false;
    }

    // A .csflib holds no sound to play by itself. Only .csf and .minicsf files play.
    static bool g_is_our_path(const char* /*p_path*/, const char* p_extension)
    {
        return stricmp_utf8(p_extension, "csf") == 0 || stricmp_utf8(p_extension, "minicsf") == 0;
    }

    static const char* g_get_name()
    {
        return "CSF decoder";
    }

    static GUID g_get_guid()
    {
        return kInputGuid;
    }

private:
    // The length and fade that Advanced preferences give files without a length tag.
    static PlaybackOptions DefaultOptions()
    {
        PlaybackOptions options;
        options.default_length_ms = static_cast<long long>(cfg_default_length.get()) * 1000;
        options.default_fade_ms = static_cast<long long>(cfg_default_fade.get()) * 1000;

        return options;
    }

    // The file's tagged length and fade, or the defaults: the values that playback will use.
    csf::TrackLength GetLength() const
    {
        const auto length = tags_.find("length");
        const auto fade = tags_.find("fade");
        const long long length_ms = length != tags_.end() ? csf::ParseTime(length->second) : -1;
        const long long fade_ms = fade != tags_.end() ? csf::ParseTime(fade->second) : -1;
        return csf::ResolveLength(length_ms, fade_ms, DefaultOptions());
    }

    void WriteTags(const Tags& tags, abort_callback& p_abort)
    {
        const auto offset = csf::TagAreaOffset(data_);
        if (!offset)
        {
            pfc::throw_exception_with_message<exception_io_data>("corrupt CSF file");
        }

        const std::string text = csf::FormatTagArea(tags);
        file_->seek(*offset, p_abort);
        file_->set_eof(p_abort);
        if (!text.empty())
        {
            file_->write_object(text.data(), text.size(), p_abort);
        }

        // Keep our copy current for a following get_info.
        data_.resize(*offset);
        data_.insert(data_.end(), text.begin(), text.end());
        tags_ = tags;
    }

    std::string path_;
    service_ptr_t<file> file_;
    std::vector<uint8_t> data_; // the file as read (its program is small for a .minicsf)
    Tags tags_;
    std::unique_ptr<Playback> playback_; // when decode_run renders the blocks itself
    std::unique_ptr<RenderAhead> ahead_; // owns the Playback while rendering ahead
    std::vector<int16_t> buffer_;
};

input_singletrack_factory_t<InputCsf> g_input_csf_factory;

} // namespace

DECLARE_FILE_TYPE("CSF files", "*.CSF;*.MINICSF");

DECLARE_COMPONENT_VERSION("CSF Decoder", CSF_VERSION,
                          "Plays CSF (Nintendo Wii U sequenced music) rips, with the game's sound code on an emulated "
                          "Espresso or with CSF's model of the SDK's sound library, mixed by CSF's model of AX, the "
                          "system's sound library.\n\n"
                          "MIT licence. Uses zlib.");

VALIDATE_COMPONENT_FILENAME("foo_input_csf.dll");
