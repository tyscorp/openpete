/* tests/unit/exe_loader_cap_test.cpp - the disc EXE loader refuses a
 * directory size no PSX EXE can have before it allocates.
 *
 * Writes a raw (2352-byte Mode 2 sector) ISO9660 image whose root directory
 * lists SCUS_942.28 with size 0xFFFFFFF0; the loader must return NULL. A
 * file one byte over 2 MiB + 2048 whose bytes are all present is refused
 * (the previous loader read it whole); a 4-byte file loads. */
#include "openpete_psx.h"
#include "openpete_log.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

/** @brief Store a both-endian ISO9660 733 field (little-endian half used by the reader). */
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

/** @brief Write user-data sectors to f as raw 2352-byte Mode 2 Form 1 sectors (sync, header, subheader, data, zeroed EDC/ECC).
 *
 * @param f The output file.
 * @param user The user data, 2048 bytes per sector.
 * @param sectors Sector count. */
static void write_raw(FILE* f, const uint8_t* user, size_t sectors) {
    uint8_t raw[2352];
    for (size_t i = 0; i < sectors; i++) {
        std::memset(raw, 0, sizeof raw);
        std::memset(raw + 1, 0xFF, 10);
        raw[15] = 2;
        std::memcpy(raw + 24, user + i * 2048, 2048);
        std::fwrite(raw, 1, sizeof raw, f);
    }
}

/** @brief Write an image at path whose root holds SCUS_942.28;1 of the given directory size at sector 18. */
static void write_image(const std::string& path, uint32_t exe_size) {
    /* The body sector holds up to 2 MiB + 2049 bytes, so a file one byte over
     * the cap reads whole and only the cap refuses it. */
    static uint8_t img[18 * 2048 + OPENPETE_PSX_RAM_SIZE + 2048u + 2048u];
    std::memset(img, 0, sizeof img);
    uint8_t* pvd = img + 16 * 2048;
    pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    dir_record(pvd + 156, "\0", 17, 2048, 2);
    uint8_t* root = img + 17 * 2048;
    int o = dir_record(root, "\0", 17, 2048, 2);
    o += dir_record(root + o, "\1", 17, 2048, 2);
    dir_record(root + o, "SCUS_942.28;1", 18, exe_size, 0);
    std::memcpy(img + 18 * 2048, "EXE!", 4);
    FILE* f = std::fopen(path.c_str(), "wb");
    write_raw(f, img, sizeof img / 2048);
    std::fclose(f);
}

/** @brief Runs the oversize and valid cases.
 * @return 0 when both pass. */
int main() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/exe_loader_cap_test.bin";
    int fail = 0;
    size_t n = 0;

    write_image(path, 0xFFFFFFF0u);
    uint8_t* b = openpete_psx_load_exe_from_disc(path.c_str(), "SCUS_942.28", &n);
    if (b) { std::fprintf(stderr, "FAIL oversize EXE loaded (%zu bytes)\n", n); std::free(b); fail++; }

    write_image(path, OPENPETE_PSX_RAM_SIZE + 2048u + 1u);
    b = openpete_psx_load_exe_from_disc(path.c_str(), "SCUS_942.28", &n);
    if (b) { std::fprintf(stderr, "FAIL EXE one byte over the cap loaded\n"); std::free(b); fail++; }

    write_image(path, 4u);
    b = openpete_psx_load_exe_from_disc(path.c_str(), "SCUS_942.28", &n);
    if (!b || n != 4 || std::memcmp(b, "EXE!", 4) != 0) { std::fprintf(stderr, "FAIL valid EXE\n"); fail++; }
    std::free(b);
    std::remove(path.c_str());
    if (fail) return 1;
    std::printf("exe_loader_cap_test: OK\n");
    return 0;
}
