/* tests/unit/disc_cache_auto_test.c - --disc-cache auto end to end, on a
 * synthetic image in the temp dir, with latency injected into every file
 * read.
 *
 * The target links with pread and pread64 wrapped (ld --wrap): every
 * pread of disc_image.c and disc_fs_probe.c reaches dc_test_pread, which
 * sleeps s_delay_us and then
 * generates the image's bytes: the probe's uncached reads and the readers'
 * file reads both pay the delay, and no device time enters. The loader's
 * buffered fill (fread) reads the real file.
 * OPENPETE_DISC_CACHE_TEST_DELAY_US sets the starting delay; it is read
 * here only.
 *
 * fast (delay 0): auto measures the file fast and detaches; reads are
 * direct and match the file; warm reads over five ticks do not wake the
 * watchdog. Then the delay rises to 3 ms: the third cold demand read in
 * one tick passes the quarter-tick ceiling, the cache attaches again, the
 * loader starts at that read's chunk, and the reads become hits with the
 * file's bytes.
 *
 * slow (delay 3000 us): auto measures the file slow and keeps the cache;
 * the loader fills the image and every read hits. */
#include "openpete_disc_cache.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_log.h"
#include "psx/libcd.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTORS 7400u   /* 17.4 MB: three 8 MB chunks, past OPENPETE_DISC_PROBE_MIN */

static SDL_AtomicInt s_delay_us;
static SDL_AtomicInt s_vbl;
static int s_fail;

/** @brief The byte at image offset i. */
static uint8_t byte_at(uint64_t i) { return (uint8_t)(i * 131u + (i >> 11)); }

static uint8_t* s_image;   /* the image's bytes, as write_image wrote them */

/** @brief pread of the image: s_delay_us of latency, then the image's bytes copied from s_image.
 *
 * The bytes are the ones write_image put in the file, so the device's own
 * speed never enters the measurement; the injected delay is the only
 * latency the probe and the watchdog see. */
static ssize_t dc_test_pread(int fd, void* buf, size_t n, int64_t off) {
    (void)fd;
    const int us = SDL_GetAtomicInt(&s_delay_us);
    if (us > 0) SDL_DelayPrecise((Uint64)us * 1000u);
    const uint64_t size = (uint64_t)SECTORS * 2352u;
    if ((uint64_t)off >= size) return 0;
    if (n > size - (uint64_t)off) n = (size_t)(size - (uint64_t)off);
    memcpy(buf, s_image + off, n);
    return (ssize_t)n;
}
/** @brief The linker's replacement for pread. */
ssize_t __wrap_pread(int fd, void* buf, size_t n, off_t off);
/** @brief The linker's replacement for pread. */
ssize_t __wrap_pread(int fd, void* buf, size_t n, off_t off) { return dc_test_pread(fd, buf, n, (int64_t)off); }
/** @brief The linker's replacement for pread64. */
ssize_t __wrap_pread64(int fd, void* buf, size_t n, int64_t off);
/** @brief The linker's replacement for pread64. */
ssize_t __wrap_pread64(int fd, void* buf, size_t n, int64_t off) { return dc_test_pread(fd, buf, n, off); }

/** @brief PsyCross stub: the read callback is not exercised here. */
void PsyX_CDFS_SetMemRead(int (*cb)(void*, size_t, size_t, int)) { (void)cb; }
/** @brief PsyCross stub: the image declaration is not exercised here. */
void PsyX_CDFS_Init_Mem(const u_int* data, int size, int track, int sector_size) {
    (void)data; (void)size; (void)track; (void)sector_size;
}
/** @brief PsyCross stub: the read observer is not exercised here. */
void PsyX_CDFS_SetReadObserver(void (*cb)(size_t, size_t, int)) { (void)cb; }
/** @brief Pace log stub. */
void openpete_pace_cd_miss(int64_t waited_ns, uint64_t end_offset) { (void)waited_ns; (void)end_offset; }
/** @brief Crash handler stub. */
void openpete_crash_thread_attach(void) {}
/** @brief Logger: prints the cache's info lines, which carry the decision. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    printf("  [%s] ", subsystem);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/** @brief The VBL source the watchdog's tick is read from. */
static int test_vbl(void) { return SDL_GetAtomicInt(&s_vbl); }

/** @brief Record a failure when ok is 0. */
static void check(int ok, const char* what) {
    if (!ok) { printf("FAIL %s\n", what); s_fail++; }
}

/** @brief Fill s_image and write it to path; a byte pattern, no volume (the cache never parses it). */
static int write_image(const char* path) {
    const size_t size = (size_t)SECTORS * 2352u;
    s_image = (uint8_t*)malloc(size);
    if (!s_image) return 0;
    for (size_t i = 0; i < size; i++) s_image[i] = byte_at(i);
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(s_image, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

/** @brief Read one sector through the cached handle and check its bytes.
 *
 * @return The read's result: OPENPETE_DISC_HIT, _MISS or _DIRECT. */
static int read_check(disc_image_t* img, uint32_t lba, const char* what) {
    uint8_t got[2352];
    const int r = disc_image_read_bytes(img, (uint64_t)lba * 2352u, got, sizeof got,
                                        OPENPETE_DISC_TAG_LIBCD);
    int same = 1;
    for (uint32_t i = 0; i < 2352u; i++) same &= got[i] == byte_at((uint64_t)lba * 2352u + i);
    check(same, what);
    return r;
}

/** @brief Wait up to ms for openpete_disc_cache_active() to equal want. */
static int wait_active(int want, int ms) {
    for (int i = 0; i < ms; i++) {
        if (openpete_disc_cache_active() == want) return 1;
        SDL_Delay(1);
    }
    return openpete_disc_cache_active() == want;
}

/** @brief Wait up to ms for a hit on lba. */
static int wait_hit(disc_image_t* img, uint32_t lba, int ms) {
    for (int i = 0; i < ms; i++) {
        if (read_check(img, lba, "bytes while waiting for a hit") == OPENPETE_DISC_HIT) return 1;
        SDL_Delay(1);
    }
    return 0;
}

/** @brief The fast file: detach, no flip on warm reads, a flip on a cold burst, hits after. */
static void fast_case(disc_image_t* img) {
    check(openpete_disc_cache_attach_auto(disc_image_bin_path(img)) == 1, "auto attaches measuring");
    check(wait_active(0, 10000), "auto detaches a fast file");
    check(read_check(img, 100, "direct bytes") == OPENPETE_DISC_DIRECT, "detached read is direct");

    SDL_SetAtomicInt(&s_vbl, 0);
    for (int v = 0; v < 10; v++) {            /* five ticks of warm reads */
        SDL_SetAtomicInt(&s_vbl, v);
        for (uint32_t i = 0; i < 50; i++) (void)read_check(img, 200u + i, "warm bytes");
    }
    check(openpete_disc_cache_active() == 0, "warm reads leave the cache off");

    SDL_SetAtomicInt(&s_delay_us, 3000);
    SDL_SetAtomicInt(&s_vbl, 20);             /* tick 10 */
    const uint32_t cold = 6000;               /* chunk 1 (sectors 3566..7131) */
    int flipped_at = -1;
    for (int i = 0; i < 6 && flipped_at < 0; i++) {
        (void)read_check(img, cold + (uint32_t)i, "cold bytes");
        if (openpete_disc_cache_active()) flipped_at = i;
    }
    printf("disc_cache_auto_test: watchdog attached the cache on cold read %d\n", flipped_at);
    /* A sleep never returns early, so three 3 ms reads always pass the 8.36 ms
     * quarter tick; under load a sleep overruns, and one read can pass it alone.
     * The exact read count is asserted on synthetic timings in disc_fs_probe_test. */
    check(flipped_at >= 0 && flipped_at <= 2, "the cache attaches by the third 3 ms read in one tick");
    SDL_SetAtomicInt(&s_delay_us, 0);
    check(wait_hit(img, cold, 10000), "the missed chunk becomes resident");
    check(wait_hit(img, 10, 20000), "the whole image becomes resident");
}

/** @brief The slow file: the cache stays attached and fills. */
static void slow_case(disc_image_t* img) {
    check(openpete_disc_cache_attach_auto(disc_image_bin_path(img)) == 1, "auto attaches measuring");
    check(wait_hit(img, SECTORS - 1u, 30000), "the last sector becomes resident");
    check(openpete_disc_cache_active() == 1, "a slow file keeps the cache");
    check(wait_hit(img, 4000, 30000), "the middle chunk becomes resident");
    check(wait_hit(img, 10, 30000), "the first chunk becomes resident");
}

/** @brief Run the case named by argv[1] (fast or slow); exit 1 on any failure. */
int main(int argc, char** argv) {
    const int slow = argc > 1 && strcmp(argv[1], "slow") == 0;
    const char* env = getenv("OPENPETE_DISC_CACHE_TEST_DELAY_US");
    SDL_SetAtomicInt(&s_delay_us, env && *env ? atoi(env) : (slow ? 3000 : 0));
    SDL_SetAtomicInt(&s_vbl, -1);
    openpete_disc_trace_set_vbl_source(test_vbl);
    const char* tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/disc_cache_auto_test_%d.bin", tmp && *tmp ? tmp : "/tmp",
             (int)getpid());
    remove(path);
    check(write_image(path), "write the image");
    disc_image_t* img = disc_image_open(path, DISC_READ_CACHED, OPENPETE_DISC_TAG_LIBCD);
    check(img != NULL, "open the image");
    if (img) {
        if (slow) slow_case(img); else fast_case(img);
        openpete_disc_cache_shutdown();
        disc_image_close(img);
    }
    remove(path);
    if (s_fail) return 1;
    printf("disc_cache_auto_test %s: OK\n", slow ? "slow" : "fast");
    return 0;
}
