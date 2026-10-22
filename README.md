# ps5-hires-player

A hi-res audio player for a jailbroken PS5, forked from
**[BFplayer](https://github.com/ItsBlurf/BFplayer)** by ItsBlurf (GPL-3.0-or-later) and being
specialized toward audio: deeper lossless/hi-res format support, gapless and bit-perfect output,
and a library experience built around artists/albums/playlists rather than movies and TV.

On screen the app is now called **Hi-res AMP** — that is the name on the window, the main menu,
the PS5 home-screen tile, and the launch notification. The project and repository are still named
`ps5-hires-player`, and the release files underneath (`bfplayer-standalone.elf`,
`BFplayer-websrv.zip`) have not been renamed yet.

**Status: Phase 1 specialization in progress.** The video-only UI (subtitles, aspect/crop/scaling,
HDR tonemap picker, video thumbnails) has been removed, the library now reads and groups by music
metadata, and the library and now-playing screens have been reworked for audio. Format coverage,
gapless playback, and bit-perfect output are still ahead. See [`docs/research/`](docs/research/)
for the research that led to forking rather than building from zero.

## Features

- A music library grouped by artist, album, and playlist, with fast incremental scanning (local
  storage and USB drives) that skips unchanged files by size and modification time.
- Library categories: **ALL MUSIC, CONTINUE LISTENING, RECENTLY PLAYED, FAVORITES, ARTISTS,
  ALBUMS, PLAYLISTS.** ARTISTS and ALBUMS are drill-down browsing modes — pick an artist to see
  their albums, pick an album to see its tracks (ALBUMS goes straight from an album to its
  tracks). Track lists inside an album are sorted by track number. Circle backs out one level.
- Artist, album, album artist, track number, disc number, genre, and year are read from file tags
  where present; albums with untagged files fall back to grouping by folder name.
- A now-playing screen with large, centered album art and a background color sampled from that
  artwork, replacing the plain black screen used during audio playback before.
- Manual media sources with an optional whole-library import, plus DLNA/UPnP browsing for
  compatible NAS and LAN media servers (audio resources only).
- Direct HTTP(S), FTP, RTSP, RTMP, RTP, TCP, and UDP media URLs.
- Resume playback, favorites, search, sorting, and playback queues (including M3U/PLS/XSPF
  playlists).
- Persistent settings and rotating diagnostic logs.
- Clean reinjection: a newer payload replaces the resident launcher without creating a second tile
  or resetting the library.

Playback is powered by FFmpeg 7.0.1, SDL2, and SDL_kitchensink. Decoding is software-based.

## Why fork BFplayer

Two open questions — whether a smooth native (non-browser) UI is achievable on current PS5
jailbreaks at all, and whether a real audio codec library can be cross-compiled for this target —
turned out to already be solved, by this project and its dependencies:

- **Native UI**: BFplayer renders its own interface via SDL2 (no system browser), installs its own
  Media-tab tile, and has been under active, substantial development (4K HDR playback, WebM
  playback fixes, decoder memory bounds — see its commit history).
- **Codecs**: playback runs on FFmpeg 7.0.1, itself cross-compiled for this exact PS5 target by
  [PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo), which also carries FLAC, Opus,
  mpg123, LAME, libmad, FAAD2, libsndfile, libsamplerate, and libvorbis/libogg as working,
  CI-built packages — the format-compatibility groundwork a hi-res player needs is already proven
  on real hardware (see `ps5-payload-dev/websrv`'s own FFplay sample).

Full detail and sources: [`docs/research/`](docs/research/).

## What changes from here

BFplayer is a general movie/TV/music player built for video first. The plan is to specialize, not
rewrite from scratch. Done so far:

- Stripped the video-specific UI and controls (subtitles, aspect/crop/scaling, HDR tonemap
  picker, video thumbnails) down to what an audio player actually needs, and narrowed DLNA/UPnP
  browsing to audio resources.
- Reworked the library view around artist/album/playlist browsing instead of movies/shows/seasons,
  and added a now-playing screen built around the current track's album art.
- Re-scoped the library scanner to audio metadata: artist, album, album artist, track number,
  disc number, genre, and year read from file tags, with a folder-name fallback for untagged
  files.

Still ahead:

- Broaden and verify hi-res/lossless format coverage specifically (FLAC, ALAC, DSD, WAV, APE),
  since BFplayer's own docs note its format testing has been video-focused so far.
- Add audio-specific playback behavior: gapless transitions, bit-perfect/exclusive output where
  the hardware path allows it, ReplayGain/loudness normalization.
- Rename the release files and on-disk paths themselves (they are still named after BFplayer; only
  the on-screen name has changed so far).
- The add-media browser still speaks in BFplayer's original folder/movie terms rather than
  music-specific ones — see [Install](#install) below.

## Install

### Standalone Media tile

Download `bfplayer-standalone.elf` from the latest release and inject that payload after each
jailbreak. It installs or updates the **Hi-res AMP** tile in the Media section and stays resident
to launch the player.

Nothing needs to be copied to `/data/homebrew` beforehand. The payload creates and maintains its
own runtime folder. Reinjection preserves the library, settings, sources, playback history, and
logs. Repeated tile launches are coalesced, and a player-instance lock prevents two player
processes from initializing PS5 video memory at the same time.

### websrv

Download `BFplayer-websrv.zip` from the latest release and extract its `BFplayer` folder into a
websrv homebrew root such as `/data/homebrew`, `/mnt/usb0/homebrew`, or `/mnt/ext0/homebrew`. The
final entrypoint is `BFplayer/eboot.elf`.

websrv launches the player directly, so this method does not use `bfplayer-standalone.elf`, the
Media tile, port 9040, or the resident launcher. See [docs/WEBSRV.md](docs/WEBSRV.md) for the
exact package layout.

## Controls

| Button | Action |
| --- | --- |
| Cross | Open, play, or pause |
| Circle | Back out of artist/album browsing; no action at the library root |
| Square | Change audio track |
| Triangle | Remove the selected source (press twice) |
| D-pad Left/Right | Change library category |
| D-pad Up/Down | Move through the list |
| L1/R1 | Previous or next page/chapter |
| L3 | Add or remove favorite |
| R3 | Change sort mode |
| Touchpad | Add media, or show the full control list during playback |
| Options | Open the main menu, or the playback menu during playback |

The Add Media browser still uses BFplayer's original folder/movie terms rather than
music-specific ones: Cross opens a folder or adds a single file as a "movie", and Triangle adds a
folder as one "TV show". Either path works for adding music files and folders; the wording just
hasn't been updated yet.

## Network and NAS media

Open **Options > Browse DLNA / NAS** to discover UPnP MediaServer devices on the same local
network. The player browses their ContentDirectory folders and plays compatible audio resources
through FFmpeg — video and image items are filtered out of the listing. Discovery and browsing run
on a cancellable worker with bounded response and listing sizes.

For a server that exposes a direct stream, use **Options > Open network URL**. Direct SMB is not
linked. A NAS can instead expose its library through DLNA, HTTP(S), FTP, or another supported
streaming protocol. URLs containing username/password authority fields are rejected, and signed
query values are redacted from logs and excluded from playback-history persistence.

## Building

Inherited from BFplayer for now — uses [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)
and PacBrew v0.37:

```powershell
. ..\..\ps5dev-env.ps1
.\build.ps1 -Configuration Release
```

Release artifacts are written to `dist/`. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
for every bundled component and its license.

## Credits

- **[ItsBlurf](https://github.com/ItsBlurf)** started, maintains, and does all the real console
  testing for BFplayer, the project this is forked from. Everything that currently works here is
  their work; this fork exists to specialize it, not to claim it.
- BFplayer's own acknowledgements (carried forward, since this is still substantially their code):
  [ps5-payload-websrv](https://github.com/ps5-payload-dev/websrv) for the BigApp/ELF-loading
  foundation and the websrv homebrew format, [dlnaplay](https://github.com/ps5-payload-dev/dlnaplay)
  for the DLNA/audio-clock/bounded-buffer design reference, [FFmpeg and ffplay](https://ffmpeg.org/)
  for the media and playback foundation, [PacBrew](https://github.com/ps5-payload-dev/pacbrew),
  SDL2, and SDL_kitchensink for the PS5 playback stack, and [VLC](https://www.videolan.org/vlc/)
  as a reference for display/aspect/crop behavior.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE), unchanged from BFplayer's own.
