# PS5 hi-res audio output: is bit-perfect / >16-bit/48kHz reachable via sceAudioOut?

Research for the `ps5-hires-player` fork of BFplayer. Question: can a jailbroken PS5 payload get
bit-perfect, higher-bit-depth, higher-sample-rate (and eventually DSD) audio output by calling
`sceAudioOut` directly instead of going through SDL2's PS5 AudioOut backend?

## A note on source hygiene

Several repos surfaced by search look like PS4/PS5 homebrew audio projects but are **not
trustworthy primary sources** and were excluded:

- `boykopovar/AnyPS5` ("PS5 games natively on PC, no emulation") — confirmed via `gh api
  repos/boykopovar/AnyPS5` to have **46,045 stargazers and 4,236 forks but only 1 contributor**,
  created 2026-08-03. That stars/contributors mismatch, plus the technically-implausible premise
  (native porting of PS5 executables with no emulation), are the signature of an astroturfed/scam
  repo. Its PRs referencing `sceAudioOut2EnableChat` etc. were not used as evidence, even though
  (see below) that symbol name does turn out to be genuine.
- `pLabs5/pYTM5` — repo size is 5 KB, no language set, effectively empty. Not usable.
- `alechurri/nx-on-orbis`, `OnlyMoisties/skate3-ps4` — real but tiny (25–32 stars, repo sizes
  1.5 KB/157 KB), one explicitly self-described as "built with AI" / "experimental, closed
  experiment." Treated as unreliable color, not cited as evidence below.

Two repos by GitHub user `sainsaji` (`EVO-PLAYER-PS5`, `PS5-Audio-Passthrough-Research`) are real,
modestly-starred (40 and 10 stars), GPL-licensed, single-author projects with substantial real
source code. They're used below as corroborating (not authoritative) evidence, with an explicit
flag where their two documents **contradict each other** on whether bitstream passthrough works.

## 1. The official `ps5-payload-dev/sdk` stub — what it actually contains

Confirmed directly via the GitHub API and by downloading the real file:
`sce_stubs/libSceAudioOut.c` in `ps5-payload-dev/sdk`
(https://github.com/ps5-payload-dev/sdk/blob/master/sce_stubs/libSceAudioOut.c, 18,712 bytes,
544 lines).

**It contains no format enum, no struct, no documentation — only bare `asm()` trampolines that
declare the symbol name for the linker**, e.g.:

```c
asm(".global sceAudioOutOpen\n"
    ".type sceAudioOutOpen @function\n"
    "sceAudioOutOpen:\n");
```

This matches the SDK's own description of `sce_stubs/`: stubs give the linker correct symbol
names and NIDs; the real implementation is resolved at runtime against the PS5 OS's own
`libSceAudioOut.sprx`. The SDK's `include/ps5/` directory (`iommu.h`, `kernel.h`, `klog.h`,
`mdbg.h`, `nid.h`, `payload.h`) has **no audio header at all** — so the SDK alone proves the
function is callable but gives zero information about the format parameter.

What the stub file's symbol list *does* show (grep of all `sceAudioOut*` exports) is that PS5's
audio library is far richer than the PS4-era API:
- The plain PS4-compatible set: `sceAudioOutOpen`, `sceAudioOutOpenEx`, `sceAudioOutOutput`,
  `sceAudioOutOutputs`, `sceAudioOutPtOpen`, `sceAudioOutExPtOpen`, `sceAudioOutClose`, etc.
- A large, PS5-only `sceAudioOut2*` family (`sceAudioOut2ContextCreate`, `sceAudioOut2PortCreate`,
  `sceAudioOut2MasteringInit`, `sceAudioOut2GetSpeakerArrayCoefficients`,
  `sceAudioOut2EnableChat`, ~70 symbols total) — almost certainly the Tempest 3D AudioTech /
  object-based audio engine's API surface.

So `sceAudioOut2EnableChat` is a **real** exported symbol on PS5 (confirmed from the authentic
SDK stub, independent of the untrustworthy AnyPS5 repo that also mentioned it). But because the
SDK ships no header for either the AudioOut or AudioOut2 families, nothing about sample
rate/format/bit-depth limits for AudioOut2 could be determined here — that would need hands-on
probing on real hardware.

## 2. Real format enum and sample-rate limit, from PS4's `sceAudioOutOpen` (same API family)

Two independent sources agree on the exact enum and a hard 48 kHz lock:

**shadPS4** (`shadps4-emu/shadPS4`, confirmed legitimate: created 2022-10-24, 33,826 stars,
2,821 forks — a long-running, widely-known PS4 emulator that must reproduce the real console API's
accept/reject behavior to run unmodified retail games). Source:
`src/core/libraries/audio/audioout.h` and `audioout.cpp`
(https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/audio/audioout.h):

```c
enum class OrbisAudioOutParamFormat : u32 {
    S16Mono = 0,
    S16Stereo = 1,
    S16_8CH = 2,
    FloatMono = 3,
    FloatStereo = 4,
    Float_8CH = 5,
    S16_8CH_Std = 6,
    Float_8CH_Std = 7
};

s32 PS4_SYSV_ABI sceAudioOutOpen(UserService::OrbisUserServiceUserId user_id,
                                 OrbisAudioOutPort port_type, s32 index, u32 length,
                                 u32 sample_rate, OrbisAudioOutParamExtendedInformation param_type);
```

And in `audioout.cpp`'s `sceAudioOutOpen` implementation (reproducing the real validation path):

```c
if (sample_rate != 48000) {
    LOG_ERROR(Lib_AudioOut, "Invalid sample rate");
    return ORBIS_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ;
}
...
if (format < OrbisAudioOutParamFormat::S16Mono ||
    format > OrbisAudioOutParamFormat::Float_8CH_Std) {
    return ORBIS_AUDIO_OUT_ERROR_INVALID_FORMAT;
}
```

`GetFormatInfo()` backs this up with concrete sample sizes: every `S16*` format is 2 bytes/sample
(16-bit int), every `Float*` format is 4 bytes/sample (32-bit float) — **there is no 24-bit integer
format anywhere in the enum.**

**Independent corroboration** from `sainsaji/EVO-PLAYER-PS5`
(`projects/common/include/evo_ps5.h`, a real reverse-engineering header for a hobby PS5 audio
player), which defines the *same* values under different names:

```c
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO       0
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO     1
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_8CH        2
#define SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_MONO     3
#define SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO   4
#define SCE_AUDIO_OUT_SAMPLE_RATE                 48000
```

Two unrelated codebases (one an emulator built to run real retail games, one a from-scratch
console-side reverse-engineering effort) independently landing on the identical enum ordering and
the identical 48 kHz constant is strong circumstantial evidence this reflects the *real* PS4/PS5
OS API, not a guess by either project.

**Conclusion for point 3 of the question:** the 48 kHz-only restriction and the absence of a
24-bit integer format are almost certainly a **hard limit of `sceAudioOutOpen` itself** — i.e. an
OS/API-level ceiling, not an SDL2-port-only limitation. SDL2's PS5 AudioOut backend presumably
just maps onto `S16Stereo`, which is the most conservative legal choice, but widening what SDL
exposes would not get you past 48 kHz or past the enum's format list, because the console's own
`sceAudioOutOpen` rejects anything else with `ORBIS_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ` /
`ORBIS_AUDIO_OUT_ERROR_INVALID_FORMAT`.

**However, there is a real, confirmed opportunity inside this same ceiling:** `FloatStereo` /
`Float_8CH` (32-bit float, 4 bytes/sample) is a legal, documented format value distinct from
`S16Stereo`. A 32-bit float container has far more than 16 bits of effective precision, so
**routing 24-bit source material through `sceAudioOutOpen` with the `FloatStereo`/`Float_8CH`
format would be bit-perfect for 24-bit-or-less audio** (no truncation to 16-bit occurs) — something
SDL2's current PS5 AudioOut backend is not doing per BFplayer's own docs ("FFmpeg downmixes output
to 48 kHz stereo signed 16-bit PCM"). That gap is plausibly fixable by calling `sceAudioOutOpen`
directly with the float format instead of going through SDL2. This is the one part of the
question where the limitation does look SDL2-specific rather than OS-level — worth testing first
since it needs no new reverse engineering, only calling an already-stubbed function with a
different (also real, same-enum) constant.

## 3. Does any real homebrew project call `sceAudioOut` directly, bypassing SDL?

Yes: `sainsaji/EVO-PLAYER-PS5` does exactly this (`sceAudioOutOpen` used directly in
`projects/evoplayer/core/src/services/PlaybackController.cpp`,
`SoundEffectEngine.cpp`, `SurroundTestService.cpp`, and `evo_audio_resample.c`). Per its own
`docs/research/audio-passthrough.md`:

- Standard PCM path: `sceAudioOutOpen` with `S16_8CH` at 48 kHz for 5.1/7.1, or `S16_STEREO` at
  48 kHz for stereo. (Not `FloatStereo` in this project — it stuck to S16.)
- It documents that **`sceAudioOutPtOpen` and `sceAudioOutExPtOpen` are real, exported,
  reverse-engineered "passthrough port" functions that open successfully**, but concludes:
  "Bitstream audio passthrough (raw AC-3 / DTS / TrueHD / Atmos bitstream over HDMI to an external
  AVR) is not possible for application modules on the PS5" — system-level muting blocks it, and
  only Sony's own Blu-ray player module holds the capability bits to unmute it.
- No 24-bit or alternate-sample-rate PCM output is claimed anywhere in this project.
- DSD is not mentioned anywhere in this project's docs.

This independently confirms (separately from the SDK stub symbol list) that `sceAudioOutPtOpen`
and `sceAudioOutExPtOpen` genuinely exist as callable PS5 exports — matching what the official
SDK's stub file also shows.

### A contradicting, unverified claim

The same author's second, newer repo, `sainsaji/PS5-Audio-Passthrough-Research`
(created 2026-10-08, 10 stars), claims the opposite: that bitstream passthrough **does** work, with
very specific IEC 61937 burst parameters (AC-3 at 48 kHz/256-sample frames, E-AC-3 at 192 kHz,
DTS-HD and TrueHD specifics) and a `docs/FINDINGS.md` written as a hardware-testing log (kernel
`[AvControl]` log lines, an LG Ultragear / soundbar as the AVR, "confirmed on real hardware on
2026-10-08", TrueHD "verified" the next day).

This is **not corroborated anywhere else** — no mention in any PS5 jailbreak community discussion,
no named tester, no external replication, and it directly contradicts the conclusion in this same
author's earlier `EVO-PLAYER-PS5` repo ("not possible for application modules"). The repo also
contains an `AGENTS.md` file, consistent with AI-assisted authorship, which does not make the
content false but does lower confidence that "confirmed on real hardware" logs are what they claim
to be. **Treat this claim as unverified and do not rely on it** — it would be a major, independently
newsworthy homebrew breakthrough if true, and currently has zero independent corroboration.

## 4. DSD

No primary source found — anywhere — that mentions DSD or DoP in connection with `sceAudioOut`,
PS4, or PS5. Both of the `sainsaji` documents explicitly say DSD is out of scope / not discussed.
Given that (a) the `sceAudioOutOpen` format enum has no PCM-adjacent bitstream mode beyond the
fixed S16/Float/8CH list, (b) the sample rate is hard-locked to 48 kHz (DSD64 is conceptually a
1-bit stream clocked at 2.8224 MHz — utterly incompatible with a 48 kHz PCM port), and (c) even
compressed-bitstream passthrough (AC-3/DTS/TrueHD, a far more mainstream ask than DSD) is
either blocked (per EVO-PLAYER-PS5) or only claimed-but-unverified (per the other repo), DSD
passthrough of any kind has no plausible path through `sceAudioOut` as currently understood. This
is an honest "no public information found, and what evidence exists points the other way" rather
than a confirmed "no."

## Bottom line

**(b) Plausible but unconfirmed — with one narrower sub-claim that leans toward (a).**

- **24-bit via `FloatStereo`/`Float_8CH`**: leans toward **(a) confirmed achievable**, in the
  narrow sense that the format constant is real, documented identically by two independent
  codebases, and is clearly meant to carry higher-than-16-bit precision than `S16Stereo`. Nobody
  has yet published hands-on confirmation of calling `sceAudioOutOpen` with `FloatStereo` from a
  PS5 payload and hearing undistorted output, so it is not 100% proven — but this is the
  single most promising, low-risk next step (stub already exists in the SDK; no new reverse
  engineering needed; just link against `sceAudioOutOpen` directly instead of through SDL2 and
  pass `FLOAT_STEREO`/`FLOAT_8CH` with 48 kHz).
- **Sample rates above 48 kHz for PCM**: leans toward **(c) a hard OS-level ceiling** — two
  independent sources agree `sceAudioOutOpen` rejects anything but 48000 outright
  (`ORBIS_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ`). No evidence of any escape hatch (the PS5-only
  `AudioOut2`/Tempest API family is undocumented and could theoretically differ, but that is pure
  speculation — nobody has published its parameter ranges).
  No amount of SDL2 patching would help here.
- **DSD / DoP**: no evidence either way, but everything found (48 kHz PCM lock, no exotic format
  enum values, passthrough itself being contested/unconfirmed) points away from feasibility.
  Honest answer: unconfirmed and improbable, not "confirmed impossible."

**Most useful next piece of evidence if this needs to stay unconfirmed:** a minimal, from-scratch
test payload (using the real `ps5-payload-dev/sdk`, which already stubs `sceAudioOutOpen`/
`sceAudioOutOutput`) that calls `sceAudioOutOpen` directly with `SCE_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO`
(value `4`) at 48000 Hz and feeds it known 24-bit-sourced float samples, run on real jailbroken
PS5 hardware with a DAC/AVR capable of showing the actual bit depth/signal received. That single
hands-on test would convert the most promising half of this question (24-bit via float) from
"plausible" to "confirmed," and a quick negative-control test (same call with `sample_rate=96000`)
would nail down the ceiling claim beyond the emulator/reverse-engineering corroboration gathered
here.
