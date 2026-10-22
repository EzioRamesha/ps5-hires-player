# Phase 1 follow-ups (backlog, not yet planned)

Items left over after Plan 1a (backend/data layer) and Plan 1b (UI rework) both closed with
clean final reviews. Recorded here because the plans' working ledgers are deleted on close.
None of these block using the player; they are the input for the next planning pass.

## Cleanup commit candidates (pure deletions, enumerated by the final review)

- Orphaned subtitle settings: `kSettingAutoSubtitles`/`kSettingSubdlApiKey`/`kSettingSubtitleLanguages`
  keys and the three matching fields in `include/bfplayer/player_settings.hpp`, plus
  `normalize_subtitle_languages` in `src/core/player_settings.cpp` and its tests in
  `tests/player_settings_tests.cpp`. Nothing reads or writes any of them anymore.
- `annotate_media_sources` no-op in `src/core/media_sources.cpp` and its 8 call sites in
  `src/ui/library_ui.cpp` (decision recorded in the code comments at the close of Plan 1b).
  Keep `MediaSourceKind::tv_folder`/`movie_file` — they encode directory-vs-file semantics that
  `media_entry_is_covered_by_sources` depends on — but rename them to folder/file terms.
- `PlaybackOsd::show_panel`'s unused 4th `enabled` parameter and the unreachable disabled-row
  rendering in `src/ui/playback_osd.cpp`.
- `cycle_stream`'s dead `allow_off` parameter and the unreachable `KIT_STREAMTYPE_SUBTITLE`
  branches in `src/main.cpp` (including the `cycle_subtitle` remote-command no-op and dead
  `remote_status.subtitle_stream` telemetry).
- `VideoPresentLayout::crop_source` is always false; the crop branch in the render copy is dead.
- Four stale rationale strings in `tests/standalone_launcher_tests.js` describing removed features.
- The in-playback controls panel row "Triangle — Change video track" (accurate but useless for
  music; drop the row or the binding).

## Functional follow-ups

- Leaf track sort inside an album ignores `disc_number` — multi-disc albums interleave. One extra
  comparator term in `src/ui/library_ui.cpp`'s album-leaf sort.
- Move the pure grouping/display helpers (`artist_group_key`, `library_item_display_name`, the
  leaf track comparator) out of `src/ui/library_ui.cpp` into `src/core/library_view.cpp` with
  host tests, matching where `media_album_key`/`media_album_display_name` already live.
- The add-source browser still speaks movie/TV terms ("add file as movie", "add folder as TV
  show") — retheme to music terms together with the `MediaSourceKind` rename.
- Album-count labels don't gate on audio kind (a video file in an album folder counts as a
  "TRACK" in the row label); "SORT: TRACK ORDER" shows one level too early (artist's album list);
  group counts ignore an active search query. All cosmetic.
- OSD transport-strip height (170) is duplicated as a magic number in `src/main.cpp` and
  `src/ui/playback_osd.cpp` — hoist a constant into `playback_osd.hpp`.
- Now-playing art centers in the strip-reduced region even while the strip is auto-hidden
  (its dominant state) — tune against a real on-console screenshot; `PlaybackOsd::visible()` is
  already public if per-frame region choice is wanted.

## Docs and infrastructure

- `docs/ARCHITECTURE.md` still documents the removed `--subtitle` CLI argument and persisted
  subtitle stream indices; `COMPLETION_MATRIX.md` and `PRODUCT_UX_AUDIT.md` still describe the
  video player. Needs a sweep.
- CI (`.github/workflows/offline-tests.yml`) silently never builds `media_probe_tests` (missing
  FFmpeg dev packages) or `library_database_tests` (missing the SQLite amalgamation fetch), and
  neither CMake gate logs its skip. Needs a human decision on installing the deps in CI.
- `build.ps1` still compiles Kitchensink's subtitle renderers and links libass with no reachable
  caller — needs a real cross-build to verify removal safely.
- No PS5 cross-build is possible from this repo checkout alone: `build.ps1` expects a
  `ps5dev-env.ps1` two directories above the repo, a pacbrew toolchain copy, and a prebuilt
  SDL-PS5 video override object. Setting up a reproducible build (or porting `build.sh`-style
  tooling) is its own task, and is the blocker for producing a flashable test ELF.
