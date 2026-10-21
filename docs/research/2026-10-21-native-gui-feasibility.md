# Native (non-web) GUI feasibility + storage scanning — PS5 hi-res audio player

Note on scope: the prior research file this task asked to be read first
(`hires-audio-player-research.md`) does not exist in this scratchpad — only
unrelated leftovers (`memscan/`, `sources/`, old `.dSYM` dirs) from other
sessions were found there. This pass therefore stands alone; it does not
assume or repeat findings from a file that isn't present. One of those
leftovers, `memscan/sdk/ps5-payload-sdk/`, turned out to be a full local
checkout of the real ps5-payload-dev SDK, which was used directly as a
primary source below (headers, `target/lib/*.so` stub list, and sample
sources were read from disk, not summarized secondhand).

---

## 1. Is native (non-web) GUI rendering a solved problem on PS5 homebrew?

**Short answer: yes, as of the current PS5 jailbreak/homebrew ecosystem** —
this is a more mature situation than "unexplored territory." Multiple real,
running, open-source projects render their own on-screen UI via SDL2 and, in
some cases, drive the GPU directly.

### What the SDK itself ships

Reading the local checkout of `ps5-payload-dev/sdk`
(`memscan/sdk/ps5-payload-sdk`) directly:

- `target/lib/` contains proprietary Sony stub libraries including
  `libSceAgc.so`, `libSceAgcDriver.so`, `libSceGnmDriver.so`,
  `libSceGnmDriverForNeoMode.so`, `libSceVideoOut.so`, and
  `libSceGLSlimVSH.so` — i.e. the SDK's linker already has access to the
  console's low-level graphics (AGC/GNM) and video-out libraries.
- `target/include/ps5/` (the SDK's own PS5-specific header dir) only has
  `iommu.h`, `kernel.h`, `klog.h`, `mdbg.h`, `nid.h`, `payload.h` — **no
  header exposes AGC/GNM function prototypes**. The SDK ships the libraries
  to link against but not documented call signatures for them; the
  `GLES2`/`EGL`/`KHR` headers that do exist under `target/include` are
  generic Khronos headers with no matching `libEGL.so`/`libGLESv2.so` in
  `target/lib`, so they are not wired to anything on PS5.
- The one GUI-adjacent sample in `samples/` is `browser/main.c`, which calls
  `sceSystemServiceLaunchWebBrowser(PS5_URL, 0)` — i.e. the SDK's own
  built-in answer for "show the user something" is literally "open the
  system browser on a URL." This is the mechanism PS4Mount and fps60 use.

So out of the box, the official SDK's documented, header-backed path to a
UI is the browser. Direct AGC/GNM use is possible (the libs are linkable)
but entirely undocumented by the SDK — consistent with the PS5 GPU being
programmed through Sony's private AGC interface with no public graphics API
(confirmed by the community PR discussion below).

### What the community has actually built on top of it

- **`ps5-payload-dev/sdk` PR #71** ("sce_stubs: add AGC imports used by
  homebrew graphics", merged, closed 2026-10-10, fixed in commit `1a04f75`):
  contributor mihawk-99 states "Vulkan and RetroArch ports currently build
  their own AGC link stubs because this SDK does not ship `libSceAgc` or
  `libSceAgcDriver` stubs" — confirming Vulkan and RetroArch ports on PS5
  already existed and were hand-rolling AGC stubs before this PR
  consolidated 17 `libSceAgc` + 7 `libSceAgcDriver` symbols into the SDK.
  Maintainer john-tornblom initially argued graphics headers were "out of
  scope" for the base SDK, which is why this stays community-maintained
  rather than first-party documented.
  https://github.com/ps5-payload-dev/sdk/pull/71

- **`mpereiraesaa/ps5-agc-gears`** (GPL-3.0-or-later) — a `glxgears`/
  `es2gears`-style hardware-accelerated spinning-gears demo that drives the
  PS5 GPU directly through AGC: "native initialization, direct memory,
  color/depth surfaces, shader metadata, command composition, submission,
  synchronization, VideoOut presentation and teardown," using independently
  authored `gfx1013` shaders compiled through LLPC. It reports "an
  uninterrupted 60,000-frame run on one PS5 with firmware 12.02 ... with
  zero renderer errors," but explicitly disclaims it is "not a GPU
  benchmark" and that results apply only to the tested firmware. It
  contains no jailbreak/payload-delivery code — it's a rendering-technique
  demo meant to run under an existing loader.
  https://github.com/mpereiraesaa/ps5-agc-gears

- **`darkxex/SwanStationPS5`** (GPL-3.0-or-later) — a PS1 emulator that
  "installs as an app on the PS5 home screen," built on
  john-tornblom's ps5-payload-dev SDK plus PacBrew's SDL2/libcurl ports.
  Its README states it is "now 100% Vulkan, the game's picture and the
  whole interface drawn by the GPU," reaching the TV "through Vulkan," with
  full 60fps and resolution upscaling. This is a second, independent
  confirmation that a real Vulkan path to the PS5 display exists for
  homebrew today, not just a tech demo.
  https://github.com/darkxex/SwanStationPS5

- **`Rufidj/Nativehbl`** (GPL-3.0) — "Native PS5 homebrew launcher built
  with SDL2 and `ps5-payload-sdk`," explicitly contrasted with the browser
  workflow: "a native SDL2 launcher UI" providing "direct homebrew
  launching without the browser workflow." Its themes are "software-
  rendered," i.e. SDL2 2D blitting, not GPU-accelerated — a lighter-weight
  precedent closer to what a first version of an audio-player UI would
  need.
  https://github.com/Rufidj/Nativehbl

- **`mihawk-99/PS5_RetroArch`** — RetroArch for PS5 "with Vulkan graphics,"
  35 libretro cores, built against the ps5-payload-dev SDK with clang/lld
  18 and prebuilt SDL2, FFmpeg, FreeType, zlib.
  https://github.com/mihawk-99/PS5_RetroArch

- **`ps5-payload-dev/pacbrew-repo`** (GPL-3.0) — the community package
  repo that makes all of this practical: PKGBUILD-style recipes that
  cross-compile third-party libraries against the ps5-payload-sdk
  toolchain, including **SDL2, SDL2_image, SDL2_ttf, SDL2_mixer,
  SDL2_gfx, SDL2_kitchensink, Mesa, GLEW, GLM, GLU, OpenAL, ImGui, and
  RmlUI**. ImGui and RmlUI are both ready-made immediate-mode/retained
  GUI toolkits, pre-packaged for this toolchain — directly relevant to
  building a real player UI rather than hand-rolling widgets.
  https://github.com/ps5-payload-dev/pacbrew-repo

- **Closest direct precedent — `ItsBlurf/BFplayer`** (GPL-3.0-or-later):
  "A PS5 media and video player that also functions as a complete media
  library, with support for subtitles, multiple audio tracks, aspect-ratio
  adjustment, cropping, and many other playback settings." Built against
  ps5-payload-sdk + PacBrew v0.37, with "playback powered by FFmpeg 7.0.1,
  SDL2, SDL_kitchensink, and libass" — i.e. a native SDL2-rendered UI, not
  a browser panel. This is essentially a video-first sibling of the exact
  project under consideration here (media player + library on jailbroken
  PS5, native UI, FFmpeg-backed decoding). Its own README caveat: "Decoding
  is still software-based, so demanding HEVC, AV1, unusual encodes, or slow
  storage/network sources can exceed the available CPU or I/O budget" —
  i.e. no hardware video decode path is exposed to homebrew, only software
  FFmpeg decode. For audio-only playback this is much less of a concern
  (audio codecs are cheap relative to video), but it's a direct, named data
  point on where the ceiling is.
  https://github.com/ItsBlurf/BFplayer

### PS4/OpenOrbis comparison

PS4 homebrew did solve this earlier and more thoroughly, confirming the
pattern generalizes:

- **OpenOrbis-PS4-Toolchain** (the real, known PS4 homebrew SDK) ships
  **SDL-PS4**, a port of SDL done by znullptr, with "SDL2 headers as well as
  a mini game sample," plus "an OpenGL/piglet GPU rendering sample and
  headers... thanks to Nikita Krapivin," and **OrbisGL**, a GUI framework
  "integrated with OpenGL through SharpGLES for PS4 compatibility." GPU
  rendering finalization was tracked for toolchain v0.7.
  https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
- None of OpenOrbis's PS4-specific binary stubs/headers are directly
  portable to PS5 (different firmware, different proprietary library
  layout, different toolchain), but the pattern — SDL2 port + a GL-based
  GUI framework layered on top — is exactly what the PS5 side
  (ps5-payload-sdk + PacBrew SDL2/Mesa/ImGui/RmlUI) has now reproduced
  independently.

### Verdict for section 1

**Achievable, not pioneering.** A native SDL2 UI (2D, software- or
GL-rendered) for a PS5 homebrew payload is a known, exercised path with
multiple independent working examples (Nativehbl, BFplayer, PS5_RetroArch,
SwanStationPS5) built on a community-maintained, cross-compiled SDL2/Mesa/
ImGui/RmlUI package set (PacBrew) layered on the official ps5-payload-dev
SDK. Going further to direct GPU/Vulkan rendering is also demonstrated
working (ps5-agc-gears, SwanStationPS5, PS5_RetroArch) but rests on
undocumented, community-reverse-engineered AGC stubs that the SDK
maintainer still treats as out-of-scope/unofficial — so it carries real
firmware-compatibility risk ("results apply only to the console and
firmware actually tested"). For a hi-res audio player, a simple SDL2 2D UI
(album art, track list, transport controls) is squarely in the "solved,
low-risk" category; full GPU-accelerated effects are "solved but still
somewhat bleeding-edge."

---

## 2. Fallback path — is SDL2/GL actually usable, or is browser still the only option?

Given section 1's findings, this fallback question has a firmer answer than
"might be unexplored": **the browser is not the only practical option
anymore.** SDL2 (2D, software-rendered) via PacBrew + ps5-payload-sdk is a
proven, multiply-reused path (Nativehbl, BFplayer). GPU-backed rendering
(Mesa/GL, or direct AGC, or Vulkan via the community AGC stubs) also works
today but is the less-trodden, higher-risk tier — mpereiraesaa's own gears
demo explicitly disclaims firmware-general validity, and the AGC stub
headers live outside the official SDK. A sound v1 plan is:

1. Start with SDL2 2D rendering (text, bitmap album art, simple widgets) —
   same tier as Nativehbl/BFplayer, the best-trodden path, no need for
   AGC/GNM/Vulkan at all.
2. Treat GPU-accelerated rendering (Mesa/GL or Vulkan) as a stretch goal,
   not a v1 dependency, given the "out of scope for this SDK,"
   firmware-specific-validation caveats found above.

The browser-panel approach (PS4Mount/fps60's own pattern,
`sceSystemServiceLaunchWebBrowser`) remains available as a zero-risk
fallback if SDL2 integration proves harder than expected for this specific
project, but is not required to meet the "non-web" requirement — real
precedent for native rendering now exists.

---

## 3. Internal + external storage scanning

### What the SDK exposes for enumerating mounted storage

The local SDK checkout's `samples/mntinfo/main.c` (GPL-3.0-or-later, by
John Törnblom) is a complete, minimal answer to "how do you enumerate all
mounted storage":

```c
#include <sys/mount.h>
...
struct statfs *buf = 0;
int nitems = getmntinfo(&buf, MNT_WAIT);
for (int i = 0; i < nitems; i++) {
  printf("%s on %s (%s, flags=0x%lx)\n",
         buf[i].f_mntfromname, buf[i].f_mntonname,
         buf[i].f_fstypename, buf[i].f_flags);
}
```

This is the standard BSD `getmntinfo(3)` call (PS5's OS is a FreeBSD-
derived kernel) and it returns **every** currently mounted filesystem —
internal and external alike — with no need to guess paths. This is a
strictly more general technique than hardcoding `/mnt/usbN`/`/mnt/extN`,
and it's already present as an official SDK sample, so it's a safe,
supported building block for a from-scratch scan routine.

### Known concrete paths (from this project and the wider ecosystem)

- This repo's own architecture doc (`/Users/rameshvenketraman/ps4mount/
  CLAUDE.md`) documents PS4Mount's current approach: scan loop over
  `/mnt/usbN` / `/mnt/extN` for drives, with console state kept at
  `/data/ps4mount/` and generated install files/symlinks at
  `/system_data/ps4mount/` (`HOLD_DIR`), nullfs-mounted over
  `/user/app/<TITLE_ID>`. `/mnt/usbN` is external USB, `/mnt/extN` is PS5
  "extended storage" (console-managed external SSD/HDD) — both are
  external media from the player's point of view, just two different
  console-recognized classes of external drive.
- `drakmor/ShadowMountPlus` (auto-mount payload for jailbroken PS5)
  documents installing game dumps either to **`/data/homebrew`** on the
  internal SSD, or to a `homebrew` folder at the root of a USB drive — i.e.
  the writable "internal storage" location homebrew payloads are actually
  given on PS5 is the `/data` partition (the jailbreak's own homebrew data
  area), not a general-purpose user media folder. There is no retail
  PS5 concept of a built-in "Music library" the way older consoles had a
  cross-media-bar Music app, so "internal storage media the user already
  has" in practice means whatever the user has dropped into `/data/...`
  or into the console's extended-storage drives, not a pre-existing
  system-provided music folder.
- `owendswang/ps5-web-file-manager` (GPL-3.0-or-later; LGPL notice for
  bundled libmicrohttpd/libsmb2) confirms the same high-level picture in
  its own README — it lists devices at top as clickable links to paths
  like `/data`, `/mnt/usb`, `/mnt/ext` — but its public README does not
  go into enumeration code detail (no `getmntinfo` vs. hardcoded-path
  confirmation available without reading its source tree directly, which
  this pass did not have repo-clone access to do beyond the README).

### Practical scanning design for this project

Given the above, the concrete recommendation is: use `getmntinfo()` once
at startup to get the authoritative mount list (same primitive as the
SDK's own `mntinfo` sample), filter to filesystem types/mount-point
prefixes that correspond to USB (`/mnt/usb*`) and extended storage
(`/mnt/ext*`), and additionally always include the payload's own `/data/...`
area as the "internal" library root, mirroring ShadowMountPlus's own
convention — rather than trying to discover a PS5-native music folder that
doesn't exist.

### Fast scanning technique

This project's own CLAUDE.md documents PS4Mount's catalog approach directly:
`src/catalog.[ch]` "finds every `.pkg` on a drive, reads headers once
(cached in `/data/ps4mount/catalog-<drive>.txt`)" — i.e. mtime/identity-
keyed metadata caching so a rescan only re-reads files that are new or
changed, rather than re-parsing every file's headers every time. This is
exactly the standard technique used by mainstream music-library indexers:
TagLib-based scanners (used by Strawberry, DeaDBeeF, and many others per
the prior research this task references) typically key a cache row by
path + mtime + size, and skip re-reading tags/headers when those are
unchanged since the last scan; MPD's own database does the same
(mtime-keyed incremental update rather than full rescan) per its
long-documented `update` command behavior. PS4Mount's catalog cache is
architecturally the same pattern applied to PKG headers instead of audio
tags — confirming this is the standard, not a PS4Mount-specific trick, and
validating reusing the same design (per-drive cache file, keyed by path +
mtime/size, skip unchanged entries) for an audio library scanner.

---

## 4. "Max compatibility" — anything new on a single C library for broad format support

Quick-check only, confirming rather than re-deriving the prior survey's
likely FFmpeg conclusion:

- **License**: FFmpeg's core is **LGPL v2.1+**; several specific
  components (notably `libx264`/`libx265` and other GPL-licensed optional
  parts) are **GPL v2+ and disabled by default** — they only activate, and
  only then flip FFmpeg's own effective license to GPL, when the build
  explicitly passes `--enable-gpl` (and further to GPLv3-compatible terms
  with `--enable-gpl` combined with GPLv3-only components). This means a
  GPL-3.0-or-later project (which this one is, matching PS4Mount's own
  license) can freely build FFmpeg with `--enable-gpl` and bundle/statically
  link it without any license conflict — no need to stay on the LGPL
  subset.
- **Portability to PS4/PS5 homebrew toolchains — confirmed, multiple
  times, by name**:
  - `ItsBlurf/BFplayer` ships **FFmpeg 7.0.1** cross-compiled against
    ps5-payload-sdk + PacBrew for its media-library player.
  - `ps5-payload-dev/pacbrew-repo` carries FFmpeg "as a static
    cross-compiled library in the ps5-payload-dev package group,"
    available as a ready-made PKGBUILD-style recipe rather than something
    each project re-ports from scratch.
  - `mihawk-99/PS5_RetroArch` builds against "prebuilt third-party
    libraries including SDL2, ffmpeg, freetype, and zlib" for its PS5
    RetroArch port (used for in-game video playback/recording).
  - `tsuramatsu1/psp5` (a PSP emulator for PS5) has its own
    `build-ffmpeg.sh` cross-compilation script, a third independent proof
    point.
  - On PS4/OpenOrbis, a large project like the RPCS3-for-PS4 port lists
    FFmpeg among its cross-compiled dependencies (alongside Mesa, LLVM,
    wolfSSL, curl), showing the same pattern one generation back.

So nothing has changed in substance from the likely prior conclusion:
FFmpeg/libavcodec remains the single best-leverage choice for broad codec
coverage in a from-scratch C payload, it is now a known-working,
already-packaged (PacBrew) dependency on this exact toolchain rather than
a hypothetical port, and its license poses no conflict for a GPL-3.0-or-
later project.

---

## Bottom line

**Yes — a native-GUI (non-web) PS5 homebrew payload for a hi-res audio
player is realistically buildable today**, and it would not be pioneering
new ground: it would be following the same, already-proven stack
(ps5-payload-sdk + PacBrew's SDL2/FFmpeg/libass/SDL_kitchensink) that
`ItsBlurf/BFplayer` — a close sibling project (PS5 homebrew, native UI,
media library, FFmpeg-backed decode) — already ships under GPL-3.0-or-later.
Nativehbl and PS5_RetroArch independently confirm SDL2-native UI and
Vulkan-native UI respectively both work on real hardware outside of a
browser. Storage scanning has a clean, SDK-sample-backed answer
(`getmntinfo()`, the same primitive PS4Mount could use instead of/alongside
its own `/mnt/usbN`/`/mnt/extN` convention), and PS4Mount's own catalog-
caching pattern is already the textbook incremental-rescan technique used
industry-wide.

**The single riskiest/hardest unknown** is not "can we draw a UI" — it's
**how reliable the SDL2-via-PacBrew runtime path stays across PS5 firmware
updates and across different jailbreak/payload-loader combinations in the
wild.** The clearest documented sign of this fragility: `ps5-agc-gears`'s
own README limits its validation to "the exact current commit" on "PS5
firmware 12.02" and says results don't generalize beyond the tested
console/firmware; and the AGC graphics stubs needed by Vulkan/RetroArch
ports were, until very recently (PR #71, October 2026), duplicated ad hoc
by each project because the base SDK maintainer considered them "out of
scope." Even staying on the safer SDL2-2D tier (not touching AGC/GNM/Vulkan
at all, like Nativehbl/BFplayer), the dependency chain (PacBrew's
cross-compiled SDL2/FFmpeg build, the ps5-payload-sdk toolchain itself, and
the jailbreak/payload-loader the user is running) is maintained by a loose
federation of community projects rather than a single stable vendor SDK —
so the real engineering risk for this project is less "will rendering
work" and more "staying buildable/working as the SDK, PacBrew packages,
and jailbreak scene all continue to shift under it."
