# ps5-hires-player

A hi-res audio player for a jailbroken PS5, forked from
**[BFplayer](https://github.com/ItsBlurf/BFplayer)** by ItsBlurf (GPL-3.0-or-later) and being
specialized toward audio: deeper lossless/hi-res format support, gapless and bit-perfect output,
and a library experience built around artists/albums/playlists rather than movies and TV.

**Status: early fork, not yet specialized.** The codebase right now is BFplayer's own, unmodified
— a real, working native PS5 media player (no browser, its own home-screen tile), not a stub. See
[`docs/research/`](docs/research/) for the research that led to forking rather than building from
zero, and watch this README's own history for when the audio-specific work actually starts.

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
rewrite from scratch:

- Strip the video-specific UI and controls (subtitles, aspect/crop/scaling, DLNA video browsing)
  down to what an audio player actually needs.
- Broaden and verify hi-res/lossless format coverage specifically (FLAC, ALAC, DSD, WAV, APE),
  since BFplayer's own docs note its format testing has been video-focused so far.
- Add audio-specific playback behavior: gapless transitions, bit-perfect/exclusive output where
  the hardware path allows it, ReplayGain/loudness normalization.
- Rework the library view around artist/album/playlist browsing instead of movies/shows/seasons.
- Both internal and external (USB) storage, with fast incremental scanning (skip unchanged files
  by size/mtime) — BFplayer already has a library scanner; this mostly means re-scoping it to
  audio metadata (tags) rather than video.

None of this is scoped into tasks yet — that comes after a proper design pass, same process as
ps4mount's own feature work: research → brainstorm → written plan → implementation.

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
