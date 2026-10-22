# Audio format and codec audit

This audit records what is actually present for **audio** decoding and demuxing in the
linked PacBrew v0.37 FFmpeg 7.0.1 static archives (`libavformat.a`, `libavcodec.a`). It
follows the exact method [`FFMPEG_SYMBOL_AUDIT.md`](FFMPEG_SYMBOL_AUDIT.md) already
established for this project, filtered down to the audio-relevant subset, and extended to
be exhaustive rather than representative: every `ff_*_decoder` and `ff_*_demuxer` symbol in
the archive was classified, not just the ones a prior pass happened to name.

Like the base audit, this is evidence of what code is linked into the archive, not a PS5
hardware playback guarantee. The current open PS5 stack decodes everything on CPU; nothing
here claims real-time performance on console.

## Headline numbers

| Archive | Evidence | Total symbols | Audio-relevant |
| --- | --- | ---: | ---: |
| `libavcodec.a` | unique `ff_*_decoder` definitions | 500 | 206 |
| `libavformat.a` | unique `ff_*_demuxer` definitions | 353 | 137 |

The total counts (500 decoders, 353 demuxers) match `FFMPEG_SYMBOL_AUDIT.md` exactly,
confirming this audit ran against the same archive build.

## Important finding: DSD is present, not absent

The task that produced this audit expected DSD support to be **absent** from this FFmpeg
build and asked for that absence to be confirmed explicitly. The actual symbol list shows
the opposite: DSD decoding **is** linked.

```text
ff_dsd_lsbf_decoder          (DSD, least-significant-bit-first)
ff_dsd_lsbf_planar_decoder   (DSD, LSB-first, planar)
ff_dsd_msbf_decoder          (DSD, most-significant-bit-first)
ff_dsd_msbf_planar_decoder   (DSD, MSB-first, planar)
ff_dst_decoder                (DST — the lossy-losslessly-compressed SACD/DSD codec)

ff_dsf_demuxer                (Sony DSF — the common .dsf DSD file container)
ff_wsd_demuxer                (WSD — Philips Wideband Single-bit Data, a DSD-family format)
```

**Implication:** any Phase 1/Phase 2 planning that assumed DSD playback needs a bespoke
decoder, external library, or "not supported" product message should be revisited — the
linked FFmpeg already carries raw-DSD decode (`.dsf` via `ff_dsf_demuxer` +
`ff_dsd_*_decoder`) and DST decode (SACD-style compressed DSD). What has **not** been
verified here is PS5 runtime behavior (performance, `swresample` downsampling path to PCM,
or whether Kitchensink's audio pipeline accepts the 1-bit DSD sample format without extra
glue) — that is a hardware/runtime question for a later task, not a static-archive one.

## Explicit checklist from the task brief

| Format | Decoder symbol(s) | Demuxer symbol(s) | Status |
| --- | --- | --- | --- |
| FLAC | `ff_flac_decoder` | `ff_flac_demuxer` | Present |
| ALAC | `ff_alac_decoder` | *(none — ALAC has no standalone demuxer; it demuxes out of `ff_mov_demuxer`/`ff_caf_demuxer`)* | Present |
| WAV / PCM variants | `ff_pcm_*_decoder` (35 variants, see table below) | `ff_wav_demuxer`, `ff_w64_demuxer`, plus raw `ff_pcm_*_demuxer` | Present |
| WavPack | `ff_wavpack_decoder` | `ff_wv_demuxer` | Present |
| Monkey's Audio (APE) | `ff_ape_decoder` | `ff_ape_demuxer` | Present |
| Musepack (MPC) | `ff_mpc7_decoder`, `ff_mpc8_decoder` | `ff_mpc_demuxer`, `ff_mpc8_demuxer` | Present |
| TTA (True Audio) | `ff_tta_decoder` | `ff_tta_demuxer` | Present |
| Shorten | `ff_shorten_decoder` | `ff_shorten_demuxer` | Present |
| OptimFROG | *(none found)* | *(none found)* | **Confirmed absent** |
| DSD | `ff_dsd_lsbf_decoder`, `ff_dsd_lsbf_planar_decoder`, `ff_dsd_msbf_decoder`, `ff_dsd_msbf_planar_decoder`, `ff_dst_decoder` | `ff_dsf_demuxer`, `ff_wsd_demuxer` | **Present — see finding above, this contradicts the expected result** |
| MP3 | `ff_mp3_decoder` + 5 variants (`mp3float`, `mp3adu`, `mp3adufloat`, `mp3on4`, `mp3on4float`) | `ff_mp3_demuxer` | Present |
| AAC | `ff_aac_decoder`, `ff_aac_fixed_decoder`, `ff_aac_latm_decoder` | `ff_aac_demuxer`, `ff_loas_demuxer` (LATM/LOAS) | Present |
| Opus | `ff_opus_decoder` | *(none standalone — carried in `ff_ogg_demuxer`/`ff_matroska_demuxer`/`ff_mov_demuxer`)* | Present |
| Vorbis | `ff_vorbis_decoder` | *(none standalone — carried in `ff_ogg_demuxer`/`ff_matroska_demuxer`)* | Present |
| DTS | `ff_dca_decoder` (FFmpeg's internal name for DTS Coherent Acoustics is `dca`) | `ff_dts_demuxer`, `ff_dtshd_demuxer` | Present |
| TrueHD | `ff_truehd_decoder` | `ff_truehd_demuxer` | Present |
| AC-3 / E-AC-3 | `ff_ac3_decoder`, `ff_ac3_fixed_decoder`, `ff_eac3_decoder` | `ff_ac3_demuxer`, `ff_eac3_demuxer` | Present |
| WMA / WMA Lossless | `ff_wmav1_decoder`, `ff_wmav2_decoder`, `ff_wmapro_decoder`, `ff_wmavoice_decoder`, `ff_wmalossless_decoder` | *(none standalone — carried in `ff_asf_demuxer`)* | Present |

OptimFROG is the one format on this explicit list that is genuinely absent: there is no
`ff_*decoder`/`ff_*demuxer` symbol containing `ofr` or `optimfrog` anywhere in either
archive. FFmpeg upstream has never shipped an OptimFROG decoder, so this is expected and
not a PacBrew packaging gap.

## Complete audio decoder table (206 of 500 `ff_*_decoder` symbols)

Every symbol below was kept because its name identifies an audio codec. The remaining 294
decoder symbols were checked and excluded as video, image, subtitle, or internal-helper
symbols (e.g. `ff_h264_decoder`, `ff_png_decoder`, `ff_srt_decoder`,
`ff_init_cabac_decoder`) — none of those are reproduced here since they carry no audio
relevance, but the full 500-symbol list is reproducible with the command in the
Reproduction section below.

**Hi-res / lossless-relevant:**

```text
ff_flac_decoder            ff_alac_decoder             ff_wavpack_decoder
ff_ape_decoder              ff_tta_decoder              ff_shorten_decoder
ff_wmalossless_decoder      ff_mlp_decoder              ff_truehd_decoder
ff_tak_decoder              ff_osq_decoder              ff_sonic_decoder
ff_wavarc_decoder           ff_rka_decoder              ff_ralf_decoder
ff_als_decoder              ff_dst_decoder              ff_dsd_lsbf_decoder
ff_dsd_lsbf_planar_decoder  ff_dsd_msbf_decoder         ff_dsd_msbf_planar_decoder
ff_qoa_decoder
```

**Mainstream lossy:**

```text
ff_mp1_decoder          ff_mp1float_decoder     ff_mp2_decoder
ff_mp2float_decoder      ff_mp3_decoder          ff_mp3adu_decoder
ff_mp3adufloat_decoder   ff_mp3float_decoder     ff_mp3on4_decoder
ff_mp3on4float_decoder   ff_aac_decoder          ff_aac_fixed_decoder
ff_aac_latm_decoder      ff_opus_decoder         ff_vorbis_decoder
ff_ac3_decoder           ff_ac3_fixed_decoder    ff_eac3_decoder
ff_dca_decoder           ff_speex_decoder        ff_wmav1_decoder
ff_wmav2_decoder         ff_wmapro_decoder       ff_wmavoice_decoder
```

**Raw / uncompressed PCM (35 variants):**

```text
ff_pcm_alaw_decoder         ff_pcm_bluray_decoder       ff_pcm_dvd_decoder
ff_pcm_f16le_decoder        ff_pcm_f24le_decoder        ff_pcm_f32be_decoder
ff_pcm_f32le_decoder        ff_pcm_f64be_decoder        ff_pcm_f64le_decoder
ff_pcm_lxf_decoder          ff_pcm_mulaw_decoder        ff_pcm_s16be_decoder
ff_pcm_s16be_planar_decoder ff_pcm_s16le_decoder        ff_pcm_s16le_planar_decoder
ff_pcm_s24be_decoder        ff_pcm_s24daud_decoder      ff_pcm_s24le_decoder
ff_pcm_s24le_planar_decoder ff_pcm_s32be_decoder        ff_pcm_s32le_decoder
ff_pcm_s32le_planar_decoder ff_pcm_s64be_decoder        ff_pcm_s64le_decoder
ff_pcm_s8_decoder           ff_pcm_s8_planar_decoder    ff_pcm_sga_decoder
ff_pcm_u16be_decoder        ff_pcm_u16le_decoder        ff_pcm_u24be_decoder
ff_pcm_u24le_decoder        ff_pcm_u32be_decoder        ff_pcm_u32le_decoder
ff_pcm_u8_decoder           ff_pcm_vidc_decoder
```

**ADPCM and DPCM variants (61):**

```text
ff_adpcm_4xm_decoder         ff_adpcm_adx_decoder         ff_adpcm_afc_decoder
ff_adpcm_agm_decoder         ff_adpcm_aica_decoder        ff_adpcm_argo_decoder
ff_adpcm_ct_decoder          ff_adpcm_dtk_decoder         ff_adpcm_ea_decoder
ff_adpcm_ea_maxis_xa_decoder ff_adpcm_ea_r1_decoder       ff_adpcm_ea_r2_decoder
ff_adpcm_ea_r3_decoder       ff_adpcm_ea_xas_decoder      ff_adpcm_g722_decoder
ff_adpcm_g726_decoder        ff_adpcm_g726le_decoder      ff_adpcm_ima_acorn_decoder
ff_adpcm_ima_alp_decoder     ff_adpcm_ima_amv_decoder     ff_adpcm_ima_apc_decoder
ff_adpcm_ima_apm_decoder     ff_adpcm_ima_cunning_decoder ff_adpcm_ima_dat4_decoder
ff_adpcm_ima_dk3_decoder     ff_adpcm_ima_dk4_decoder     ff_adpcm_ima_ea_eacs_decoder
ff_adpcm_ima_ea_sead_decoder ff_adpcm_ima_iss_decoder     ff_adpcm_ima_moflex_decoder
ff_adpcm_ima_mtf_decoder     ff_adpcm_ima_oki_decoder     ff_adpcm_ima_qt_decoder
ff_adpcm_ima_rad_decoder     ff_adpcm_ima_smjpeg_decoder  ff_adpcm_ima_ssi_decoder
ff_adpcm_ima_wav_decoder     ff_adpcm_ima_ws_decoder      ff_adpcm_ms_decoder
ff_adpcm_mtaf_decoder        ff_adpcm_psx_decoder         ff_adpcm_sbpro_2_decoder
ff_adpcm_sbpro_3_decoder     ff_adpcm_sbpro_4_decoder     ff_adpcm_swf_decoder
ff_adpcm_thp_decoder         ff_adpcm_thp_le_decoder      ff_adpcm_vima_decoder
ff_adpcm_xa_decoder          ff_adpcm_xmd_decoder         ff_adpcm_yamaha_decoder
ff_adpcm_zork_decoder        ff_cbd2_dpcm_decoder         ff_derf_dpcm_decoder
ff_gremlin_dpcm_decoder      ff_interplay_dpcm_decoder    ff_roq_dpcm_decoder
ff_sdx2_dpcm_decoder         ff_sol_dpcm_decoder          ff_wady_dpcm_decoder
ff_xan_dpcm_decoder
```

**Speech / VoIP codecs:**

```text
ff_acelp_kelvin_decoder   ff_amrnb_decoder        ff_amrwb_decoder
ff_comfortnoise_decoder   ff_dss_sp_decoder       ff_evrc_decoder
ff_g723_1_decoder         ff_g729_decoder         ff_gsm_decoder
ff_gsm_ms_decoder         ff_ilbc_decoder         ff_qcelp_decoder
ff_sipr_decoder           ff_siren_decoder        ff_truespeech_decoder
```

**Legacy / proprietary / game audio:**

```text
ff_apac_decoder          ff_aptx_decoder          ff_aptx_hd_decoder
ff_atrac1_decoder        ff_atrac3_decoder        ff_atrac3al_decoder
ff_atrac3p_decoder       ff_atrac3pal_decoder     ff_atrac9_decoder
ff_binkaudio_dct_decoder ff_binkaudio_rdft_decoder ff_bmv_audio_decoder
ff_bonk_decoder          ff_cook_decoder          ff_dolby_e_decoder
ff_dsicinaudio_decoder   ff_dvaudio_decoder       ff_eightsvx_exp_decoder
ff_eightsvx_fib_decoder  ff_fastaudio_decoder     ff_ffwavesynth_decoder
ff_hca_decoder           ff_hcom_decoder          ff_iac_decoder
ff_imc_decoder           ff_interplay_acm_decoder ff_mace3_decoder
ff_mace6_decoder         ff_metasound_decoder     ff_msnsiren_decoder
ff_nellymoser_decoder    ff_on2avc_decoder        ff_paf_audio_decoder
ff_qdm2_decoder          ff_qdmc_decoder          ff_ra_144_decoder
ff_ra_288_decoder        ff_s302m_decoder         ff_sbc_decoder
ff_smackaud_decoder      ff_twinvq_decoder        ff_vmdaudio_decoder
ff_ws_snd1_decoder       ff_xma1_decoder          ff_xma2_decoder
ff_dfpwm_decoder         ff_mpc7_decoder          ff_mpc8_decoder
```

**Null/placeholder:**

```text
ff_anull_decoder
```

## Complete audio-relevant demuxer table (137 of 353 `ff_*_demuxer` symbols)

134 symbols are audio-only containers; 3 more (`ff_mov_demuxer`, `ff_matroska_demuxer`,
`ff_ogg_demuxer`) are general containers that also carry audio-only payloads (M4A, MKA/WebM
audio, OGG/Opus/Vorbis/FLAC-in-Ogg) and are counted here for that reason even though they
also demux video.

**Explicitly requested containers:**

```text
ff_wav_demuxer        (WAV)
ff_flac_demuxer       (FLAC, native)
ff_ogg_demuxer        (Ogg — Vorbis/Opus/FLAC-in-Ogg/Speex)
ff_matroska_demuxer   (Matroska/WebM — audio-only via .mka/.weba is the same demuxer)
ff_mov_demuxer        (MP4/M4A/3GP/3G2/MJ2 — same demuxer registers all of these)
ff_ape_demuxer        (Monkey's Audio)
ff_wv_demuxer         (WavPack)
ff_caf_demuxer        (Core Audio Format)
ff_aiff_demuxer       (AIFF)
```

**Other confirmed audio-only containers:**

```text
ff_aa_demuxer          ff_aac_demuxer         ff_aax_demuxer
ff_ac3_demuxer         ff_ac4_demuxer         ff_acm_demuxer
ff_act_demuxer         ff_adp_demuxer         ff_ads_demuxer
ff_adx_demuxer         ff_aea_demuxer         ff_afc_demuxer
ff_aix_demuxer         ff_alp_demuxer         ff_amr_demuxer
ff_amrnb_demuxer       ff_amrwb_demuxer       ff_apac_demuxer
ff_apc_demuxer         ff_apm_demuxer         ff_aptx_demuxer
ff_aptx_hd_demuxer     ff_argo_asf_demuxer    ff_argo_cvg_demuxer
ff_ast_demuxer         ff_au_demuxer          ff_avr_demuxer
ff_bfstm_demuxer       ff_binka_demuxer       ff_bit_demuxer
ff_bonk_demuxer        ff_brstm_demuxer       ff_codec2_demuxer
ff_codec2raw_demuxer   ff_daud_demuxer        ff_derf_demuxer
ff_dfpwm_demuxer       ff_dsf_demuxer         ff_dss_demuxer
ff_dts_demuxer         ff_dtshd_demuxer       ff_eac3_demuxer
ff_epaf_demuxer        ff_fsb_demuxer         ff_g722_demuxer
ff_g723_1_demuxer      ff_g726_demuxer        ff_g726le_demuxer
ff_g729_demuxer        ff_genh_demuxer        ff_gsm_demuxer
ff_hca_demuxer         ff_hcom_demuxer        ff_iamf_demuxer
ff_ilbc_demuxer        ff_ircam_demuxer       ff_iss_demuxer
ff_kvag_demuxer        ff_loas_demuxer        ff_mlp_demuxer
ff_mp3_demuxer         ff_mpc_demuxer         ff_mpc8_demuxer
ff_msf_demuxer         ff_mtaf_demuxer        ff_musx_demuxer
ff_nistsphere_demuxer  ff_nsp_demuxer         ff_oma_demuxer
ff_osq_demuxer         ff_pcm_alaw_demuxer    ff_pcm_f32be_demuxer
ff_pcm_f32le_demuxer   ff_pcm_f64be_demuxer   ff_pcm_f64le_demuxer
ff_pcm_mulaw_demuxer   ff_pcm_s16be_demuxer   ff_pcm_s16le_demuxer
ff_pcm_s24be_demuxer   ff_pcm_s24le_demuxer   ff_pcm_s32be_demuxer
ff_pcm_s32le_demuxer   ff_pcm_s8_demuxer      ff_pcm_u16be_demuxer
ff_pcm_u16le_demuxer   ff_pcm_u24be_demuxer   ff_pcm_u24le_demuxer
ff_pcm_u32be_demuxer   ff_pcm_u32le_demuxer   ff_pcm_u8_demuxer
ff_pcm_vidc_demuxer    ff_pp_bnk_demuxer      ff_pvf_demuxer
ff_qcp_demuxer         ff_qoa_demuxer         ff_redspark_demuxer
ff_rka_demuxer         ff_rso_demuxer         ff_s337m_demuxer
ff_sap_demuxer         ff_sbc_demuxer         ff_sbg_demuxer
ff_scd_demuxer         ff_sds_demuxer         ff_sdx_demuxer
ff_shorten_demuxer     ff_sln_demuxer         ff_sol_demuxer
ff_sox_demuxer         ff_spdif_demuxer       ff_svag_demuxer
ff_svs_demuxer         ff_tak_demuxer         ff_truehd_demuxer
ff_tta_demuxer         ff_vag_demuxer         ff_voc_demuxer
ff_vpk_demuxer         ff_vqf_demuxer         ff_w64_demuxer
ff_wady_demuxer        ff_wavarc_demuxer      ff_wsaud_demuxer
ff_wsd_demuxer         ff_xa_demuxer          ff_xmd_demuxer
ff_xvag_demuxer        ff_xwma_demuxer
```

**Excluded as ambiguous (name alone was not enough to confirm audio vs. a game-specific
video/archive format without further research — listed here rather than guessed into
either bucket):** `ff_ace_demuxer`, `ff_adf_demuxer`, `ff_dcstr_demuxer`, `ff_fwse_demuxer`,
`ff_luodat_demuxer`, `ff_mca_demuxer`, `ff_mcc_demuxer`, `ff_msp_demuxer`, `ff_rsd_demuxer`,
`ff_sdns_demuxer`, `ff_sdr2_demuxer`, `ff_sga_demuxer`.

## Confirmed absent

- **OptimFROG** — no `ff_*decoder`/`ff_*demuxer` symbol containing `ofr` or `optimfrog` in
  either archive. FFmpeg upstream has never carried an OptimFROG codec, so this is an
  upstream limitation, not a PacBrew packaging gap.

No other format named in the task brief's explicit checklist came back absent — see the
DSD finding above, which the brief expected to land here instead.

## Reproduction

This reproduces [`FFMPEG_SYMBOL_AUDIT.md`](FFMPEG_SYMBOL_AUDIT.md)'s own method, filtered to
the audio-relevant suffixes. The project's own `ps5dev-env.ps1` + PacBrew toolchain is a
Windows PowerShell setup; this run instead obtained the identical PacBrew v0.37 release
tarball directly from its canonical GitHub release and audited the same two archives with
LLVM's `nm`, which produces identical output to the PS5 SDK's own `llvm-nm` wrapper (both
are upstream LLVM `nm` built against the same object-format support). The tarball is the
same `v0.37`-tagged GitHub release asset that `ps5dev-env.ps1` would otherwise place at
`<toolchains>/pacbrew-v0.37/homebrew/lib/` — same canonical source, same build — this run
just skipped the PowerShell wrapper and extracted the two archives directly, since no
PowerShell/Windows host or existing sibling checkout was available on this machine.

```sh
# 1. Obtain PacBrew v0.37 (the exact version pinned in CMakeLists.txt / ARCHITECTURE.md).
#    This project's ps5dev-env.ps1 pulls PacBrew from a sibling toolchain directory that is
#    not part of this repo checkout. The release tarball below is the toolchain's own
#    canonical distribution and produces the same libavformat.a/libavcodec.a.
curl -L -o ps5-payload-dev.tar.gz \
  https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.37/ps5-payload-dev.tar.gz
# SHA/size check: 339461839 bytes, release published 2026-07-07T21:37:25Z.

tar xzf ps5-payload-dev.tar.gz \
  opt/ps5-payload-sdk/target/user/homebrew/lib/libavcodec.a \
  opt/ps5-payload-sdk/target/user/homebrew/lib/libavformat.a

# 2. Enumerate defined symbols with LLVM nm and count unique names matching these suffixes.
NM=llvm-nm   # any upstream LLVM nm works; the PS5 SDK's own llvm-nm wraps the same tool
cd opt/ps5-payload-sdk/target/user/homebrew/lib

$NM --defined-only libavformat.a | awk '{print $3}' \
  | grep -E '^ff_.*_demuxer$' | sort -u > demuxers.txt
wc -l demuxers.txt                      # 353

$NM --defined-only libavcodec.a | awk '{print $3}' \
  | grep -E '^ff_.*_decoder$' | sort -u > decoders.txt
wc -l decoders.txt                      # 500

# 3. Filter for audio relevance by codec/container name (no code execution required —
#    this is pure symbol-name classification against FFmpeg's own public codec/format
#    naming, cross-checked against the upstream libavcodec/codec_desc.c and
#    libavformat demuxer registration names).
```

Audited inputs:

```text
opt/ps5-payload-sdk/target/user/homebrew/lib/libavcodec.a    (from PacBrew v0.37 release tarball)
opt/ps5-payload-sdk/target/user/homebrew/lib/libavformat.a   (from PacBrew v0.37 release tarball)
```

On a machine with the project's normal Windows toolchain set up, the same two archives live
at:

```text
<toolchains>/pacbrew-v0.37/homebrew/lib/libavformat.a
<toolchains>/pacbrew-v0.37/homebrew/lib/libavcodec.a
```

## Research cross-checks

- [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) releases
  page lists a tagged, versioned release for every PacBrew version including `v0.37`
  (`https://github.com/ps5-payload-dev/pacbrew-repo/releases/tag/v0.37`), each with its own
  `ps5-payload-dev.tar.gz` asset — not just the latest tag. This is how this audit obtained
  the exact pinned version without a working `ps5dev-env.ps1` sibling checkout.
- [FFmpeg/FFmpeg](https://github.com/FFmpeg/FFmpeg) is the upstream reference for every
  decoder/demuxer name used to classify symbols as audio-relevant in this document.

The local PacBrew archives remain authoritative for what this exact ELF links; this
document and `FFMPEG_SYMBOL_AUDIT.md` can both be regenerated from the same tarball.
