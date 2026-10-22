# Phase 1a: Backend and Data Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Specialize the forked BFplayer codebase's backend/data layer for a music player: remove
video-only code, document the real (not assumed) audio format coverage, extend `MediaEntry` and
its SQLite persistence with music metadata, and narrow DLNA to audio. This is the first of two
plans for Phase 1 — the UI rework (`library_ui.cpp`, `playback_osd.cpp`) is a separate plan,
written after those files are read in full rather than guessed at here.

**Architecture:** No structural change to BFplayer's decision (one resident payload, SDL2 +
FFmpeg/Kitchensink + SQLite). This plan only removes files, extends the shared `MediaEntry`
struct and its SQLite columns, extends the existing FFmpeg metadata probe, and adds one new pure
function pair (`media_album_key`/`media_album_display_name`) alongside the existing
`media_season_root` it replaces.

**Tech Stack:** C++17, SQLite3, FFmpeg/libavformat/libavcodec (via PacBrew v0.37), CMake, the
project's existing plain `check(condition, message)` host-test harness (no external test
framework — see `tests/library_view_tests.cpp` for the pattern).

## Global Constraints

- Full spec: `docs/specs/2026-10-21-phase1-audio-specialization-design.md` — every task here
  implements a section of it.
- **Corrected 2026-10-22, after Task 1 landed:** DSD *decoding* is actually in scope — Task 1's
  audit found FFmpeg's linked archive carries real DSD decoder/demuxer symbols, so a `.dsf` file
  decodes like any other confirmed format, no extra work needed. What's still out of scope is
  *native, bit-perfect DSD output* — the PS5 audio-output API is hard-locked to 48kHz PCM
  regardless of source format (separate finding, still correct). Don't write code assuming DSD
  files are unplayable; don't write code claiming DSD gets any special/bit-perfect treatment
  either. See the spec's own correction note for the full explanation.
- Format coverage must be **confirmed by audit**, never assumed from BFplayer's existing
  `docs/FORMAT_SUPPORT.md` (that list was written for video-testing priorities, not exhaustively).
- Every removed file's build references (CMakeLists.txt entries, test executables) must be removed
  in the same task that removes the file — a task must never leave the build broken.
- SPDX/license headers: this codebase does not currently carry per-file SPDX headers (checked:
  none of the files read in preparing this plan have one) — do not add them unless asked; match
  existing style.
- Conventional commits, one purpose per commit.

---

## Task 1: Exhaustive audio format audit

**Files:**
- Create: `docs/FORMAT_SUPPORT_AUDIO.md`

**Interfaces:**
- Produces: a documented, reproducible list of every audio-relevant decoder, demuxer, and
  protocol symbol actually linked — later tasks (and the eventual README) cite this file rather
  than re-deriving the list.

BFplayer's own `docs/FFMPEG_SYMBOL_AUDIT.md` already establishes the reproducible method: load
`ps5dev-env.ps1`, locate PacBrew beside the SDK, and enumerate **defined** symbols in the linked
static archives with LLVM `nm`, counting unique names matching `ff_*_demuxer`, `ff_*_decoder`, and
`ff_*_protocol`. That audit found **500 unique decoders** and **353 demuxers** total, but only
documented a representative subset (the ones relevant to testing video playback). This task reruns
the same method, filtered to produce the complete audio-relevant list, not a subset.

- [ ] **Step 1: Reproduce the base audit.** Following `docs/FFMPEG_SYMBOL_AUDIT.md`'s own
  "Reproduction" section exactly (same toolchain paths, same `nm` + suffix-matching approach),
  regenerate the full `ff_*_decoder` and `ff_*_demuxer` symbol lists from:
  ```text
  <toolchains>/pacbrew-v0.37/homebrew/lib/libavformat.a
  <toolchains>/pacbrew-v0.37/homebrew/lib/libavcodec.a
  ```

- [ ] **Step 2: Filter for audio-relevant symbols.** From the full decoder list, keep every
  symbol whose name corresponds to an audio codec (not video/subtitle) — cross-reference each
  `ff_*_decoder` symbol name against FFmpeg's own public codec ID naming (the decoder symbol name
  directly embeds the codec, e.g. `ff_flac_decoder`, `ff_wavpack_decoder`, `ff_ape_decoder`,
  `ff_tta_decoder`, `ff_wmalossless_decoder` — match by name, this does not require running any
  code, just reading the symbol list). Record every one found, not just the ones BFplayer already
  named. Explicitly search for (confirm present or absent, do not skip any): FLAC, ALAC, WAV/PCM
  variants, WavPack, Monkey's Audio (APE), Musepack (MPC), TTA (True Audio), Shorten, OptimFROG,
  DSD (expected absent — confirm this explicitly, it is the one format this audit should show is
  NOT there), MP3, AAC, Opus, Vorbis, DTS, TrueHD, AC-3/E-AC-3, WMA/WMA Lossless.

- [ ] **Step 3: Filter demuxers the same way** for audio-relevant containers: WAV, FLAC (native),
  Ogg, Matroska/WebM (audio-only), MP4/M4A, APE, WavPack, CAF, AIFF, and any others found.

- [ ] **Step 4: Write `docs/FORMAT_SUPPORT_AUDIO.md`** with: a table of every confirmed audio
  decoder symbol found (format name, symbol name), a table of every confirmed audio-relevant
  demuxer, an explicit "confirmed absent" line for DSD (and anything else searched for and not
  found), and a "Reproduction" section identical in structure to the one in
  `docs/FFMPEG_SYMBOL_AUDIT.md` so this stays independently reproducible, not a one-time claim.

- [ ] **Step 5: Commit.**
  ```bash
  git add docs/FORMAT_SUPPORT_AUDIO.md
  git commit -m "docs: exhaustive audio format audit (500-decoder archive, not a subset)"
  ```

---

## Task 2: Remove video-only code

**Files:**
- Delete: `src/core/hdr_tonemap.cpp`, `include/bfplayer/hdr_tonemap.hpp`
- Delete: `src/core/video_layout.cpp`, `include/bfplayer/video_layout.hpp`
- Delete: `src/core/video_thumbnail.cpp`, `include/bfplayer/video_thumbnail.hpp`
- Delete: `src/playback/external_subtitles.cpp`, `include/bfplayer/external_subtitles.hpp`
- Delete: `src/core/subtitle_browser.cpp`, `include/bfplayer/subtitle_browser.hpp`
- Delete: `src/core/subtitle_provider.cpp`, `include/bfplayer/subtitle_provider.hpp`
- Delete: `src/playback/kitchensink_subtitle_timing.c`, `include/bfplayer/kitchensink_subtitle_timing.h`
- Delete: `src/playback/kitsubimage_safe.c`
- Delete: `tests/hdr_tonemap_tests.cpp`, `tests/video_layout_tests.cpp`,
  `tests/subtitle_provider_tests.cpp`, `tests/subtitle_browser_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:** None produced — this is pure removal. Other tasks in this plan do not depend on
anything these files exported.

- [ ] **Step 1: Find every caller of the code being removed, before deleting anything.** Run, from
  the repo root:
  ```bash
  grep -rln "hdr_tonemap\|video_layout\|video_thumbnail\|external_subtitles\|subtitle_browser\|subtitle_provider\|kitchensink_subtitle_timing\|kitsubimage_safe" src/ include/ --include="*.cpp" --include="*.hpp" --include="*.c" --include="*.h" | grep -v -E "hdr_tonemap\.|video_layout\.|video_thumbnail\.|external_subtitles\.|subtitle_browser\.|subtitle_provider\.|kitchensink_subtitle_timing\.|kitsubimage_safe\."
  ```
  This finds files OTHER than the ones being deleted that reference them (expected hits:
  `src/ui/library_ui.cpp`, `src/ui/playback_osd.cpp`, possibly `src/main.cpp`). Record this list —
  it is NOT this task's job to fix those call sites (that is the UI rework plan's job), but this
  task must know about them so Step 4's "does it still build" check has an honest expected
  outcome: **the main player target will NOT build cleanly after this task alone**, because
  `library_ui.cpp`/`playback_osd.cpp` still call the removed functions until the UI rework plan
  updates them. That is expected and acceptable for this task — see Step 4.

- [ ] **Step 2: Delete the eight source/header pairs and four test files** listed above.

- [ ] **Step 3: Update `CMakeLists.txt`.** Remove every line referencing a deleted file — the
  source-list entries (confirmed present at lines 13, 23-25 as of this plan's writing; re-check
  with `grep -n "hdr_tonemap\|subtitle_browser\|subtitle_provider\|video_layout" CMakeLists.txt`
  since line numbers shift) and the four corresponding test-executable blocks (confirmed present
  around lines 56-67 for `video_layout_tests`/`hdr_tonemap_tests` and 89-95 for
  `subtitle_provider_tests`/`subtitle_browser_tests` — each block is an `add_executable`/
  `target_link_libraries`/`add_test` trio, remove the whole trio for each).

- [ ] **Step 4: Verify the EXPECTED build state.** Run the project's existing build
  (`build.ps1 -Configuration Release` per `README.md`'s "Building" section, or the equivalent
  direct CMake invocation if running outside PowerShell). Expect the `bfplayer_core` library
  target and the test executables for files NOT touched by this task to build and pass. Expect the
  main player executable target to FAIL to link, with undefined-reference errors naming the
  removed functions, referenced from `library_ui.cpp`/`playback_osd.cpp` (and `main.cpp` if Step 1
  found it there). This confirms Step 1's finding was complete (if the link fails with DIFFERENT
  undefined symbols than Step 1 predicted, Step 1 missed a caller — go back and find it) and
  records the exact, real list of call sites the UI rework plan must address — write that exact
  compiler output into this task's completion notes for the next plan to start from.

- [ ] **Step 5: Commit** — the deletion itself, with the expected-broken-link state documented in
  the commit message so it's clear this is intentional, not a mistake:
  ```bash
  git add -A
  git commit -m "refactor: remove video-only playback code (HDR, layout, thumbnails, subtitles)

  The main player target does not link after this commit — library_ui.cpp
  and playback_osd.cpp still call the removed functions. This is expected;
  fixing those call sites is the UI rework plan's job, not this one's."
  ```

---

## Task 3: `MediaEntry` schema change

**Files:**
- Modify: `include/bfplayer/media.hpp`
- Modify: `src/core/library_database.cpp`
- Test: `tests/library_database_tests.cpp`

**Interfaces:**
- Produces: `MediaEntry` gains `artist`, `album`, `album_artist`, `track_number`, `disc_number`,
  `genre`, `year` (types below); loses `series_root`, `series_title`, `explicit_movie`. Task 4
  (metadata probing) and Task 5 (album grouping) depend on these exact field names and types.

- [ ] **Step 1: Write the failing test.** `library_database_tests.cpp` already has tests that
  round-trip a `MediaEntry` through the database (insert, read back, compare fields — follow the
  existing pattern in that file for a round-trip test, same `check(condition, message)` style used
  throughout this project's tests). Add one asserting the new fields survive a round trip:
  ```cpp
  MediaEntry audio_entry{
      "/music/Artist/Album/01 Track.flac",
      "01 Track.flac",
      MediaKind::audio};
  audio_entry.artist = "Test Artist";
  audio_entry.album = "Test Album";
  audio_entry.album_artist = "Test Artist";
  audio_entry.track_number = 1;
  audio_entry.disc_number = 1;
  audio_entry.genre = "Test Genre";
  audio_entry.year = 2026;
  // ...insert via the database's existing upsert function, read it back, then:
  check(read_back.artist == "Test Artist", "artist round-trips through the database");
  check(read_back.album == "Test Album", "album round-trips through the database");
  check(read_back.album_artist == "Test Artist", "album_artist round-trips through the database");
  check(read_back.track_number == 1, "track_number round-trips through the database");
  check(read_back.disc_number == 1, "disc_number round-trips through the database");
  check(read_back.genre == "Test Genre", "genre round-trips through the database");
  check(read_back.year == 2026, "year round-trips through the database");
  ```
  (Use the exact insert/read function names already present in `library_database_tests.cpp` —
  read that file first to match its existing round-trip test's calling convention precisely.)

- [ ] **Step 2: Run to verify it fails.** Build and run `library_database_tests` — expect a
  compile error, since `artist`/`album`/etc. do not exist on `MediaEntry` yet.

- [ ] **Step 3: Update `include/bfplayer/media.hpp`.** Remove `series_root`, `series_title`,
  `explicit_movie`. Add, placed after the existing `audio_codec` field to keep related fields
  together:
  ```cpp
  std::string artist;
  std::string album;
  std::string album_artist;  // falls back to `artist` when a file has no explicit album artist
  int track_number = 0;
  int disc_number = 1;
  std::string genre;
  int year = 0;
  ```

- [ ] **Step 4: Update `src/core/library_database.cpp`'s schema.** Add new columns to the `media`
  table using the EXACT existing additive-migration pattern already used for `resume.completed`
  and `resume.last_played_unix` (find the `kMigrations` array — a list of
  `{table, column, declaration}` entries fed through an `ensure_column(table, column, declaration)`
  helper — and add seven entries to it, one per new column):
  ```cpp
  {"media", "artist", "artist TEXT NOT NULL DEFAULT ''"},
  {"media", "album", "album TEXT NOT NULL DEFAULT ''"},
  {"media", "album_artist", "album_artist TEXT NOT NULL DEFAULT ''"},
  {"media", "track_number", "track_number INTEGER NOT NULL DEFAULT 0"},
  {"media", "disc_number", "disc_number INTEGER NOT NULL DEFAULT 1"},
  {"media", "genre", "genre TEXT NOT NULL DEFAULT ''"},
  {"media", "year", "year INTEGER NOT NULL DEFAULT 0"},
  ```
  This is additive and backward-compatible (existing rows get the defaults), consistent with how
  the existing migrations work — no `PRAGMA user_version` bump is needed since `ensure_column` is
  itself idempotent and safe to run against an already-migrated database (confirm this by reading
  `ensure_column`'s own implementation before relying on it; if it is NOT idempotent, this step's
  approach needs to change to use the versioned-migration path instead — check this explicitly,
  do not assume).

- [ ] **Step 5: Update every SQL statement that reads/writes the `media` table** (the
  insert/upsert function and the row-reading function that populates a `MediaEntry` from a query
  result) to include the seven new columns and drop the three removed ones. Find these via
  `grep -n "series_root\|series_title\|explicit_movie" src/core/library_database.cpp` — fix every
  hit.

- [ ] **Step 6: Run to verify the test passes.**

- [ ] **Step 7: Find and fix every OTHER reference to the removed fields** outside this file:
  ```bash
  grep -rln "series_root\|series_title\|explicit_movie" src/ include/ tests/
  ```
  Expect hits in `src/core/library_view.cpp`/`include/bfplayer/library_view.hpp` (the
  `media_season_root` function and its test — Task 5 removes these properly; for THIS task, if the
  build does not compile without touching them, that is a sign Task 5 must happen before this task
  is considered complete — reorder if needed, or leave a clear note in this task's completion
  record for Task 5 to pick up) and possibly `src/ui/library_ui.cpp` (expected, per Task 2's
  finding — not this task's job to fix, same reasoning as Task 2 Step 4).

- [ ] **Step 8: Commit.**
  ```bash
  git add include/bfplayer/media.hpp src/core/library_database.cpp tests/library_database_tests.cpp
  git commit -m "feat: add music metadata fields to MediaEntry and its SQLite schema

  Removes series_root/series_title/explicit_movie (video-only, no longer
  needed) and adds artist/album/album_artist/track_number/disc_number/
  genre/year via the project's existing additive ensure_column migration
  pattern."
  ```

---

## Task 4: Populate music metadata from FFmpeg

**Files:**
- Modify: `src/core/media_probe.cpp`
- Test: new test file `tests/media_probe_tests.cpp` (none currently exists for this module)
- Modify: `CMakeLists.txt` (register the new test)

**Interfaces:**
- Consumes: `MediaEntry`'s new fields from Task 3.
- Produces: nothing new consumed by later tasks — this is a leaf change.

`probe_media_metadata` already reads `title` via
`av_dict_get(format->metadata, "title", nullptr, 0)` followed by
`entry.title = bounded_text(title->value);` (confirmed at `src/core/media_probe.cpp:197-200`).
`bounded_text(const char*, std::size_t maximum = 512)` is the project's existing string-length
guard, already used for every other metadata string field — reuse it for the new fields too, for
the same reason (an untrusted file's tags must not be allowed to blow past a sane length into the
database).

FFmpeg's `AVDictionary` metadata keys for these fields are its own standard, stable key names
(used identically by every FFmpeg-based tool, not something unique to this probe): `"artist"`,
`"album"`, `"album_artist"`, `"track"`, `"disc"`, `"genre"`, `"date"`.

- [ ] **Step 1: Write a test file for the probe.** No test currently exists for
  `media_probe.cpp` — create `tests/media_probe_tests.cpp` following the exact structure of
  `tests/media_sources_tests.cpp` (same `#include`s, same `int failures = 0; void check(...)`
  harness, same `int main()` shape printing `media_probe_tests: PASS`/`FAIL` and returning
  `failures`). The test needs one real small audio file with known tags to probe — check whether
  `tests/media-src/` already has one (it currently holds only subtitle sample files per the file
  listing seen while preparing this plan: `sample.en.srt`, `sample.es.srt`, `sample.ffmetadata`,
  `sample.styled.ass` — note `sample.ffmetadata` may already be exactly the kind of fixture
  needed; read it first before creating a new one, since FFmpeg's `.ffmetadata` format can tag an
  existing or newly-generated minimal audio file without needing a licensed/binary sample
  committed to the repo). If no suitable audio fixture exists, generate a tiny tagged test WAV at
  test-run time (not committed) using the project's already-linked FFmpeg, following whatever
  pattern `tests/media-src/` establishes for other generated-vs-committed fixtures.
  ```cpp
  #include "bfplayer/media.hpp"
  #include "bfplayer/media_probe.hpp"
  #include <iostream>

  int failures = 0;
  void check(bool condition, const char* message) {
      if (!condition) {
          std::cerr << "FAIL: " << message << "\n";
          ++failures;
      }
  }

  int main() {
      bfplayer::MediaEntry entry{
          /* path */ "tests/media-src/sample-tagged.flac",
          /* name */ "sample-tagged.flac",
          bfplayer::MediaKind::audio};
      std::string error;
      bool ok = bfplayer::probe_media_metadata(entry, error);
      check(ok, "probe succeeds on a tagged sample file");
      check(entry.artist == "Test Artist", "artist tag is extracted");
      check(entry.album == "Test Album", "album tag is extracted");
      check(entry.track_number == 3, "track number tag is extracted and parsed as an integer");
      check(entry.year == 2026, "year is extracted from the date tag");

      if (failures == 0) {
          std::cout << "media_probe_tests: PASS\n";
      }
      return failures;
  }
  ```

- [ ] **Step 2: Register the test in `CMakeLists.txt`**, following the exact
  `add_executable`/`target_link_libraries`/`add_test` trio pattern already used for every other
  test in that file.

- [ ] **Step 3: Run to verify it fails** (missing fixture and/or missing extraction code).

- [ ] **Step 4: Extend `probe_media_metadata` in `src/core/media_probe.cpp`**, immediately after
  the existing title-extraction block (after line 200 as of this plan's writing):
  ```cpp
  if (const AVDictionaryEntry* artist =
          av_dict_get(format->metadata, "artist", nullptr, 0)) {
      entry.artist = bounded_text(artist->value);
  }
  if (const AVDictionaryEntry* album =
          av_dict_get(format->metadata, "album", nullptr, 0)) {
      entry.album = bounded_text(album->value);
  }
  if (const AVDictionaryEntry* album_artist =
          av_dict_get(format->metadata, "album_artist", nullptr, 0)) {
      entry.album_artist = bounded_text(album_artist->value);
  } else {
      entry.album_artist = entry.artist;  // fall back per the spec's documented contract
  }
  if (const AVDictionaryEntry* genre =
          av_dict_get(format->metadata, "genre", nullptr, 0)) {
      entry.genre = bounded_text(genre->value);
  }
  if (const AVDictionaryEntry* track =
          av_dict_get(format->metadata, "track", nullptr, 0)) {
      // FFmpeg's "track" tag is often "N" or "N/total" — parse the leading integer only,
      // and leave track_number at its 0 default on anything unparseable rather than guessing.
      entry.track_number = std::atoi(track->value);
  }
  if (const AVDictionaryEntry* disc =
          av_dict_get(format->metadata, "disc", nullptr, 0)) {
      int parsed = std::atoi(disc->value);
      entry.disc_number = parsed > 0 ? parsed : 1;  // keep the struct's documented default of 1
  }
  if (const AVDictionaryEntry* date =
          av_dict_get(format->metadata, "date", nullptr, 0)) {
      // FFmpeg's "date" tag is often a bare year or an ISO date; take the leading 4 digits.
      int parsed_year = std::atoi(date->value);
      if (parsed_year > 1000) {  // reject obvious non-year garbage without being falsely strict
          entry.year = parsed_year;
      }
  }
  ```

- [ ] **Step 5: Run to verify the test passes.**

- [ ] **Step 6: Commit.**
  ```bash
  git add src/core/media_probe.cpp tests/media_probe_tests.cpp CMakeLists.txt
  git commit -m "feat: extract artist/album/track/disc/genre/year from FFmpeg metadata"
  ```

---

## Task 5: Album grouping, replacing season grouping

**Files:**
- Modify: `include/bfplayer/library_view.hpp`, `src/core/library_view.cpp`
- Modify: `tests/library_view_tests.cpp`

**Interfaces:**
- Consumes: `MediaEntry::artist`/`album`/`album_artist` from Task 3.
- Produces: `media_album_key(const MediaEntry&)` and `media_album_display_name(const MediaEntry&)`
  — the UI rework plan groups and labels albums using these two functions, by name.

`media_season_root(const MediaEntry& entry, std::string_view series_root)` is being removed (it
only makes sense for the video/TV grouping this project no longer has); its replacement groups by
metadata instead of folder path, for the reason given in the spec: music files are reliably
tagged, folder conventions are not.

- [ ] **Step 1: Write the failing tests.** Replace the existing `media_season_root` test block in
  `tests/library_view_tests.cpp` (the block shown in this plan's preparation, asserting nested/
  direct/sibling/Windows-path season grouping) with:
  ```cpp
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
  ```
  (The `\x1F` unit-separator byte is a deliberate, documented choice to join artist+album into one
  key string without an ambiguous delimiter a real tag could contain — e.g. a literal `/` or `-`
  in an artist name must not accidentally collide with another artist+album pair.)

- [ ] **Step 2: Run to verify it fails** (compile error — the old `media_season_root` is gone from
  the test but the new functions do not exist yet).

- [ ] **Step 3: Remove `media_season_root` from `include/bfplayer/library_view.hpp`** and its
  implementation from `src/core/library_view.cpp`.

- [ ] **Step 4: Add the new declarations to `include/bfplayer/library_view.hpp`**, placed where
  `media_season_root`'s declaration was:
  ```cpp
  // Groups a media entry for album browsing: by (album_artist, album) metadata when at least
  // one of the album-identifying tags is present, falling back to the entry's containing folder
  // when neither tag is set (so untagged music is still browsable, never hidden). The returned
  // key is an opaque grouping identifier, not display text — use media_album_display_name for
  // what the UI shows.
  [[nodiscard]] std::string media_album_key(const MediaEntry& entry);

  // The human-readable label for the group media_album_key(entry) belongs to: the album title
  // when tagged, or the containing folder's name when not.
  [[nodiscard]] std::string media_album_display_name(const MediaEntry& entry);
  ```

- [ ] **Step 5: Implement both in `src/core/library_view.cpp`.** Place them near the removed
  `media_season_root`'s old location. Use this exact logic (matching the test cases precisely):
  ```cpp
  namespace {
  std::string containing_folder(const std::string& path) {
      const std::size_t slash = path.find_last_of("/\\");
      return slash == std::string::npos ? std::string{} : path.substr(0, slash);
  }

  std::string folder_name(const std::string& path) {
      const std::string folder = containing_folder(path);
      const std::size_t slash = folder.find_last_of("/\\");
      return slash == std::string::npos ? folder : folder.substr(slash + 1);
  }
  } // namespace

  std::string media_album_key(const MediaEntry& entry) {
      if (!entry.album_artist.empty() || !entry.album.empty()) {
          return entry.album_artist + '\x1F' + entry.album;
      }
      return containing_folder(entry.path);
  }

  std::string media_album_display_name(const MediaEntry& entry) {
      if (!entry.album.empty()) {
          return entry.album;
      }
      return folder_name(entry.path);
  }
  ```

- [ ] **Step 6: Run to verify the tests pass.**

- [ ] **Step 7: Commit.**
  ```bash
  git add include/bfplayer/library_view.hpp src/core/library_view.cpp tests/library_view_tests.cpp
  git commit -m "feat: group the library by album metadata instead of TV-season folder paths"
  ```

---

## Task 6: Narrow DLNA browsing to audio

**Files:**
- Modify: `src/network/dlna_client.cpp`

**Interfaces:** None produced — this is a filtering-logic change only.

- [ ] **Step 1: Find the exact resource-type filter.** `is_supported_stream_uri` is referenced at
  `src/network/dlna_client.cpp:831`, `867`, and `883` (confirmed while preparing this plan) but
  that function name concerns URI/protocol support, not audio-vs-video media kind — it is NOT
  necessarily the kind filter itself. Before changing anything, run:
  ```bash
  grep -n "MediaKind::video\|MediaKind::audio\|\"audio\"\|\"video\"\|object\.class\|upnp:class" src/network/dlna_client.cpp src/core/dlna_protocol.cpp
  ```
  to find where a DLNA `ContentDirectory` result's UPnP class or resolved media kind is actually
  checked before an item is added to the visible listing. DLNA items advertise their type via the
  standard `upnp:class` field (e.g. `object.item.audioItem`, `object.item.videoItem`) — the real
  check is most likely a string comparison or prefix match against that field, or a check against
  a `MediaKind` value this project already resolves from it.

- [ ] **Step 2: Narrow the filter to audio only.** Once the real check is found (from Step 1),
  change it so only audio-classified items (or items resolved to `MediaKind::audio`) enter the
  visible listing — matching the exact pattern already used elsewhere in this codebase for
  kind-based filtering (do not invent a new filtering idiom; reuse whatever comparison style the
  surrounding code already uses, since Step 1's grep will show it).

- [ ] **Step 3: Update the architecture doc's own description of this behavior.** The inherited
  `docs/ARCHITECTURE.md` currently says "plays compatible audio or video resources through
  FFmpeg" — update this one line to say "audio resources" now that video is excluded, so the doc
  does not silently drift from what the code does.

- [ ] **Step 4: Build and manually sanity-check** against the project's existing DLNA integration
  test (`tests/dlna_client_integration_tests.cpp` — check whether it already covers a
  mixed-audio/video listing; if so, verify it still passes with the narrower filter, which it
  should if Step 2's change is correct; if the existing test asserts video items ARE listed, that
  assertion must be updated to reflect the new, intentional behavior, not left as a now-false
  expectation).

- [ ] **Step 5: Commit.**
  ```bash
  git add src/network/dlna_client.cpp docs/ARCHITECTURE.md
  git commit -m "feat: narrow DLNA/UPnP browsing to audio resources"
  ```

---

## Self-Review

**Spec coverage:** every Phase 1a backend item from
`docs/specs/2026-10-21-phase1-audio-specialization-design.md` has a task — format audit (Task 1),
video code removal (Task 2), `MediaEntry`/SQLite schema (Task 3), metadata probing (Task 4), album
grouping replacing season grouping (Task 5), DLNA narrowing (Task 6). The UI rework
(`library_ui.cpp`, `playback_osd.cpp`) is explicitly out of this plan's scope — see the Goal
section — and needs its own plan written after those files are read in full.

**Placeholder scan:** no "TBD"/"add appropriate handling"-style steps — every code step has real,
grounded code; every investigation step (Task 2 Step 1, Task 6 Step 1) gives an exact command and
an exact, real reason the output isn't already known, not a vague "look into it."

**Type consistency:** `MediaEntry`'s new fields (Task 3) are used identically by name and type in
Task 4 (probe) and Task 5 (grouping) — `artist`/`album`/`album_artist` as `std::string`,
`track_number`/`disc_number`/`year` as `int`. `media_album_key`/`media_album_display_name`'s
signatures in Task 5 match what a future UI-rework plan would need to call them by (one
`MediaEntry` argument, `std::string` return) — confirmed consistent across both tasks that touch
them.

**Known handoff risk, stated explicitly rather than hidden:** Task 2 intentionally leaves the main
player target unable to link, and Task 3 Step 7 flags that `library_view.cpp`'s old season-grouping
code may need Task 5 to land first for a clean build. Whoever executes this plan task-by-task
should run Tasks 2 and 3 back to back with Task 5 before expecting anything to link — the plan
does not hide this, it documents the exact expected broken-build state at each point so it is never
mistaken for an error.
