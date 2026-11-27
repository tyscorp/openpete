/* tests/unit/disc_ingest_resolve_test.cpp - the .bin a disc path resolves
 * to (openpete_psx_resolve_disc_bin), which the library's ingest copies and
 * reads the serial from, against cues beside another game's image.
 *
 * Writes raw (2352-byte Mode 2 sector) ISO9660 images holding only a
 * SYSTEM.CNF whose BOOT line names a serial: a cue whose FILE line names a
 * bin of another name resolves to that bin and reads its serial, both when
 * the bin sits beside another game's .bin and when it sits in a
 * subdirectory; a cue with several FILE lines resolves to the first; a cue
 * with no FILE line beside two .bin files resolves to nothing rather than to
 * either. */
#include "openpete_psx.h"
#include "openpete_log.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

/** @brief Logger stub. */
extern "C" void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) {
    (void)level; (void)subsystem; (void)fmt;
}

/** @brief Disc cache stub: no cache holds the image, so every read goes to the file. */
extern "C" int openpete_disc_cache_copy(const char* bin_path, uint64_t offset, void* dst,
                                        size_t len, int tag) {
    (void)bin_path; (void)offset; (void)dst; (void)len; (void)tag;
    return -1;
}
/** @brief Disc cache stub: never reached, since no read misses. */
extern "C" void openpete_disc_cache_miss_served(int tag, uint64_t offset, size_t len, int64_t ns) {
    (void)tag; (void)offset; (void)len; (void)ns;
}
/** @brief Disc cache stub: the watchdog is never armed here. */
extern "C" void openpete_disc_cache_direct_served(const char* bin_path, int tag, uint64_t offset,
                                                  size_t len, int64_t ns) {
    (void)bin_path; (void)tag; (void)offset; (void)len; (void)ns;
}

static int s_fail;

/** @brief Record a failed check with its message. */
static void check(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    s_fail++;
}

/** @brief Store a both-endian ISO9660 733 field. */
static void put733(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); p[7 - i] = (uint8_t)(v >> (8 * i)); }
}

/** @brief Write one directory record at p for name with lba and size.
 * @return The record length. */
static int dir_record(uint8_t* p, const char* name, uint32_t lba, uint32_t size, uint8_t flags) {
    const int nl = (int)std::strlen(name);
    const int len = 33 + nl + ((33 + nl) & 1);
    p[0] = (uint8_t)len;
    put733(p + 2, lba);
    put733(p + 10, size);
    p[25] = flags;
    p[32] = (uint8_t)nl;
    std::memcpy(p + 33, name, (size_t)nl);
    return len;
}

/** @brief Write an image at path whose root holds SYSTEM.CNF;1 booting the given executable name (SCUS_942.28).
 * @param path The image file.
 * @param boot The BOOT line's executable name. */
static void write_image(const fs::path& path, const char* boot) {
    static uint8_t img[19 * 2048];
    std::memset(img, 0, sizeof img);
    uint8_t* pvd = img + 16 * 2048;
    pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    dir_record(pvd + 156, "\0", 17, 2048, 2);
    char cnf[128];
    const int cn = std::snprintf(cnf, sizeof cnf, "BOOT = cdrom:\\%s;1\r\nTCB = 4\r\n", boot);
    uint8_t* root = img + 17 * 2048;
    int o = dir_record(root, "\0", 17, 2048, 2);
    o += dir_record(root + o, "\1", 17, 2048, 2);
    dir_record(root + o, "SYSTEM.CNF;1", 18, (uint32_t)cn, 0);
    std::memcpy(img + 18 * 2048, cnf, (size_t)cn);
    FILE* f = std::fopen(path.string().c_str(), "wb");
    uint8_t raw[2352];
    for (size_t i = 0; i < sizeof img / 2048; i++) {
        std::memset(raw, 0, sizeof raw);
        std::memset(raw + 1, 0xFF, 10);
        raw[15] = 2;
        std::memcpy(raw + 24, img + i * 2048, 2048);
        std::fwrite(raw, 1, sizeof raw, f);
    }
    std::fclose(f);
}

/** @brief Write a text file. */
static void write_text(const fs::path& path, const std::string& text) {
    FILE* f = std::fopen(path.string().c_str(), "wb");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

/** @brief Resolve a disc path and read the serial of the file it resolves to.
 * @param disc The disc path.
 * @param bin Receives the resolved path, empty when none.
 * @return The serial, empty when the path resolves to nothing or holds none. */
static std::string resolve_serial(const fs::path& disc, std::string& bin) {
    char out[4096], serial[32] = {};
    bin.clear();
    if (openpete_psx_resolve_disc_bin(disc.string().c_str(), out, sizeof out, nullptr, 0) != 0) return {};
    bin = out;
    if (openpete_psx_read_disc_serial(out, serial, sizeof serial) != 0) return {};
    return serial;
}

/** @brief Runs the cases.
 * @return 0 when every case passes. */
int main() {
    const char* tmp = std::getenv("TMPDIR");
    const fs::path root = fs::path(tmp && *tmp ? tmp : "/tmp") / "disc_ingest_resolve_test.d";
    std::error_code ec;
    fs::remove_all(root, ec);
    std::string bin;

    /* A cue naming "A (USA).bin" beside B.bin, another game's image. */
    const fs::path beside = root / "beside";
    fs::create_directories(beside);
    write_image(beside / "A (USA).bin", "SCUS_942.28");
    write_image(beside / "B.bin", "SCUS_944.25");
    write_text(beside / "A.cue", "FILE \"A (USA).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
    std::string s = resolve_serial(beside / "A.cue", bin);
    check(bin == (beside / "A (USA).bin").string(), "beside: A.cue resolved to " + bin);
    check(s == "SCUS-94228", "beside: serial " + s);

    /* The named bin in a subdirectory, B.bin the only .bin beside the cue. */
    const fs::path sub = root / "sub";
    fs::create_directories(sub / "tracks");
    write_image(sub / "tracks" / "A (USA).bin", "SCUS_942.28");
    write_image(sub / "B.bin", "SCUS_944.25");
    write_text(sub / "A.cue", "FILE \"tracks/A (USA).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    s = resolve_serial(sub / "A.cue", bin);
    check(bin == (sub / "tracks" / "A (USA).bin").string(), "sub: A.cue resolved to " + bin);
    check(s == "SCUS-94228", "sub: serial " + s);

    /* Several FILE lines: the first. */
    const fs::path multi = root / "multi";
    fs::create_directories(multi);
    write_image(multi / "A (Track 1).bin", "SCUS_942.28");
    write_image(multi / "A (Track 2).bin", "SCUS_944.25");
    write_text(multi / "A.cue", "FILE \"A (Track 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                                "FILE \"A (Track 2).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n");
    s = resolve_serial(multi / "A.cue", bin);
    check(bin == (multi / "A (Track 1).bin").string(), "multi: A.cue resolved to " + bin);
    check(s == "SCUS-94228", "multi: serial " + s);

    /* No FILE line, no A.bin, two .bin files beside the cue: nothing, never a guess. */
    const fs::path none = root / "none";
    fs::create_directories(none);
    write_image(none / "C.bin", "SCUS_942.28");
    write_image(none / "D.bin", "SCUS_944.25");
    write_text(none / "A.cue", "REM no file line\n");
    s = resolve_serial(none / "A.cue", bin);
    check(bin.empty(), "none: A.cue resolved to " + bin);

    fs::remove_all(root, ec);
    if (s_fail) return 1;
    std::printf("disc_ingest_resolve_test: OK\n");
    return 0;
}
