# PS5 Payload Build/Toolchain Research — for ps5-hires-player

All facts below were pulled live from the GitHub API (`gh api`) on 2026-10-21 against the
actual files in each repo (not memory/training data). Every claim has a repo + path citation.

---

## 1. Survey of real open-source PS4/PS5 homebrew build setups

### Confirmed-real repos (verified via GitHub API, not assumed)

`gh api orgs/ps5-payload-dev/repos` returned 28 real repos, including:
`sdk`, `pacbrew-repo`, `SDL`, `elfldr`, `klogsrv`, `shsrv`, `ftpsrv`, `LakeSnes`, `websrv`,
`gdbsrv`, `offact`, `fetchpkg`, `regdump`, `FBNeo`, `yquake2`, `linkdev`, `bdj-ipv6-hen`,
`kstuff`, `libcross2d`, `sscrap`, `pemu`, `tvhp`, `dlnaplay`, `jtplay`, `svtplay`, `tv4play`,
`plutotv`, `launchpad`.

- `OpenOrbis/OpenOrbis-PS4-Toolchain` — real, actively pushed (`pushed_at: 2026-02-27`).
- `owendswang/ps5-web-file-manager` — real, actively pushed (`pushed_at: 2026-10-19`, i.e. two
  days before this research).
- `GoldHEN/GoldHEN` — real org/repo, but **the repo contains no source**: `gh api
  repos/GoldHEN/GoldHEN/contents` returns only `.github`, `CHANGELOG.md`, `CHEATMENU.md`,
  `README.md`, and three prebuilt `.bin` files (`goldhen_2.3_505.bin`, `_672.bin`, `_900.bin`).
  GoldHEN's actual build process is **not publicly inspectable** — correct the assumption that
  it's a usable reference; it isn't.

### `ps5-payload-dev/elfldr` — toolchain + deploy pattern
`Makefile` (root of repo):
```
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif
...
test: elfldr-ps5.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
```
Confirms the SDK/toolchain pattern PS4Mount already uses is the house style for the whole org.

### `ps5-payload-dev/websrv` — same pattern, plus a `homebrew/` catalog of real payloads
`Makefile` is structurally identical to elfldr's (`include .../toolchain/prospero.mk`,
`$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^`), links `libmicrohttpd`/`microdns`/`libsmb2` via
`$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config <lib> --libs`.

`websrv/homebrew/` contains 20+ real ported payloads, each with its own
`.github/workflows/<name>.yml`: `CrispyDoom`, `DevilutionX`, `EDuke32`, `FBNeo`, **`FFplay`**,
`LakeSnes`, `LinkDev`, `Mari0`, `Mednafen`, `OffAct`, `Omnispeak`, `OpenJazz`, `PKGInstall`,
`RetroArch`, `SVTplay`, `ScummVM`, `SverigesRadio`, `VVVVVV`, `Wolf4SDL`, `YQuake2`.

### `owendswang/ps5-web-file-manager` — dependency-install script read directly
`install-libmicrohttpd.sh` (full file read): downloads libmicrohttpd source, `source
"${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"`, then:
```sh
./configure --prefix="${PREFIX}" --host=x86_64-pc-freebsd \
  --enable-static --disable-shared --disable-doc --disable-curl --disable-examples
"${MAKE:-make}" && "${MAKE:-make}" install
"${PS5_CROSS_FIX_ROOT}" "${DESTDIR}/${PREFIX}"
```
i.e. it's a plain autotools cross-compile against the prospero toolchain, gated by a
`prospero-pkg-config --exists` check so it's a no-op if the lib is already in the SDK. Its
`Makefile` sources `prospero.mk`/`prospero.sh` the same way, uses
`$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config libmicrohttpd --cflags/--libs`, and vendors
`libsmb2` as a **git submodule** built via its own CMake toolchain file
(`-DBUILD_SHARED_LIBS=OFF ...`) — so this project uses *two* different third-party-library
strategies side by side: an install script for libmicrohttpd, a git submodule + CMake for libsmb2.

### `OpenOrbis/OpenOrbis-PS4-Toolchain` (PS4-side equivalent)
`README.md`: clang + lld based (not vendor SDK), same family of tools as ps5-payload-dev's SDK
but no PacBrew-equivalent package repo of its own. Sets `OO_PS4_TOOLCHAIN` env var (role
equivalent to `PS5_PAYLOAD_SDK`). No evidence of a curated third-party-library ecosystem; it's
scoped to headers/stubs/CRT/VS templates.

---

## 2. How a payload pulls in a nontrivial third-party C library — **this is a solved problem**

The single biggest finding of this research: **`ps5-payload-dev/pacbrew-repo`** is a real,
actively maintained PacBrew (Arch-pacman-format) package repository that already contains
**pre-written cross-compile recipes (PKGBUILDs) for FFmpeg and essentially every mainstream
audio codec library**, built against the exact same `prospero` toolchain PS4Mount uses.

`gh api repos/ps5-payload-dev/pacbrew-repo/contents` lists ~100 packages, including (directly
relevant to a hi-res audio player):
`ffmpeg`, `flac`, `opus`, `mpg123`, `lame`, `libmad`, `faad2`, `libmodplug`, `libsndfile`,
`libsamplerate`, `libvorbis`, `libogg`, `libtheora`, `libvpx`, `libx264`, `a52dec`, `openal`,
plus the SDL2 family, `curl`, `libressl`/`openssl`, `libarchive`, `sqlite`, `libmicrohttpd`.

### FFmpeg — `ffmpeg/PKGBUILD` (read in full)
```
pkgname=ps5-payload-ffmpeg
pkgver=7.0.1
depends=('ps5-payload-zlib' 'ps5-payload-openssl' 'ps5-payload-freetype'
         'ps5-payload-libfribidi' 'ps5-payload-harfbuzz' 'ps5-payload-libass'
         'ps5-payload-libx264')
build() {
    source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"
    ./configure --prefix="${PREFIX}" --enable-cross-compile \
        --cross-prefix="${PS5_PAYLOAD_SDK}/bin/prospero-" \
        --enable-static --disable-shared \
        --arch=x86_64 --target-os=freebsd \
        --enable-openssl --enable-version3 \
        --enable-libfreetype --enable-libfribidi --enable-libharfbuzz \
        --enable-gpl --enable-libx264 --disable-debug --disable-doc --enable-libass \
        --cc="${CC}" --cxx="${CXX}" --nm="${NM}" --strip="${STRIP}" \
        --ar="${AR}" --ranlib="${RANLIB}" --pkg-config="${PKG_CONFIG}"
    ${MAKE}
}
package() { ... ${MAKE} install; ${PS5_CROSS_FIX_ROOT} "${DESTDIR}/${PREFIX}"; }
```
This is a completely standard FFmpeg `./configure --enable-cross-compile --target-os=freebsd`
invocation, just pointed at the prospero cross-compiler names.

**And it's not just compiled — it's actually running as a deployed payload.** `websrv`'s
`homebrew/FFplay/build.sh` (read in full):
```sh
source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh" || exit 1
# ffplay is included with sdk, just copy it
cp "${PS5_SYSROOT}/${PS5_HBROOT}/bin/ffplay" "${SCRIPT_DIR}/ffplay.elf"
```
`websrv/.github/workflows/ffplay.yml` builds this in CI by installing the **prebuilt** PacBrew
tarball (`wget .../pacbrew-repo/releases/latest/download/ps5-payload-dev.tar.gz && sudo tar xf
... -C /`) and zips/releases `ffplay.elf` as a GitHub release artifact. So: FFmpeg's own `ffplay`
binary (full avcodec/avformat/swscale/avutil stack, decoding real media) is a real, CI-built,
GitHub-released PS5 payload today.

### FLAC — `flac/PKGBUILD` (read in full)
```
makedepends=('ps5-payload-sdk' 'ps5-payload-libcxx' 'ps5-payload-libogg')
build() {
    source ".../prospero.sh"
    ./configure --prefix="${PREFIX}" --host=x86_64-pc-freebsd \
        --enable-static --disable-shared --enable-cpplibs \
        --disable-thorough-tests --disable-ogg
    ${MAKE} -C src/libFLAC
}
```

### Opus — `opus/PKGBUILD` (read in full)
```
./configure --prefix="${PREFIX}" --host=x86_64-pc-freebsd \
    --enable-static --disable-shared --enable-custom-modes
```

### mpg123 — `mpg123/PKGBUILD` (read in full)
```
./configure --prefix="${PREFIX}" --host=x86_64-pc-freebsd \
    --enable-static --disable-shared --disable-components --enable-libmpg123
```

### libsndfile — `libsndfile/PKGBUILD` (read in full) — depends on FLAC, lame, libogg,
libvorbis, mpg123, opus, so it's a working composite build showing multi-library dependency
resolution through the package manager (`depends=(...)` + `makepkg`/`pacman -U`).

**Common cross-compile recipe pattern across all of the above** (this is the generalizable
template for any `./configure && make` library, including a new codec your project might need):
1. `source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"` — sets `CC`/`CXX`/`AR`/`NM`/`RANLIB`/
   `STRIP`/`PKG_CONFIG`/`PREFIX`/`DESTDIR`/`PS5_CROSS_FIX_ROOT` as env vars (confirmed indirectly;
   `prospero.mk` sets the Make-variable equivalents, see §3).
2. `./configure --prefix="${PREFIX}" --host=x86_64-pc-freebsd --enable-static --disable-shared`
   (FFmpeg instead uses `--enable-cross-compile --cross-prefix=.../bin/prospero- --target-os=freebsd
   --arch=x86_64` since it has its own cross-compile flag convention).
3. `${MAKE}` / `${MAKE} install` with `DESTDIR="${pkgdir}/${DESTDIR}"`.
4. `${PS5_CROSS_FIX_ROOT} "${DESTDIR}/${PREFIX}"` at the end — a repo-provided post-install fixup
   tool (`host/bin/prospero-cross-fix-root`, confirmed present in the SDK's file tree) that
   presumably rewrites install paths/symlinks for the sysroot layout.

### Is a prebuilt package repo actually published?
Yes. `pacbrew-repo/.github/workflows/ps5-payload-libs.yml` runs `bash ci-libs.sh` (which builds
every package in `PKGS=(... flac opus mpg123 lame libmad faad2 libsndfile ... ffmpeg ...)` via
`makepkg` + `pacman -U` for each), then `tar -czf ps5-payload-dev.tar.gz -C / opt/ps5-payload-sdk`
and uploads it as a release artifact. Any new project (including ps5-hires-player) can skip
building FFmpeg/FLAC/Opus/etc from scratch entirely and just download
`https://github.com/ps5-payload-dev/pacbrew-repo/releases/latest/download/ps5-payload-dev.tar.gz`,
`sudo tar xf ... -C /`, and link against the already-built static libs via
`prospero-pkg-config <lib> --cflags/--libs` — exactly what `websrv/.github/workflows/ffplay.yml`
does.

### Verdict on "is porting a codec library to this target solved?"
**Solved**, not merely attempted. FFmpeg, FLAC, Opus, mpg123, LAME, libmad, FAAD2, libsndfile,
libsamplerate, libvorbis/libogg are all real, currently-maintained PKGBUILDs in
`ps5-payload-dev/pacbrew-repo`, built in CI, published as a binary tarball release, and at least
one of them (FFmpeg's `ffplay`) is demonstrably deployed and runnable as an actual PS5 homebrew
payload via `ps5-payload-dev/websrv`'s own homebrew catalog. This is a materially different
(much better) picture than "maybe someone attempted this once" — it means ps5-hires-player's
audio-decode layer is largely a *reuse*, not a *port*, problem: consume `ps5-payload-flac` /
`ps5-payload-ffmpeg` / `ps5-payload-opus` etc. as PacBrew dependencies rather than reinventing
the cross-compile.

---

## 3. Required host-side tools, concretely

### From `ps5-payload-dev/sdk/README.md` (read in full), the SDK's own stated prerequisites:

**Debian/Ubuntu:**
```
sudo apt-get install bash clang-18 lld-18 wget                      # required
sudo apt-get install socat cmake meson pkg-config python3 python3-pyelftools   # optional
```
**Fedora (tested on 41):**
```
sudo dnf install bash llvm-devel clang lld wget                     # required
sudo dnf install socat cmake meson pkg-config python3 python3-pyelftools      # optional
```
**macOS (tested on Sequoia), via Homebrew:**
```
brew install llvm@18 wget                                           # required
export LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config        # required
brew install socat cmake meson python && pip3 install pyelftools    # optional
```
So the SDK itself is genuinely cross-platform (Linux/macOS confirmed by its own README; PS4Mount's
CI only exercises Ubuntu). `socat` is called out specifically as the deploy transport. `cmake` and
`meson` are both "optional" at the SDK level because **both are supported build systems for
individual payloads** — confirmed by `samples/hello_cmake/CMakeLists.txt` (uses
`add_custom_target(test_c COMMAND ${PS5_DEPLOY} hello_c.elf)`) and the existence of a
`samples/hello_meson` sample alongside `hello_world`/`hello_cxx`/`hello_cxx23`/`hello_argv`/
`hello_dlfcn`/`hello_kenv`/`hello_so`/`hello_sprx`/`hello_stdio`/`browser`/`notify`/`notify_debug`/
`ps`/`hwinfo`/`mntinfo`/`install_app`/`http2_get`/`list_files`/`arbitrary_syscall`/
`relocatable_linking`/`test_privileges`.

`host/toolchain/prospero.mk` (read in full) is what `include $(PS5_PAYLOAD_SDK)/toolchain/
prospero.mk` actually pulls in; it defines:
```makefile
PS5_SYSROOT := $(PS5_PAYLOAD_SDK)/target
PS5_HBROOT  := /user/homebrew
DESTDIR ?= $(PS5_SYSROOT)
PREFIX  ?= $(PS5_HBROOT)
PS5_DEPLOY := $(PS5_PAYLOAD_SDK)/bin/prospero-deploy
AS/CC/CXX/OBJC/OBJCXX/LD/PKG_CONFIG/AR/NM/OBJCOPY/RANLIB/STRIP := $(PS5_PAYLOAD_SDK)/bin/prospero-<tool>
```
`host/toolchain/prospero.cmake` (read in full) is the CMake-toolchain-file equivalent: sets
`CMAKE_SYSTEM_NAME FreeBSD`, `CMAKE_SYSTEM_VERSION 9`, `CMAKE_CROSSCOMPILING 1`,
`CMAKE_SYSROOT=.../target`, `CMAKE_INSTALL_PREFIX=/user/homebrew`,
`BUILD_SHARED_LIBS OFF`, `CMAKE_POSITION_INDEPENDENT_CODE ON`, and points every
`CMAKE_<LANG>_COMPILER`/`CMAKE_AR`/`CMAKE_RANLIB`/`CMAKE_STRIP`/`PKG_CONFIG_EXECUTABLE` at the
`prospero-*` tool wrappers (with a `WIN32` branch using `.cmd` wrappers under `host/win/`).

The SDK's own repo tree (`host/bin/`) also ships: `prospero-clang`, `prospero-clang++`,
`prospero-cmake`, `prospero-cross-fix-root`, `prospero-deploy`, `prospero-fetchpkg`,
`prospero-lld`, `prospero-llvm`, `prospero-llvm-config`, `prospero-meson`, `prospero-nid` (+
`.c` source), `prospero-pkg-config` — i.e. full wrapper coverage for autotools, CMake, *and*
Meson cross builds, not just raw Make.

`pacbrew-repo/README.md` (read in full) lists a much larger prerequisite set needed specifically
to build the *library ecosystem* (not just the SDK/one payload):
```
sudo apt-get install cmake pkg-config meson clang lld build-essential autoconf libtool yasm nasm \
    bison flex gperf pkgconf libarchive-tools autopoint po4a git curl doxygen makepkg \
    pacman-package-manager python3-mako python3-glad
```
Its CI job (`pacbrew-repo/.github/workflows/ps5-payload-libs.yml`, read in full) additionally
installs `llvm-18-dev libclang-18-dev libclang-cpp18-dev libclc-18-dev libllvmspirvlib-18-dev
smpq spirv-tools glslang-tools python3-packaging` and pins `meson>=1.4.0` via pip — needed
because the package set includes GPU/graphics libs (`mesa`, `glew`, `glu`) and a scripting VM
(`luajit`, `libquickjs`), not because audio codecs need them.

### GNU Make specifically?
`prospero.mk` is GNU-Make syntax (`?=`, `$(abspath ...)`, `ifdef`). PS4Mount's own `build.sh`
comment about needing `xxd`+bash is a PS4Mount-specific asset-embedding step, not an SDK
requirement — confirmed the SDK itself has no `xxd` dependency anywhere in its README or
Makefile.inc.

---

## 4. Transfer/deploy tooling — confirmed mechanism

`host/bin/prospero-deploy` (read in full, it's a bash script) is the real, canonical deploy tool,
referenced by `PS5_DEPLOY` in every Makefile/CMakeLists.txt surveyed (`elfldr`, `websrv`,
`hello_cmake`, `prospero.cmake`'s `PAYLOAD_DEPLOY` alias). Its actual logic:
```sh
if [ -z "$PORT" ]; then PORT=9021; fi
...
if [[ -z $INTERACTIVE ]]; then
    if [ -e "$1" ]; then
        socat -t 9999999 - TCP:$HOST:$PORT < $1
    else
        echo "$1" | socat -t 9999999 - TCP:$HOST:$PORT
    fi
else
    { cat "$1" && cat /dev/stdin; } | socat - TCP:$HOST:$PORT     # -i: interactive, pipes stdin too
fi
```
So: **confirmed, port 9021 default**, driven by `socat`, controllable via `-h HOST`/`-p PORT`/`-i`
flags or `$PS5_HOST`/`$PS5_PORT` env vars (same two env vars every sampled Makefile declares:
`PS5_HOST ?= ps5`, `PS5_PORT ?= 9021`). `elfldr`'s own Makefile sets `PS5_PORT ?= 9020` as its
*local default* for its own `elfldr-ps5.elf` loader binary (the loader needs a fixed port to
listen on), which is distinct from `prospero-deploy`'s built-in fallback of 9021 — don't conflate
the two; `prospero-deploy` is the generic "send any payload to a running loader" tool, and 9021 is
specifically the well-known port `elfldr`'s own long-running socket server (`socksrv.elf`) listens
on once bootstrapped (visible in `elfldr`'s Makefile: `socksrv.elf: socksrv.o selfldr.o ...`).
There's also `.cmd` Windows wrappers (`host/win/prospero-deploy.cmd`) confirming Windows is a
supported deploy host too, even though building the SDK itself is Linux/macOS-first.

---

## What to install, in order — concrete checklist for ps5-hires-player

1. **Host OS**: Linux (Debian/Ubuntu or Fedora) or macOS. (Windows only gets wrapper `.cmd`
   tooling for deploy, not full build support per the SDK README.)
2. **Core compiler toolchain**: `clang-18`/`lld-18` (apt) or `llvm-devel clang lld` (dnf) or
   `brew install llvm@18` + `export LLVM_CONFIG=...llvm@18/bin/llvm-config` (macOS). This is a
   hard requirement, not optional.
3. **`bash`, `wget`** — required by the SDK's own scripts.
4. **Optional-but-you'll-need-them**: `socat` (deploy transport), `cmake`, `meson`, `pkg-config`,
   `python3` + `python3-pyelftools`.
5. **Download/build the SDK**: either `wget .../sdk/releases/latest/download/ps5-payload-sdk.zip`
   + `unzip -d /opt`, or build it yourself (`sudo make DESTDIR=/opt/ps5-payload-sdk install`);
   either way `export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk`.
6. **Pull in PacBrew's prebuilt libraries instead of hand cross-compiling FFmpeg/FLAC/Opus/etc**:
   `wget https://github.com/ps5-payload-dev/pacbrew-repo/releases/latest/download/ps5-payload-dev.tar.gz`
   `sudo tar xf ps5-payload-dev.tar.gz -C /` — this drops a fully pre-built `/opt/ps5-payload-sdk`
   containing `ps5-payload-ffmpeg`, `ps5-payload-flac`, `ps5-payload-opus`, `ps5-payload-mpg123`,
   `ps5-payload-libsndfile`, etc. already compiled for prospero, queryable via
   `$(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config <lib> --cflags --libs`. Only build a package
   yourself from `pacbrew-repo/<pkg>/PKGBUILD` (via `makepkg` + `pacman -U`) if you need a newer
   version or a codec that isn't in the list yet.
7. **Pick a build system for the payload itself**: a plain Makefile with `include
   $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk` (PS4Mount's own pattern, and elfldr/websrv's) is the
   most common; CMake via `-DCMAKE_TOOLCHAIN_FILE=$(PS5_PAYLOAD_SDK)/toolchain/prospero.cmake` is
   equally supported and is what you'd want if pulling in a CMake-based dependency directly
   (e.g. vendoring `libsmb2` as ps5-web-file-manager does).
8. **`socat`** on the dev machine to actually push the built `.elf` to the console's ELF loader at
   `$PS5_HOST:9021` via `$(PS5_PAYLOAD_SDK)/bin/prospero-deploy` (or the raw `socat -t 9999999 -
   TCP:$HOST:9021 < payload.elf` it wraps).
9. CI pattern to copy (same shape PS4Mount already uses, confirmed identical in
   `websrv/.github/workflows/ffplay.yml`): apt-install `clang-18 lld-18 build-essential`, install
   the PacBrew prebuilt tarball instead of building deps, build with `PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
   ./build.sh`, zip, and release on tag push.

## Honest note on the hardest unknown

Going in, the open question was whether cross-compiling a real audio codec library for "prospero"
had ever actually been done, or would need to be pioneered. **That question is resolved, and
resolved favorably**: it's not just done, it's packaged, CI-built, published as binary releases,
and at least one full decode/playback stack (FFmpeg + `ffplay`) is a real, running PS5 homebrew
payload today via `ps5-payload-dev/websrv`. The remaining genuine unknowns are narrower and
specific to *this* project, not to the ecosystem:
- Whether the *specific* hi-res formats ps5-hires-player cares about (e.g. DSD, high-bit-depth
  ALAC, MQA-adjacent containers) are enabled in the existing `ffmpeg`/`flac` PKGBUILD configure
  flags as-is, or need a PKGBUILD fork with different `./configure` flags (FLAC's recipe already
  disables `--disable-ogg`, for instance — worth checking before assuming full format coverage).
- Whether PS5 audio *output* (not decode) — getting PCM from a decoded buffer to the actual audio
  hardware/SceAudioOut-equivalent — has prior art; none of the packages surveyed here address
  playback-to-speakers, only decode (`ffplay` itself likely uses SDL2's audio output, which is
  also in PacBrew, so this is probably fine, but it wasn't directly verified in this pass).
