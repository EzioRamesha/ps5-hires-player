# Phase 1: specialize the BFplayer fork into a music player

## Goal

Turn the forked BFplayer codebase (a general movie/TV/music player) into a dedicated music
player: strip video-only code, rework the library around artist/album/playlist browsing instead
of movies/shows/seasons, and confirm the real lossless/hi-res-adjacent format coverage. Audio
output stays on BFplayer's existing, proven SDL/S16 path in this phase — the direct
`sceAudioOutOpen` bit-depth work is Phase 2, deliberately not bundled here (see "Out of scope"
below for why).

## Background

`ps5-hires-player` is a fork of [BFplayer](https://github.com/ItsBlurf/BFplayer) (GPL-3.0-or-later),
a real, actively-maintained native PS5 media player (SDL2 + FFmpeg 7.0.1 + SDL_kitchensink +
libass, its own home-screen tile, no browser dependency). The fork was chosen over building from
zero because BFplayer already solves the two hardest open questions for this project: native
(non-browser) GUI rendering on jailbroken PS5, and cross-compiling a real codec library (FFmpeg)
for the "prospero" target. Full research trail: `docs/research/`.

BFplayer's own architecture (`docs/ARCHITECTURE.md`, inherited in the fork) already establishes:
one resident Prospero payload, SDL2 for rendering/input, Kitchensink for decode queues/sync,
SQLite for the library index, and a set of library-scanning safety rules (no-follow symlinks,
bounded recursion, transactional persistence, incremental re-scan by size/mtime) that are already
exactly what a "fast scanning" music player needs — this spec reuses them, not replaces them.

## Architecture

No change to BFplayer's top-level decision: one resident payload, same BigApp/ELF-loader core,
same Media-tab tile pattern. This phase changes what the payload *contains and shows*, not how
it's packaged or launched.

### What's removed

Pure video-only code with no music-player use:

- `src/core/hdr_tonemap.cpp` + `include/bfplayer/hdr_tonemap.hpp` — HDR10/PQ/HLG tone mapping
- `src/core/video_layout.cpp` + `include/bfplayer/video_layout.hpp` — aspect/crop/scale renderer
  modes
- `src/core/video_thumbnail.cpp` + `include/bfplayer/video_thumbnail.hpp`
- `src/playback/external_subtitles.cpp` + `include/bfplayer/external_subtitles.hpp`
- `src/core/subtitle_browser.cpp` + `include/bfplayer/subtitle_browser.hpp`
- `src/core/subtitle_provider.cpp` + `include/bfplayer/subtitle_provider.hpp`
- `src/playback/kitchensink_subtitle_timing.c` + `include/bfplayer/kitchensink_subtitle_timing.h`
- `src/playback/kitsubimage_safe.c` (the hardened subtitle bitmap renderer override — subtitle
  rendering goes away entirely, so its safety patch has nothing left to protect)
- The subtitle entries in `vendor/SDL_kitchensink` stay (removing a vendored dependency's internal
  files is riskier than just not calling the subtitle entry points — Kitchensink is used for audio
  decode/sync regardless) but nothing in this project's own code calls them after this phase.

### What's kept, unmodified

The library-scanning and persistence layer, because it already does exactly what this project
needs and reworking it would be pure risk for no benefit:

- `src/core/library_scanner.cpp` / `include/bfplayer/library_scanner.hpp` — incremental scan,
  safety rules, transactional persistence
- `src/core/library_database.cpp` / `include/bfplayer/library_database.hpp` — SQLite index, resume
  position, favorites, sort persistence
- `src/core/media_sources.cpp` / `include/bfplayer/media_sources.hpp` — source folder management
- `src/core/playlist.cpp` / `include/bfplayer/playlist.hpp` — M3U/PLS/XSPF loading (already
  generic, not video-specific)
- `src/core/dlna_protocol.cpp`, `src/network/dlna_client.cpp` — DLNA/UPnP discovery and browsing,
  **narrowed to audio resources** (see "DLNA" below) rather than removed
- `src/core/list_navigation.cpp`, `src/core/source_uri.cpp`, `src/core/safe_read_file.cpp`,
  `src/core/bulk_import.cpp`, `src/core/artwork.cpp`, `src/core/diagnostics.cpp` — none of these
  are video-specific; no changes needed

### What's reworked

**`include/bfplayer/media.hpp` (`MediaEntry`)** — add fields for music metadata:

```cpp
struct MediaEntry {
    // ...existing fields unchanged...
    std::string artist;
    std::string album;
    std::string album_artist;  // falls back to `artist` when a file has no explicit album artist
    int track_number = 0;
    int disc_number = 1;
    std::string genre;
    int year = 0;
};
```

These are populated from the same FFmpeg metadata probe `media_probe.cpp` already runs for
`title`/`video_codec`/`audio_codec` — this is additive to an existing probe call, not a new
probing pass. The video-only fields `series_root`/`series_title`/`explicit_movie` are removed from
`MediaEntry` in this phase (nothing references them once TV/movie grouping is gone).

**`include/bfplayer/library_view.hpp` / `src/core/library_view.cpp`** — add artist/album grouping
alongside the existing generic sort/search/query functions (`sort_media_indices`,
`media_matches_query`, etc., which are already media-kind-agnostic and need no changes):

```cpp
// Mirrors media_season_root's existing contract, but by metadata instead of folder path:
// groups by (album_artist, album) when both are present; falls back to the containing
// folder name when a file has no album tag at all (so untagged music is still browsable,
// not hidden).
[[nodiscard]] std::string media_album_key(const MediaEntry& entry);
[[nodiscard]] std::string media_album_display_name(const MediaEntry& entry);
```

**`src/ui/library_ui.cpp`** — the UI's public interface (`LibraryUi` in
`include/bfplayer/library_ui.hpp`) does not need to change shape: `open`/`close`/`handle_event`/
`tick_navigation`/`playback_queue`/`render`/`show_notice` all stay as they are, since
`LibraryAction::play_queue` already models a playback queue as a first-class action. The 5,258-line
implementation gets its internal view logic reworked from movie/show/season categories to
artist/album/playlist categories, and its rendering from poster-grid layouts to a list/grid
suited to album art + track listings. This is the single largest piece of work in this phase by
line count, but it is UI logic behind an already-small, stable public interface — nothing outside
`library_ui.cpp` needs to know its internals changed.

**`src/ui/playback_osd.cpp`** — rebuilt around now-playing (album art, artist/album/track text,
progress bar, queue position) instead of video controls (track/subtitle/aspect selection). The
windowing and input-handling plumbing is reused; the rendered content is replaced.

### DLNA

`dlna_protocol.cpp`/`dlna_client.cpp` stay, since a NAS music library or internet radio station
over DLNA is a real, useful feature for a music player, not scope creep. The one change: the
existing filter ("only audio/video items with supported stream URLs enter the visible listing" —
`docs/ARCHITECTURE.md`) narrows to audio-only resources, dropping the `|| video` branch of
whatever check currently allows both.

## Format scope

Confirmed present in the linked FFmpeg archive per BFplayer's own `docs/FORMAT_SUPPORT.md`: FLAC,
ALAC, MP2/MP3, AAC, Opus, Vorbis, DTS/DCA, TrueHD. WAV and APE (Monkey's Audio) are not explicitly
listed in that doc (it was written with a video player's testing priorities) but are very likely
already linked, since they're standard FFmpeg demuxer/decoder coverage — this must be **confirmed,
not assumed**, using the same method BFplayer's own `docs/FFMPEG_SYMBOL_AUDIT.md` already
establishes (grep the actual linked `libavformat.a`/`libavcodec.a` archive for the real symbols,
the same reproducible method already used for the video codec list).

**Correction (added after Task 1's audit landed, 2026-10-22): the DSD claim below was wrong on one
half.** The original research concluded DSD was entirely out of reach; Task 1's exhaustive symbol
audit (`docs/FORMAT_SUPPORT_AUDIO.md`) found FFmpeg's linked archive DOES carry DSD decoder
symbols (`ff_dsd_lsbf_decoder`, `ff_dsd_lsbf_planar_decoder`, `ff_dsd_msbf_decoder`,
`ff_dsd_msbf_planar_decoder`, `ff_dst_decoder`) and demuxers (`ff_dsf_demuxer`, `ff_wsd_demuxer`).
These two findings are about different layers and both stand:

- **Decoding a DSD file into PCM is possible** — FFmpeg can open and decode one like any other
  format. A `.dsf` file should be playable, same as any other confirmed format.
- **Native, bit-perfect DSD output is still not possible.** The audio-output research's actual
  finding — confirmed against real `sceAudioOutOpen` validation code in the shadPS4 emulator —
  is that the PS5's audio-output API is hard-locked to 48kHz PCM, full stop. DSD's whole value
  proposition (a 1-bit stream at megahertz rates, played without converting to PCM) cannot survive
  that conversion — a "DSD-supported" player on this platform means "can decode and downsample a
  DSD file to 48kHz PCM like any lossy source," not "can play DSD content with any of the fidelity
  DSD actually exists for."

So: DSD file playback is in scope (nothing extra needed — it's just another FFmpeg-decoded format,
covered by the same code path as everything else). Native/bit-perfect DSD output remains out of
scope, for the original, still-correct reason. The original paragraph below is kept for the
record rather than deleted, since it documents the (partially wrong) state of understanding before
Task 1's audit — do not treat it as current.

---

**DSD is out of scope, not deferred.** The research found no evidence any audio API available to a
PS5 homebrew payload (including direct `sceAudioOut` calls) could plausibly support DSD, native or
DoP. This is not "Phase 2" — it is excluded from the project's goals entirely, and should be
documented as such in the top-level README once this phase lands, so it isn't quietly assumed
later.

## Out of scope for this phase (Phase 2 and beyond)

- **The direct `sceAudioOutOpen` float-format audio-output path.** Deliberately not bundled into
  Phase 1: it is the one piece of this whole project backed by "plausible but unconfirmed on real
  hardware" research rather than a working precedent, and bundling it with everything else risks
  stalling a working, shippable Phase 1 on its riskiest, least-proven part. Phase 1 ships on
  BFplayer's existing, working SDL/S16 audio path — a real, usable music player — and Phase 2
  replaces just the output module once Phase 1 is proven. See
  `docs/research/2026-10-21-bit-perfect-audio-output.md` for the full finding and the concrete
  hardware-verification step that would convert "plausible" to "confirmed."
- Gapless playback, ReplayGain/loudness normalization, and bit-perfect/exclusive output claims —
  not promised until Phase 2's output work lands, since none of them mean anything on top of a
  downmixed S16 path.
- Any UI/feature work beyond what's described here (equalizer, visualizations, lyrics, casting) —
  not scoped into any phase yet; candidates for a future design pass once Phase 1 and 2 are real.

## Testing

Match the existing project's own established pattern, not a new one:

- Pure logic gets host-side unit tests the same way `library_scanner_tests.cpp`,
  `library_view_tests.cpp`, `playlist_tests.cpp` already do: `media_album_key`/
  `media_album_display_name` get the same kind of test coverage `media_season_root` already has
  (grouping by tag, fallback-to-folder-name when untagged, disc/track ordering).
- The metadata-extraction addition to `media_probe.cpp` gets tests the way `media_sources_tests.cpp`
  already covers that module's existing behavior.
- Anything touching real playback/hardware gets the same kind of per-build verification doc
  BFplayer already writes (`docs/ALPHA*_VERIFICATION.md` pattern) — not skipped because this is
  "just a fork."
- `bash`/CI: the existing `.github/workflows/offline-tests.yml` stays as the host-testable gate;
  no new CI infrastructure needed for this phase.

## Self-review notes

- Checked every "removed" file against the current codebase (`docs/ARCHITECTURE.md` and direct
  file listing) before listing it — nothing removed that Phase 1's kept features actually depend
  on (verified: Kitchensink's subtitle internals in `vendor/` are decode-library-internal, not
  called by anything outside the removed subtitle files).
- The `MediaEntry` field removal (`series_root`/`series_title`/`explicit_movie`) is listed
  explicitly rather than left implicit, since leaving stale video-only fields on a shared struct
  after this phase would be exactly the kind of oversight that causes confusion later.
- Resolved one ambiguity in writing this doc: whether DLNA should be removed or narrowed. Removing
  it would lose a real feature (NAS/internet-radio audio) for no reason tied to the project's
  actual goals; narrowing it to audio-only is the smaller, correct change.
