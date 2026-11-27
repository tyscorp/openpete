/* tests/unit/disc_cache_short_read_test.c - a chunk the loader cannot read
 * whole is never served as uninitialised heap bytes.
 *
 * disc_cache.c is compiled into this TU with fread and malloc replaced: the
 * allocation is filled with 0xCD, and the loader's reads of chunk 1 come up
 * short on demand. Case A: the first read of chunk 1 is short and the retry
 * reads it whole; every byte of the image is then served from memory and
 * matches the file. Case B: every loader read of chunk 1 is short; the chunk
 * is never served from memory, and a disc_image read of it is a miss whose
 * bytes come from the file, never 0xCD. Case C: with a boot-order profile
 * installed, the loader's first reads are the profile's ranges, in profile
 * order and at sector granularity, then chunk 0 without the sectors already
 * read; OPENPETE_DISC_BOOT_ORDER=0 restores file order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int s_short_left;       /* short reads still to inject on chunk 1 */
static size_t s_short_at;      /* file offset of chunk 1 */
static long s_read_at[16];     /* file offsets of the first loader reads */
static size_t s_read_len[16];  /* their byte counts */
static int s_reads;

/** @brief fread that records the first 16 reads and returns half the request for a read starting at chunk 1 while s_short_left > 0. */
static size_t dc_test_fread(void* p, size_t sz, size_t n, FILE* f) {
    if (s_reads < 16) { s_read_at[s_reads] = ftell(f); s_read_len[s_reads] = n; s_reads++; }
    if (s_short_left != 0 && (size_t)ftell(f) == s_short_at && sz == 1) {
        if (s_short_left > 0) s_short_left--;
        return fread(p, 1, n / 2, f);
    }
    return fread(p, sz, n, f);
}
/** @brief malloc that fills the block with 0xCD, standing in for stale heap contents. */
static void* dc_test_malloc(size_t n) {
    void* p = malloc(n);
    if (p) memset(p, 0xCD, n);
    return p;
}
#define fread dc_test_fread
#define malloc dc_test_malloc
#include "../../core/psx/disc_cache.c"
#undef fread
#undef malloc

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
/** @brief Logger stub. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) {
    (void)level; (void)subsystem; (void)fmt;
}

/** @brief Wait for the loader to finish every chunk. */
static void dc_join(void) {
    dc_lock();
    SDL_Thread* t = s_thr;
    s_thr = NULL;
    dc_unlock();
    if (t) SDL_WaitThread(t, NULL);
}

/** @brief Resets the module's statics between cases. */
static void dc_reset(void) {
    openpete_disc_cache_shutdown();
    SDL_SetAtomicInt(&s_state, DC_DETACHED);
    dc_lock();
    s_abort = 0; s_img = NULL; s_have = NULL; s_sec = NULL; s_thr = NULL; s_misses = 0;
    dc_unlock();
    s_reads = 0;
}

/* The test image every case reads: the handle, the file's bytes, and a
 * buffer of the same size. */
typedef struct {
    disc_image_t*  img;
    const uint8_t* file;
    uint8_t*       got;
    size_t         size;
} dc_fixture_t;

/** @brief Attach the cache to the fixture's image and wait for the loader to finish.
 *
 * @param fx The fixture.
 * @param name The case name for the failure line.
 * @return 1 when attached. */
static int dc_load(const dc_fixture_t* fx, const char* name) {
    if (!openpete_disc_cache_attach(disc_image_bin_path(fx->img))) {
        fprintf(stderr, "FAIL attach %s\n", name);
        return 0;
    }
    dc_join();
    return 1;
}

/** @brief Report whether the whole image is resident and equal to the file.
 *
 * @param fx The fixture.
 * @return 1 when every byte is served from memory and matches. */
static int dc_whole(const dc_fixture_t* fx) {
    return openpete_disc_cache_copy(disc_image_bin_path(fx->img), 0, fx->got, fx->size,
                                    OPENPETE_DISC_TAG_LIBCD) == 1 &&
           memcmp(fx->got, fx->file, fx->size) == 0;
}

/** @brief Case A: the first loader read of chunk 1 is short and the retry reads it whole.
 *
 * @param fx The fixture.
 * @return The number of failed checks. */
static int case_retry(const dc_fixture_t* fx) {
    s_short_left = 1;
    if (!dc_load(fx, "A")) return 1;
    int fail = 0;
    if (!dc_whole(fx)) { fprintf(stderr, "FAIL A: image not whole after a retried short read\n"); fail++; }
    dc_reset();
    return fail;
}

/** @brief Case B: every loader read of chunk 1 is short; it is never served from memory and reads of it come from the file.
 *
 * @param fx The fixture.
 * @return The number of failed checks. */
static int case_unread(const dc_fixture_t* fx) {
    s_short_left = -1;              /* every loader read of chunk 1 is short */
    if (!dc_load(fx, "B")) return 1;
    int fail = 0;
    dc_lock();
    const unsigned chunk1 = s_have[1];
    dc_unlock();
    if (chunk1 != DC_UNREAD) { fprintf(stderr, "FAIL B: chunk 1 state %u\n", chunk1); fail++; }
    if (openpete_disc_cache_copy(disc_image_bin_path(fx->img), DC_CHUNK, fx->got, 16u, OPENPETE_DISC_TAG_LIBCD) != 0)
        { fprintf(stderr, "FAIL B: an unread chunk was served from memory\n"); fail++; }
    memset(fx->got, 0xCD, fx->size);
    int r = disc_image_read_bytes(fx->img, 0, fx->got, fx->size, OPENPETE_DISC_TAG_LIBCD);
    if (r != OPENPETE_DISC_MISS) { fprintf(stderr, "FAIL B: read result %d, want miss\n", r); fail++; }
    if (memcmp(fx->got, fx->file, fx->size) != 0) { fprintf(stderr, "FAIL B: bytes differ from the file\n"); fail++; }
    dc_lock();
    const unsigned long long misses_after = s_misses;
    dc_unlock();
    if (misses_after != 1) { fprintf(stderr, "FAIL B: %llu misses counted, want 1\n", misses_after); fail++; }
    r = disc_image_read_bytes(fx->img, 0, fx->got, 4096u, OPENPETE_DISC_TAG_LIBCD);
    if (r != OPENPETE_DISC_HIT) { fprintf(stderr, "FAIL B: chunk 0 read result %d, want hit\n", r); fail++; }
    dc_reset();
    return fail;
}

/** @brief Compare loader read i with the expected file offset and length.
 *
 * @param i Read index.
 * @param at Expected file offset.
 * @param len Expected byte count.
 * @return 1 when the read differs or was not made. */
static int read_differs(int i, long at, size_t len) {
    if (i < s_reads && s_read_at[i] == at && s_read_len[i] == len) return 0;
    fprintf(stderr, "FAIL C: loader read %d at %ld len %zu, want %ld len %zu\n", i,
            i < s_reads ? s_read_at[i] : -1L, i < s_reads ? s_read_len[i] : (size_t)0, at, len);
    return 1;
}

/** @brief Case C: a boot-order profile's ranges load first, in order, at sector granularity; OPENPETE_DISC_BOOT_ORDER=0 restores file order.
 *
 * Ranges {5000, 3}, {10, 2}, {11, 4}: the loader reads sectors 5000-5002,
 * 10-11, 12-14 (11 is already read), then chunk 0 as its missing runs, 0-9
 * and 15 onward.
 * @param fx The fixture.
 * @return The number of failed checks. */
static int case_profile_order(const dc_fixture_t* fx) {
    static const openpete_disc_range_t prof[] = { { 5000u, 3u }, { 10u, 2u }, { 11u, 4u } };
    static const long want_at[] = { 5000L * 2352, 10L * 2352, 12L * 2352, 0L, 15L * 2352 };
    static const size_t want_len[] = { 3u * 2352u, 2u * 2352u, 3u * 2352u, 10u * 2352u,
                                       (DC_CHUNK_SECTORS - 15u) * 2352u };
    static char order_off[] = "OPENPETE_DISC_BOOT_ORDER=0";
    static char order_on[] = "OPENPETE_DISC_BOOT_ORDER=";
    s_short_left = 0;
    openpete_disc_cache_set_boot_order(prof, 3);
    if (!dc_load(fx, "C")) return 1;
    int fail = 0;
    for (int i = 0; i < 5; i++) fail += read_differs(i, want_at[i], want_len[i]);
    if (!dc_whole(fx)) { fprintf(stderr, "FAIL C: image not whole after profile order\n"); fail++; }
    dc_reset();
    putenv(order_off);              /* putenv: the MinGW CRT has no setenv */
    if (!dc_load(fx, "C0")) return fail + 1;
    if (s_reads < 1 || s_read_at[0] != 0 || s_read_len[0] != DC_CHUNK)
        { fprintf(stderr, "FAIL C0: first loader read at %ld, want chunk 0 whole\n",
                  s_reads ? s_read_at[0] : -1L); fail++; }
    putenv(order_on);
    openpete_disc_cache_set_boot_order(NULL, 0);
    dc_reset();
    return fail;
}

/** @brief Runs the retry, persistent short-read and profile-order cases.
 * @return 0 when every case passes. */
int main(void) {
    const char* tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/disc_cache_short_read_test.bin", tmp && *tmp ? tmp : "/tmp");
    /* A whole number of 2352-byte sectors past two chunks, so disc_image opens it. */
    const size_t size = ((2u * DC_CHUNK) / 2352u + 4u) * 2352u;
    uint8_t* file = (uint8_t*)calloc(1, size);
    uint8_t* got = (uint8_t*)calloc(1, size);
    for (size_t i = 0; i < size; i++) file[i] = (uint8_t)(i * 7u + 1u);
    FILE* f = fopen(path, "wb");
    fwrite(file, 1, size, f); fclose(f);
    s_short_at = DC_CHUNK;

    disc_image_t* img = disc_image_open(path, DISC_READ_CACHED, OPENPETE_DISC_TAG_LIBCD);
    if (!img) { fprintf(stderr, "FAIL open\n"); return 1; }
    const dc_fixture_t fx = { img, file, got, size };
    int fail = case_retry(&fx) + case_unread(&fx) + case_profile_order(&fx);
    disc_image_close(img);
    remove(path);
    free(file); free(got);
    if (fail) return 1;
    printf("disc_cache_short_read_test: OK\n");
    return 0;
}
