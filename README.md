# ps5-hires-player

A hi-res audio player payload for a jailbroken PS5: broad format support (lossless/hi-res and
common lossy formats), a native on-console interface (not a web panel), internal and external
(USB) storage, and fast library scanning.

**Status: research phase.** No code yet — see [`docs/research/`](docs/research/) for what has
been established so far and what is still open. Nothing here is final until a design is written
up and reviewed.

## Why this exists

A music player for the jailbroken-PS5 homebrew scene, in the same spirit as
[PS4Mount](https://github.com/gasb07/ps4mount) (a PS4-pkg loader) and
[fps60](https://github.com/eladharels/ps5-60fps) (a frame-rate-patch payload): a focused,
single-purpose payload rather than a general media-center port.

## What's decided so far

- **Native UI, not a web panel.** Unlike PS4Mount/fps60, which both serve an HTML page to the
  system browser, this player should render its own interface directly on the console. Whether
  that is realistically achievable on current PS5 jailbreaks — and how — is the first open
  question being researched; see `docs/research/`.
- **Both internal and external storage.** Scans the console's own storage in addition to USB
  drives, unlike PS4Mount which is USB-only by design.
- **Fast scanning.** Library scans should cache what has already been read (file size/mtime,
  tags) so re-scanning only looks at what changed — the same principle PS4Mount already uses for
  its pkg catalog.

## What's still open

Tracked in `docs/research/` as it's resolved. The two biggest unknowns before any real design can
be written:

1. **Is a smooth native GUI actually achievable** on a jailbroken PS5 payload today, or is the
   system browser still the only practical rendered surface? This determines the whole shape of
   the project.
2. **Format/codec approach** — a single library covering the most formats with the least
   integration work (FFmpeg/libavcodec is the leading candidate) versus a plugin-per-format model
   like several established open-source players use.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Nothing here currently contains code copied from
another project; any project this ends up building on or taking architectural inspiration from
will be credited here, the same way PS4Mount credits its own references.
