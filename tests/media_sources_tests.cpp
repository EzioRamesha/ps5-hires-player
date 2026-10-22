#include "bfplayer/media_sources.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace bfplayer;
    const std::vector<MediaSource> sources{
        {MediaSourceKind::tv_folder, "/mnt/usb0/One Pace/", "One Pace"},
        {MediaSourceKind::movie_file, "/mnt/usb0/Movie.mkv", "Movie"},
    };
    const std::string encoded = serialize_media_sources(sources);
    const std::vector<MediaSource> decoded = parse_media_sources(encoded);
    check(decoded.size() == 2, "source round trip count");
    check(decoded[0].path == "/mnt/usb0/One Pace" &&
              decoded[0].kind == MediaSourceKind::tv_folder,
          "TV source normalization");
    check(decoded[1].title == "Movie", "movie source title");

    // annotate_media_sources used to tag entries with series_root/series_title/
    // explicit_movie, which task 3 removed from MediaEntry (video-only fields
    // with no music-player equivalent); it is now a no-op pending task 5's
    // decision on whether the TV-folder source model still has a purpose.
    // The checks below only exercise the parts of this file unrelated to that
    // annotation: serialization, coverage checks, and retention.
    std::vector<MediaEntry> entries{
        {"/mnt/usb0/One Pace/Episode 1.mkv", "Episode 1.mkv", MediaKind::video},
        {"/mnt/usb0/Movie.mkv", "Movie.mkv", MediaKind::video},
    };
    std::vector<MediaEntry> sibling{
        {"/mnt/usb0/One Pace Extra/Episode.mkv",
         "Episode.mkv",
         MediaKind::video},
    };
    check(!media_entry_is_covered_by_sources(sibling[0], decoded),
          "unconfigured sibling is not covered");
    sibling.push_back(entries[0]);
    retain_configured_media(sibling, decoded);
    check(sibling.size() == 1 &&
              sibling[0].path == "/mnt/usb0/One Pace/Episode 1.mkv",
          "manual source mode removes legacy auto-indexed rows");

    check(media_source_default_title("/mnt/usb0/My_Show") == "My Show",
          "default source title");
    return 0;
}
