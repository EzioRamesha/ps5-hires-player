#include <SDL.h>
#include <kitchensink2/kitchensink.h>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

#include "bfplayer/artwork.hpp"
#include "bfplayer/controller_buttons.hpp"
#include "bfplayer/diagnostics.hpp"
#include "bfplayer/hdr_yuv_tonemap.h"
#include "bfplayer/library_database.hpp"
#include "bfplayer/library_scanner.hpp"
#include "bfplayer/library_ui.hpp"
#include "bfplayer/list_navigation.hpp"
#include "bfplayer/playlist.hpp"
#include "bfplayer/playback_osd.hpp"
#include "bfplayer/player_settings.hpp"
#include "bfplayer/remote_control.hpp"
#include "bfplayer/safe_read_file.hpp"
#include "bfplayer/source_uri.hpp"

#include <SDL_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <fcntl.h>

#if defined(BFPLAYER_PS5)
extern "C" int BFplayer_PS5_IsNativeHdrOutput(void)
    __attribute__((weak));
extern "C" int BFplayer_PS5_SupportsNativeHdr(void)
    __attribute__((weak));
extern "C" int BFplayer_PS5_SetNativeHdrOutput(int enabled)
    __attribute__((weak));
extern "C" int BFplayer_PS5_PrepareNativeHdrHlg(void)
    __attribute__((weak));
extern "C" int BFplayer_PS5_PresentNativeHdrYuv420P10Frame(
    const void* y_plane,
    int y_pitch_bytes,
    const void* u_plane,
    int u_pitch_bytes,
    const void* v_plane,
    int v_pitch_bytes,
    int width,
    int height,
    int output_width,
    int output_height,
    int source_x,
    int source_y,
    int source_width,
    int source_height,
    int destination_x,
    int destination_y,
    int destination_width,
    int destination_height,
    int full_range,
    int hlg_to_pq,
    const void* overlay_pixels,
    int overlay_pitch_bytes,
    int overlay_width,
    int overlay_height,
    int overlay_x,
    int overlay_y,
    int overlay_region_width,
    int overlay_region_height) __attribute__((weak));
#endif

namespace {

extern "C" int sceSystemServiceGetAppIdOfRunningBigApp(void);
extern "C" int sceSystemServiceGetAppTitleId(int app_id, char* title_id);
extern "C" int sceSystemServiceKillApp(
    int app_id,
    int how,
    int reason,
    int core_dump);

#ifndef BFPLAYER_VERSION
#define BFPLAYER_VERSION "development"
#endif

constexpr int kWindowWidth = 1920;
constexpr int kWindowHeight = 1080;
// Margin kept clear around the centered now-playing artwork so it never
// touches the screen edges.
constexpr int kNowPlayingArtworkMargin = 140;
// Height of PlaybackOsd's transport strip (src/ui/playback_osd.cpp's
// `SDL_Rect panel{0, kScreenHeight - 170, kScreenWidth, 170}`), which is
// painted fully opaque on purpose. The now-playing artwork must be centered
// above this strip, not in the full window, or its bottom edge sits under
// the (eventually auto-shown) strip for any cover with aspect ratio <= ~2:1
// -- i.e. effectively all real album art.
constexpr int kNowPlayingOsdStripHeight = 170;
constexpr int kPreferredOutputWidth = 3840;
constexpr int kPreferredOutputHeight = 2160;
constexpr int kPreferredOutputRefresh = 60;
constexpr std::uint16_t kRemoteControlPort = 9042;
constexpr int kAudioBufferBytes = 64 * 1024;
constexpr int kVideoDecoderThreads = 16;
constexpr int kVideoPacketBufferCount = 64;
constexpr int kVideoFrameBufferCount = 3;
constexpr int kNetworkVideoPacketBufferCount = 128;
constexpr int kNetworkVideoFrameBufferCount = 4;
constexpr int kAudioPacketBufferCount = 64;
constexpr int kNetworkAudioPacketBufferCount = 128;
constexpr int kAudioFrameBufferCount = 64;
constexpr std::uint64_t kAudioBytesPerSecond = 48000ULL * 2ULL * 2ULL;

bool ps5_native_hdr_output_active() noexcept {
#if defined(BFPLAYER_PS5)
    return BFplayer_PS5_IsNativeHdrOutput != nullptr &&
        BFplayer_PS5_PresentNativeHdrYuv420P10Frame != nullptr &&
        BFplayer_PS5_IsNativeHdrOutput() != 0;
#else
    return false;
#endif
}

bool ps5_native_hdr_output_supported() noexcept {
#if defined(BFPLAYER_PS5)
    return BFplayer_PS5_SupportsNativeHdr != nullptr &&
        BFplayer_PS5_SetNativeHdrOutput != nullptr &&
        BFplayer_PS5_PresentNativeHdrYuv420P10Frame != nullptr &&
        BFplayer_PS5_SupportsNativeHdr() != 0;
#else
    return false;
#endif
}

int set_ps5_native_hdr_output(bool enabled) noexcept {
#if defined(BFPLAYER_PS5)
    if (BFplayer_PS5_SetNativeHdrOutput != nullptr) {
        return BFplayer_PS5_SetNativeHdrOutput(enabled ? 1 : 0);
    }
#else
    (void)enabled;
#endif
    return enabled ? -1 : 0;
}

int prepare_ps5_native_hdr_hlg() noexcept {
#if defined(BFPLAYER_PS5)
    return BFplayer_PS5_PrepareNativeHdrHlg != nullptr
        ? BFplayer_PS5_PrepareNativeHdrHlg()
        : -1;
#else
    return -1;
#endif
}

int present_ps5_native_hdr_yuv420p10_frame(
    const void* y_plane,
    int y_pitch_bytes,
    const void* u_plane,
    int u_pitch_bytes,
    const void* v_plane,
    int v_pitch_bytes,
    int width,
    int height,
    int output_width,
    int output_height,
    int source_x,
    int source_y,
    int source_width,
    int source_height,
    int destination_x,
    int destination_y,
    int destination_width,
    int destination_height,
    bool full_range,
    bool hlg_to_pq,
    const void* overlay_pixels,
    int overlay_pitch_bytes,
    int overlay_width,
    int overlay_height,
    int overlay_x,
    int overlay_y,
    int overlay_region_width,
    int overlay_region_height) noexcept {
#if defined(BFPLAYER_PS5)
    if (BFplayer_PS5_PresentNativeHdrYuv420P10Frame != nullptr) {
        return BFplayer_PS5_PresentNativeHdrYuv420P10Frame(
            y_plane,
            y_pitch_bytes,
            u_plane,
            u_pitch_bytes,
            v_plane,
            v_pitch_bytes,
            width,
            height,
            output_width,
            output_height,
            source_x,
            source_y,
            source_width,
            source_height,
            destination_x,
            destination_y,
            destination_width,
            destination_height,
            full_range ? 1 : 0,
            hlg_to_pq ? 1 : 0,
            overlay_pixels,
            overlay_pitch_bytes,
            overlay_width,
            overlay_height,
            overlay_x,
            overlay_y,
            overlay_region_width,
            overlay_region_height);
    }
#else
    (void)y_plane;
    (void)y_pitch_bytes;
    (void)u_plane;
    (void)u_pitch_bytes;
    (void)v_plane;
    (void)v_pitch_bytes;
    (void)width;
    (void)height;
    (void)output_width;
    (void)output_height;
    (void)source_x;
    (void)source_y;
    (void)source_width;
    (void)source_height;
    (void)destination_x;
    (void)destination_y;
    (void)destination_width;
    (void)destination_height;
    (void)full_range;
    (void)hlg_to_pq;
    (void)overlay_pixels;
    (void)overlay_pitch_bytes;
    (void)overlay_width;
    (void)overlay_height;
    (void)overlay_x;
    (void)overlay_y;
    (void)overlay_region_width;
    (void)overlay_region_height;
#endif
    return -1;
}

struct PlaybackBufferPolicy {
    int video_packets = kVideoPacketBufferCount;
    int video_frames = kVideoFrameBufferCount;
    int audio_packets = kAudioPacketBufferCount;
    int audio_frames = kAudioFrameBufferCount;
};

PlaybackBufferPolicy playback_buffer_policy(bool network) {
    if (!network) {
        return {};
    }
    return {
        kNetworkVideoPacketBufferCount,
        kNetworkVideoFrameBufferCount,
        kNetworkAudioPacketBufferCount,
        kAudioFrameBufferCount,
    };
}

void apply_playback_buffer_policy(const PlaybackBufferPolicy& policy) {
    Kit_SetHint(KIT_HINT_VIDEO_BUFFER_PACKETS, policy.video_packets);
    Kit_SetHint(KIT_HINT_VIDEO_BUFFER_FRAMES, policy.video_frames);
    Kit_SetHint(KIT_HINT_AUDIO_BUFFER_PACKETS, policy.audio_packets);
    Kit_SetHint(KIT_HINT_AUDIO_BUFFER_FRAMES, policy.audio_frames);
}

std::uint64_t process_peak_rss_kib() noexcept {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(usage.ru_maxrss);
}

double timeval_seconds(const timeval& value) noexcept {
    return static_cast<double>(value.tv_sec) +
        static_cast<double>(value.tv_usec) / 1'000'000.0;
}

double process_cpu_seconds(const rusage& usage) noexcept {
    return timeval_seconds(usage.ru_utime) + timeval_seconds(usage.ru_stime);
}

double elapsed_performance_ms(
    Uint64 start,
    Uint64 end,
    Uint64 frequency) noexcept {
    if (end < start || frequency == 0) {
        return 0.0;
    }
    return static_cast<double>(end - start) * 1000.0 /
        static_cast<double>(frequency);
}

struct TimingWindow {
    double total_ms = 0.0;
    double maximum_ms = 0.0;
    std::uint64_t count = 0;
    std::array<std::uint32_t, 256> histogram{};

    void add(double milliseconds) noexcept {
        if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
            return;
        }
        total_ms += milliseconds;
        maximum_ms = std::max(maximum_ms, milliseconds);
        ++count;
        const std::size_t bucket = static_cast<std::size_t>(
            std::clamp(milliseconds, 0.0, 255.0));
        ++histogram[bucket];
    }

    [[nodiscard]] double average() const noexcept {
        return count > 0 ? total_ms / static_cast<double>(count) : 0.0;
    }

    [[nodiscard]] double percentile(double fraction) const noexcept {
        if (count == 0) {
            return 0.0;
        }
        const std::uint64_t wanted = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(
                std::ceil(static_cast<double>(count) * fraction)));
        std::uint64_t accumulated = 0;
        for (std::size_t index = 0; index < histogram.size(); ++index) {
            accumulated += histogram[index];
            if (accumulated >= wanted) {
                return static_cast<double>(index);
            }
        }
        return 255.0;
    }

    void reset() noexcept {
        total_ms = 0.0;
        maximum_ms = 0.0;
        count = 0;
        histogram.fill(0);
    }
};

enum class PlaybackOverlay {
    none,
    menu,
    controls,
    settings,
};

// Background job state for the now-playing artwork decode (see
// update_now_playing_artwork()). The worker thread (now_playing_artwork_job_
// entry()) only ever touches the "request_"/"result_"/"ready" fields below,
// and only while holding `mutex`; the main/render thread does the same. The
// decoded SDL_Surface crosses from the worker to the main thread this way,
// but the SDL_Texture created from it is only ever touched on the main
// thread (consume_now_playing_artwork_job()), since SDL textures are tied
// to the renderer and are not safe to create off its thread.
struct NowPlayingArtworkJob {
    SDL_Thread* thread = nullptr;
    SDL_mutex* mutex = nullptr;
    std::atomic<bool> cancel{false};
    // Guarded by `mutex`:
    std::string request_path;
    bool ready = false;
    std::string result_artwork_path;
    SDL_Surface* result_surface = nullptr;
    SDL_Color result_color{0, 0, 0, 255};
};

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_GameController* controller = nullptr;
    Kit_Source* source = nullptr;
    Kit_Player* player = nullptr;
    SDL_Texture* video = nullptr;
    SDL_AudioDeviceID audio = 0;
    bfplayer::SafeReadFile local_media;
    bfplayer::PlaybackOsd osd;
    bfplayer::RemoteControlServer remote_control;
    std::string current_media_path;
    // Now-playing artwork shown in place of a black screen during audio-only
    // playback. Cached per track; see update_now_playing_artwork(). The
    // decode itself happens on now_playing_artwork_job's worker thread, off
    // the render/audio-feed loop; only the texture fields below are ever
    // touched by anything other than that worker.
    SDL_Texture* now_playing_artwork = nullptr;
    int now_playing_artwork_width = 0;
    int now_playing_artwork_height = 0;
    // The media path this artwork was (attempted to be) loaded for, used to
    // detect a track change. Kept even when no artwork was found, so a
    // missing/corrupt-artwork track is not re-probed every frame.
    std::string now_playing_artwork_media_path;
    std::string now_playing_artwork_path;
    SDL_Color now_playing_background_color{0, 0, 0, 255};
    NowPlayingArtworkJob now_playing_artwork_job;
    std::string fallback_font;
    std::string ui_logo;
    std::string pending_remote_media_path;
    double source_frame_rate = 0.0;
    int source_width = 0;
    int source_height = 0;
    bool source_hdr = false;
    std::string source_hdr_transfer;
    int volume_percent = 100;
    int previous_volume_percent = 100;
    double source_display_aspect = 0.0;
    int output_width = kWindowWidth;
    int output_height = kWindowHeight;
    bool true_4k_output = false;
    bool native_hdr_output = false;
    bfplayer::PlayerSettings settings;
    PlaybackOverlay playback_overlay = PlaybackOverlay::none;
    int playback_overlay_selected = 0;
    bfplayer::ListNavigationRepeat navigation_repeat;
    bool running = true;
    bool playback_running = false;
    bool paused = false;
    bool redraw_requested = true;
    std::atomic<bool> io_cancel{false};
    std::atomic<std::int64_t> source_open_deadline_ms{0};
};

void update_decoder_buffer_status(
    const App& app,
    bfplayer::RemotePlaybackStatus& status) {
    if (!app.player) {
        return;
    }
    if (Kit_GetPlayerVideoStream(app.player) >= 0) {
        Kit_GetPlayerVideoBufferState(
            app.player,
            &status.video_frames_length,
            &status.video_frames_capacity,
            &status.video_packets_length,
            &status.video_packets_capacity);
    }
    if (Kit_GetPlayerAudioStream(app.player) >= 0) {
        Kit_GetPlayerAudioBufferState(
            app.player,
            &status.audio_frames_length,
            &status.audio_frames_capacity,
            &status.audio_packets_length,
            &status.audio_packets_capacity);
    }
}

Kit_VideoToneMapInfo update_tone_map_status(
    const App& app,
    bfplayer::RemotePlaybackStatus& status) {
    Kit_VideoToneMapInfo info{};
    if (app.player) {
        Kit_GetPlayerVideoToneMapInfo(app.player, &info);
    }
    status.hdr_tone_map_active = info.active != 0;
    status.hdr_input_full_range = info.input_full_range != 0;
    status.hdr_input_bt2020 = info.input_bt2020 != 0;
    status.hdr_source_peak_nits = info.source_peak_nits;
    status.hdr_target_peak_nits = info.target_peak_nits;
    status.hdr_tone_map_frames = info.frames;
    status.hdr_tone_map_time_us = info.processing_us;
    status.hdr_tone_map_workers = info.workers;
    status.hdr_tone_map_average_ms =
        info.frames > 0
        ? static_cast<double>(info.processing_us) /
              static_cast<double>(info.frames) /
              1000.0
        : 0.0;
    return info;
}

enum class PlaybackOutcome {
    finished,
    user_return,
    error,
};

enum class PlayerLockResult {
    acquired,
    already_running,
    error,
};

void boot_stage_stderr(const char* stage) noexcept {
    std::fprintf(
        stderr,
        "BFPLAYER_BOOT_STAGE stage=%s pid=%ld\n",
        stage ? stage : "<unknown>",
        static_cast<long>(getpid()));
    std::fflush(stderr);
}

void boot_stage(const char* stage) noexcept {
    boot_stage_stderr(stage);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "boot-stage stage=%s pid=%ld",
        stage ? stage : "<unknown>",
        static_cast<long>(getpid()));
}

class PlayerInstanceLock {
public:
    PlayerInstanceLock() = default;
    PlayerInstanceLock(const PlayerInstanceLock&) = delete;
    PlayerInstanceLock& operator=(const PlayerInstanceLock&) = delete;

    ~PlayerInstanceLock() {
        if (descriptor_ >= 0) {
            (void)flock(descriptor_, LOCK_UN);
            close(descriptor_);
        }
    }

    [[nodiscard]] PlayerLockResult acquire() noexcept {
        constexpr const char* directory = "/data/BFplayer";
        constexpr const char* path = "/data/BFplayer/player.lock";
        if (mkdir(directory, 0777) != 0 && errno != EEXIST) {
            return PlayerLockResult::error;
        }
        descriptor_ = open(path, O_RDWR | O_CREAT, 0600);
        if (descriptor_ < 0) {
            return PlayerLockResult::error;
        }
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int lock_error = errno;
            close(descriptor_);
            descriptor_ = -1;
            return lock_error == EWOULDBLOCK || lock_error == EAGAIN
                ? PlayerLockResult::already_running
                : PlayerLockResult::error;
        }

        char owner[64];
        const int length = std::snprintf(
            owner,
            sizeof(owner),
            "pid=%ld\n",
            static_cast<long>(getpid()));
        if (length > 0) {
            (void)ftruncate(descriptor_, 0);
            (void)lseek(descriptor_, 0, SEEK_SET);
            (void)write(
                descriptor_,
                owner,
                static_cast<std::size_t>(
                    std::min(length, static_cast<int>(sizeof(owner) - 1))));
        }
        return PlayerLockResult::acquired;
    }

private:
    int descriptor_ = -1;
};

struct VideoSourceInfo {
    const char* codec = "none";
    const char* pixel_format = "unknown";
    int profile = -1;
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int sample_aspect_numerator = 0;
    int sample_aspect_denominator = 0;
    double display_aspect = 0.0;
    double frame_rate = 0.0;
    std::int64_t bit_rate = 0;
    const char* color_range = "unknown";
    const char* color_space = "unknown";
    const char* color_transfer = "unknown";
    const char* color_primaries = "unknown";
    AVColorRange color_range_value = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace color_space_value = AVCOL_SPC_UNSPECIFIED;
    AVColorTransferCharacteristic color_transfer_value =
        AVCOL_TRC_UNSPECIFIED;
    bool hdr_source = false;
    bool demanding_software_decode = false;
};

void close_media(App& app);

void log_sdl(const char* operation) {
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::error,
        "sdl operation=%s error=%s",
        operation ? operation : "<unknown>",
        SDL_GetError());
}

// Converts decoded frame dimensions plus a sample/pixel aspect ratio (SAR)
// into the final display aspect ratio (DAR). Invalid SAR falls back to square
// pixels, so a 1920x1080 frame remains 16:9 rather than becoming 1:1.
// (Reimplemented locally: this pure geometry helper used to live in the
// video-layout-mode module removed with the subtitle/HDR-layout feature set,
// but the HDR presentation path below still needs the resulting aspect
// ratio.)
double display_aspect_from_sample_aspect(
    int frame_width,
    int frame_height,
    int sample_aspect_numerator,
    int sample_aspect_denominator) noexcept {
    if (frame_width <= 0 || frame_height <= 0) {
        return 0.0;
    }
    const double frame_aspect =
        static_cast<double>(frame_width) / static_cast<double>(frame_height);
    if (sample_aspect_numerator <= 0 || sample_aspect_denominator <= 0) {
        return frame_aspect;
    }
    const double display_aspect =
        frame_aspect *
        static_cast<double>(sample_aspect_numerator) /
        static_cast<double>(sample_aspect_denominator);
    return std::isfinite(display_aspect) && display_aspect > 0.0
        ? display_aspect
        : frame_aspect;
}

VideoSourceInfo inspect_video_source(
    const AVFormatContext* format,
    int stream_index) {
    VideoSourceInfo info{};
    if (!format || stream_index < 0 ||
        stream_index >= static_cast<int>(format->nb_streams)) {
        return info;
    }
    const AVStream* stream = format->streams[stream_index];
    if (!stream || !stream->codecpar) {
        return info;
    }
    const AVCodecParameters* parameters = stream->codecpar;
    info.codec = avcodec_get_name(parameters->codec_id);
    info.profile = parameters->profile;
    info.width = parameters->width;
    info.height = parameters->height;
    const AVRational guessed_aspect = av_guess_sample_aspect_ratio(
        const_cast<AVFormatContext*>(format),
        const_cast<AVStream*>(stream),
        nullptr);
    if (guessed_aspect.num > 0 && guessed_aspect.den > 0) {
        info.sample_aspect_numerator = guessed_aspect.num;
        info.sample_aspect_denominator = guessed_aspect.den;
    }
    info.display_aspect = display_aspect_from_sample_aspect(
        info.width,
        info.height,
        info.sample_aspect_numerator,
        info.sample_aspect_denominator);
    info.bit_rate = parameters->bit_rate > 0
        ? parameters->bit_rate
        : format->bit_rate;
    if (const char* name = av_color_range_name(parameters->color_range)) {
        info.color_range = name;
    }
    info.color_range_value = parameters->color_range;
    if (const char* name = av_color_space_name(parameters->color_space)) {
        info.color_space = name;
    }
    info.color_space_value = parameters->color_space;
    if (const char* name =
            av_color_transfer_name(parameters->color_trc)) {
        info.color_transfer = name;
    }
    info.color_transfer_value = parameters->color_trc;
    if (const char* name =
            av_color_primaries_name(parameters->color_primaries)) {
        info.color_primaries = name;
    }
    info.hdr_source =
        parameters->color_trc == AVCOL_TRC_SMPTE2084 ||
        parameters->color_trc == AVCOL_TRC_ARIB_STD_B67;
    const auto pixel_format =
        static_cast<AVPixelFormat>(parameters->format);
    if (const char* name = av_get_pix_fmt_name(pixel_format)) {
        info.pixel_format = name;
    }
    if (const AVPixFmtDescriptor* descriptor =
            av_pix_fmt_desc_get(pixel_format)) {
        info.bit_depth = descriptor->comp[0].depth;
    }
    const AVRational guessed_rate =
        av_guess_frame_rate(const_cast<AVFormatContext*>(format),
                            const_cast<AVStream*>(stream), nullptr);
    if (guessed_rate.num > 0 && guessed_rate.den > 0) {
        info.frame_rate = av_q2d(guessed_rate);
    }
    const std::int64_t pixels =
        static_cast<std::int64_t>(info.width) * info.height;
    const bool expensive_codec =
        parameters->codec_id == AV_CODEC_ID_VP9 ||
        parameters->codec_id == AV_CODEC_ID_AV1 ||
        parameters->codec_id == AV_CODEC_ID_HEVC;
    info.demanding_software_decode =
        pixels >= 3840LL * 2160LL &&
        (info.frame_rate >= 50.0 ||
         (expensive_codec && info.bit_depth > 8));
    return info;
}

Kit_VideoFormatRequest make_video_format_request(
    const VideoSourceInfo& source,
    int output_width,
    int output_height,
    bool native_hdr_output) {
    Kit_VideoFormatRequest request{};
    Kit_ResetVideoFormatRequest(&request);
    /*
     * Kitchensink can preserve 10-bit planar frames for BFplayer's direct
     * PS5 presentation path, but SDL textures cannot upload I010. Keep every
     * non-direct path explicitly on SDL's supported IYUV format instead of
     * allowing a 10-bit SDR source to select the private I010 format.
     */
    request.format = native_hdr_output
        ? KIT_PIXELFORMAT_YUV420P10LE
        : SDL_PIXELFORMAT_IYUV;
    if (output_width < 2 || output_height < 2) {
        output_width = kWindowWidth;
        output_height = kWindowHeight;
    }
    if (source.width < 1 || source.height < 1 ||
        (source.width <= output_width && source.height <= output_height)) {
        return request;
    }
    const double scale = std::min(
        static_cast<double>(output_width) / source.width,
        static_cast<double>(output_height) / source.height);
    request.width = std::max(
        2,
        static_cast<int>(std::floor(source.width * scale)) & ~1);
    request.height = std::max(
        2,
        static_cast<int>(std::floor(source.height * scale)) & ~1);
    return request;
}

void sdl_log_output(
    void*,
    int category,
    SDL_LogPriority priority,
    const char* message) {
    const bfplayer::DiagnosticLevel level =
        priority >= SDL_LOG_PRIORITY_ERROR
            ? bfplayer::DiagnosticLevel::error
            : (priority == SDL_LOG_PRIORITY_WARN
                   ? bfplayer::DiagnosticLevel::warning
                   : bfplayer::DiagnosticLevel::info);
    bfplayer::diagnostics_log(
        level,
        "sdl-log category=%d priority=%d message=%s",
        category,
        static_cast<int>(priority),
        message ? message : "<null>");
}

std::string executable_directory(const char* executable_path) {
    if (!executable_path || executable_path[0] != '/') {
        return {};
    }
    const char* slash = std::strrchr(executable_path, '/');
    if (!slash) {
        return {};
    }
    if (slash == executable_path) {
        return "/";
    }
    return std::string(executable_path, static_cast<std::size_t>(slash - executable_path));
}

std::string executable_asset_path(const char* executable_path, const char* relative_path) {
    const std::string directory = executable_directory(executable_path);
    if (directory.empty()) {
        return relative_path ? relative_path : std::string{};
    }
    return directory + "/" + (relative_path ? relative_path : "");
}

void migrate_legacy_library_database() {
    constexpr const char* legacy_database =
        "/data/PS5-" "Media-" "Center/library.db";
    constexpr const char* current_directory = "/data/BFplayer";
    constexpr const char* current_database =
        "/data/BFplayer/library.db";
    struct stat destination_status {};
    if (lstat(current_database, &destination_status) == 0) {
        return;
    }
    if (errno != ENOENT) {
        return;
    }
    struct stat source_status {};
    if (lstat(legacy_database, &source_status) != 0 ||
        !S_ISREG(source_status.st_mode) ||
        S_ISLNK(source_status.st_mode) ||
        source_status.st_size < 1) {
        return;
    }
    if ((mkdir(current_directory, 0777) != 0 && errno != EEXIST)) {
        std::fprintf(
            stderr,
            "BFplayer migration: cannot create %s errno=%d\n",
            current_directory,
            errno);
        return;
    }
    const int input = open(legacy_database, O_RDONLY | O_NOFOLLOW);
    if (input < 0) {
        std::fprintf(
            stderr,
            "BFplayer migration: cannot open legacy database errno=%d\n",
            errno);
        return;
    }
    struct stat opened_source_status {};
    if (fstat(input, &opened_source_status) != 0 ||
        !S_ISREG(opened_source_status.st_mode) ||
        opened_source_status.st_dev != source_status.st_dev ||
        opened_source_status.st_ino != source_status.st_ino) {
        std::fprintf(
            stderr,
            "BFplayer migration: legacy database changed during open\n");
        close(input);
        return;
    }
    char temporary[96]{};
    std::snprintf(
        temporary,
        sizeof(temporary),
        "%s.migrate.%ld",
        current_database,
        static_cast<long>(getpid()));
    const int output = open(
        temporary,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
        0600);
    if (output < 0) {
        std::fprintf(
            stderr,
            "BFplayer migration: cannot create temporary database errno=%d\n",
            errno);
        close(input);
        return;
    }
    bool copied = true;
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        const ssize_t received = read(input, buffer.data(), buffer.size());
        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            copied = false;
            break;
        }
        ssize_t offset = 0;
        while (offset < received) {
            const ssize_t written = write(
                output,
                buffer.data() + offset,
                static_cast<std::size_t>(received - offset));
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                copied = false;
                break;
            }
            offset += written;
        }
        if (!copied) {
            break;
        }
    }
    if (copied && fsync(output) != 0) {
        copied = false;
    }
    close(input);
    if (close(output) != 0) {
        copied = false;
    }
    if (!copied || rename(temporary, current_database) != 0) {
        const int migration_errno = errno;
        (void)unlink(temporary);
        std::fprintf(
            stderr,
            "BFplayer migration: database copy failed errno=%d\n",
            migration_errno);
        return;
    }
    std::fprintf(
        stderr,
        "BFplayer migration: library database preserved at %s\n",
        current_database);
}

void log_installed_manifest(const char* executable_path) {
    const std::string manifest_path = executable_asset_path(executable_path, "build-manifest.json");
    FILE* manifest = std::fopen(manifest_path.c_str(), "rb");
    if (!manifest) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "build-manifest unavailable path=%s errno=%d",
            manifest_path.c_str(),
            errno);
        return;
    }
    char contents[4096]{};
    const std::size_t bytes = std::fread(contents, 1, sizeof(contents) - 1U, manifest);
    const bool complete = std::feof(manifest) != 0;
    std::fclose(manifest);
    contents[bytes] = '\0';
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "build-manifest bytes=%zu complete=%d content=%s",
        bytes,
        complete ? 1 : 0,
        contents);
}

bfplayer::PlayerSettings load_player_settings(
    bfplayer::LibraryDatabase& database) {
    bfplayer::PlayerSettings settings{};
    std::string value;
    int integer = 0;
    bool boolean = false;
    if (database.get_setting(
            std::string(bfplayer::kSettingVolumePercent),
            value) &&
        bfplayer::parse_setting_integer(value, 0, 100, integer)) {
        settings.volume_percent = integer;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingShortSeekSeconds),
            value) &&
        bfplayer::parse_setting_integer(value, 1, 300, integer)) {
        settings.short_seek_seconds = integer;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingLongSeekSeconds),
            value) &&
        bfplayer::parse_setting_integer(value, 1, 900, integer)) {
        settings.long_seek_seconds = integer;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingOsdDurationMs),
            value) &&
        bfplayer::parse_setting_integer(value, 500, 30000, integer)) {
        settings.osd_duration_ms = integer;
    }
    if (database.get_setting(
            std::string(bfplayer::kSettingResumePlayback),
            value) &&
        bfplayer::parse_setting_boolean(value, boolean)) {
        settings.resume_playback = boolean;
    }
    return bfplayer::normalized_player_settings(settings);
}

std::int64_t monotonic_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string ffmpeg_error(int value) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    return av_strerror(value, buffer, sizeof(buffer)) == 0
        ? std::string(buffer)
        : "FFmpeg error " + std::to_string(value);
}

std::string bounded_metadata(const char* value, std::size_t maximum) {
    if (!value) {
        return {};
    }
    std::size_t length = 0;
    while (length < maximum && value[length] != '\0') {
        ++length;
    }
    return std::string(value, length);
}

std::int64_t seconds_to_milliseconds(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return 0;
    }
    constexpr long double kMaximumSeconds =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max()) /
        1000.0L;
    if (static_cast<long double>(seconds) >= kMaximumSeconds) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(std::llround(seconds * 1000.0));
}

bool playback_completed(std::int64_t position_ms, std::int64_t duration_ms) {
    return position_ms >= 0 && duration_ms > 0 &&
        static_cast<long double>(position_ms) /
            static_cast<long double>(duration_ms) >= 0.92L;
}

int interrupt_source_io(void* opaque) {
    const auto* app = static_cast<const App*>(opaque);
    if (!app || app->io_cancel.load(std::memory_order_relaxed)) {
        return 1;
    }
    const std::int64_t deadline =
        app->source_open_deadline_ms.load(std::memory_order_relaxed);
    return deadline > 0 && monotonic_milliseconds() >= deadline ? 1 : 0;
}

int read_local_media(void* opaque, std::uint8_t* buffer, int length) {
    auto* file = static_cast<bfplayer::SafeReadFile*>(opaque);
    if (!file) {
        return AVERROR(EINVAL);
    }
    const int result = file->read(buffer, length);
    return result == 0 ? AVERROR_EOF : result;
}

std::int64_t seek_local_media(void* opaque, std::int64_t offset, int whence) {
    auto* file = static_cast<bfplayer::SafeReadFile*>(opaque);
    if (!file) {
        return AVERROR(EINVAL);
    }
    if ((whence & AVSEEK_SIZE) == AVSEEK_SIZE) {
        return file->size() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ? AVERROR(EOVERFLOW)
            : static_cast<std::int64_t>(file->size());
    }
    const int origin = whence & ~AVSEEK_FORCE;
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
        return AVERROR(EINVAL);
    }
    return file->seek(offset, origin);
}

void free_custom_avio(AVIOContext*& context) {
    if (context) {
        av_freep(&context->buffer);
        avio_context_free(&context);
    }
}

Kit_Source* create_bounded_source(
    App& app,
    const char* path,
    std::string& error) {
    error.clear();
    if (!path || !path[0]) {
        error = "Empty media source";
        bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::error, "source-open rejected reason=empty");
        return nullptr;
    }
    const bool network = bfplayer::is_network_uri(path);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "source-open begin kind=%s path=%s",
        network ? "network" : "local",
        bfplayer::redact_uri_secrets(path).c_str());
    if (network && !bfplayer::is_supported_stream_uri(path)) {
        error = "Unsupported network protocol";
        bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::error, "source-open rejected reason=unsupported-protocol");
        return nullptr;
    }
    if (network && bfplayer::uri_has_credentials(path)) {
        error = "Network URLs containing usernames or passwords are rejected";
        bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::error, "source-open rejected reason=credentials");
        return nullptr;
    }

    app.local_media.close();
    app.io_cancel.store(false, std::memory_order_relaxed);
    app.source_open_deadline_ms.store(
        monotonic_milliseconds() + 30000,
        std::memory_order_relaxed);

    AVFormatContext* format = avformat_alloc_context();
    AVIOContext* custom_io = nullptr;
    if (!format) {
        error = "Unable to allocate FFmpeg source context";
        app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
        return nullptr;
    }
    format->interrupt_callback.callback = interrupt_source_io;
    format->interrupt_callback.opaque = &app;

    if (!network) {
        if (!app.local_media.open(path, error)) {
            avformat_free_context(format);
            app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
            return nullptr;
        }
        constexpr int kAvioBufferBytes = 64 * 1024;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(kAvioBufferBytes));
        if (!buffer) {
            error = "Unable to allocate local media I/O buffer";
            app.local_media.close();
            avformat_free_context(format);
            app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
            return nullptr;
        }
        custom_io = avio_alloc_context(
            buffer,
            kAvioBufferBytes,
            0,
            &app.local_media,
            read_local_media,
            nullptr,
            seek_local_media);
        if (!custom_io) {
            av_free(buffer);
            error = "Unable to allocate local media AVIO context";
            app.local_media.close();
            avformat_free_context(format);
            app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
            return nullptr;
        }
        custom_io->seekable = AVIO_SEEKABLE_NORMAL;
        format->pb = custom_io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "probesize", "33554432", 0);
    av_dict_set(&options, "analyzeduration", "15000000", 0);
    av_dict_set(&options, "max_probe_packets", "10000", 0);
    if (network) {
        av_dict_set(&options, "rw_timeout", "15000000", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_on_network_error", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "5", 0);
        av_dict_set(
            &options,
            "protocol_whitelist",
            "async,cache,crypto,data,ftp,http,httpproxy,https,mmsh,mmst,"
            "rtmp,rtmpe,rtmps,rtmpt,rtmpte,rtmpts,rtp,rtsp,sctp,srtp,"
            "tcp,tls,udp,udplite",
            0);
    } else {
        av_dict_set(&options, "protocol_whitelist", "crypto,data,file", 0);
    }

    int result = avformat_open_input(&format, path, nullptr, &options);
    av_dict_free(&options);
    if (result >= 0) {
        result = avformat_find_stream_info(format, nullptr);
    }
    app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
    if (result < 0) {
        error = "Open media source: " + ffmpeg_error(result);
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "source-open failed path=%s ffmpeg=%d error=%s",
            bfplayer::redact_uri_secrets(path).c_str(),
            result,
            error.c_str());
        avformat_close_input(&format);
        free_custom_avio(custom_io);
        app.local_media.close();
        return nullptr;
    }

    auto* source = static_cast<Kit_Source*>(std::calloc(1, sizeof(Kit_Source)));
    if (!source) {
        error = "Unable to allocate Kitchensink source";
        avformat_close_input(&format);
        free_custom_avio(custom_io);
        app.local_media.close();
        return nullptr;
    }
    source->format_ctx = format;
    source->avio_ctx = custom_io;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "source-open success path=%s streams=%u duration_us=%lld format=%s",
        bfplayer::redact_uri_secrets(path).c_str(),
        format->nb_streams,
        static_cast<long long>(format->duration),
        format->iformat && format->iformat->name ? format->iformat->name : "<unknown>");
    return source;
}

std::string track_description(const App& app, int index) {
    if (!app.source || index < 0) {
        return "Off";
    }
    const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx);
    if (!format || index >= static_cast<int>(format->nb_streams)) {
        return "Track " + std::to_string(index);
    }
    const AVStream* stream = format->streams[index];
    std::string description;
    if (const AVDictionaryEntry* language =
            av_dict_get(stream->metadata, "language", nullptr, 0)) {
        description = bounded_metadata(language->value, 64);
    }
    if (const AVDictionaryEntry* title =
            av_dict_get(stream->metadata, "title", nullptr, 0)) {
        if (!description.empty()) {
            description += " - ";
        }
        description += bounded_metadata(title->value, 256);
    }
    const char* codec = avcodec_get_name(stream->codecpar->codec_id);
    if (codec && codec[0]) {
        if (!description.empty()) {
            description += " - ";
        }
        description += codec;
    }
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        stream->codecpar->ch_layout.nb_channels > 0) {
        description += " - " + std::to_string(stream->codecpar->ch_layout.nb_channels) + "ch";
    }
    return description.empty() ? "Track " + std::to_string(index) : description;
}

const AVStream* source_stream(const App& app, int index) {
    if (!app.source || index < 0) {
        return nullptr;
    }
    const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx);
    if (!format || index >= static_cast<int>(format->nb_streams)) {
        return nullptr;
    }
    return format->streams[index];
}

std::string stream_language(const App& app, int index) {
    const AVStream* stream = source_stream(app, index);
    if (!stream) {
        return {};
    }
    const AVDictionaryEntry* language =
        av_dict_get(stream->metadata, "language", nullptr, 0);
    return language && language->value
        ? bounded_metadata(language->value, 64)
        : std::string{};
}

AVMediaType media_type_for(Kit_StreamType type) {
    switch (type) {
        case KIT_STREAMTYPE_VIDEO:
            return AVMEDIA_TYPE_VIDEO;
        case KIT_STREAMTYPE_AUDIO:
            return AVMEDIA_TYPE_AUDIO;
        case KIT_STREAMTYPE_SUBTITLE:
            return AVMEDIA_TYPE_SUBTITLE;
        default:
            return AVMEDIA_TYPE_UNKNOWN;
    }
}

int resolve_preferred_stream(
    const App& app,
    Kit_StreamType type,
    int saved_index,
    const std::string& saved_language) {
    const AVMediaType wanted_type = media_type_for(type);
    const AVStream* saved_stream = source_stream(app, saved_index);
    if (saved_stream && saved_stream->codecpar->codec_type == wanted_type &&
        (saved_language.empty() || stream_language(app, saved_index) == saved_language)) {
        return saved_index;
    }
    if (saved_language.empty()) {
        return -1;
    }
    int index = Kit_GetNextSourceStream(app.source, type, -1, 0);
    while (index >= 0) {
        if (stream_language(app, index) == saved_language) {
            return index;
        }
        index = Kit_GetNextSourceStream(app.source, type, index, 0);
    }
    return -1;
}

SDL_Renderer* create_renderer(SDL_Window* window) {
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
    }
    return renderer;
}

bool find_exact_display_mode(
    int width,
    int height,
    SDL_DisplayMode& selected) {
    const int count = SDL_GetNumDisplayModes(0);
    bool found = false;
    for (int index = 0; index < count; ++index) {
        SDL_DisplayMode candidate{};
        if (SDL_GetDisplayMode(0, index, &candidate) != 0 ||
            candidate.w != width ||
            candidate.h != height) {
            continue;
        }
        if (!found || candidate.refresh_rate > selected.refresh_rate) {
            selected = candidate;
            found = true;
        }
    }
    return found;
}

bool configure_fullscreen_output(App& app) {
    SDL_DisplayMode fallback{};
    if (!find_exact_display_mode(
            kWindowWidth,
            kWindowHeight,
            fallback)) {
        fallback.format = SDL_PIXELFORMAT_ABGR8888;
        fallback.w = kWindowWidth;
        fallback.h = kWindowHeight;
        fallback.refresh_rate = kPreferredOutputRefresh;
    }
    SDL_SetWindowSize(app.window, kWindowWidth, kWindowHeight);
    if (SDL_SetWindowDisplayMode(app.window, &fallback) != 0) {
        log_sdl("SDL_SetWindowDisplayMode(1080p)");
        return false;
    }
    if (SDL_SetWindowFullscreen(
            app.window,
            SDL_WINDOW_FULLSCREEN) != 0) {
        log_sdl("SDL_SetWindowFullscreen(1080p)");
        return false;
    }
    return true;
}

void log_display_output(App& app) {
    int window_width = 0;
    int window_height = 0;
    int renderer_width = 0;
    int renderer_height = 0;
    SDL_GetWindowSize(app.window, &window_width, &window_height);
    if (SDL_GetRendererOutputSize(
            app.renderer,
            &renderer_width,
            &renderer_height) != 0) {
        log_sdl("SDL_GetRendererOutputSize");
        renderer_width = window_width;
        renderer_height = window_height;
    }
    SDL_DisplayMode current{};
    if (SDL_GetCurrentDisplayMode(0, &current) != 0) {
        log_sdl("SDL_GetCurrentDisplayMode");
    }
    SDL_RendererInfo renderer_info{};
    if (SDL_GetRendererInfo(app.renderer, &renderer_info) != 0) {
        log_sdl("SDL_GetRendererInfo");
    }
    app.output_width = renderer_width > 0
        ? renderer_width
        : kWindowWidth;
    app.output_height = renderer_height > 0
        ? renderer_height
        : kWindowHeight;
    app.true_4k_output =
        app.output_width == kPreferredOutputWidth &&
        app.output_height == kPreferredOutputHeight &&
        current.w == kPreferredOutputWidth &&
        current.h == kPreferredOutputHeight;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "display-output current=%dx%d@%d window=%dx%d renderer=%dx%d logical=%dx%d true_4k=%d renderer_name=%s flags=0x%x",
        current.w,
        current.h,
        current.refresh_rate,
        window_width,
        window_height,
        renderer_width,
        renderer_height,
        kWindowWidth,
        kWindowHeight,
        app.true_4k_output ? 1 : 0,
        renderer_info.name ? renderer_info.name : "unknown",
        renderer_info.flags);
}

bool switch_fullscreen_output(
    App& app,
    int width,
    int height,
    const char* reason) {
    if (!app.window || !app.renderer || width <= 0 || height <= 0) {
        return false;
    }
    if (app.output_width == width && app.output_height == height) {
        return true;
    }

    SDL_DisplayMode mode{};
    if (!find_exact_display_mode(width, height, mode)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "display-switch unavailable requested=%dx%d reason=%s",
            width,
            height,
            reason ? reason : "<none>");
        return false;
    }
    if (SDL_SetWindowDisplayMode(app.window, &mode) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "display-switch failed requested=%dx%d reason=%s error=%s",
            width,
            height,
            reason ? reason : "<none>",
            SDL_GetError());
        return false;
    }
    SDL_PumpEvents();
    if (SDL_RenderSetLogicalSize(
            app.renderer,
            kWindowWidth,
            kWindowHeight) != 0) {
        log_sdl("SDL_RenderSetLogicalSize(display switch)");
        return false;
    }
    log_display_output(app);
    const bool matched =
        app.output_width == width && app.output_height == height;
    bfplayer::diagnostics_log(
        matched
            ? bfplayer::DiagnosticLevel::info
            : bfplayer::DiagnosticLevel::warning,
        "display-switch requested=%dx%d actual=%dx%d reason=%s matched=%d",
        width,
        height,
        app.output_width,
        app.output_height,
        reason ? reason : "<none>",
        matched ? 1 : 0);
    return matched;
}

void close_audio(App& app) {
    if (app.audio != 0) {
        SDL_ClearQueuedAudio(app.audio);
        SDL_CloseAudioDevice(app.audio);
        app.audio = 0;
    }
}

bool open_audio(App& app) {
    close_audio(app);
    if (Kit_GetPlayerAudioStream(app.player) < 0) {
        return true;
    }

    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);
    SDL_AudioSpec wanted{};
    SDL_AudioSpec obtained{};
    wanted.freq = info.audio_format.sample_rate;
    wanted.format = static_cast<SDL_AudioFormat>(info.audio_format.format);
    wanted.channels = static_cast<Uint8>(info.audio_format.channels);
    wanted.samples = 1024;
    app.audio = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
    if (app.audio == 0) {
        log_sdl("SDL_OpenAudioDevice");
        return false;
    }
    SDL_PauseAudioDevice(app.audio, 0);
    return true;
}

void destroy_video_textures(App& app) {
    SDL_DestroyTexture(app.video);
    app.video = nullptr;
}

void clear_now_playing_artwork(App& app) {
    SDL_DestroyTexture(app.now_playing_artwork);
    app.now_playing_artwork = nullptr;
    app.now_playing_artwork_width = 0;
    app.now_playing_artwork_height = 0;
    app.now_playing_artwork_media_path.clear();
    app.now_playing_artwork_path.clear();
    app.now_playing_background_color = SDL_Color{0, 0, 0, 255};
}

// Cancels and joins the now-playing artwork worker thread (if any) and
// discards any unconsumed result, without touching `job.mutex` itself (that
// SDL_mutex is created once in main() and lives for the whole process, since
// a new job is started on every track change). Safe to call when no job is
// running. Must only be called from the main thread.
void stop_now_playing_artwork_job(App& app) {
    NowPlayingArtworkJob& job = app.now_playing_artwork_job;
    job.cancel.store(true, std::memory_order_relaxed);
    if (job.thread) {
        SDL_WaitThread(job.thread, nullptr);
        job.thread = nullptr;
    }
    if (job.mutex) {
        SDL_LockMutex(job.mutex);
        SDL_FreeSurface(job.result_surface);
        job.result_surface = nullptr;
        job.result_artwork_path.clear();
        job.ready = false;
        SDL_UnlockMutex(job.mutex);
    }
    job.cancel.store(false, std::memory_order_relaxed);
}

// Reads the raw pixel value at (x, y) in `surface`'s native format, sized
// according to its BytesPerPixel, and packs it into a Uint32 the way
// SDL_GetRGB() expects (native byte order; see the "Getting and setting
// pixels" article on the SDL2 wiki, the standard pattern for reading
// arbitrary-format pixels without converting the whole surface). Caller
// must hold `surface`'s lock (if SDL_MUSTLOCK requires one) and pass
// in-bounds (x, y).
Uint32 read_surface_pixel(const SDL_Surface* surface, int x, int y) {
    const Uint8* row =
        static_cast<const Uint8*>(surface->pixels) +
        static_cast<std::size_t>(y) * surface->pitch;
    const Uint8* p = row + static_cast<std::size_t>(x) * surface->format->BytesPerPixel;
    switch (surface->format->BytesPerPixel) {
        case 1:
            return *p;
        case 2:
            return *reinterpret_cast<const Uint16*>(p);
        case 3:
            if (SDL_BYTEORDER == SDL_BIG_ENDIAN) {
                return (p[0] << 16) | (p[1] << 8) | p[2];
            }
            return p[0] | (p[1] << 8) | (p[2] << 16);
        case 4:
            return *reinterpret_cast<const Uint32*>(p);
        default:
            return 0;
    }
}

// Samples a bounded grid of pixels from a decoded artwork surface to
// approximate its average color, run on the artwork worker thread (see
// now_playing_artwork_job_entry() below) right after the JPEG/PNG decode
// that already produced `surface`. The sample walks an actual strided
// (x, y) grid -- at most kGridSamples steps in each dimension, so at most
// kGridSamples^2 pixels are ever touched, never the full surface (which can
// be up to the ArtworkLimits cap of ~20 million pixels). Deliberately does
// NOT call SDL_ConvertSurfaceFormat: at the ArtworkLimits cap that would
// blit and allocate a full ~80MB RGBA32 copy of the already-decoded ~80MB
// surface just to sample 4096 pixels. Instead each sampled pixel is read in
// the surface's native format via read_surface_pixel() and unpacked with
// SDL_GetRGB(), the standard SDL2 pattern for reading arbitrary-format
// pixels without converting the whole surface. Walking both dimensions
// (rather than a flat 1D index stride) also avoids the degenerate case
// where a power-of-two cover (1024x1024, 2048x2048, ...) makes a 1D stride
// divide evenly into the width and collapse the sample down to a handful of
// vertical columns.
SDL_Color sample_now_playing_background_color(SDL_Surface* surface) {
    SDL_Color fallback{0, 0, 0, 255};
    if (!surface || surface->w <= 0 || surface->h <= 0) {
        return fallback;
    }
    const bool must_lock = SDL_MUSTLOCK(surface);
    if (must_lock && SDL_LockSurface(surface) != 0) {
        return fallback;
    }
    constexpr int kGridSamples = 64; // up to 64x64 = 4096 samples.
    const int step_x = std::max(1, surface->w / kGridSamples);
    const int step_y = std::max(1, surface->h / kGridSamples);
    std::uint64_t sampled = 0;
    std::uint64_t sum_r = 0;
    std::uint64_t sum_g = 0;
    std::uint64_t sum_b = 0;
    for (int y = 0; y < surface->h; y += step_y) {
        for (int x = 0; x < surface->w; x += step_x) {
            const Uint32 pixel_value = read_surface_pixel(surface, x, y);
            Uint8 r = 0;
            Uint8 g = 0;
            Uint8 b = 0;
            SDL_GetRGB(pixel_value, surface->format, &r, &g, &b);
            sum_r += r;
            sum_g += g;
            sum_b += b;
            ++sampled;
        }
    }
    if (must_lock) {
        SDL_UnlockSurface(surface);
    }
    if (sampled == 0) {
        return fallback;
    }
    // Scale the sampled average down so bright/light artwork does not wash
    // out the OSD text drawn on top of it; this keeps the backdrop a
    // subtle, artwork-tinted neutral rather than a vivid color. 0.35 means
    // the backdrop keeps 35% of the sampled brightness (i.e. darkened 65%).
    constexpr double kBackgroundBrightness = 0.35;
    return SDL_Color{
        static_cast<Uint8>((sum_r / sampled) * kBackgroundBrightness),
        static_cast<Uint8>((sum_g / sampled) * kBackgroundBrightness),
        static_cast<Uint8>((sum_b / sampled) * kBackgroundBrightness),
        255};
}

// Worker thread entry for the now-playing artwork job: find/read/decode the
// artwork file and sample its background color -- the same work
// update_now_playing_artwork() used to do inline on the render/audio-feed
// loop (src/main.cpp's main loop queues audio via SDL_QueueAudio with only
// kAudioBufferBytes = 64 KiB of headroom, so a decode stall here must never
// block that loop). Runs entirely off the main thread; the only thing it
// shares with the main thread is the decoded SDL_Surface, handed over
// through `job->result_surface` under `job->mutex`. The SDL_Texture upload
// itself happens later, on the main thread, in
// consume_now_playing_artwork_job().
int now_playing_artwork_job_entry(void* userdata) {
    auto* job = static_cast<NowPlayingArtworkJob*>(userdata);
    std::string media_path;
    SDL_LockMutex(job->mutex);
    media_path = job->request_path;
    SDL_UnlockMutex(job->mutex);

    std::string artwork_path;
    SDL_Surface* surface = nullptr;
    SDL_Color color{0, 0, 0, 255};
    if (!job->cancel.load(std::memory_order_relaxed)) {
        artwork_path = bfplayer::find_local_artwork(media_path);
    }
    if (!artwork_path.empty() && !job->cancel.load(std::memory_order_relaxed)) {
        bfplayer::ArtworkData artwork;
        std::string artwork_error;
        if (!bfplayer::load_local_artwork(artwork_path, artwork, artwork_error)) {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::warning,
                "now-playing-artwork load failed path=%s error=%s",
                artwork_path.c_str(),
                artwork_error.c_str());
            artwork_path.clear();
        } else if (!job->cancel.load(std::memory_order_relaxed)) {
            SDL_RWops* source = SDL_RWFromConstMem(
                artwork.encoded.data(),
                static_cast<int>(artwork.encoded.size()));
            if (!source) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::warning,
                    "now-playing-artwork rwops failed error=%s",
                    SDL_GetError());
                artwork_path.clear();
            } else {
                SDL_Surface* decoded =
                    artwork.format == bfplayer::ArtworkFormat::png
                        ? IMG_LoadPNG_RW(source)
                        : IMG_LoadJPG_RW(source);
                SDL_RWclose(source);
                if (!decoded) {
                    bfplayer::diagnostics_log(
                        bfplayer::DiagnosticLevel::warning,
                        "now-playing-artwork decode failed path=%s error=%s",
                        artwork_path.c_str(),
                        IMG_GetError());
                    artwork_path.clear();
                } else if (
                    decoded->w <= 0 || decoded->h <= 0 ||
                    static_cast<std::uint32_t>(decoded->w) != artwork.width ||
                    static_cast<std::uint32_t>(decoded->h) != artwork.height) {
                    bfplayer::diagnostics_log(
                        bfplayer::DiagnosticLevel::warning,
                        "now-playing-artwork dimensions changed path=%s expected=%ux%u actual=%dx%d",
                        artwork_path.c_str(),
                        artwork.width,
                        artwork.height,
                        decoded->w,
                        decoded->h);
                    SDL_FreeSurface(decoded);
                    artwork_path.clear();
                } else {
                    color = sample_now_playing_background_color(decoded);
                    surface = decoded;
                }
            }
        }
    }
    if (job->cancel.load(std::memory_order_relaxed)) {
        SDL_FreeSurface(surface);
        surface = nullptr;
    }
    SDL_LockMutex(job->mutex);
    if (!job->cancel.load(std::memory_order_relaxed) &&
        media_path == job->request_path) {
        SDL_FreeSurface(job->result_surface);
        job->result_surface = surface;
        job->result_artwork_path = artwork_path;
        job->result_color = color;
        job->ready = true;
    } else {
        SDL_FreeSurface(surface);
    }
    SDL_UnlockMutex(job->mutex);
    return 0;
}

// Picks up a finished now-playing artwork job, if any, and uploads its
// decoded surface as an SDL_Texture. This is the only place the texture
// upload happens, and it only runs once per completed job (not once per
// frame): the mutex section itself is a handful of pointer/bool moves, so
// calling this every loop iteration costs nothing on a cache hit.
void consume_now_playing_artwork_job(App& app) {
    NowPlayingArtworkJob& job = app.now_playing_artwork_job;
    if (!job.mutex) {
        return;
    }
    bool ready = false;
    SDL_Surface* surface = nullptr;
    std::string artwork_path;
    SDL_Color color{0, 0, 0, 255};
    SDL_Thread* completed_thread = nullptr;
    SDL_LockMutex(job.mutex);
    if (job.ready) {
        ready = true;
        surface = job.result_surface;
        job.result_surface = nullptr;
        artwork_path = std::move(job.result_artwork_path);
        job.result_artwork_path.clear();
        color = job.result_color;
        job.ready = false;
        completed_thread = job.thread;
        job.thread = nullptr;
    }
    SDL_UnlockMutex(job.mutex);
    if (!ready) {
        return;
    }
    if (completed_thread) {
        SDL_WaitThread(completed_thread, nullptr);
    }
    if (!surface) {
        // No local artwork found, or it failed to load/decode; leave the
        // plain black backdrop in place (already the default).
        return;
    }
    app.now_playing_artwork_path = artwork_path;
    app.now_playing_background_color = color;
    app.now_playing_artwork = SDL_CreateTextureFromSurface(app.renderer, surface);
    app.now_playing_artwork_width = surface->w;
    app.now_playing_artwork_height = surface->h;
    SDL_FreeSurface(surface);
    if (!app.now_playing_artwork) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "now-playing-artwork texture failed error=%s",
            SDL_GetError());
        app.now_playing_artwork_path.clear();
        app.now_playing_artwork_width = 0;
        app.now_playing_artwork_height = 0;
        app.now_playing_background_color = SDL_Color{0, 0, 0, 255};
    }
}

// Loads (or clears) the cached now-playing artwork texture when the current
// track changes. Mirrors LibraryUi::update_artwork()'s caching lifetime
// (see src/ui/library_ui.cpp): a cheap path comparison is done every call,
// and a real change is remembered immediately (including a failed/missing
// lookup, so it is not retried every frame for the rest of the track) even
// though the actual find/decode/sample work happens asynchronously on
// now_playing_artwork_job's worker thread -- see now_playing_artwork_job_
// entry() and consume_now_playing_artwork_job() above. Unlike
// LibraryUi::update_artwork(), this one runs on a loop that is also feeding
// the audio queue every iteration, so it must never block on the decode.
void update_now_playing_artwork(App& app) {
    consume_now_playing_artwork_job(app);
    if (app.current_media_path == app.now_playing_artwork_media_path) {
        return;
    }
    stop_now_playing_artwork_job(app);
    clear_now_playing_artwork(app);
    app.now_playing_artwork_media_path = app.current_media_path;
    if (app.current_media_path.empty()) {
        return;
    }
    NowPlayingArtworkJob& job = app.now_playing_artwork_job;
    if (!job.mutex) {
        // The mutex failed to create at startup; fall back to no artwork
        // for this track rather than start an unsynchronized job.
        return;
    }
    SDL_LockMutex(job.mutex);
    job.request_path = app.current_media_path;
    job.ready = false;
    SDL_UnlockMutex(job.mutex);
    job.cancel.store(false, std::memory_order_relaxed);
    job.thread = SDL_CreateThread(
        now_playing_artwork_job_entry,
        "bfplayer-now-playing-art",
        &job);
    if (!job.thread) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "now-playing-artwork thread create failed error=%s",
            SDL_GetError());
    }
}

bool create_video_textures(App& app) {
    destroy_video_textures(app);
    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);

    if (Kit_GetPlayerVideoStream(app.player) >= 0 &&
        !app.native_hdr_output) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        app.video = SDL_CreateTexture(
            app.renderer,
            info.video_format.format,
            SDL_TEXTUREACCESS_STATIC,
            info.video_format.width,
            info.video_format.height);
        if (!app.video) {
            log_sdl("SDL_CreateTexture(video)");
            return false;
        }
    }
    SDL_RenderSetLogicalSize(app.renderer, kWindowWidth, kWindowHeight);

    return true;
}

double player_display_aspect(
    App& app,
    int frame_width,
    int frame_height) {
    // Prefer the demuxer's source dimensions and sample-aspect metadata. The
    // decoder output may be proportionally downscaled for performance, and
    // its frame metadata is not guaranteed to survive that conversion.
    if (app.source_display_aspect > 0.0 &&
        std::isfinite(app.source_display_aspect)) {
        return app.source_display_aspect;
    }
    int numerator = 0;
    int denominator = 0;
    Kit_GetPlayerAspectRatio(app.player, &numerator, &denominator);
    return display_aspect_from_sample_aspect(
        frame_width,
        frame_height,
        numerator,
        denominator);
}

// Replaces the deleted video-layout-mode module's VideoLayout/
// compute_video_layout(): this player no longer offers fit/fill/stretch,
// aspect-override, or crop-override selection, so the geometry below is
// always a plain letterbox/pillarbox "best fit" of the source into the
// window, with no source cropping.
struct VideoPresentRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct VideoPresentLayout {
    VideoPresentRect source;
    VideoPresentRect destination;
    bool crop_source = false;
};

VideoPresentLayout player_video_layout(App& app) {
    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);
    const int frame_width = info.video_format.width;
    const int frame_height = info.video_format.height;
    VideoPresentLayout layout{};
    if (frame_width <= 0 || frame_height <= 0) {
        return layout;
    }
    layout.source = {0, 0, frame_width, frame_height};
    layout.destination = {0, 0, kWindowWidth, kWindowHeight};
    double display_aspect = player_display_aspect(
        app, frame_width, frame_height);
    if (!(display_aspect > 0.0) || !std::isfinite(display_aspect)) {
        display_aspect =
            static_cast<double>(frame_width) / static_cast<double>(frame_height);
    }
    const double output_aspect =
        static_cast<double>(kWindowWidth) / static_cast<double>(kWindowHeight);
    if (display_aspect >= output_aspect) {
        const int height = std::clamp(
            static_cast<int>(std::llround(kWindowWidth / display_aspect)),
            1,
            kWindowHeight);
        layout.destination = {
            0, (kWindowHeight - height) / 2, kWindowWidth, height};
    } else {
        const int width = std::clamp(
            static_cast<int>(std::llround(kWindowHeight * display_aspect)),
            1,
            kWindowWidth);
        layout.destination = {
            (kWindowWidth - width) / 2, 0, width, kWindowHeight};
    }
    return layout;
}

bool set_stream(App& app, Kit_StreamType type, int index) {
    const int previous = Kit_GetPlayerStream(app.player, type);
    const double refresh_position = Kit_GetPlayerPosition(app.player);
    if (previous == index) {
        return true;
    }
    if (Kit_SetPlayerStream(app.player, type, index) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "track-switch failed type=%d previous=%d requested=%d error=%s",
            type,
            previous,
            index,
            Kit_GetError());
        return false;
    }
    bool resources_ready = true;
    if (type == KIT_STREAMTYPE_AUDIO) {
        resources_ready = open_audio(app);
    } else if (type == KIT_STREAMTYPE_VIDEO) {
        resources_ready = create_video_textures(app);
    }
    if (!resources_ready) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "track-switch resource-failure type=%d requested=%d restoring=%d",
            type,
            index,
            previous);
        const bool decoder_restored =
            Kit_SetPlayerStream(app.player, type, previous) == 0;
        const bool resources_restored = decoder_restored &&
            (type == KIT_STREAMTYPE_AUDIO
                 ? open_audio(app)
                 : create_video_textures(app));
        if (!resources_restored) {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::error,
                "track-switch rollback-failed type=%d error=%s",
                type,
                Kit_GetError());
            app.osd.show("Track switch failed; returning to library", 5000);
            app.playback_running = false;
        } else {
            app.osd.show("Track switch failed; previous track restored", 4000);
        }
        return false;
    }
    if (type == KIT_STREAMTYPE_VIDEO) {
        app.source_display_aspect = inspect_video_source(
            static_cast<const AVFormatContext*>(app.source->format_ctx),
            index).display_aspect;
    }
    if (index >= 0 &&
        (type == KIT_STREAMTYPE_AUDIO || type == KIT_STREAMTYPE_SUBTITLE) &&
        std::isfinite(refresh_position)) {
        /*
         * The demuxer may already be several seconds ahead of the playback
         * clock. Refresh packets around the current scene so the new track is
         * visible/audible immediately without changing the viewing position.
         */
        const double refresh_from = std::max(0.0, refresh_position);
        if (Kit_PlayerSeek(app.player, refresh_from) == 0) {
            if (app.audio != 0 && type == KIT_STREAMTYPE_AUDIO) {
                SDL_ClearQueuedAudio(app.audio);
            }
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "track-switch refresh type=%d position=%.3f seek=%.3f",
                type,
                refresh_position,
                refresh_from);
        }
    }

    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "track-switch success type=%d previous=%d current=%d description=%s",
        type,
        previous,
        index,
        track_description(app, index).c_str());
    const char* type_name = type == KIT_STREAMTYPE_AUDIO ? "Audio" :
        (type == KIT_STREAMTYPE_VIDEO ? "Video" : "Subtitles");
    app.osd.show(std::string(type_name) + ": " + track_description(app, index));
    return true;
}

void cycle_stream(App& app, Kit_StreamType type, bool allow_off) {
    const int current = Kit_GetPlayerStream(app.player, type);
    int next = Kit_GetNextSourceStream(app.source, type, current, 0);
    if (next < 0 && allow_off && current >= 0) {
        set_stream(app, type, -1);
        return;
    }
    if (next < 0) {
        next = Kit_GetNextSourceStream(app.source, type, -1, 0);
    }
    if (next >= 0 && next != current) {
        set_stream(app, type, next);
    }
}

bfplayer::TrackPreferences current_track_preferences(const App& app) {
    bfplayer::TrackPreferences preferences{};
    if (!app.player) {
        return preferences;
    }
    preferences.audio_stream = Kit_GetPlayerAudioStream(app.player);
    preferences.audio_language = stream_language(app, preferences.audio_stream);
    return preferences;
}

void restore_track_preferences(
    App& app,
    const bfplayer::TrackPreferences& preferences) {
    const int audio_stream = resolve_preferred_stream(
        app,
        KIT_STREAMTYPE_AUDIO,
        preferences.audio_stream,
        preferences.audio_language);
    if (audio_stream >= 0 && audio_stream != Kit_GetPlayerAudioStream(app.player)) {
        set_stream(app, KIT_STREAMTYPE_AUDIO, audio_stream);
    }
}

void seek_relative(App& app, double seconds) {
    const double duration = Kit_GetPlayerDuration(app.player);
    const double position = Kit_GetPlayerPosition(app.player);
    if (!std::isfinite(duration) || duration <= 0.0 ||
        !std::isfinite(position)) {
        app.osd.show("Seeking is unavailable for this live/unknown-duration source");
        return;
    }
    const double target = std::clamp(
        position + seconds, 0.0, duration);
    if (Kit_PlayerSeek(app.player, target) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "seek failed from=%.3f delta=%.3f target=%.3f error=%s",
            position,
            seconds,
            target,
            Kit_GetError());
    } else if (app.audio != 0) {
        SDL_ClearQueuedAudio(app.audio);
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "seek success from=%.3f delta=%.3f target=%.3f",
            position,
            seconds,
            target);
    }
    app.osd.show(
        std::string(seconds >= 0.0 ? "Seek +" : "Seek ") +
        std::to_string(static_cast<int>(seconds)) + " seconds");
}

void seek_chapter(App& app, int direction) {
    const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx);
    if (!format || format->nb_chapters == 0) {
        seek_relative(app, direction < 0 ? -600.0 : 600.0);
        return;
    }
    const double position = Kit_GetPlayerPosition(app.player);
    if (!std::isfinite(position)) {
        app.osd.show("Chapter navigation is unavailable");
        return;
    }
    int target_index = -1;
    if (direction > 0) {
        for (unsigned int index = 0; index < format->nb_chapters; ++index) {
            const AVChapter* chapter = format->chapters[index];
            const double start = chapter->start * av_q2d(chapter->time_base);
            if (start > position + 1.0) {
                target_index = static_cast<int>(index);
                break;
            }
        }
    } else {
        for (unsigned int index = 0; index < format->nb_chapters; ++index) {
            const AVChapter* chapter = format->chapters[index];
            const double start = chapter->start * av_q2d(chapter->time_base);
            if (start < position - 5.0) {
                target_index = static_cast<int>(index);
            } else {
                break;
            }
        }
        if (target_index < 0 && format->nb_chapters > 0) {
            target_index = 0;
        }
    }
    if (target_index < 0 || target_index >= static_cast<int>(format->nb_chapters)) {
        app.osd.show(direction > 0 ? "Last chapter" : "First chapter");
        return;
    }
    const AVChapter* chapter = format->chapters[target_index];
    const double target = chapter->start * av_q2d(chapter->time_base);
    if (Kit_PlayerSeek(app.player, target) == 0) {
        if (app.audio != 0) {
            SDL_ClearQueuedAudio(app.audio);
        }
        std::string label = "Chapter " + std::to_string(target_index + 1);
        if (const AVDictionaryEntry* title =
                av_dict_get(chapter->metadata, "title", nullptr, 0)) {
            label += ": ";
            label += title->value;
        }
        app.osd.show(std::move(label));
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "chapter-seek success index=%d target=%.3f",
            target_index,
            target);
    } else {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "chapter-seek failed index=%d target=%.3f error=%s",
            target_index,
            target,
            Kit_GetError());
    }
}

void set_volume(App& app, int percent) {
    app.volume_percent = std::clamp(percent, 0, 100);
    if (app.volume_percent > 0) {
        app.previous_volume_percent = app.volume_percent;
    }
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "volume percent=%d",
        app.volume_percent);
    app.osd.show("Volume: " + std::to_string(app.volume_percent) + "%");
}

void toggle_mute(App& app) {
    if (app.volume_percent == 0) {
        set_volume(app, app.previous_volume_percent);
    } else {
        app.previous_volume_percent = app.volume_percent;
        set_volume(app, 0);
    }
}

void toggle_pause(App& app) {
    const bool pause = !app.paused;
    app.paused = pause;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "pause state=%s position=%.3f",
        app.paused ? "paused" : "playing",
        app.player ? Kit_GetPlayerPosition(app.player) : 0.0);
    if (app.paused) {
        Kit_PlayerPause(app.player);
        if (app.audio != 0) {
            SDL_PauseAudioDevice(app.audio, 1);
        }
    } else {
        Kit_PlayerPlay(app.player);
        if (app.audio != 0) {
            SDL_PauseAudioDevice(app.audio, 0);
        }
    }
    app.osd.show(app.paused ? "Paused" : "Playing");
    app.redraw_requested = true;
}

void set_pause_state(App& app, bool pause) {
    if (!app.player || app.paused == pause) {
        return;
    }
    toggle_pause(app);
}

void seek_absolute(App& app, double target) {
    if (!app.player) {
        return;
    }
    const double duration = Kit_GetPlayerDuration(app.player);
    const double position = Kit_GetPlayerPosition(app.player);
    if (!std::isfinite(duration) || duration <= 0.0 ||
        !std::isfinite(position) || !std::isfinite(target)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "remote-seek rejected position=%.3f duration=%.3f target=%.3f",
            position,
            duration,
            target);
        return;
    }
    target = std::clamp(target, 0.0, duration);
    if (Kit_PlayerSeek(app.player, target) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "remote-seek failed from=%.3f target=%.3f error=%s",
            position,
            target,
            Kit_GetError());
        return;
    }
    if (app.audio != 0) {
        SDL_ClearQueuedAudio(app.audio);
    }
    app.redraw_requested = true;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "remote-seek success from=%.3f target=%.3f",
        position,
        target);
}

void consume_remote_commands(App& app, bool in_playback) {
    bfplayer::RemoteCommand command;
    while (app.remote_control.poll(command)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "remote-command apply sequence=%llu command=%d in_playback=%d",
            static_cast<unsigned long long>(command.sequence),
            static_cast<int>(command.type),
            in_playback ? 1 : 0);
        switch (command.type) {
            case bfplayer::RemoteCommandType::open:
                app.pending_remote_media_path = std::move(command.path);
                if (in_playback) {
                    app.playback_running = false;
                }
                break;
            case bfplayer::RemoteCommandType::play:
                if (in_playback) {
                    set_pause_state(app, false);
                }
                break;
            case bfplayer::RemoteCommandType::pause:
                if (in_playback) {
                    set_pause_state(app, true);
                }
                break;
            case bfplayer::RemoteCommandType::toggle_pause:
                if (in_playback) {
                    toggle_pause(app);
                }
                break;
            case bfplayer::RemoteCommandType::seek_relative:
                if (in_playback) {
                    seek_relative(app, command.value);
                    app.redraw_requested = true;
                }
                break;
            case bfplayer::RemoteCommandType::seek_absolute:
                if (in_playback) {
                    seek_absolute(app, command.value);
                }
                break;
            case bfplayer::RemoteCommandType::cycle_audio:
                if (in_playback) {
                    cycle_stream(app, KIT_STREAMTYPE_AUDIO, false);
                }
                break;
            case bfplayer::RemoteCommandType::cycle_subtitle:
                // Subtitle tracks were removed with the rest of the
                // video-only subtitle feature; nothing to cycle.
                break;
            case bfplayer::RemoteCommandType::button: {
                if (command.button < 0 ||
                    command.button >= SDL_CONTROLLER_BUTTON_MAX) {
                    break;
                }
                SDL_Event down{};
                down.type = SDL_CONTROLLERBUTTONDOWN;
                down.cbutton.type = SDL_CONTROLLERBUTTONDOWN;
                down.cbutton.state = SDL_PRESSED;
                down.cbutton.button =
                    static_cast<Uint8>(command.button);
                SDL_Event up = down;
                up.type = SDL_CONTROLLERBUTTONUP;
                up.cbutton.type = SDL_CONTROLLERBUTTONUP;
                up.cbutton.state = SDL_RELEASED;
                if (SDL_PushEvent(&down) < 0 || SDL_PushEvent(&up) < 0) {
                    bfplayer::diagnostics_log(
                        bfplayer::DiagnosticLevel::warning,
                        "remote-button injection failed button=%d error=%s",
                        command.button,
                        SDL_GetError());
                }
                break;
            }
            case bfplayer::RemoteCommandType::stop:
                if (in_playback) {
                    app.playback_running = false;
                }
                break;
            case bfplayer::RemoteCommandType::exit:
                app.playback_running = false;
                app.running = false;
                break;
        }
    }
}

bool persist_active_player_settings(App& app) {
    const bfplayer::PlayerSettings settings =
        bfplayer::normalized_player_settings(app.settings);
    bfplayer::LibraryDatabase database;
    const bool saved =
        database.open("/data/BFplayer/library.db") &&
        database.set_settings({
            {std::string(bfplayer::kSettingVolumePercent),
             std::to_string(settings.volume_percent)},
            {std::string(bfplayer::kSettingShortSeekSeconds),
             std::to_string(settings.short_seek_seconds)},
            {std::string(bfplayer::kSettingLongSeekSeconds),
             std::to_string(settings.long_seek_seconds)},
            {std::string(bfplayer::kSettingOsdDurationMs),
             std::to_string(settings.osd_duration_ms)},
            {std::string(bfplayer::kSettingResumePlayback),
             settings.resume_playback ? "1" : "0"},
        });
    if (!saved) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "player-settings live-save failed error=%s",
            database.error().c_str());
        return false;
    }
    app.settings = settings;
    app.volume_percent = settings.volume_percent;
    if (app.volume_percent > 0) {
        app.previous_volume_percent = app.volume_percent;
    }
    app.osd.set_default_duration(
        static_cast<std::uint64_t>(settings.osd_duration_ms));
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "player-settings live-save volume=%d short_seek=%d long_seek=%d osd_ms=%d resume=%d",
        settings.volume_percent,
        settings.short_seek_seconds,
        settings.long_seek_seconds,
        settings.osd_duration_ms,
        settings.resume_playback ? 1 : 0);
    return true;
}

void refresh_playback_overlay(App& app) {
    if (app.playback_overlay == PlaybackOverlay::none) {
        app.osd.hide_panel();
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::controls) {
        app.osd.show_panel(
            "Playback controls",
            {
                "Cross             Play or pause",
                "D-pad Left/Right  Seek short step",
                "D-pad Down/Up     Seek long step",
                "L1 / R1           Previous / next chapter",
                "Square            Change audio track",
                "Triangle          Change video track",
                "L3 / R3           Volume down / up",
                "Touchpad          Show this controls page",
                "Options           Menu, settings, return",
            },
            -1);
        return;
    }
    if (app.playback_overlay == PlaybackOverlay::settings) {
        app.osd.show_panel(
            "Playback settings - Left/Right change | Square reset | Circle back",
            {
                "Volume                          " +
                    std::to_string(app.settings.volume_percent) + "%",
                "Short seek step                 " +
                    std::to_string(app.settings.short_seek_seconds) +
                    " seconds",
                "Long seek step                  " +
                    std::to_string(app.settings.long_seek_seconds) +
                    " seconds",
                "Pop-up message duration         " +
                    std::to_string(app.settings.osd_duration_ms / 1000) +
                    " seconds",
                std::string("Resume where I stopped         ") +
                    (app.settings.resume_playback ? "On" : "Off"),
                "Restore playback defaults",
            },
            app.playback_overlay_selected);
        return;
    }
    app.osd.show_panel(
        "Playback menu",
        {
            "Resume playback",
            "View all controls",
            "Playback settings",
            app.volume_percent == 0 ? "Unmute audio" : "Mute audio",
            "Return to library",
        },
        app.playback_overlay_selected);
}

void open_playback_overlay(App& app, PlaybackOverlay overlay) {
    app.playback_overlay = overlay;
    app.playback_overlay_selected = 0;
    refresh_playback_overlay(app);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-overlay open=%s",
        overlay == PlaybackOverlay::menu
            ? "menu"
            : (overlay == PlaybackOverlay::controls
                   ? "controls"
                   : (overlay == PlaybackOverlay::settings
                          ? "settings"
                          : "none")));
}


bool handle_playback_overlay_button(
    App& app,
    SDL_GameControllerButton button) {
    if (app.playback_overlay == PlaybackOverlay::none) {
        return false;
    }
    if (button == bfplayer::kControllerOptionsButton) {
        app.playback_overlay = PlaybackOverlay::none;
        refresh_playback_overlay(app);
        return true;
    }
    if (button == SDL_CONTROLLER_BUTTON_B) {
        if (app.playback_overlay != PlaybackOverlay::menu) {
            open_playback_overlay(app, PlaybackOverlay::menu);
        } else {
            app.playback_overlay = PlaybackOverlay::none;
            refresh_playback_overlay(app);
        }
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::controls) {
        return true;
    }
    if (app.playback_overlay == PlaybackOverlay::settings) {
        if (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
            button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
            const int direction =
                button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
            app.playback_overlay_selected =
                bfplayer::wrap_list_index(
                    app.playback_overlay_selected, direction, 6);
            refresh_playback_overlay(app);
            return true;
        }
        if (button == SDL_CONTROLLER_BUTTON_X) {
            const bfplayer::PlayerSettings previous = app.settings;
            app.settings = bfplayer::PlayerSettings{};
            if (!persist_active_player_settings(app)) {
                app.settings = previous;
                app.osd.show("Unable to save playback settings", 8000);
            }
            refresh_playback_overlay(app);
            return true;
        }
        if (button != SDL_CONTROLLER_BUTTON_A &&
            button != SDL_CONTROLLER_BUTTON_DPAD_LEFT &&
            button != SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            return true;
        }
        const int direction =
            button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1;
        const bfplayer::PlayerSettings previous = app.settings;
        switch (app.playback_overlay_selected) {
            case 0:
                app.settings.volume_percent = std::clamp(
                    app.settings.volume_percent + direction * 5,
                    0,
                    100);
                break;
            case 1:
                app.settings.short_seek_seconds =
                    bfplayer::next_short_seek_seconds(
                        app.settings.short_seek_seconds,
                        direction);
                break;
            case 2:
                app.settings.long_seek_seconds =
                    bfplayer::next_long_seek_seconds(
                        app.settings.long_seek_seconds,
                        direction);
                break;
            case 3:
                app.settings.osd_duration_ms =
                    bfplayer::next_osd_duration_ms(
                        app.settings.osd_duration_ms,
                        direction);
                break;
            case 4:
                app.settings.resume_playback =
                    !app.settings.resume_playback;
                break;
            case 5:
                app.settings = bfplayer::PlayerSettings{};
                break;
            default:
                return true;
        }
        if (!persist_active_player_settings(app)) {
            app.settings = previous;
            app.osd.show("Unable to save playback settings", 8000);
        }
        refresh_playback_overlay(app);
        return true;
    }
    if (button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
        button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        const int direction =
            button == SDL_CONTROLLER_BUTTON_DPAD_UP ? -1 : 1;
        app.playback_overlay_selected =
            bfplayer::wrap_list_index(
                app.playback_overlay_selected, direction, 5);
        refresh_playback_overlay(app);
        return true;
    }
    if (button != SDL_CONTROLLER_BUTTON_A) {
        return true;
    }
    switch (app.playback_overlay_selected) {
        case 0:
            app.playback_overlay = PlaybackOverlay::none;
            refresh_playback_overlay(app);
            break;
        case 1:
            open_playback_overlay(app, PlaybackOverlay::controls);
            break;
        case 2:
            open_playback_overlay(app, PlaybackOverlay::settings);
            break;
        case 3:
            toggle_mute(app);
            refresh_playback_overlay(app);
            break;
        case 4:
            app.playback_running = false;
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "playback-stop requested=menu");
            break;
        default:
            break;
    }
    return true;
}

void on_controller_button(App& app, SDL_GameControllerButton button) {
    if (handle_playback_overlay_button(app, button)) {
        return;
    }
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            toggle_pause(app);
            break;
        case SDL_CONTROLLER_BUTTON_X:
            cycle_stream(app, KIT_STREAMTYPE_AUDIO, false);
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            cycle_stream(app, KIT_STREAMTYPE_VIDEO, false);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            seek_relative(
                app,
                -static_cast<double>(app.settings.short_seek_seconds));
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            seek_relative(
                app,
                static_cast<double>(app.settings.short_seek_seconds));
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            seek_relative(
                app,
                -static_cast<double>(app.settings.long_seek_seconds));
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            seek_relative(
                app,
                static_cast<double>(app.settings.long_seek_seconds));
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            seek_chapter(app, -1);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            seek_chapter(app, 1);
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            set_volume(app, app.volume_percent - 5);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            set_volume(app, app.volume_percent + 5);
            break;
        case bfplayer::kControllerTouchpadButton:
            open_playback_overlay(app, PlaybackOverlay::controls);
            break;
#if !defined(BFPLAYER_PS5)
        case SDL_CONTROLLER_BUTTON_BACK:
            toggle_mute(app);
            break;
#endif
        case bfplayer::kControllerOptionsButton:
            open_playback_overlay(app, PlaybackOverlay::menu);
            break;
        default:
            break;
    }
}

void pump_events(App& app) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        app.redraw_requested = true;
        if (event.type == SDL_QUIT) {
            app.running = false;
            app.playback_running = false;
            continue;
        }
        switch (event.type) {
            case SDL_CONTROLLERDEVICEADDED:
                if (!app.controller && SDL_IsGameController(event.cdevice.which)) {
                    app.controller = SDL_GameControllerOpen(event.cdevice.which);
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (app.controller &&
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(app.controller)) ==
                        event.cdevice.which) {
                    SDL_GameControllerClose(app.controller);
                    app.controller = nullptr;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (app.playback_overlay != PlaybackOverlay::none) {
                    if (event.cbutton.button ==
                        SDL_CONTROLLER_BUTTON_DPAD_UP) {
                        app.navigation_repeat.press(
                            -1,
                            SDL_GetTicks64());
                    } else if (
                        event.cbutton.button ==
                        SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                        app.navigation_repeat.press(
                            1,
                            SDL_GetTicks64());
                    }
                }
                on_controller_button(
                    app, static_cast<SDL_GameControllerButton>(event.cbutton.button));
                break;
            case SDL_CONTROLLERBUTTONUP:
                if (event.cbutton.button ==
                    SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    app.navigation_repeat.release(-1);
                } else if (
                    event.cbutton.button ==
                    SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    app.navigation_repeat.release(1);
                }
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    if (app.playback_overlay == PlaybackOverlay::none) {
                        open_playback_overlay(app, PlaybackOverlay::menu);
                    } else {
                        app.playback_overlay = PlaybackOverlay::none;
                        refresh_playback_overlay(app);
                    }
                } else if (event.key.keysym.sym == SDLK_F1) {
                    open_playback_overlay(app, PlaybackOverlay::controls);
                } else if (app.playback_overlay != PlaybackOverlay::none) {
                    if (event.key.keysym.sym == SDLK_UP) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_DPAD_UP);
                    } else if (event.key.keysym.sym == SDLK_DOWN) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
                    } else if (event.key.keysym.sym == SDLK_LEFT) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
                    } else if (event.key.keysym.sym == SDLK_RIGHT) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
                    } else if (event.key.keysym.sym == SDLK_RETURN) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_A);
                    } else if (event.key.keysym.sym == SDLK_BACKSPACE) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_B);
                    } else if (event.key.keysym.sym == SDLK_x) {
                        on_controller_button(app, SDL_CONTROLLER_BUTTON_X);
                    }
                } else if (event.key.keysym.sym == SDLK_SPACE) {
                    toggle_pause(app);
                } else if (event.key.keysym.sym == SDLK_LEFT) {
                    seek_relative(
                        app,
                        -static_cast<double>(app.settings.short_seek_seconds));
                } else if (event.key.keysym.sym == SDLK_RIGHT) {
                    seek_relative(
                        app,
                        static_cast<double>(app.settings.short_seek_seconds));
                }
                break;
            default:
                break;
        }
    }
    if (app.playback_overlay == PlaybackOverlay::none) {
        app.navigation_repeat.reset();
        return;
    }
    const int repeated =
        app.navigation_repeat.poll(SDL_GetTicks64());
    const SDL_GameControllerButton repeated_button =
        repeated < 0
        ? SDL_CONTROLLER_BUTTON_DPAD_UP
        : SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    for (int index = 0; index < std::abs(repeated); ++index) {
        on_controller_button(app, repeated_button);
    }
}

PlaybackOutcome run_player(
    App& app,
    const char* path) {
    app.source_display_aspect = 0.0;
    app.current_media_path = path ? bfplayer::redact_uri_secrets(path) : std::string{};
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-start path=%s",
        app.current_media_path.c_str());
    app.playback_running = true;
    app.paused = false;
    app.playback_overlay = PlaybackOverlay::none;
    app.playback_overlay_selected = 0;
    const bool native_hdr_surface_at_open =
        ps5_native_hdr_output_active();
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-open-stage stage=opening-osd-open-begin");
    const bool opening_osd_ready =
        app.osd.open(app.renderer, app.fallback_font);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-open-stage stage=opening-osd-open-complete ready=%d",
        opening_osd_ready ? 1 : 0);
    if (opening_osd_ready && !native_hdr_surface_at_open) {
        SDL_SetRenderDrawColor(app.renderer, 8, 13, 25, 255);
        SDL_RenderClear(app.renderer);
        app.osd.show("Opening media...", 60000);
        app.osd.render(0.0, 0.0, false);
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "playback-open-stage stage=opening-frame-present-begin");
        SDL_RenderPresent(app.renderer);
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "playback-open-stage stage=opening-frame-present-complete");
    }
    const auto show_open_error = [&](const std::string& message) {
        if (!opening_osd_ready) {
            return;
        }
        const Uint64 end = SDL_GetTicks64() + 2200;
        app.osd.show("Unable to play: " + message, 2200);
        while (SDL_GetTicks64() < end) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_CONTROLLERBUTTONDOWN &&
                     event.cbutton.button ==
                         bfplayer::kControllerOptionsButton)) {
                    return;
                }
            }
            SDL_SetRenderDrawColor(app.renderer, 8, 13, 25, 255);
            SDL_RenderClear(app.renderer);
            app.osd.render(0.0, 0.0, false);
            SDL_RenderPresent(app.renderer);
            SDL_Delay(16);
        }
    };
    std::string source_error;
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-open-stage stage=source-open-begin");
    app.source = create_bounded_source(app, path, source_error);
    bfplayer::diagnostics_log(
        app.source
            ? bfplayer::DiagnosticLevel::info
            : bfplayer::DiagnosticLevel::error,
        "playback-open-stage stage=source-open-complete success=%d error=%s",
        app.source ? 1 : 0,
        source_error.empty() ? "<none>" : source_error.c_str());
    if (!app.source) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "playback-start failed path=%s error=%s",
            app.current_media_path.c_str(),
            source_error.c_str());
        show_open_error(source_error);
        return PlaybackOutcome::error;
    }
    bfplayer::LibraryDatabase resume_database;
    const bool database_available =
        resume_database.open("/data/BFplayer/library.db");
    app.settings = database_available
        ? load_player_settings(resume_database)
        : bfplayer::PlayerSettings{};
    app.volume_percent = app.settings.volume_percent;
    app.previous_volume_percent =
        app.volume_percent > 0 ? app.volume_percent : 100;
    app.osd.set_default_duration(
        static_cast<std::uint64_t>(app.settings.osd_duration_ms));
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "player-settings volume=%d short_seek=%d long_seek=%d osd_ms=%d resume=%d",
        app.settings.volume_percent,
        app.settings.short_seek_seconds,
        app.settings.long_seek_seconds,
        app.settings.osd_duration_ms,
        app.settings.resume_playback ? 1 : 0);
    if (const auto* format = static_cast<const AVFormatContext*>(app.source->format_ctx)) {
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVMediaType type = format->streams[index]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO ||
                type == AVMEDIA_TYPE_SUBTITLE) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::info,
                    "track index=%u type=%d description=%s",
                    index,
                    type,
                    track_description(app, static_cast<int>(index)).c_str());
            }
        }
    }

    Kit_AudioFormatRequest audio_request{};
    Kit_ResetAudioFormatRequest(&audio_request);
    // SDL_kitchensink 2.0 only maps U8/S16/S32 SDL formats to FFmpeg.
    // Requesting F32 produces AV_SAMPLE_FMT_NONE and makes swr_init fail.
    audio_request.format = AUDIO_S16SYS;
    audio_request.is_signed = 1;
    audio_request.bytes = 2;
    audio_request.sample_rate = 48000;
    audio_request.channels = 2;

    const int initial_video =
        Kit_GetBestSourceStream(app.source, KIT_STREAMTYPE_VIDEO);
    const int initial_audio =
        Kit_GetBestSourceStream(app.source, KIT_STREAMTYPE_AUDIO);
    const bool network_source = bfplayer::is_network_uri(path);
    const PlaybackBufferPolicy buffer_policy =
        playback_buffer_policy(network_source);
    apply_playback_buffer_policy(buffer_policy);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-buffer-policy kind=%s decoder_threads=%d video_packets=%d video_frames=%d audio_packets=%d audio_frames=%d peak_rss_kib=%llu",
        network_source ? "network" : "local",
        Kit_GetHint(KIT_HINT_THREAD_COUNT),
        buffer_policy.video_packets,
        buffer_policy.video_frames,
        buffer_policy.audio_packets,
        buffer_policy.audio_frames,
        static_cast<unsigned long long>(process_peak_rss_kib()));
    const VideoSourceInfo source_video = inspect_video_source(
        static_cast<const AVFormatContext*>(app.source->format_ctx),
        initial_video);
    app.source_frame_rate = source_video.frame_rate;
    app.source_width = source_video.width;
    app.source_height = source_video.height;
    app.source_hdr = source_video.hdr_source;
    app.source_hdr_transfer = source_video.color_transfer;
    app.source_display_aspect = source_video.display_aspect;
    const bool backend_native_hdr =
        ps5_native_hdr_output_supported();
    const char* native_hdr_policy = std::getenv("BFPLAYER_NATIVE_HDR");
    const bool native_hdr_allowed =
        !native_hdr_policy ||
        std::strcmp(native_hdr_policy, "0") != 0;
    const bool hlg_source =
        source_video.color_transfer_value == AVCOL_TRC_ARIB_STD_B67;
    const bool pq_source =
        source_video.color_transfer_value == AVCOL_TRC_SMPTE2084;
    const bool native_hdr_requested =
        source_video.hdr_source &&
        (pq_source || hlg_source) &&
        backend_native_hdr &&
        native_hdr_allowed;
    const int native_hdr_result =
        native_hdr_requested
        ? set_ps5_native_hdr_output(true)
        : 0;
    const bool native_hdr_output =
        native_hdr_requested && native_hdr_result == 0;
    app.native_hdr_output = native_hdr_output;
    const char* hdr_output_policy =
        native_hdr_output
            ? hlg_source
                ? "native-bt2020-hlg-to-pq-10bit"
                : "native-bt2020-pq-10bit"
            : source_video.hdr_source
                ? "software-tone-map-bt709-limited"
                : "passthrough";
    const Uint64 hlg_prepare_started =
        SDL_GetPerformanceCounter();
    const int hlg_prepare_result =
        native_hdr_output && hlg_source
            ? prepare_ps5_native_hdr_hlg()
            : 0;
    const double hlg_prepare_ms =
        native_hdr_output && hlg_source
            ? elapsed_performance_ms(
                  hlg_prepare_started,
                  SDL_GetPerformanceCounter(),
                  SDL_GetPerformanceFrequency())
            : 0.0;
    if (native_hdr_output && hlg_source) {
        bfplayer::diagnostics_log(
            hlg_prepare_result == 0
                ? bfplayer::DiagnosticLevel::info
                : bfplayer::DiagnosticLevel::warning,
            "native-hdr-hlg-prepare result=%d elapsed_ms=%.3f",
            hlg_prepare_result,
            hlg_prepare_ms);
    }
    bfplayer::diagnostics_log(
        native_hdr_requested && !native_hdr_output
            ? bfplayer::DiagnosticLevel::warning
            : bfplayer::DiagnosticLevel::info,
        "native-hdr-switch requested=%d active=%d result=%d",
        native_hdr_requested ? 1 : 0,
        native_hdr_output ? 1 : 0,
        native_hdr_result);
    SDL_YUV_CONVERSION_MODE yuv_conversion_mode =
        SDL_YUV_CONVERSION_AUTOMATIC;
    if (source_video.hdr_source && !native_hdr_output) {
        // The decoder tone-maps HDR to limited-range BT.709 SDR in place.
        yuv_conversion_mode = SDL_YUV_CONVERSION_BT709;
    } else if (source_video.color_range_value == AVCOL_RANGE_JPEG) {
        yuv_conversion_mode = SDL_YUV_CONVERSION_JPEG;
    } else if (source_video.color_space_value == AVCOL_SPC_BT709) {
        yuv_conversion_mode = SDL_YUV_CONVERSION_BT709;
    }
    SDL_SetYUVConversionMode(yuv_conversion_mode);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-color-output source_hdr=%d sdl_yuv_mode=%d hdr_policy=%s",
        source_video.hdr_source ? 1 : 0,
        static_cast<int>(yuv_conversion_mode),
        hdr_output_policy);
    const bool source_above_1080p =
        source_video.width > kWindowWidth ||
        source_video.height > kWindowHeight;
    /*
     * Preserve source resolution by default. BFplayer's direct native-PQ path
     * and bounded software path avoid the old redundant 4K texture copies, so
     * silently reducing demanding sources to 1080p is no longer appropriate.
     * An explicit "smooth" policy remains available as a compatibility escape
     * hatch for unusually expensive files or storage/network bottlenecks.
     */
    const char* output_policy = std::getenv("BFPLAYER_OUTPUT_POLICY");
    const bool request_smooth_output =
        output_policy &&
        std::strcmp(output_policy, "smooth") == 0;
    const bool smooth_software_output =
        source_above_1080p &&
        source_video.demanding_software_decode &&
        request_smooth_output &&
        !native_hdr_output;
    const bool wants_true_4k =
        source_above_1080p && !smooth_software_output;
    if (wants_true_4k) {
        (void)switch_fullscreen_output(
            app,
            kPreferredOutputWidth,
            kPreferredOutputHeight,
            "source-above-1080p");
    } else {
        (void)switch_fullscreen_output(
            app,
            kWindowWidth,
            kWindowHeight,
            smooth_software_output
                ? "demanding-4k-software-decode"
                : "source-at-or-below-1080p");
    }
    const Kit_VideoFormatRequest video_request =
        make_video_format_request(
            source_video,
            app.output_width,
            app.output_height,
            native_hdr_output);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-source codec=%s profile=%d pix_fmt=%s bit_depth=%d size=%dx%d sar=%d:%d dar=%.6f fps=%.3f bitrate=%lld range=%s colorspace=%s transfer=%s primaries=%s hdr=%d demanding_software_decode=%d",
        source_video.codec,
        source_video.profile,
        source_video.pixel_format,
        source_video.bit_depth,
        source_video.width,
        source_video.height,
        source_video.sample_aspect_numerator,
        source_video.sample_aspect_denominator,
        source_video.display_aspect,
        source_video.frame_rate,
        static_cast<long long>(source_video.bit_rate),
        source_video.color_range,
        source_video.color_space,
        source_video.color_transfer,
        source_video.color_primaries,
        source_video.hdr_source ? 1 : 0,
        source_video.demanding_software_decode ? 1 : 0);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "video-output-request source=%dx%d output=%dx%d display=%dx%d downscale=%d true_4k=%d smooth_software_output=%d output_policy=%s native_hdr=%d pixel_format=%u hdr_chroma_levels=%d hdr_static_dither=1",
        source_video.width,
        source_video.height,
        video_request.width,
        video_request.height,
        app.output_width,
        app.output_height,
        video_request.width > 0 && video_request.height > 0 ? 1 : 0,
        app.true_4k_output ? 1 : 0,
        smooth_software_output ? 1 : 0,
        smooth_software_output ? "smooth" : "source",
        native_hdr_output ? 1 : 0,
        video_request.format,
        BFPLAYER_HDR_CHROMA_LEVELS);
    // Subtitles are no longer selected or rendered; always pass -1.
    app.player = Kit_CreatePlayer(
        app.source,
        initial_video,
        initial_audio,
        -1,
        &video_request,
        &audio_request,
        kWindowWidth,
        kWindowHeight);
    if (!app.player) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "player-create failed error=%s",
            Kit_GetError());
        show_open_error(Kit_GetError());
        return PlaybackOutcome::error;
    }
    if (Kit_GetPlayerVideoStream(app.player) < 0 &&
        Kit_GetPlayerAudioStream(app.player) < 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "player-create failed reason=no-decodable-audio-video");
        return PlaybackOutcome::error;
    }

    if (!open_audio(app) || !create_video_textures(app)) {
        show_open_error(SDL_GetError());
        return PlaybackOutcome::error;
    }
    if (!opening_osd_ready) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "osd-open failed error=%s",
            app.osd.error().c_str());
    }

    const bool source_persistence_allowed =
        !bfplayer::uri_has_sensitive_components(path);
    bfplayer::ResumeState saved_resume{};
    double pending_resume_seconds = 0.0;
    if (app.settings.resume_playback &&
        database_available &&
        source_persistence_allowed &&
        resume_database.load_resume(path, saved_resume)) {
        const double resume_seconds = static_cast<double>(saved_resume.position_ms) / 1000.0;
        const double duration = Kit_GetPlayerDuration(app.player);
        if (std::isfinite(duration) && !saved_resume.completed &&
            resume_seconds >= 10.0 &&
            resume_seconds < duration - 30.0) {
            // Kitchensink ignores/rejects seeks while the player is stopped.
            // Apply this immediately after Kit_PlayerPlay() below.
            pending_resume_seconds = resume_seconds;
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "resume-pending seconds=%.3f duration=%.3f",
                resume_seconds,
                duration);
        } else {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "resume-skipped saved_ms=%lld duration=%.3f completed=%d",
                static_cast<long long>(saved_resume.position_ms),
                duration,
                saved_resume.completed ? 1 : 0);
        }
    } else if (app.settings.resume_playback && database_available &&
               source_persistence_allowed) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "resume-not-found");
    }
    bfplayer::TrackPreferences saved_preferences{};
    if (database_available && source_persistence_allowed &&
        resume_database.load_track_preferences(path, saved_preferences)) {
        restore_track_preferences(app, saved_preferences);
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "preferences-restored audio=%d",
            saved_preferences.audio_stream);
    }

    Kit_PlayerInfo info{};
    Kit_GetPlayerInfo(app.player, &info);
    int sample_aspect_numerator = 0;
    int sample_aspect_denominator = 0;
    const int sample_aspect_status = Kit_GetPlayerAspectRatio(
        app.player,
        &sample_aspect_numerator,
        &sample_aspect_denominator);
    const double initial_display_aspect =
        display_aspect_from_sample_aspect(
            info.video_format.width,
            info.video_format.height,
            sample_aspect_numerator,
            sample_aspect_denominator);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-format video=%s threads=%d frame_threading=%u slice_threading=%u output=%dx%d format=%u audio=%s threads=%d %dHz/%dch duration_s=%.3f",
        info.video_codec.name,
        info.video_codec.threads,
        info.video_codec.frame_threading,
        info.video_codec.slice_threading,
        info.video_format.width,
        info.video_format.height,
        static_cast<unsigned int>(info.video_format.format),
        info.audio_codec.name,
        info.audio_codec.threads,
        info.audio_format.sample_rate,
        info.audio_format.channels,
        Kit_GetPlayerDuration(app.player));
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-aspect frame=%dx%d sar=%d:%d sar_status=%d dar=%.6f",
        info.video_format.width,
        info.video_format.height,
        sample_aspect_numerator,
        sample_aspect_denominator,
        sample_aspect_status,
        initial_display_aspect);

    Kit_PlayerPlay(app.player);
    if (pending_resume_seconds > 0.0) {
        if (Kit_PlayerSeek(app.player, pending_resume_seconds) == 0) {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "resume-applied seconds=%.3f actual=%.3f",
                pending_resume_seconds,
                Kit_GetPlayerPosition(app.player));
            app.osd.show(
                "Resumed at " +
                    std::to_string(
                        static_cast<long long>(pending_resume_seconds)) +
                    " seconds",
                5000);
        } else {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::error,
                "resume-seek failed seconds=%.3f error=%s",
                pending_resume_seconds,
                Kit_GetError());
        }
    } else if (native_hdr_output) {
        app.osd.show(
            "Native 4K HDR   Cross Play/Pause   D-pad Seek   Options Menu",
            4000);
    } else if (source_video.demanding_software_decode) {
        app.osd.show(
            "High-load 4K video: software decoding may drop frames",
            5000);
    } else {
        app.osd.show(
            "Cross Play/Pause   D-pad Seek   Touchpad Controls   Options Menu",
            6000);
    }
    Uint64 last_resume_save = SDL_GetTicks64();
    Uint64 last_diagnostics = last_resume_save;
    Uint64 last_remote_status = last_resume_save;
    Uint64 last_visual_render = 0;
    SDL_Rect previous_native_overlay_bounds{};
    bool previous_native_overlay_active = false;
    bool native_overlay_surface_initialized = false;
    const Uint64 performance_frequency = SDL_GetPerformanceFrequency();
    TimingWindow loop_timing;
    TimingWindow audio_pull_timing;
    TimingWindow video_pull_timing;
    TimingWindow render_timing;
    TimingWindow present_timing;
    double active_playback_ms = 0.0;
    struct rusage diagnostics_usage {};
    (void)getrusage(RUSAGE_SELF, &diagnostics_usage);
    bfplayer::RemotePlaybackStatus remote_status;
    remote_status.phase = "playback";
    remote_status.media_path = app.current_media_path;
    remote_status.playing = true;
    remote_status.source_fps = source_video.frame_rate;
    remote_status.source_width = source_video.width;
    remote_status.source_height = source_video.height;
    remote_status.output_width = info.video_format.width;
    remote_status.output_height = info.video_format.height;
    remote_status.display_width = app.output_width;
    remote_status.display_height = app.output_height;
    remote_status.audio_stream =
        Kit_GetPlayerAudioStream(app.player);
    remote_status.subtitle_stream =
        Kit_GetPlayerSubtitleStream(app.player);
    remote_status.hdr_source = source_video.hdr_source;
    remote_status.native_hdr_output = native_hdr_output;
    remote_status.hdr_transfer = source_video.color_transfer;
    remote_status.hdr_output_policy = hdr_output_policy;
    std::uint64_t video_updates = 0;
    std::uint64_t video_empty_polls = 0;
    static_assert(kAudioBufferBytes % sizeof(std::int16_t) == 0);
    std::array<std::int16_t, kAudioBufferBytes / sizeof(std::int16_t)> audio_buffer{};

    while (app.running && app.playback_running &&
           Kit_GetPlayerState(app.player) != KIT_STOPPED) {
        const Uint64 loop_started = SDL_GetPerformanceCounter();
        pump_events(app);
        consume_remote_commands(app, true);

        if (!app.paused && app.audio != 0) {
            const Uint32 queued = SDL_GetQueuedAudioSize(app.audio);
            if (queued < kAudioBufferBytes) {
                const std::size_t available =
                    static_cast<std::size_t>(kAudioBufferBytes - queued);
                const Uint64 audio_pull_started = SDL_GetPerformanceCounter();
                const int got = Kit_GetPlayerAudioData(
                    app.player,
                    queued,
                    reinterpret_cast<unsigned char*>(audio_buffer.data()),
                    available);
                audio_pull_timing.add(elapsed_performance_ms(
                    audio_pull_started,
                    SDL_GetPerformanceCounter(),
                    performance_frequency));
                if (got > 0) {
                    if (static_cast<std::size_t>(got) > available ||
                        got % static_cast<int>(sizeof(std::int16_t)) != 0) {
                        bfplayer::diagnostics_log(
                            bfplayer::DiagnosticLevel::error,
                            "audio-pull invalid-bytes got=%d available=%zu",
                            got,
                            available);
                        app.playback_running = false;
                        continue;
                    }
                    if (app.volume_percent < 100) {
                        const std::size_t sample_count =
                            static_cast<std::size_t>(got) / sizeof(std::int16_t);
                        for (std::size_t index = 0; index < sample_count; ++index) {
                            const std::int32_t scaled =
                                static_cast<std::int32_t>(audio_buffer[index]) *
                                app.volume_percent / 100;
                            audio_buffer[index] = static_cast<std::int16_t>(scaled);
                        }
                    }
                    if (SDL_QueueAudio(
                            app.audio,
                            audio_buffer.data(),
                            static_cast<Uint32>(got)) != 0) {
                        log_sdl("SDL_QueueAudio");
                        app.playback_running = false;
                    }
                }
            }
        }

        bool video_frame_updated = false;
        if ((app.video || native_hdr_output) && !app.paused) {
            const Uint64 video_pull_started = SDL_GetPerformanceCounter();
            int video_result = 1;
            if (native_hdr_output) {
                unsigned char** frame_data = nullptr;
                int* frame_lines = nullptr;
                SDL_Rect frame_area{};
                video_result = Kit_LockPlayerVideoRawFrame(
                    app.player,
                    &frame_data,
                    &frame_lines,
                    &frame_area);
                video_pull_timing.add(elapsed_performance_ms(
                    video_pull_started,
                    SDL_GetPerformanceCounter(),
                    performance_frequency));
                if (video_result == 0) {
                    SDL_Surface* overlay_surface = nullptr;
                    bool overlay_active = false;
                    SDL_Rect overlay_bounds{};
                    const auto include_overlay_bounds =
                        [&](SDL_Rect bounds) {
                            const int left = std::clamp(
                                bounds.x, 0, kWindowWidth);
                            const int top = std::clamp(
                                bounds.y, 0, kWindowHeight);
                            const int right = std::clamp(
                                bounds.x + bounds.w,
                                0,
                                kWindowWidth);
                            const int bottom = std::clamp(
                                bounds.y + bounds.h,
                                0,
                                kWindowHeight);
                            if (right <= left || bottom <= top) {
                                return;
                            }
                            if (!overlay_active) {
                                overlay_bounds = {
                                    left,
                                    top,
                                    right - left,
                                    bottom - top};
                            } else {
                                const int union_left = std::min(
                                    overlay_bounds.x, left);
                                const int union_top = std::min(
                                    overlay_bounds.y, top);
                                const int union_right = std::max(
                                    overlay_bounds.x + overlay_bounds.w,
                                    right);
                                const int union_bottom = std::max(
                                    overlay_bounds.y + overlay_bounds.h,
                                    bottom);
                                overlay_bounds = {
                                    union_left,
                                    union_top,
                                    union_right - union_left,
                                    union_bottom - union_top};
                            }
                            overlay_active = true;
                        };
                    /*
                     * The native frame is 4K, so SDL's software surface is
                     * also 4K even though UI coordinates remain 1920x1080.
                     * Clearing all 33 MiB every frame starves software video
                     * decoding. Clear only the prior overlay footprint after
                     * the first frame.
                     */
                    SDL_RenderSetLogicalSize(
                        app.renderer, kWindowWidth, kWindowHeight);
                    SDL_SetRenderDrawBlendMode(
                        app.renderer, SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 0);
                    if (!native_overlay_surface_initialized) {
                        SDL_Rect full_overlay{
                            0, 0, kWindowWidth, kWindowHeight};
                        SDL_RenderFillRect(
                            app.renderer, &full_overlay);
                        native_overlay_surface_initialized = true;
                    } else if (previous_native_overlay_active) {
                        SDL_RenderFillRect(
                            app.renderer,
                            &previous_native_overlay_bounds);
                    }
                    SDL_SetRenderDrawBlendMode(
                        app.renderer, SDL_BLENDMODE_BLEND);
                    if (app.paused || app.osd.visible()) {
                        app.osd.render(
                            Kit_GetPlayerPosition(app.player),
                            Kit_GetPlayerDuration(app.player),
                            app.paused);
                        include_overlay_bounds(
                            {0, kWindowHeight - 170, kWindowWidth, 170});
                        if (app.osd.panel_visible()) {
                            include_overlay_bounds(
                                {0, 126, kWindowWidth, 824});
                        }
                    }
                    if (overlay_active ||
                        previous_native_overlay_active) {
                        (void)SDL_RenderFlush(app.renderer);
                    }
                    previous_native_overlay_bounds = overlay_bounds;
                    previous_native_overlay_active = overlay_active;
                    if (overlay_active) {
                        overlay_surface = SDL_GetWindowSurface(app.window);
                        if (!overlay_surface ||
                            overlay_surface->format->BytesPerPixel != 4) {
                            overlay_active = false;
                            bfplayer::diagnostics_log(
                                bfplayer::DiagnosticLevel::error,
                                "native-hdr-overlay unavailable error=%s",
                                SDL_GetError());
                        }
                    }
                    SDL_Rect physical_overlay_bounds{};
                    if (overlay_active && overlay_surface) {
                        const auto scale_floor = [](
                                                     int value,
                                                     int output,
                                                     int logical) {
                            return static_cast<int>(
                                static_cast<std::int64_t>(value) *
                                output / logical);
                        };
                        const auto scale_ceil = [](
                                                    int value,
                                                    int output,
                                                    int logical) {
                            return static_cast<int>(
                                (static_cast<std::int64_t>(value) *
                                     output +
                                 logical - 1) /
                                logical);
                        };
                        const int left = scale_floor(
                            overlay_bounds.x,
                            overlay_surface->w,
                            kWindowWidth);
                        const int top = scale_floor(
                            overlay_bounds.y,
                            overlay_surface->h,
                            kWindowHeight);
                        const int right = scale_ceil(
                            overlay_bounds.x + overlay_bounds.w,
                            overlay_surface->w,
                            kWindowWidth);
                        const int bottom = scale_ceil(
                            overlay_bounds.y + overlay_bounds.h,
                            overlay_surface->h,
                            kWindowHeight);
                        physical_overlay_bounds = {
                            left,
                            top,
                            std::max(0, right - left),
                            std::max(0, bottom - top)};
                    }
                    const VideoPresentLayout native_layout =
                        player_video_layout(app);
                    const auto scale_layout_value = [](
                                                        int value,
                                                        int output,
                                                        int logical) {
                        return static_cast<int>(std::llround(
                            static_cast<double>(value) *
                            static_cast<double>(output) /
                            static_cast<double>(logical)));
                    };
                    const int native_source_x =
                        native_layout.crop_source
                        ? native_layout.source.x
                        : 0;
                    const int native_source_y =
                        native_layout.crop_source
                        ? native_layout.source.y
                        : 0;
                    const int native_source_width =
                        native_layout.crop_source
                        ? native_layout.source.width
                        : frame_area.w;
                    const int native_source_height =
                        native_layout.crop_source
                        ? native_layout.source.height
                        : frame_area.h;
                    const int native_destination_x =
                        scale_layout_value(
                            native_layout.destination.x,
                            app.output_width,
                            kWindowWidth);
                    const int native_destination_y =
                        scale_layout_value(
                            native_layout.destination.y,
                            app.output_height,
                            kWindowHeight);
                    const int native_destination_width =
                        scale_layout_value(
                            native_layout.destination.width,
                            app.output_width,
                            kWindowWidth);
                    const int native_destination_height =
                        scale_layout_value(
                            native_layout.destination.height,
                            app.output_height,
                            kWindowHeight);
                    const Uint64 present_started =
                        SDL_GetPerformanceCounter();
                    const int present_result =
                        present_ps5_native_hdr_yuv420p10_frame(
                            frame_data ? frame_data[0] : nullptr,
                            frame_lines ? frame_lines[0] : 0,
                            frame_data ? frame_data[1] : nullptr,
                            frame_lines ? frame_lines[1] : 0,
                            frame_data ? frame_data[2] : nullptr,
                            frame_lines ? frame_lines[2] : 0,
                            frame_area.w,
                            frame_area.h,
                            app.output_width,
                            app.output_height,
                            native_source_x,
                            native_source_y,
                            native_source_width,
                            native_source_height,
                            native_destination_x,
                            native_destination_y,
                            native_destination_width,
                            native_destination_height,
                            source_video.color_range_value ==
                                AVCOL_RANGE_JPEG,
                            hlg_source,
                            overlay_active
                                ? overlay_surface->pixels
                                : nullptr,
                            overlay_active
                                ? overlay_surface->pitch
                                : 0,
                            overlay_active
                                ? overlay_surface->w
                                : 0,
                            overlay_active
                                ? overlay_surface->h
                                : 0,
                            overlay_active
                                ? physical_overlay_bounds.x
                                : 0,
                            overlay_active
                                ? physical_overlay_bounds.y
                                : 0,
                            overlay_active
                                ? physical_overlay_bounds.w
                                : 0,
                            overlay_active
                                ? physical_overlay_bounds.h
                                : 0);
                    present_timing.add(elapsed_performance_ms(
                        present_started,
                        SDL_GetPerformanceCounter(),
                        performance_frequency));
                    Kit_UnlockPlayerVideoRawFrame(app.player);
                    if (present_result == 0) {
                        video_frame_updated = true;
                        ++video_updates;
                    } else {
                        bfplayer::diagnostics_log(
                            bfplayer::DiagnosticLevel::error,
                            "native-hdr-present failed result=%d frame=%dx%d output=%dx%d",
                            present_result,
                            frame_area.w,
                            frame_area.h,
                            app.output_width,
                            app.output_height);
                        app.playback_running = false;
                    }
                } else {
                    ++video_empty_polls;
                }
                video_result = -1;
            } else {
                video_result = Kit_GetPlayerVideoSDLTexture(
                    app.player, app.video, nullptr);
                video_pull_timing.add(elapsed_performance_ms(
                    video_pull_started,
                    SDL_GetPerformanceCounter(),
                    performance_frequency));
                if (video_result == 0) {
                    video_frame_updated = true;
                    ++video_updates;
                } else {
                    ++video_empty_polls;
                }
            }
        }
        Uint64 now = SDL_GetTicks64();
        const bool audio_only_refresh =
            !app.video && now - last_visual_render >= 33;
        const bool should_render =
            app.redraw_requested || video_frame_updated ||
            audio_only_refresh;
        if (native_hdr_output) {
            if (video_frame_updated) {
                app.redraw_requested = false;
                last_visual_render = now;
            } else {
                SDL_Delay(app.paused ? 8 : 1);
            }
        } else if (should_render) {
            const Uint64 render_started = SDL_GetPerformanceCounter();
            if (!app.video) {
                update_now_playing_artwork(app);
            }
            if (!app.video && app.now_playing_artwork) {
                SDL_SetRenderDrawColor(
                    app.renderer,
                    app.now_playing_background_color.r,
                    app.now_playing_background_color.g,
                    app.now_playing_background_color.b,
                    255);
            } else {
                SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
            }
            SDL_RenderClear(app.renderer);
            if (app.video) {
                const VideoPresentLayout layout = player_video_layout(app);
                const SDL_Rect source{
                    layout.source.x,
                    layout.source.y,
                    layout.source.width,
                    layout.source.height};
                const SDL_Rect destination{
                    layout.destination.x,
                    layout.destination.y,
                    layout.destination.width,
                    layout.destination.height};
                SDL_RenderCopy(
                    app.renderer,
                    app.video,
                    layout.crop_source ? &source : nullptr,
                    &destination);
            } else if (
                app.now_playing_artwork &&
                app.now_playing_artwork_width > 0 &&
                app.now_playing_artwork_height > 0) {
                // Large, centered album art over the sampled-color backdrop,
                // Poweramp-style, instead of a near-empty black frame.
                //
                // Centered in the region ABOVE PlaybackOsd's opaque
                // transport strip (kNowPlayingOsdStripHeight), not the full
                // window: that strip is painted at full alpha whenever it is
                // shown, and for any cover with aspect ratio <= ~2:1 (i.e.
                // effectively all real album art, square or portrait) this
                // path is height-bound, so centering in the full window
                // would put the art's bottom edge under the strip.
                const int available_width =
                    kWindowWidth - 2 * kNowPlayingArtworkMargin;
                const int available_region_height =
                    kWindowHeight - kNowPlayingOsdStripHeight;
                const int available_height =
                    available_region_height - 2 * kNowPlayingArtworkMargin;
                const double scale = std::min(
                    static_cast<double>(available_width) /
                        app.now_playing_artwork_width,
                    static_cast<double>(available_height) /
                        app.now_playing_artwork_height);
                const int width = std::max(
                    1,
                    static_cast<int>(app.now_playing_artwork_width * scale));
                const int height = std::max(
                    1,
                    static_cast<int>(app.now_playing_artwork_height * scale));
                const SDL_Rect target{
                    (kWindowWidth - width) / 2,
                    (available_region_height - height) / 2,
                    width,
                    height};
                SDL_RenderCopy(
                    app.renderer, app.now_playing_artwork, nullptr, &target);
            }
            app.osd.render(
                Kit_GetPlayerPosition(app.player),
                Kit_GetPlayerDuration(app.player),
                app.paused);
            render_timing.add(elapsed_performance_ms(
                render_started,
                SDL_GetPerformanceCounter(),
                performance_frequency));
            const Uint64 present_started = SDL_GetPerformanceCounter();
            SDL_RenderPresent(app.renderer);
            present_timing.add(elapsed_performance_ms(
                present_started,
                SDL_GetPerformanceCounter(),
                performance_frequency));
            app.redraw_requested = false;
            last_visual_render = now;
        } else {
            SDL_Delay(app.paused ? 8 : 1);
        }
        now = SDL_GetTicks64();
        const double current_loop_ms = elapsed_performance_ms(
            loop_started,
            SDL_GetPerformanceCounter(),
            performance_frequency);
        loop_timing.add(current_loop_ms);
        if (!app.paused) {
            active_playback_ms += current_loop_ms;
        }
        if (now - last_diagnostics >= 5000) {
            const double diagnostics_seconds =
                static_cast<double>(now - last_diagnostics) / 1000.0;
            const double video_update_rate = diagnostics_seconds > 0.0
                ? static_cast<double>(video_updates) / diagnostics_seconds
                : 0.0;
            const double expected_frames =
                source_video.frame_rate > 0.0
                ? source_video.frame_rate * active_playback_ms / 1000.0
                : 0.0;
            const std::uint64_t missed_frames =
                expected_frames > static_cast<double>(video_updates)
                ? static_cast<std::uint64_t>(std::ceil(
                      expected_frames - static_cast<double>(video_updates)))
                : 0;
            const Uint32 audio_queued =
                app.audio != 0 ? SDL_GetQueuedAudioSize(app.audio) : 0U;
            const std::uint64_t audio_queue_ms =
                static_cast<std::uint64_t>(audio_queued) * 1000ULL /
                kAudioBytesPerSecond;
            struct rusage current_usage {};
            (void)getrusage(RUSAGE_SELF, &current_usage);
            const double cpu_seconds =
                process_cpu_seconds(current_usage) -
                process_cpu_seconds(diagnostics_usage);
            const double user_cpu_ms =
                (timeval_seconds(current_usage.ru_utime) -
                 timeval_seconds(diagnostics_usage.ru_utime)) * 1000.0;
            const double system_cpu_ms =
                (timeval_seconds(current_usage.ru_stime) -
                 timeval_seconds(diagnostics_usage.ru_stime)) * 1000.0;
            const double cpu_core_equivalents =
                diagnostics_seconds > 0.0
                ? std::max(0.0, cpu_seconds) / diagnostics_seconds
                : 0.0;
            const std::uint64_t voluntary_switches =
                current_usage.ru_nvcsw >= diagnostics_usage.ru_nvcsw
                ? static_cast<std::uint64_t>(
                      current_usage.ru_nvcsw - diagnostics_usage.ru_nvcsw)
                : 0;
            const std::uint64_t involuntary_switches =
                current_usage.ru_nivcsw >= diagnostics_usage.ru_nivcsw
                ? static_cast<std::uint64_t>(
                      current_usage.ru_nivcsw - diagnostics_usage.ru_nivcsw)
                : 0;
            const bfplayer::SafeReadFileStats file_stats =
                app.local_media.stats();
            remote_status.delivered_fps = video_update_rate;
            remote_status.loop_average_ms = loop_timing.average();
            remote_status.loop_max_ms = loop_timing.maximum_ms;
            remote_status.video_pull_average_ms =
                video_pull_timing.average();
            remote_status.video_pull_max_ms =
                video_pull_timing.maximum_ms;
            remote_status.render_average_ms = render_timing.average();
            remote_status.render_max_ms = render_timing.maximum_ms;
            remote_status.present_average_ms = present_timing.average();
            remote_status.present_max_ms = present_timing.maximum_ms;
            remote_status.present_p95_ms = present_timing.percentile(0.95);
            remote_status.present_p99_ms = present_timing.percentile(0.99);
            remote_status.audio_pull_average_ms =
                audio_pull_timing.average();
            remote_status.audio_pull_max_ms =
                audio_pull_timing.maximum_ms;
            remote_status.cpu_core_equivalents = cpu_core_equivalents;
            remote_status.user_cpu_ms = std::max(0.0, user_cpu_ms);
            remote_status.system_cpu_ms = std::max(0.0, system_cpu_ms);
            remote_status.voluntary_context_switches = voluntary_switches;
            remote_status.involuntary_context_switches =
                involuntary_switches;
            remote_status.video_updates = video_updates;
            remote_status.video_empty_polls = video_empty_polls;
            remote_status.estimated_missed_frames = missed_frames;
            remote_status.peak_rss_kib = process_peak_rss_kib();
            remote_status.media_bytes_read = file_stats.bytes_read;
            remote_status.media_read_calls = file_stats.read_calls;
            remote_status.media_read_time_us = file_stats.read_time_us;
            remote_status.media_seek_calls = file_stats.seek_calls;
            remote_status.media_seek_time_us = file_stats.seek_time_us;
            remote_status.audio_queued_bytes = audio_queued;
            update_decoder_buffer_status(app, remote_status);
            const Kit_VideoToneMapInfo tone_map_info =
                update_tone_map_status(app, remote_status);
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "playback-heartbeat path=%s position_s=%.3f duration_s=%.3f paused=%d state=%d active_ms=%.3f source_fps=%.3f video_updates=%llu video_update_fps=%.3f expected_frames=%.3f estimated_missed=%llu video_empty_polls=%llu video_frames=%u/%u video_packets=%u/%u audio_frames=%u/%u audio_packets=%u/%u audio_queued=%u audio_queue_ms=%llu peak_rss_kib=%llu cpu_cores=%.3f user_cpu_ms=%.3f system_cpu_ms=%.3f voluntary_ctx=%llu involuntary_ctx=%llu loop_avg_ms=%.3f loop_max_ms=%.3f audio_pull_avg_ms=%.3f audio_pull_max_ms=%.3f video_pull_avg_ms=%.3f video_pull_max_ms=%.3f render_avg_ms=%.3f render_max_ms=%.3f present_avg_ms=%.3f present_max_ms=%.3f present_p95_ms=%.3f present_p99_ms=%.3f present_count=%llu media_bytes=%llu media_reads=%llu media_read_ms=%.3f media_seeks=%llu media_seek_ms=%.3f",
                app.current_media_path.c_str(),
                Kit_GetPlayerPosition(app.player),
                Kit_GetPlayerDuration(app.player),
                app.paused ? 1 : 0,
                Kit_GetPlayerState(app.player),
                active_playback_ms,
                source_video.frame_rate,
                static_cast<unsigned long long>(video_updates),
                video_update_rate,
                expected_frames,
                static_cast<unsigned long long>(missed_frames),
                static_cast<unsigned long long>(video_empty_polls),
                remote_status.video_frames_length,
                remote_status.video_frames_capacity,
                remote_status.video_packets_length,
                remote_status.video_packets_capacity,
                remote_status.audio_frames_length,
                remote_status.audio_frames_capacity,
                remote_status.audio_packets_length,
                remote_status.audio_packets_capacity,
                audio_queued,
                static_cast<unsigned long long>(audio_queue_ms),
                static_cast<unsigned long long>(remote_status.peak_rss_kib),
                cpu_core_equivalents,
                std::max(0.0, user_cpu_ms),
                std::max(0.0, system_cpu_ms),
                static_cast<unsigned long long>(voluntary_switches),
                static_cast<unsigned long long>(involuntary_switches),
                loop_timing.average(),
                loop_timing.maximum_ms,
                audio_pull_timing.average(),
                audio_pull_timing.maximum_ms,
                video_pull_timing.average(),
                video_pull_timing.maximum_ms,
                render_timing.average(),
                render_timing.maximum_ms,
                present_timing.average(),
                present_timing.maximum_ms,
                present_timing.percentile(0.95),
                present_timing.percentile(0.99),
                static_cast<unsigned long long>(present_timing.count),
                static_cast<unsigned long long>(file_stats.bytes_read),
                static_cast<unsigned long long>(file_stats.read_calls),
                static_cast<double>(file_stats.read_time_us) / 1000.0,
                static_cast<unsigned long long>(file_stats.seek_calls),
                static_cast<double>(file_stats.seek_time_us) / 1000.0);
            if (source_video.hdr_source && native_hdr_output) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::info,
                    "hdr-output mode=native-hdr10 transfer=%s policy=%s tone_map=0",
                    source_video.color_transfer,
                    hdr_output_policy);
            } else if (source_video.hdr_source) {
                bfplayer::diagnostics_log(
                    tone_map_info.active
                        ? bfplayer::DiagnosticLevel::info
                        : bfplayer::DiagnosticLevel::warning,
                    "hdr-tone-map active=%d transfer=%s input_range=%s input_primaries=%s source_peak_nits=%.3f target_peak_nits=%.3f output=bt709-limited workers=%u frames=%llu processing_ms=%.3f average_ms=%.6f",
                    tone_map_info.active,
                    source_video.color_transfer,
                    tone_map_info.input_full_range ? "full" : "limited",
                    tone_map_info.input_bt2020 ? "bt2020" : "bt709",
                    tone_map_info.source_peak_nits,
                    tone_map_info.target_peak_nits,
                    tone_map_info.workers,
                    tone_map_info.frames,
                    static_cast<double>(tone_map_info.processing_us) /
                        1000.0,
                    tone_map_info.frames > 0
                        ? static_cast<double>(
                              tone_map_info.processing_us) /
                              static_cast<double>(tone_map_info.frames) /
                              1000.0
                        : 0.0);
            }
            diagnostics_usage = current_usage;
            video_updates = 0;
            video_empty_polls = 0;
            active_playback_ms = 0.0;
            loop_timing.reset();
            audio_pull_timing.reset();
            video_pull_timing.reset();
            render_timing.reset();
            present_timing.reset();
            last_diagnostics = now;
        }
        if (now - last_remote_status >= 500) {
            remote_status.running = app.running;
            remote_status.playing = app.playback_running;
            remote_status.paused = app.paused;
            remote_status.player_state = Kit_GetPlayerState(app.player);
            remote_status.position_seconds =
                Kit_GetPlayerPosition(app.player);
            remote_status.duration_seconds =
                Kit_GetPlayerDuration(app.player);
            remote_status.audio_queued_bytes =
                app.audio != 0 ? SDL_GetQueuedAudioSize(app.audio) : 0U;
            remote_status.audio_stream =
                Kit_GetPlayerAudioStream(app.player);
            remote_status.subtitle_stream =
                Kit_GetPlayerSubtitleStream(app.player);
            remote_status.peak_rss_kib = process_peak_rss_kib();
            const bfplayer::SafeReadFileStats file_stats =
                app.local_media.stats();
            remote_status.media_bytes_read = file_stats.bytes_read;
            remote_status.media_read_calls = file_stats.read_calls;
            remote_status.media_read_time_us = file_stats.read_time_us;
            remote_status.media_seek_calls = file_stats.seek_calls;
            remote_status.media_seek_time_us = file_stats.seek_time_us;
            update_decoder_buffer_status(app, remote_status);
            update_tone_map_status(app, remote_status);
            app.remote_control.update_status(remote_status);
            last_remote_status = now;
        }
        if (database_available && source_persistence_allowed &&
            now - last_resume_save >= 10000) {
            bfplayer::ResumeState current{};
            current.position_ms = seconds_to_milliseconds(
                Kit_GetPlayerPosition(app.player));
            current.duration_ms = seconds_to_milliseconds(
                Kit_GetPlayerDuration(app.player));
            current.last_played_unix = static_cast<std::int64_t>(std::time(nullptr));
            current.completed = playback_completed(
                current.position_ms, current.duration_ms);
            if (!resume_database.save_resume(path, current)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "resume-save failed error=%s",
                    resume_database.error().c_str());
            }
            const bfplayer::TrackPreferences preferences = current_track_preferences(app);
            if (!bfplayer::uri_has_sensitive_components(preferences.external_subtitle) &&
                !resume_database.save_track_preferences(path, preferences)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "track-preference-save failed error=%s",
                    resume_database.error().c_str());
            }
            last_resume_save = now;
        }
    }
    const double final_position = Kit_GetPlayerPosition(app.player);
    const double final_duration = Kit_GetPlayerDuration(app.player);
    const bool reached_end =
        std::isfinite(final_position) && std::isfinite(final_duration) &&
        final_duration > 0.0 &&
        final_position / final_duration >= 0.92;
    const bool finished = app.running && app.playback_running && reached_end &&
                          Kit_GetPlayerState(app.player) == KIT_STOPPED;
    if (database_available) {
        if (source_persistence_allowed) {
            bfplayer::ResumeState current{};
            // Use the values captured before any cleanup/stop transition.
            // Some decoder backends reset their public clock when stopping.
            current.position_ms = seconds_to_milliseconds(final_position);
            current.duration_ms = seconds_to_milliseconds(final_duration);
            current.last_played_unix = static_cast<std::int64_t>(std::time(nullptr));
            current.completed = playback_completed(
                current.position_ms, current.duration_ms);
            if (!resume_database.save_resume(path, current)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "final-resume-save failed error=%s",
                    resume_database.error().c_str());
            }
            const bfplayer::TrackPreferences preferences = current_track_preferences(app);
            if (!bfplayer::uri_has_sensitive_components(
                    preferences.external_subtitle) &&
                !resume_database.save_track_preferences(path, preferences)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "final-track-preference-save failed error=%s",
                    resume_database.error().c_str());
            }
        }
        if (!resume_database.set_settings({
                {"volume_percent", std::to_string(app.volume_percent)},
            })) {
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::error,
                "playback-setting-save failed error=%s",
                resume_database.error().c_str());
        }
    }
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "playback-end path=%s outcome=%s position_s=%.3f duration_s=%.3f reached_end=%d",
        app.current_media_path.c_str(),
        finished ? "finished" : (app.playback_running ? "user-return" : "stopped"),
        final_position,
        final_duration,
        reached_end ? 1 : 0);
    return finished ? PlaybackOutcome::finished : PlaybackOutcome::user_return;
}

void close_media(App& app) {
    if (!app.current_media_path.empty()) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::info,
            "media-close path=%s",
            app.current_media_path.c_str());
    }
    if (app.native_hdr_output) {
        const int hdr_result = set_ps5_native_hdr_output(false);
        bfplayer::diagnostics_log(
            hdr_result == 0
                ? bfplayer::DiagnosticLevel::info
                : bfplayer::DiagnosticLevel::error,
            "native-hdr-switch requested=0 active=%d result=%d",
            hdr_result == 0 ? 0 : 1,
            hdr_result);
        app.native_hdr_output = false;
    }
    app.io_cancel.store(true, std::memory_order_relaxed);
    app.source_open_deadline_ms.store(0, std::memory_order_relaxed);
    if (app.player) {
        Kit_PlayerStop(app.player);
    }
    close_audio(app);
    app.osd.close();
    destroy_video_textures(app);
    stop_now_playing_artwork_job(app);
    clear_now_playing_artwork(app);
    Kit_ClosePlayer(app.player);
    if (app.source) {
        Kit_CloseSource(app.source);
    }
    app.local_media.close();
    app.player = nullptr;
    app.source = nullptr;
    app.current_media_path.clear();
    app.playback_running = false;
    app.paused = false;
    app.playback_overlay = PlaybackOverlay::none;
    app.playback_overlay_selected = 0;
    app.navigation_repeat.reset();
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_AUTOMATIC);
}

int run_library(
    App& app,
    const std::vector<bfplayer::MediaSource>& initial_sources = {}) {
    bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::info, "library-start");
    bfplayer::LibraryUi library;
    if (!library.open(
            app.renderer,
            app.fallback_font,
            app.ui_logo,
            initial_sources)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "library-open failed error=%s",
            library.error().c_str());
        return 1;
    }
    Uint64 last_remote_status = 0;

    while (app.running) {
        SDL_Event event{};
        bool play_selected = false;
        bool play_queue = false;
        std::string selected_path;
        consume_remote_commands(app, false);
        if (!app.pending_remote_media_path.empty()) {
            selected_path = std::move(app.pending_remote_media_path);
            app.pending_remote_media_path.clear();
            play_selected = true;
        }
        const Uint64 library_now = SDL_GetTicks64();
        if (library_now - last_remote_status >= 1000) {
            bfplayer::RemotePlaybackStatus status;
            status.phase = "library";
            status.running = app.running;
            status.output_width = app.output_width;
            status.output_height = app.output_height;
            status.peak_rss_kib = process_peak_rss_kib();
            app.remote_control.update_status(status);
            last_remote_status = library_now;
        }
        while (!play_selected && SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED &&
                !app.controller &&
                SDL_IsGameController(event.cdevice.which)) {
                app.controller =
                    SDL_GameControllerOpen(event.cdevice.which);
                bfplayer::diagnostics_log(
                    app.controller
                        ? bfplayer::DiagnosticLevel::info
                        : bfplayer::DiagnosticLevel::warning,
                    "library-controller added id=%d opened=%d error=%s",
                    event.cdevice.which,
                    app.controller ? 1 : 0,
                    app.controller ? "<none>" : SDL_GetError());
            } else if (
                event.type == SDL_CONTROLLERDEVICEREMOVED &&
                app.controller &&
                SDL_JoystickInstanceID(
                    SDL_GameControllerGetJoystick(app.controller)) ==
                    event.cdevice.which) {
                SDL_GameControllerClose(app.controller);
                app.controller = nullptr;
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::info,
                    "library-controller removed id=%d",
                    event.cdevice.which);
            }
            const bfplayer::LibraryAction action = library.handle_event(event, selected_path);
            if (action == bfplayer::LibraryAction::exit) {
                app.running = false;
                break;
            }
            if (action == bfplayer::LibraryAction::play) {
                play_selected = true;
                break;
            }
            if (action == bfplayer::LibraryAction::play_queue) {
                play_selected = true;
                play_queue = true;
                break;
            }
        }
        if (!app.running) {
            break;
        }
        if (play_selected) {
            std::vector<std::string> queue;
            if (bfplayer::is_generic_playlist_path(selected_path)) {
                bfplayer::PlaylistLoadResult playlist =
                    bfplayer::load_generic_playlist(selected_path);
                if (playlist.items.empty()) {
                    const std::string error = playlist.error.empty()
                        ? "Unsupported playlist"
                        : playlist.error;
                    bfplayer::diagnostics_log(
                        bfplayer::DiagnosticLevel::error,
                        "playlist-load failed path=%s error=%s",
                        selected_path.c_str(),
                        error.c_str());
                    library.show_notice("PLAYLIST: " + error);
                    continue;
                }
                if (playlist.truncated) {
                    bfplayer::diagnostics_log(
                        bfplayer::DiagnosticLevel::warning,
                        "playlist-load truncated path=%s items=%zu",
                        selected_path.c_str(),
                        playlist.items.size());
                }
                queue = std::move(playlist.items);
            } else {
                queue = play_queue
                    ? library.playback_queue(selected_path)
                    : std::vector<std::string>{selected_path};
            }
            bfplayer::diagnostics_log(
                bfplayer::DiagnosticLevel::info,
                "library-play-request queue_mode=%d items=%zu selected=%s",
                play_queue ? 1 : 0,
                queue.size(),
                selected_path.c_str());
            // Do not let metadata scanning compete with software video decode.
            library.close();
            for (const std::string& queued_path : queue) {
                const PlaybackOutcome outcome = run_player(app, queued_path.c_str());
                close_media(app);
                if (!app.running || outcome != PlaybackOutcome::finished) {
                    break;
                }
            }
            if (app.running) {
                (void)switch_fullscreen_output(
                    app,
                    kWindowWidth,
                    kWindowHeight,
                    "library");
            }
            if (app.running && !library.open(
                    app.renderer,
                    app.fallback_font,
                    app.ui_logo)) {
                bfplayer::diagnostics_log(
                    bfplayer::DiagnosticLevel::error,
                    "library-reopen failed error=%s",
                    library.error().c_str());
                app.running = false;
                break;
            }
            continue;
        }
        library.tick_navigation();
        library.render();
        SDL_Delay(8);
    }
    library.close();
    bfplayer::diagnostics_log(bfplayer::DiagnosticLevel::info, "library-end running=%d", app.running ? 1 : 0);
    return 0;
}

void cleanup(App& app) {
    app.remote_control.stop();
    close_media(app);
    if (app.controller) {
        SDL_GameControllerClose(app.controller);
    }
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    if (app.now_playing_artwork_job.mutex) {
        SDL_DestroyMutex(app.now_playing_artwork_job.mutex);
        app.now_playing_artwork_job.mutex = nullptr;
    }
    Kit_Quit();
    SDL_Quit();
}

void return_to_playstation_home() {
    constexpr const char* kBFplayerTitleId = "PSMC00001";
    constexpr const char* kWebsrvTitleId = "FAKE00000";
    const int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id <= 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "home-return failed reason=no-running-bigapp app_id=%d",
            app_id);
        return;
    }
    std::array<char, 16> title_id{};
    const int title_result =
        sceSystemServiceGetAppTitleId(app_id, title_id.data());
    if (title_result != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "home-return failed reason=title-id app_id=%d result=0x%08x",
            app_id,
            static_cast<unsigned int>(title_result));
        return;
    }
    const bool is_bfplayer_host =
        std::strcmp(title_id.data(), kBFplayerTitleId) == 0;
    const bool is_websrv_host =
        std::strcmp(title_id.data(), kWebsrvTitleId) == 0;
    if (!is_bfplayer_host && !is_websrv_host) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "home-return skipped reason=unexpected-bigapp app_id=%d title_id=%s",
            app_id,
            title_id.data());
        return;
    }
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "home-return kill-bigapp app_id=%d title_id=%s",
        app_id,
        title_id.data());
    const int result =
        sceSystemServiceKillApp(app_id, -1, 0, 0);
    if (result != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "home-return kill-bigapp failed app_id=%d result=0x%08x",
            app_id,
            static_cast<unsigned int>(result));
    }
}

} // namespace

int main(int argc, char** argv) {
    boot_stage_stderr("process-entry");
    PlayerInstanceLock player_instance_lock;
    boot_stage_stderr("player-lock-acquire-begin");
    const PlayerLockResult lock_result = player_instance_lock.acquire();
    if (lock_result != PlayerLockResult::acquired) {
        std::fprintf(
            stderr,
            lock_result == PlayerLockResult::already_running
                ? "BFplayer launch ignored: another player is already running\n"
                : "BFplayer launch stopped: unable to acquire player lock\n");
        return lock_result == PlayerLockResult::already_running ? 0 : 1;
    }
    boot_stage_stderr("player-lock-acquired");
    boot_stage_stderr("legacy-migration-begin");
    migrate_legacy_library_database();
    boot_stage_stderr("legacy-migration-complete");
    bfplayer::diagnostics_init(argc, argv);
    boot_stage("diagnostics-ready");
    bfplayer::diagnostics_install_ffmpeg();
    SDL_LogSetOutputFunction(sdl_log_output, nullptr);
    log_installed_manifest(argc > 0 ? argv[0] : nullptr);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "application-start build=%s player_lock=acquired",
        BFPLAYER_VERSION);
    App app{};
    app.fallback_font = executable_asset_path(
        argc > 0 ? argv[0] : nullptr,
        "assets/fonts/NotoSans-Regular.ttf");
    app.ui_logo = executable_asset_path(
        argc > 0 ? argv[0] : nullptr,
        "sce_sys/icon0.png");
    boot_stage("sdl-init-begin");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        log_sdl("SDL_Init");
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    boot_stage("sdl-init-complete");
    // Linear scaling for every texture (now-playing artwork, video frames):
    // set once here so quality doesn't depend on whether a video happened to
    // play earlier in the process.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    Kit_SetHint(KIT_HINT_THREAD_COUNT, kVideoDecoderThreads);
    apply_playback_buffer_policy(playback_buffer_policy(false));
    boot_stage("kitchensink-init-begin");
    if (Kit_Init(KIT_INIT_NETWORK) != 0) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "kitchensink-init failed error=%s",
            Kit_GetError());
        SDL_Quit();
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    boot_stage("kitchensink-init-complete");
    // Created only after Kit_Init succeeds, so this early-return path (and
    // every other startup error-return above it) never has to decide
    // whether to destroy a mutex that no worker thread can have touched yet
    // -- there is nothing here for cleanup() to free, and this path exits
    // the process immediately after anyway.
    app.now_playing_artwork_job.mutex = SDL_CreateMutex();
    if (!app.now_playing_artwork_job.mutex) {
        // Not fatal: update_now_playing_artwork() falls back to showing no
        // artwork (plain black backdrop) for the whole run if this failed.
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::warning,
            "now-playing-artwork mutex create failed error=%s",
            SDL_GetError());
    }
    Kit_Version kitchensink_version{};
    Kit_GetVersion(&kitchensink_version);
    SDL_version sdl_version{};
    SDL_GetVersion(&sdl_version);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "libraries sdl=%u.%u.%u kitchensink=%u.%u.%u cpu_count=%d decoder_threads=%d video_packet_buffers=%d video_frame_buffers=%d",
        sdl_version.major,
        sdl_version.minor,
        sdl_version.patch,
        kitchensink_version.major,
        kitchensink_version.minor,
        kitchensink_version.patch,
        SDL_GetCPUCount(),
        Kit_GetHint(KIT_HINT_THREAD_COUNT),
        Kit_GetHint(KIT_HINT_VIDEO_BUFFER_PACKETS),
        Kit_GetHint(KIT_HINT_VIDEO_BUFFER_FRAMES));

    boot_stage("window-create-begin");
    app.window = SDL_CreateWindow(
        "Hi-res AMP",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        kWindowWidth,
        kWindowHeight,
        0);
    if (!app.window) {
        log_sdl("SDL_CreateWindow");
        cleanup(app);
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    if (!configure_fullscreen_output(app)) {
        cleanup(app);
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    boot_stage("window-create-complete");
    boot_stage("renderer-create-begin");
    app.renderer = create_renderer(app.window);
    if (!app.renderer) {
        log_sdl("SDL_CreateRenderer");
        cleanup(app);
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    if (SDL_RenderSetLogicalSize(
            app.renderer,
            kWindowWidth,
            kWindowHeight) != 0) {
        log_sdl("SDL_RenderSetLogicalSize");
        cleanup(app);
        bfplayer::diagnostics_shutdown();
        return 1;
    }
    log_display_output(app);
    boot_stage("renderer-create-complete");
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            app.controller = SDL_GameControllerOpen(i);
            break;
        }
    }
    boot_stage("controller-open-complete");
    std::string remote_error;
    if (!app.remote_control.start(
            kRemoteControlPort,
            BFPLAYER_VERSION,
            remote_error)) {
        bfplayer::diagnostics_log(
            bfplayer::DiagnosticLevel::error,
            "remote-control start failed port=%u error=%s",
            static_cast<unsigned int>(kRemoteControlPort),
            remote_error.c_str());
    }

    int result = 0;
    const char* media_path = nullptr;
    std::vector<bfplayer::MediaSource> initial_sources;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--add-movie") == 0 &&
            index + 1 < argc) {
            const char* source_path = argv[++index];
            initial_sources.push_back({
                bfplayer::MediaSourceKind::movie_file,
                source_path ? source_path : "",
                {}});
        } else if (std::strcmp(argv[index], "--add-tv-folder") == 0 &&
                   index + 1 < argc) {
            const char* source_path = argv[++index];
            initial_sources.push_back({
                bfplayer::MediaSourceKind::tv_folder,
                source_path ? source_path : "",
                {}});
        } else if (!media_path && argv[index][0]) {
            media_path = argv[index];
        }
    }
    if (media_path) {
        boot_stage("player-loop-enter");
        result = run_player(app, media_path) == PlaybackOutcome::error ? 1 : 0;
    } else {
        boot_stage("library-loop-enter");
        result = run_library(app, initial_sources);
    }

    cleanup(app);
    bfplayer::diagnostics_log(
        bfplayer::DiagnosticLevel::info,
        "application-end result=%d",
        result);
    return_to_playstation_home();
    bfplayer::diagnostics_shutdown();
    return result;
}
