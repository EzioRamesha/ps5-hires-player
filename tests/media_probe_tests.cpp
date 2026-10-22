#include "bfplayer/media.hpp"
#include "bfplayer/media_probe.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// No committed audio fixture exists for this probe (tests/media-src/ only
// holds subtitle and chapter-only .ffmetadata samples, and FFmpeg tags a
// container's generic metadata keys differently depending on the container,
// so a tagged FLAC has to actually be muxed, not hand-written). The project
// already links FFmpeg to build media_probe.cpp itself, and the host build
// additionally resolves a host ffmpeg executable (see CMakeLists.txt) purely
// to generate this disposable fixture at test time, so nothing binary is
// committed to the repo. The fixture is written under the test binary's own
// build directory, which is already untracked.
bool generate_tagged_flac_fixture(const std::string& path) {
    const std::string command =
        std::string(BFPLAYER_FFMPEG_EXECUTABLE) +
        " -y -hide_banner -loglevel error"
        " -f lavfi -i anullsrc=r=44100:cl=mono -t 0.2"
        " -metadata artist=\"Test Artist\""
        " -metadata album=\"Test Album\""
        " -metadata track=3"
        " -metadata date=2026"
        " \"" + path + "\"";
    return std::system(command.c_str()) == 0;
}

} // namespace

int main() {
    const std::string fixture_path =
        std::string(BFPLAYER_TEST_BINARY_DIR) + "/media-probe-sample-tagged.flac";
    std::error_code remove_error;
    std::filesystem::remove(fixture_path, remove_error);

    check(
        generate_tagged_flac_fixture(fixture_path),
        "ffmpeg generates a tagged FLAC fixture");

    bfplayer::MediaEntry entry{
        /* path */ fixture_path,
        /* name */ "media-probe-sample-tagged.flac",
        bfplayer::MediaKind::audio};
    std::string error;
    bool ok = bfplayer::probe_media_metadata(entry, error);
    check(ok, "probe succeeds on a tagged sample file");
    check(entry.artist == "Test Artist", "artist tag is extracted");
    check(entry.album == "Test Album", "album tag is extracted");
    check(entry.track_number == 3, "track number tag is extracted and parsed as an integer");
    check(entry.year == 2026, "year is extracted from the date tag");
    check(entry.album_artist == entry.artist, "album_artist falls back to artist when absent");
    check(entry.disc_number == 1, "disc_number keeps its documented default when absent");

    std::filesystem::remove(fixture_path, remove_error);

    if (failures == 0) {
        std::cout << "media_probe_tests: PASS\n";
    }
    return failures;
}
