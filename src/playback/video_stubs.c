/*
 * Link-time stubs for vendored SDL_kitchensink references to video and
 * image-subtitle features that this fork does not have an implementation
 * for.
 *
 * BFplayer is now an audio-only music player: src/main.cpp always opens
 * Kit_CreatePlayer() with subtitle_stream_index == -1, so
 * Kit_CreateSubtitleDecoder() never reaches the AV_CODEC_ID_DVD_SUBTITLE /
 * AV_CODEC_ID_DVB_SUBTITLE / AV_CODEC_ID_HDMV_PGS_SUBTITLE case in
 * vendor/SDL_kitchensink/src/internal/subtitle/kitsubtitle.c that calls
 * Kit_CreateImageSubtitleRenderer(). The video stream passed to
 * Kit_CreatePlayer() is still Kit_GetBestSourceStream(..., KIT_STREAMTYPE_VIDEO),
 * so an embedded cover-art ("attached_pic") stream can still open a real
 * video decoder in vendor/SDL_kitchensink/src/internal/video/kitvideo.c --
 * but cover art is never tagged with a PQ/HLG transfer function, so
 * Kit_ConfigureHdrLut() returns before calling any of the five
 * bfplayer_*hdr*() entry points below (see the early
 * `transfer != AVCOL_TRC_SMPTE2084 && transfer != AVCOL_TRC_ARIB_STD_B67`
 * check in kitvideo.c). In both cases the real HDR tone-map
 * (src/core/hdr_tonemap.cpp) and the hardened image-subtitle renderer
 * (src/playback/kitsubimage_safe.c) were deleted in 540ae6c as part of
 * removing video-only playback code, but the vendored kitchensink callers
 * were not patched to stop referencing them. These stubs only exist to
 * satisfy the linker; they do not reimplement any of the deleted logic.
 * Each one fails in the exact way its caller already handles:
 *
 * - bfplayer_build_hdr_yuv420_lut(): kitvideo.c's Kit_ConfigureHdrLut()
 *   treats a non-zero return as "could not build the LUT", frees the
 *   partially-built table, and returns 0 so the frame is presented without
 *   HDR-to-SDR tone-mapping (exactly its existing out-of-memory path).
 *   Returning non-zero here is safe and never dereferences `lut`.
 *
 * - bfplayer_create_hdr_worker_pool(): its result is stored unchecked in
 *   video_decoder->hdr_workers, but because bfplayer_build_hdr_yuv420_lut()
 *   above always fails first, kitvideo.c's HDR path returns before this is
 *   ever called from Kit_ConfigureHdrLut(). Returning NULL is still the
 *   correct "pool unavailable" value for any caller that does check it.
 *
 * - bfplayer_apply_hdr_yuv420_lut_parallel(): kitvideo.c's
 *   Kit_ApplyHdrToneMap() treats a non-zero return as "could not apply the
 *   LUT" and returns without touching color metadata, leaving the frame as
 *   decoded. It is unreachable in practice because Kit_ConfigureHdrLut()
 *   never succeeds (see above), but a non-zero return is safe either way:
 *   the stub never writes through the y/u/v plane pointers it is handed.
 *
 * - bfplayer_destroy_hdr_worker_pool(): called unconditionally from
 *   kitvideo.c's dec_close_video_cb(), including when hdr_workers is NULL
 *   (the normal case, since the pool is never created -- see above). Must
 *   tolerate a NULL pool, like free(NULL).
 *
 * - bfplayer_hdr_worker_count(): called unconditionally from kitvideo.c's
 *   Kit_GetVideoDecoderHdrStatus() to fill in a diagnostics field. Must
 *   tolerate a NULL pool and report zero workers.
 *
 * - Kit_CreateImageSubtitleRenderer(): vendor/SDL_kitchensink/src/internal/
 *   subtitle/kitsubtitle.c already treats a NULL renderer as a creation
 *   failure (`if(renderer == NULL) goto exit_3;`), frees the atlas it
 *   allocated, and closes the decoder cleanly. Returning NULL reproduces
 *   that exact, already-handled failure path -- it is also the path
 *   src/playback/kitsubimage_safe.c took deliberately, to keep upstream's
 *   kitsubimage.c (with its unchecked reallocs and palette-copy bug) out of
 *   the link, without silently re-admitting it here.
 */

#include "bfplayer/hdr_yuv_tonemap.h"

#include "kitchensink2/internal/kitdecoder.h"
#include "kitchensink2/internal/subtitle/renderers/kitsubrenderer.h"

int bfplayer_build_hdr_yuv420_lut(
    const BfplayerHdrYuvConfig* config,
    BfplayerHdrYuvLut* lut) {
    (void)config;
    (void)lut;
    return 1;
}

BfplayerHdrWorkerPool* bfplayer_create_hdr_worker_pool(
    unsigned int worker_count) {
    (void)worker_count;
    return NULL;
}

void bfplayer_destroy_hdr_worker_pool(
    BfplayerHdrWorkerPool* pool) {
    (void)pool;
}

unsigned int bfplayer_hdr_worker_count(
    const BfplayerHdrWorkerPool* pool) {
    (void)pool;
    return 0;
}

int bfplayer_apply_hdr_yuv420_lut_parallel(
    BfplayerHdrWorkerPool* pool,
    const BfplayerHdrYuvLut* lut,
    uint8_t* y_plane,
    int y_stride,
    uint8_t* u_plane,
    int u_stride,
    uint8_t* v_plane,
    int v_stride,
    int width,
    int height) {
    (void)pool;
    (void)lut;
    (void)y_plane;
    (void)y_stride;
    (void)u_plane;
    (void)u_stride;
    (void)v_plane;
    (void)v_stride;
    (void)width;
    (void)height;
    return 1;
}

Kit_SubtitleRenderer* Kit_CreateImageSubtitleRenderer(
    Kit_Decoder* dec,
    int video_w,
    int video_h,
    int screen_w,
    int screen_h) {
    (void)dec;
    (void)video_w;
    (void)video_h;
    (void)screen_w;
    (void)screen_h;
    return NULL;
}
