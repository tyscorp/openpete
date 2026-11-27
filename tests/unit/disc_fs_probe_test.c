/* tests/unit/disc_fs_probe_test.c - the --disc-cache auto classifier on
 * injected values: Linux statfs f_type magics (9p, NFS, CIFS, SMB, SMB2,
 * FUSE slow; ext4, btrfs, tmpfs and an unlisted value local), Windows drive
 * types (remote and CD-ROM slow; fixed, removable, ram disk and unknown
 * local) and UNC paths (\\server\share, \\wsl$, \\wsl.localhost and
 * \\?\UNC\ slow whatever the drive type; \\?\C:\ and \\.\ not UNC), and
 * the probe on this host's temporary directory, which returns a reason.
 * The read-speed decider on synthetic timings: an SSD-like probe is fast,
 * a hard-disk-like one (seeks) and a USB-2-like one (low throughput) are
 * slow, an empty one is fast; the watchdog's per-tick sum stays under the
 * ceiling on warm reads, passes it on cold ones, and restarts each tick.
 * The uncached reader returns the file's bytes at unaligned offsets. */
#include "openpete_disc_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int s_fail;

/** @brief Check one f_type: the verdict and the reason text. */
static void ftype_case(uint64_t f_type, int slow, const char* why_want) {
    char why[64];
    const int got = openpete_disc_fs_classify_ftype(f_type, why, sizeof why);
    if (got != slow || strcmp(why, why_want) != 0) {
        printf("FAIL f_type 0x%llx: %d (%s), want %d (%s)\n", (unsigned long long)f_type,
               got, why, slow, why_want);
        s_fail++;
    }
}

/** @brief Check one Windows drive type and path: the verdict and the reason text. */
static void win_case(unsigned drive_type, const char* path, int slow, const char* why_want) {
    char why[64];
    const int got = openpete_disc_fs_classify_windows(drive_type, path, why, sizeof why);
    if (got != slow || strcmp(why, why_want) != 0) {
        printf("FAIL drive %u path %s: %d (%s), want %d (%s)\n", drive_type,
               path ? path : "(null)", got, why, slow, why_want);
        s_fail++;
    }
}

/** @brief Fill a probe with n reads of OPENPETE_DISC_PROBE_BLOCK bytes, each taking ns, with every period-th read taking spike_ns instead.
 *
 * @param p The probe, zeroed here.
 * @param n Read count.
 * @param ns Time of an ordinary read.
 * @param period Spike period, 0 for none.
 * @param spike_ns Time of a spike read. */
static void probe_fill(openpete_disc_probe_t* p, unsigned n, int64_t ns, unsigned period,
                       int64_t spike_ns) {
    memset(p, 0, sizeof *p);
    for (unsigned i = 0; i < n; i++)
        openpete_disc_probe_add(p, OPENPETE_DISC_PROBE_BLOCK,
                                period && i % period == 0 ? spike_ns : ns);
}

/** @brief Check one synthetic probe's verdict. */
static void probe_case(const char* name, const openpete_disc_probe_t* p, int slow) {
    char why[160];
    const int got = openpete_disc_probe_slow(p, why, sizeof why);
    printf("disc_fs_probe_test: %s: %s (%s)\n", name, got ? "slow" : "fast", why);
    if (got != slow) { printf("FAIL %s: verdict %d, want %d\n", name, got, slow); s_fail++; }
}

/** @brief The read-speed decider and the watchdog sum on synthetic timings. */
static void decider_cases(void) {
    static openpete_disc_probe_t p;
    /* NVMe/SATA SSD: 256 KB in 0.3 ms = 874 MB/s, flat. */
    probe_fill(&p, 64, 300000, 0, 0);
    probe_case("ssd", &p, 0);
    /* Hard disk: 160 MB/s streaming (1.64 ms per read) is already under the floor. */
    probe_fill(&p, 64, 1640000, 0, 0);
    probe_case("hdd streaming", &p, 1);
    /* Fast streaming with a 9 ms seek on every 16th read: throughput passes
     * (0.80 ms per read on average, 328 MB/s), p99 (9 ms) fails. */
    probe_fill(&p, 128, 250000, 16, 9000000);
    probe_case("seeking", &p, 1);
    /* USB 2: 35 MB/s, 7.5 ms per read. */
    probe_fill(&p, 32, 7500000, 0, 0);
    probe_case("usb2", &p, 1);
    /* One spike in 200 reads sits above p99 and is ignored. */
    probe_fill(&p, 200, 300000, 200, 20000000);
    probe_case("ssd one spike", &p, 0);
    memset(&p, 0, sizeof p);
    probe_case("empty", &p, 0);
    if (openpete_disc_probe_p99(&p) != 0) { printf("FAIL empty p99\n"); s_fail++; }

    /* Watchdog: warm reads (0.05 ms) for 100 reads per tick over 5 ticks
     * stay under the ceiling; then cold reads (3 ms) cross it on the 3rd. */
    openpete_disc_burst_t b = { -2, 0 };
    int flips = 0;
    for (int64_t t = 0; t < 5; t++)
        for (int i = 0; i < 100; i++) flips += openpete_disc_burst_add(&b, t, 50000);
    if (flips) { printf("FAIL warm reads flipped %d times\n", flips); s_fail++; }
    int at = -1;
    for (int i = 0; i < 10 && at < 0; i++)
        if (openpete_disc_burst_add(&b, 5, 3000000)) at = i;
    if (at != 2) { printf("FAIL cold reads crossed at read %d, want 2\n", at); s_fail++; }
    /* A new tick restarts the sum. */
    if (openpete_disc_burst_add(&b, 6, 3000000)) { printf("FAIL sum not restarted\n"); s_fail++; }
    if (OPENPETE_DISC_BURST_NS != 8357555 || OPENPETE_DISC_MIN_BPS / 1000000u != 188u ||
        OPENPETE_DISC_MAX_P99_NS != 1392925) {
        printf("FAIL thresholds %lld ns, %llu B/s, %lld ns\n", (long long)OPENPETE_DISC_BURST_NS,
               (unsigned long long)OPENPETE_DISC_MIN_BPS, (long long)OPENPETE_DISC_MAX_P99_NS);
        s_fail++;
    }
}

/** @brief The uncached reader against the buffered bytes of a file in the temp dir, at unaligned offsets and the file's end.
 *
 * @param dir The directory to write the file in. */
static void uncached_cases(const char* dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/disc_fs_probe_test.bin", dir);
    const size_t size = 3u * OPENPETE_DISC_PROBE_BLOCK + 1234u;
    uint8_t* ref = (uint8_t*)malloc(size);
    uint8_t* got = (uint8_t*)malloc(OPENPETE_DISC_PROBE_BLOCK);
    if (!ref || !got) { printf("FAIL alloc\n"); s_fail++; free(ref); free(got); return; }
    for (size_t i = 0; i < size; i++) ref[i] = (uint8_t)(i * 131u + (i >> 9));
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(ref, 1, size, f) != size) { printf("FAIL write %s\n", path); s_fail++; }
    if (f) fclose(f);
    char how[48];
    openpete_disc_uncached_t* u = openpete_disc_uncached_open(path, how, sizeof how);
    printf("disc_fs_probe_test: uncached reader: %s\n", how);
    if (!u) { printf("FAIL uncached open: %s\n", how); s_fail++; }
    static const struct { uint64_t off; size_t len; size_t want; } c[] = {
        { 0, 4096, 4096 }, { 2352, 7056, 7056 }, { 5000, OPENPETE_DISC_PROBE_BLOCK, OPENPETE_DISC_PROBE_BLOCK },
        { 3u * OPENPETE_DISC_PROBE_BLOCK, 4096, 1234 }, { 3u * OPENPETE_DISC_PROBE_BLOCK + 1000u, 500, 234 },
    };
    for (size_t i = 0; u && i < sizeof c / sizeof c[0]; i++) {
        const size_t n = openpete_disc_uncached_read(u, c[i].off, got, c[i].len);
        if (n != c[i].want || memcmp(got, ref + c[i].off, n) != 0) {
            printf("FAIL uncached read %zu: %zu bytes at %llu, want %zu\n", i, n,
                   (unsigned long long)c[i].off, c[i].want);
            s_fail++;
        }
    }
    openpete_disc_uncached_close(u);
    remove(path);
    free(ref); free(got);
}

/** @brief Run the classifier cases and the host probe; exit 1 on any failure. */
int main(void) {
    ftype_case(0x01021997u, 1, "9p");
    ftype_case(0x6969u, 1, "nfs");
    ftype_case(0xFF534D42u, 1, "cifs");
    ftype_case(0x517Bu, 1, "smb");
    ftype_case(0xFE534D42u, 1, "smb2");
    ftype_case(0x65735546u, 1, "fuse");
    ftype_case(0xEF53u, 0, "local ext4");
    ftype_case(0x9123683Eu, 0, "local btrfs");
    ftype_case(0x01021994u, 0, "local tmpfs");
    ftype_case(0x12345678u, 0, "local fs 0x12345678");

    win_case(4, "Z:\\games\\spyro.cue", 1, "remote drive");
    win_case(5, "D:\\SCUS_942.28", 1, "cd-rom drive");
    win_case(3, "C:\\games\\spyro.bin", 0, "local fixed drive");
    win_case(2, "E:\\spyro.bin", 0, "local removable drive");
    win_case(6, NULL, 0, "local ram disk");
    win_case(1, NULL, 0, "local, drive type 1");
    win_case(3, "\\\\nas\\games\\spyro.bin", 1, "unc");
    win_case(3, "//nas/games/spyro.bin", 1, "unc");
    win_case(3, "\\\\wsl$\\Ubuntu\\home\\spyro.bin", 1, "unc wsl");
    win_case(3, "\\\\wsl.localhost\\Ubuntu\\spyro.bin", 1, "unc wsl");
    win_case(3, "\\\\?\\UNC\\nas\\games\\spyro.bin", 1, "unc");
    win_case(3, "\\\\?\\C:\\games\\spyro.bin", 0, "local fixed drive");
    win_case(3, "\\\\.\\C:\\games\\spyro.bin", 0, "local fixed drive");

    char why[96];
    const char* tmp = getenv("TMPDIR");
    const int slow = openpete_disc_fs_slow(tmp && *tmp ? tmp : "/tmp", why, sizeof why);
    printf("disc_fs_probe_test: host temp dir: %s (%s)\n", slow ? "slow" : "local", why);
    if (!why[0]) { printf("FAIL host probe gave no reason\n"); s_fail++; }
    why[0] = 'x';
    if (openpete_disc_fs_slow("", why, sizeof why) != 0 || strcmp(why, "no path") != 0) {
        printf("FAIL empty path: %s\n", why); s_fail++;
    }
    decider_cases();
    uncached_cases(tmp && *tmp ? tmp : "/tmp");
    if (s_fail) return 1;
    printf("disc_fs_probe_test: OK\n");
    return 0;
}
