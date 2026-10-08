// SPDX-License-Identifier: MIT

// Playback of a CSF set as a finite (or endless) stream: length, fade and volume from the tags or the defaults, end
// detection and seeking. Shared by the CLI renderer and the foobar2000 component so that they play a file identically.
//
// A seek renders on from the current position, or from the start when the target lies before it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "csf/player.h"

namespace csf
{

struct PlaybackOptions
{
    // The defaults of default_length_ms and default_fade_ms. csfplay's help gives them in whole seconds.
    static constexpr long long kDefaultLengthMs = 150000;
    static constexpr long long kDefaultFadeMs = 10000;
    static_assert(kDefaultLengthMs % 1000 == 0 && kDefaultFadeMs % 1000 == 0);

    // Length and fade used when a file has no length tag.
    long long default_length_ms = kDefaultLengthMs;
    long long default_fade_ms = kDefaultFadeMs;

    // Play endlessly: ignore the length and fade, and don't stop at the end of the sound.
    bool endless = false;

    // Overrides of the length and fade tags (ignored when negative).
    long long length_override_ms = -1;
    long long fade_override_ms = -1;
};

// A file's play time before its fade, and the fade's length.
struct TrackLength
{
    long long play_ms = 0;
    long long fade_ms = 0;
};

// The length for a file whose length and fade tags give `length_ms` and `fade_ms` (negative when a tag is missing).
// Without a length tag, the file gets the default length, and the default fade unless it has a fade tag.
TrackLength ResolveLength(long long length_ms, long long fade_ms, const PlaybackOptions& options);

class Playback
{
public:
    static constexpr int kSampleRate = Player::kSampleRate;

    // Loads a file and its libraries, without starting.
    bool Open(const std::string& path, const FileReader& reader, const PlaybackOptions& options = {});

    // Uses an already assembled set.
    bool Open(LoadedSet set, const PlaybackOptions& options);

    // Starts (or restarts) from the beginning.
    bool Start();

    // Renders up to `frames` stereo frames (interleaved L, R, 16-bit) with fade and volume applied. Returns the number
    // of frames written: fewer than requested at the end of the track, 0 after it or after an error (see Error()).
    std::size_t Render(int16_t* out, std::size_t frames);

    // Seeks to `frame`. Polls `abort` between blocks and returns false if it requests cancellation.
    bool Seek(uint64_t frame, const std::function<bool()>& abort = {});

    // Returns the frame a seek to `frame` starts rendering from: the current position, or 0 if the sound must restart.
    uint64_t SeekStart(uint64_t frame) const;

    // Current position in frames.
    uint64_t Position() const
    {
        return position_;
    }

    // Frames before the fade starts. Endless playback ignores this and FadeFrames.
    uint64_t PlayFrames() const
    {
        return play_frames_;
    }

    // The fade's length in frames.
    uint64_t FadeFrames() const
    {
        return fade_frames_;
    }

    // Total length in frames (0 when endless).
    uint64_t LengthFrames() const
    {
        return options_.endless ? 0 : play_frames_ + fade_frames_;
    }

    // True when the length comes from the file's tags or the options rather than the defaults.
    bool HasLengthTag() const
    {
        return tagged_;
    }

    const Tags& GetTags() const
    {
        return player_.GetTags();
    }

    const std::string& Error() const
    {
        return player_.Error();
    }

    Player& GetPlayer()
    {
        return player_;
    }

private:
    void ComputeLength();

    // Whether a seek to `frame` restarts the sound: the target lies before the current position, or the player can't go
    // on from it.
    bool Restarts(uint64_t frame) const;

    Player player_;
    PlaybackOptions options_;
    bool tagged_ = false;
    uint64_t play_frames_ = 0;
    uint64_t fade_frames_ = 0;
    uint64_t position_ = 0;
    std::optional<uint64_t> finished_at_; // the frame at which the sound reported its end (untagged files)
    std::vector<int16_t> scratch_;
};

} // namespace csf
