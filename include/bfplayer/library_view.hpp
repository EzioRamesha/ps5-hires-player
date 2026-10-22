#pragma once

#include "bfplayer/media.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bfplayer {

enum class LibrarySortMode {
    smart = 0,
    name,
    recently_played,
    newest,
    duration,
    size,
};

[[nodiscard]] const char* library_sort_mode_name(LibrarySortMode mode) noexcept;
[[nodiscard]] const char* library_sort_mode_key(LibrarySortMode mode) noexcept;
[[nodiscard]] std::optional<LibrarySortMode> parse_library_sort_mode(
    std::string_view value) noexcept;
[[nodiscard]] LibrarySortMode next_library_sort_mode(LibrarySortMode mode) noexcept;

// Search is intentionally ASCII-case-insensitive while preserving UTF-8 bytes.
// Every whitespace-separated token must match title, filename, path, container,
// or codec metadata. This avoids depending on a heavyweight Unicode library in
// the PS5 payload while still supporting exact non-ASCII searches from the IME.
[[nodiscard]] std::string normalize_media_query(std::string_view query);
[[nodiscard]] bool media_matches_query(
    const MediaEntry& entry,
    std::string_view query);
[[nodiscard]] bool media_matches_normalized_query(
    const MediaEntry& entry,
    std::string_view normalized_query);

// Sorts a filtered index set without copying MediaEntry strings. Smart sort is
// recent-first for Continue/Recently Played and natural-name order elsewhere.
void sort_media_indices(
    const std::vector<MediaEntry>& entries,
    std::vector<std::size_t>& indices,
    LibrarySortMode mode,
    bool smart_prefers_recent);

// Groups a media entry for album browsing: by (album_artist, album) metadata when at least
// one of the album-identifying tags is present, falling back to the entry's containing folder
// when neither tag is set (so untagged music is still browsable, never hidden). The returned
// key is an opaque grouping identifier, not display text — use media_album_display_name for
// what the UI shows.
[[nodiscard]] std::string media_album_key(const MediaEntry& entry);

// The human-readable label for the group media_album_key(entry) belongs to: the album title
// when tagged, or the containing folder's name when not.
[[nodiscard]] std::string media_album_display_name(const MediaEntry& entry);

// Removes exactly one UTF-8 code point from the end of text. SDL text input is
// valid UTF-8, so malformed trailing bytes are treated as one best-effort unit.
bool erase_last_utf8_codepoint(std::string& text) noexcept;

} // namespace bfplayer
