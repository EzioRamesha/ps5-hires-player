# Phase 1b: UI Rework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework the library's category list and the playback screen for a music player,
Poweramp-inspired (large album art during playback, fast artist/album browsing, no video-only
controls), and fix the compile breaks Plan 1a's Task 2 deliberately left behind when it removed
video-only modules.

**Architecture:** No change to `LibraryUi`'s or `PlaybackOsd`'s small public interfaces (both
already Pimpl-wrapped — `open`/`close`/`handle_event`/`render`/etc. stay exactly as they are).
This plan reworks their internals and one block in `src/main.cpp`'s render loop. Depends on
Plan 1a landing first (`MediaEntry`'s new fields, `media_album_key`/`media_album_display_name`).

**Tech Stack:** C++17, SDL2 (software rendering, `SDL_ttf` for text, `SDL_image` for artwork),
the project's existing plain `check(condition, message)` host-test harness where logic is
host-testable; most of this plan's changes are rendering code that is NOT host-testable (no
display in CI) and gets verified the same way BFplayer already verifies hardware-only behavior —
a dated verification doc, following the existing `docs/ALPHA*_VERIFICATION.md` pattern.

## Global Constraints

- Full spec: `docs/specs/2026-10-21-phase1-audio-specialization-design.md`.
- Depends on Plan 1a (`docs/superpowers/plans/2026-10-21-phase1-backend-and-data-layer.md`) being
  complete first — this plan uses `MediaEntry::artist`/`album`/etc. and
  `media_album_key`/`media_album_display_name` by name, which do not exist until Plan 1a lands.
- Exact pixel positions/sizes for new rendering are NOT prescribed here beyond what's needed for
  correctness (e.g. "centered," "fills the available width") — this project's own established
  practice is to tune exact layout against real rendered screenshots (see
  `tools/render-ui-preview.ps1`, already in this repo) rather than guess pixel constants
  unrendered. Each rendering task's last step is "render a preview and look at it," not "trust the
  numbers in this plan."
- Conventional commits, one purpose per commit.

---

## Task 1: Fix the compile breaks Plan 1a's Task 2 left behind

**Files:**
- Modify: `src/ui/library_ui.cpp`
- Modify: `src/ui/playback_osd.cpp`
- Modify: `src/main.cpp` (if Plan 1a's Task 2 Step 4 found references here — check its completion
  notes first)

**Interfaces:** None produced — this task's only job is making the project link again.

- [ ] **Step 1: Read Plan 1a's Task 2 Step 4 completion notes** (the exact compiler
  undefined-reference output it recorded) — this is the authoritative, real list of what to fix,
  not a guess. If those notes are unavailable, reproduce them: build the project now and read the
  linker errors directly.

- [ ] **Step 2: Remove `#include "bfplayer/video_thumbnail.hpp"`** from `src/ui/library_ui.cpp`
  (confirmed present at line 16) and every call site that used it — thumbnails were a video-grid
  feature; a music library browses by artist/album text and artwork (Task 3 below), not
  auto-generated video thumbnails.

- [ ] **Step 3: Remove subtitle/video-track selection from `playback_osd.cpp`'s internals.** The
  README's existing playback-controls table (being rewritten in Task 4) lists "Circle: Change
  subtitle track" and "Triangle: Change video track" — find and remove the `Impl` logic backing
  both (grep `playback_osd.cpp` for `subtitle` and `video_track`/`video_index` to find it), since
  `subtitle_provider.hpp`/`external_subtitles.hpp` no longer exist after Plan 1a's Task 2.
  "Square: Change audio track" stays — a music file can still have multiple audio streams
  (commentary, alternate mixes) and the existing track-switching mechanism under Kitchensink is
  still valid for audio tracks specifically.

- [ ] **Step 4: Remove any `hdr_tonemap`/`video_layout` references** still remaining in
  `src/main.cpp` per Step 1's list (the render loop block already read while preparing this plan,
  around line 4599-4620, branches on `native_hdr_output` and calls `player_video_layout` — for an
  audio player this whole HDR/layout branch has no video to apply it to; simplify it, keeping only
  the `app.video` (now always false post-Plan-1a) vs audio-only path distinction needed by Task 2
  below).

- [ ] **Step 5: Build and confirm the project links.** This is the real test for this task — no
  undefined references remain.

- [ ] **Step 6: Commit.**
  ```bash
  git add src/ui/library_ui.cpp src/ui/playback_osd.cpp src/main.cpp
  git commit -m "fix: remove remaining references to video-only modules, restore the build"
  ```

---

## Task 2: A real now-playing screen for audio

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `find_local_artwork(media_path, limits)` from `include/bfplayer/artwork.hpp` (already
  exists, already used by the library's artwork panel — this task is its second caller, not a new
  dependency).

Currently, `src/main.cpp`'s render loop (confirmed around line 4594-4609) clears the screen to
solid black for every audio-only frame (`!app.video` — true for every file after Plan 1a, since
video decoding stays linked for now but nothing in a music-focused library opens a video stream)
and relies on `PlaybackOsd`'s thin overlay for all visible content. This task replaces the plain
black clear with a real now-playing screen: large, centered album art (reusing the same artwork
lookup the library already uses) with a subtle blurred/dimmed version filling the background,
Poweramp-style, so there is always something visually substantial on screen during playback, not
a near-empty frame with a progress bar.

- [ ] **Step 1: Load artwork once per track change, not once per frame.** Find where `main.cpp`
  already tracks "the currently playing media path" (used elsewhere for title display — grep for
  the struct/variable holding the current `MediaEntry` or path in the playback loop). Add a cached
  `SDL_Texture*` for the now-playing artwork, invalidated and reloaded via
  `find_local_artwork(current_path)` only when the current path changes — loading/decoding an
  image every frame would be wasteful and is not what the library panel does either (confirm by
  checking how `library_ui.cpp`'s existing artwork panel caches its texture, and match that
  caching lifetime/invalidation approach exactly, since it already solves this same problem
  correctly).

- [ ] **Step 2: Render it.** In the `!app.video` branch of the render block (replacing the plain
  `SDL_RenderClear` fill), if artwork is loaded: draw a large copy of the art centered in the
  available area (reuse the existing SDL_image decode-and-upload path the library panel already
  uses — do not write a second image loader), and fill the remaining background with a solid
  color sampled from the artwork's average color if that's cheap to compute, or a fixed dark
  neutral color if not (don't add an expensive per-track color-extraction pass in this task if it
  isn't already trivial with what's linked — note which choice was made and why in the commit
  message, this is a judgment call to make with the real rendering in front of you, not something
  to pre-decide blind). If no artwork is found, keep the existing solid-black fallback — never show
  a broken-image state.

- [ ] **Step 3: Render a preview and look at it.** Use `tools/render-ui-preview.ps1` (already in
  this repo, used by BFplayer for exactly this kind of visual check) against a test track with
  known artwork and one with none, confirming both the artwork and no-artwork cases render
  sensibly before calling this task done.

- [ ] **Step 4: Commit.**
  ```bash
  git add src/main.cpp
  git commit -m "feat: show album art as the now-playing background instead of a black screen"
  ```

---

## Task 3: Rework the library's category list

**Files:**
- Modify: `src/ui/library_ui.cpp`

**Interfaces:**
- Consumes: `media_album_key`/`media_album_display_name` from Plan 1a's Task 5.

`filter_name(int filter)` (confirmed at `src/ui/library_ui.cpp:556`) currently defines 8
categories: `ALL MEDIA, CONTINUE WATCHING, RECENTLY PLAYED, FAVORITES, MOVIES, TV SHOWS, MUSIC,
PLAYLISTS`, selected via `active_filter` (confirmed used at lines 2488, 3838, 3898, 3969) and
cycled with `kFilterCount = 8` (confirmed at line 55).

- [ ] **Step 1: Write the new filter list.** Replace `filter_name`'s switch with:
  ```cpp
  const char* filter_name(int filter) {
      switch (filter) {
          case 1:
              return "CONTINUE LISTENING";
          case 2:
              return "RECENTLY PLAYED";
          case 3:
              return "FAVORITES";
          case 4:
              return "ARTISTS";
          case 5:
              return "ALBUMS";
          case 6:
              return "PLAYLISTS";
          default:
              return "ALL MUSIC";
      }
  }
  ```
  Update `kFilterCount` from `8` to `7` (one fewer category — `MOVIES`/`TV SHOWS` are gone, `MUSIC`
  is redundant now that everything is music, `ARTISTS`/`ALBUMS` are added as two new browsing
  modes).

- [ ] **Step 2: Find and update each filter's actual list-building logic.** Each `case` in
  `filter_name` has a counterpart somewhere that builds the visible row list for that filter
  (grep for `active_filter ==` or a `switch (active_filter)` near where the visible row vector is
  populated). The new `ARTISTS` and `ALBUMS` cases need to group the full media index using
  `media_album_key`/`media_album_display_name` (for ALBUMS) and an equivalent grouping by
  `MediaEntry::artist` alone (for ARTISTS — this one is new, not provided by Plan 1a; implement it
  directly here as a straightforward `std::string` group-by on `entry.artist`, falling back to
  `media_album_display_name`'s own folder-name fallback logic when `artist` is empty, for
  consistency with how an untagged file is already handled elsewhere). Remove the `MOVIES`/
  `TV SHOWS`/`MUSIC` cases' logic entirely (dead code once their categories are gone).

- [ ] **Step 3: Build and visually verify** via `tools/render-ui-preview.ps1` against a test
  library with multiple artists/albums, confirming the new categories list sensibly and the old
  movie/show cycling is gone.

- [ ] **Step 4: Commit.**
  ```bash
  git add src/ui/library_ui.cpp
  git commit -m "feat: replace movie/TV categories with artist and album browsing"
  ```

---

## Task 4: Update user-facing docs to match

**Files:**
- Modify: `README.md`

**Interfaces:** None — documentation only.

- [ ] **Step 1: Update the inherited README's Features/Install/Controls sections** to reflect
  Phase 1's actual state: remove references to movies/TV shows/subtitles/aspect-crop-scaling
  (gone per Tasks 1-3), update the playback-controls table to drop the removed Circle/Triangle
  subtitle/video-track rows, and add a line describing the new now-playing screen and
  artist/album/playlist browsing. Keep the sections that are still accurate as-is (Install,
  Building, Credits, License, Network/DLNA with its now-audio-only framing already updated by
  Plan 1a's Task 6).

- [ ] **Step 2: Commit.**
  ```bash
  git add README.md
  git commit -m "docs: update README for the Phase 1 music-player specialization"
  ```

---

## Self-Review

**Spec coverage:** the UI rework items from the spec — category rework, now-playing screen,
playback-controls simplification — all have tasks. The spec's "Poweramp-inspired" direction (large
album art, fast artist/album browsing) is reflected in Task 2 (art-forward now-playing) and
Task 3 (artist/album as first-class categories) rather than a separate task, since both are the
same underlying change viewed from two different screens.

**Placeholder scan:** Task 2 Step 2 explicitly flags one genuine judgment call (color-extraction
vs. fixed background) as something to decide with the real rendering in front of the implementer,
not a vague "add styling" — this is a legitimate deferred decision with clear criteria, not an
open placeholder.

**Type consistency:** `media_album_key`/`media_album_display_name` are used in Task 3 exactly as
Plan 1a's Task 5 defines them (one `MediaEntry` argument, `std::string` return).

**Dependency ordering, stated explicitly:** this plan cannot start before Plan 1a's Task 5 lands
(Task 3 here calls functions Plan 1a defines) — Task 1 of this plan can start as soon as Plan 1a's
Task 2 lands (it only needs the video-only files to already be gone), but Tasks 2-3 need Plan 1a
fully complete. Do not parallelize across the two plans past that point.
