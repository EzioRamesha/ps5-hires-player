#pragma once

#include <cstdint>
#include <string>

namespace bfplayer {

enum class MediaKind {
    unknown = 0,
    video,
    audio,
    playlist,
    subtitle,
};

struct MediaEntry {
    std::string path;
    std::string name;
    MediaKind kind = MediaKind::unknown;
    std::uint64_t size = 0;
    std::int64_t modified_unix = 0;
    std::int64_t duration_ms = 0;
    int width = 0;
    int height = 0;
    std::string container;
    std::string video_codec;
    std::string audio_codec;
    std::string artist;
    std::string album;
    std::string album_artist;  // falls back to `artist` when a file has no explicit album artist
    int track_number = 0;
    int disc_number = 1;
    std::string genre;
    int year = 0;
    std::string title;
    std::int64_t resume_position_ms = 0;
    std::int64_t resume_duration_ms = 0;
    std::int64_t last_played_unix = 0;
    bool completed = false;
    bool favorite = false;
};

} // namespace bfplayer
