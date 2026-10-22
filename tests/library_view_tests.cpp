#include "bfplayer/library_view.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using bfplayer::LibrarySortMode;
    using bfplayer::MediaEntry;
    using bfplayer::MediaKind;

    MediaEntry episode_10{"/media/Season 1/Episode 10.mkv", "Episode 10.mkv", MediaKind::video};
    episode_10.title = "The Return";
    episode_10.container = "matroska,webm";
    episode_10.video_codec = "h264";
    episode_10.audio_codec = "aac";
    episode_10.modified_unix = 100;
    episode_10.duration_ms = 3600000;
    episode_10.size = 1000;
    episode_10.last_played_unix = 50;

    MediaEntry episode_2{"/media/Season 1/Episode 2.mkv", "Episode 2.mkv", MediaKind::video};
    episode_2.modified_unix = 200;
    episode_2.duration_ms = 1800000;
    episode_2.size = 2000;
    episode_2.last_played_unix = 75;

    check(bfplayer::media_matches_query(episode_10, "RETURN h264"),
          "query matches title and codec case-insensitively");
    check(bfplayer::media_matches_query(episode_10, "season   aac"),
          "query whitespace collapses and tokens match separate fields");
    check(!bfplayer::media_matches_query(episode_10, "return hevc"),
          "every query token is required");
    episode_10.title = "Am\xC3\xA9lie";
    check(bfplayer::media_matches_query(episode_10, "am\xC3\xA9lie"),
          "UTF-8 bytes match while ASCII letters remain case-insensitive");
    episode_10.title = "The Return";
    check(bfplayer::normalize_media_query("  MOVIE\t  Night  ") == "movie night",
          "query normalization trims and collapses ASCII whitespace");

    const std::vector<MediaEntry> entries{episode_10, episode_2};
    std::vector<std::size_t> indices{0, 1};
    bfplayer::sort_media_indices(entries, indices, LibrarySortMode::name, false);
    check(indices == std::vector<std::size_t>({1, 0}), "natural display-name sort");

    indices = {0, 1};
    bfplayer::sort_media_indices(entries, indices, LibrarySortMode::smart, true);
    check(indices == std::vector<std::size_t>({1, 0}), "smart recent-first sort");

    indices = {0, 1};
    bfplayer::sort_media_indices(entries, indices, LibrarySortMode::duration, false);
    check(indices == std::vector<std::size_t>({0, 1}), "duration descending sort");

    indices = {0, 1};
    bfplayer::sort_media_indices(entries, indices, LibrarySortMode::newest, false);
    check(indices == std::vector<std::size_t>({1, 0}), "modified-time descending sort");

    check(bfplayer::parse_library_sort_mode("size") == LibrarySortMode::size,
          "sort key parses");
    check(!bfplayer::parse_library_sort_mode("invalid").has_value(),
          "invalid sort key rejected");
    check(bfplayer::next_library_sort_mode(LibrarySortMode::size) == LibrarySortMode::smart,
          "sort cycle wraps");

    MediaEntry tagged{
        "/music/Flac/01 Track.flac",
        "01 Track.flac",
        MediaKind::audio};
    tagged.album_artist = "Pink Floyd";
    tagged.album = "The Dark Side of the Moon";
    check(
        bfplayer::media_album_key(tagged) == "Pink Floyd\x1F""The Dark Side of the Moon",
        "album key combines album artist and album, separated by a byte that cannot appear in tags");
    check(
        bfplayer::media_album_display_name(tagged) == "The Dark Side of the Moon",
        "display name shows the album title");

    MediaEntry untagged{
        "/music/Ripped/Some Folder/track07.mp3",
        "track07.mp3",
        MediaKind::audio};
    // album_artist/album left at their default empty strings
    check(
        bfplayer::media_album_key(untagged) == "/music/Ripped/Some Folder",
        "an untagged file falls back to grouping by its containing folder");
    check(
        bfplayer::media_album_display_name(untagged) == "Some Folder",
        "an untagged file's display name falls back to the folder name");

    MediaEntry partially_tagged{
        "/music/Weird/05 Mystery.flac",
        "05 Mystery.flac",
        MediaKind::audio};
    partially_tagged.album = "Compilation";
    // album_artist left empty on purpose
    check(
        bfplayer::media_album_key(partially_tagged) == "\x1F""Compilation",
        "an album tag with no album-artist tag still groups by album alone, not by folder");

    MediaEntry artist_only_a{
        "/music/Live/ArtistX/2019-05-01/track01.flac",
        "track01.flac",
        MediaKind::audio};
    artist_only_a.album_artist = "ArtistX";
    // album left empty on purpose — no album tag at all

    MediaEntry artist_only_b{
        "/music/Live/ArtistX/2021-11-03/track01.flac",
        "track01.flac",
        MediaKind::audio};
    artist_only_b.album_artist = "ArtistX";
    // album left empty on purpose — same artist, different folder/recording

    check(
        bfplayer::media_album_key(artist_only_a) != bfplayer::media_album_key(artist_only_b),
        "an artist tag with no album tag groups by folder, not by artist alone, so two different "
        "untagged recordings by the same artist are not incorrectly merged into one album group");
    check(
        bfplayer::media_album_key(artist_only_a) == "/music/Live/ArtistX/2019-05-01",
        "the artist-only fallback key is the containing folder, matching the display name's own fallback");

    std::string utf8 = "caf\xC3\xA9";
    check(bfplayer::erase_last_utf8_codepoint(utf8) && utf8 == "caf",
          "UTF-8 backspace removes one complete code point");
    check(bfplayer::erase_last_utf8_codepoint(utf8) && utf8 == "ca",
          "ASCII backspace removes one byte");

    if (failures == 0) {
        std::cout << "library_view_tests: PASS\n";
    }
    return failures == 0 ? 0 : 1;
}
