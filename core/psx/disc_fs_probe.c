/* core/psx/disc_fs_probe.c - see openpete_disc_cache.h (the auto mode's
 * filesystem classifier, read-speed decider and uncached reader). The
 * classifiers and deciders are pure and compile on every platform so the
 * unit test feeds them values of any OS; the filesystem query and the
 * uncached reader call the host's own API. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE   /* O_DIRECT */
#endif
#include "openpete_disc_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__linux__)
#  include <sys/vfs.h>
#  include <fcntl.h>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <sys/param.h>
#  include <sys/mount.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

/* Linux statfs f_type values (linux/magic.h and the filesystems' own
 * headers), with the name the boot log prints. slow = 1 marks a filesystem
 * whose reads cross a network or a VM boundary. */
typedef struct {
    uint64_t    magic;
    const char* name;
    int         slow;
} dfs_magic_t;

static const dfs_magic_t k_magic[] = {
    { 0x01021997u, "9p",       1 },   /* V9FS_MAGIC: WSL2 /mnt/c (drvfs), VM shares */
    { 0x00006969u, "nfs",      1 },   /* NFS_SUPER_MAGIC */
    { 0xFF534D42u, "cifs",     1 },   /* CIFS_SUPER_MAGIC */
    { 0x0000517Bu, "smb",      1 },   /* SMB_SUPER_MAGIC */
    { 0xFE534D42u, "smb2",     1 },   /* SMB2_SUPER_MAGIC */
    { 0x65735546u, "fuse",     1 },   /* FUSE_SUPER_MAGIC: sshfs, virtiofs, ntfs-3g */
    { 0x0000EF53u, "ext4",     0 },   /* EXT4_SUPER_MAGIC, shared by ext2 and ext3 */
    { 0x9123683Eu, "btrfs",    0 },
    { 0x58465342u, "xfs",      0 },
    { 0xF2F52010u, "f2fs",     0 },
    { 0x2FC12FC1u, "zfs",      0 },
    { 0x01021994u, "tmpfs",    0 },
    { 0x794C7630u, "overlayfs",0 },
    { 0x00004D44u, "vfat",     0 },
    { 0x2011BAB0u, "exfat",    0 },
    { 0x7366746Eu, "ntfs3",    0 },
    { 0x00009660u, "iso9660",  0 },
};

int openpete_disc_fs_classify_ftype(uint64_t f_type, char* why, size_t cap) {
    for (size_t i = 0; i < sizeof k_magic / sizeof k_magic[0]; i++) {
        if (k_magic[i].magic != f_type) continue;
        snprintf(why, cap, "%s%s", k_magic[i].slow ? "" : "local ", k_magic[i].name);
        return k_magic[i].slow;
    }
    snprintf(why, cap, "local fs 0x%llx", (unsigned long long)f_type);
    return 0;
}

/** @brief Whether a path is UNC: two leading separators naming a share, or the long form with the UNC prefix; a long-form drive path or a device path is not. */
static int path_is_unc(const char* path) {
    const int sep0 = path[0] == '\\' || path[0] == '/';
    const int sep1 = sep0 && (path[1] == '\\' || path[1] == '/');
    if (!sep1) return 0;
    if ((path[2] == '?' || path[2] == '.') && (path[3] == '\\' || path[3] == '/'))
        return strncmp(path + 4, "UNC", 3) == 0 && (path[7] == '\\' || path[7] == '/');
    return 1;
}

int openpete_disc_fs_classify_windows(unsigned drive_type, const char* path,
                                      char* why, size_t cap) {
    if (path && path_is_unc(path)) {
        const char* host = path + 2;
        if (strncmp(path + 2, "?\\UNC\\", 6) == 0 || strncmp(path + 2, "?/UNC/", 6) == 0) host = path + 8;
        const int wsl = strncmp(host, "wsl$", 4) == 0 || strncmp(host, "wsl.localhost", 13) == 0;
        snprintf(why, cap, "%s", wsl ? "unc wsl" : "unc");
        return 1;
    }
    switch (drive_type) {
    case 4: snprintf(why, cap, "remote drive"); return 1;   /* DRIVE_REMOTE */
    case 5: snprintf(why, cap, "cd-rom drive"); return 1;   /* DRIVE_CDROM */
    case 2: snprintf(why, cap, "local removable drive"); return 0;   /* DRIVE_REMOVABLE */
    case 3: snprintf(why, cap, "local fixed drive"); return 0;       /* DRIVE_FIXED */
    case 6: snprintf(why, cap, "local ram disk"); return 0;          /* DRIVE_RAMDISK */
    default: snprintf(why, cap, "local, drive type %u", drive_type); return 0;
    }
}

#if defined(_WIN32)
/** @brief Convert a path to UTF-16: UTF-8 when it is valid UTF-8, else the ANSI code page fopen reads it in. */
static int path_wide(const char* path, wchar_t* w, int cap) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, cap)) return 1;
    return MultiByteToWideChar(CP_ACP, 0, path, -1, w, cap) != 0;
}
#endif

int openpete_disc_fs_slow(const char* path, char* why, size_t cap) {
    if (!path || !*path) { snprintf(why, cap, "no path"); return 0; }
#if defined(_WIN32)
    if (path_is_unc(path)) return openpete_disc_fs_classify_windows(0, path, why, cap);
    wchar_t w[4096], full[4096], root[4096];
    if (!path_wide(path, w, 4096)) { snprintf(why, cap, "path does not convert"); return 0; }
    DWORD n = GetFullPathNameW(w, 4096, full, NULL);
    if (n == 0 || n >= 4096) { snprintf(why, cap, "GetFullPathNameW error %lu", (unsigned long)GetLastError()); return 0; }
    /* A full path that resolved to a UNC share (a relative path under a
     * share's working directory) classifies as one. */
    if (full[0] == L'\\' && full[1] == L'\\' && full[2] != L'?' && full[2] != L'.')
        return openpete_disc_fs_classify_windows(0, "\\\\", why, cap);
    if (!GetVolumePathNameW(full, root, 4096)) {
        snprintf(why, cap, "GetVolumePathNameW error %lu", (unsigned long)GetLastError());
        return 0;
    }
    return openpete_disc_fs_classify_windows(GetDriveTypeW(root), NULL, why, cap);
#elif defined(__linux__)
    struct statfs sfs;
    if (statfs(path, &sfs) != 0) { snprintf(why, cap, "statfs: %s", strerror(errno)); return 0; }
    return openpete_disc_fs_classify_ftype((uint64_t)(unsigned long)sfs.f_type, why, cap);
#elif defined(__APPLE__)
    struct statfs sfs;
    if (statfs(path, &sfs) != 0) { snprintf(why, cap, "statfs: %s", strerror(errno)); return 0; }
    const int slow = (sfs.f_flags & MNT_LOCAL) == 0;
    snprintf(why, cap, "%s%s", slow ? "" : "local ", sfs.f_fstypename);
    return slow;
#else
    snprintf(why, cap, "no filesystem query on this platform");
    return 0;
#endif
}

/* ------------------------------------------------------------------ *
 * The read-speed decider                                              *
 * ------------------------------------------------------------------ */

/** @brief Order two int64 values for qsort. */
static int cmp_i64(const void* a, const void* b) {
    const int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

void openpete_disc_probe_add(openpete_disc_probe_t* p, size_t bytes, int64_t ns) {
    p->bytes += bytes;
    p->ns += ns > 0 ? ns : 0;
    if (p->n < OPENPETE_DISC_PROBE_SAMPLES) p->lat[p->n] = ns;
    p->n++;
}

int64_t openpete_disc_probe_p99(const openpete_disc_probe_t* p) {
    const uint32_t n = p->n < OPENPETE_DISC_PROBE_SAMPLES ? p->n : OPENPETE_DISC_PROBE_SAMPLES;
    if (n == 0) return 0;
    int64_t sorted[OPENPETE_DISC_PROBE_SAMPLES];
    memcpy(sorted, p->lat, n * sizeof sorted[0]);
    qsort(sorted, n, sizeof sorted[0], cmp_i64);
    return sorted[(size_t)((uint64_t)(n - 1u) * 99u / 100u)];
}

int openpete_disc_probe_slow(const openpete_disc_probe_t* p, char* why, size_t cap) {
    const int64_t p99 = openpete_disc_probe_p99(p);
    /* bytes per ns * 1e9 / 1e6 = MB/s; no reads measured counts as fast. */
    const double mbs = p->ns > 0 ? (double)p->bytes * 1e3 / (double)p->ns : 0.0;
    const int slow_rate = p->ns > 0 &&
        (double)p->bytes * 1e9 < (double)OPENPETE_DISC_MIN_BPS * (double)p->ns;
    const int slow_lat = p99 > OPENPETE_DISC_MAX_P99_NS;
    snprintf(why, cap, "%.0f MB/s, p99 %.2f ms per %u KB read over %u reads; floor %u MB/s, "
             "ceiling %.2f ms", mbs, (double)p99 / 1e6, OPENPETE_DISC_PROBE_BLOCK >> 10, p->n,
             (unsigned)(OPENPETE_DISC_MIN_BPS / 1000000u), (double)OPENPETE_DISC_MAX_P99_NS / 1e6);
    return slow_rate || slow_lat;
}

int openpete_disc_burst_add(openpete_disc_burst_t* b, int64_t tick, int64_t ns) {
    if (tick != b->tick) { b->tick = tick; b->ns = 0; }
    b->ns += ns > 0 ? ns : 0;
    return b->ns > OPENPETE_DISC_BURST_NS;
}

/* ------------------------------------------------------------------ *
 * The uncached reader                                                 *
 * ------------------------------------------------------------------ */

/* Alignment of an unbuffered read's offset, length and buffer: 4096 covers
 * the 512- and 4096-byte logical sectors of every disk O_DIRECT and
 * FILE_FLAG_NO_BUFFERING accept. */
#define DFS_ALIGN 4096u
/* Bounce buffer: one probe block plus a leading and a trailing partial page. */
#define DFS_BOUNCE (OPENPETE_DISC_PROBE_BLOCK + 2u * DFS_ALIGN)

struct openpete_disc_uncached {
#if defined(_WIN32)
    HANDLE   h;
#else
    int      fd;
#endif
    int      evict;    /* 1: the handle reads through the page cache; each read drops its pages first */
    uint8_t* bounce;   /* DFS_ALIGN-aligned, DFS_BOUNCE bytes */
    void*    bounce_raw;
};

/** @brief Allocate the reader's bounce buffer, DFS_ALIGN-aligned.
 *
 * @param u The reader.
 * @return 1 on success. */
static int bounce_alloc(openpete_disc_uncached_t* u) {
    u->bounce_raw = malloc(DFS_BOUNCE + DFS_ALIGN);
    if (!u->bounce_raw) return 0;
    u->bounce = (uint8_t*)(((uintptr_t)u->bounce_raw + DFS_ALIGN - 1u) & ~(uintptr_t)(DFS_ALIGN - 1u));
    return 1;
}

openpete_disc_uncached_t* openpete_disc_uncached_open(const char* path, char* how, size_t cap) {
    openpete_disc_uncached_t* u = (openpete_disc_uncached_t*)calloc(1, sizeof *u);
    if (!u || !path || !bounce_alloc(u)) { free(u); snprintf(how, cap, "no memory"); return NULL; }
#if defined(_WIN32)
    wchar_t w[4096];
    u->h = INVALID_HANDLE_VALUE;
    if (path_wide(path, w, 4096))
        u->h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING, NULL);
    if (u->h != INVALID_HANDLE_VALUE) { snprintf(how, cap, "FILE_FLAG_NO_BUFFERING"); return u; }
    snprintf(how, cap, "CreateFileW error %lu", (unsigned long)GetLastError());
#elif defined(__linux__)
    u->fd = open(path, O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (u->fd >= 0) { snprintf(how, cap, "O_DIRECT"); return u; }
    u->fd = open(path, O_RDONLY | O_CLOEXEC);   /* tmpfs and some FUSE mounts refuse O_DIRECT */
    if (u->fd >= 0) { u->evict = 1; snprintf(how, cap, "fadvise DONTNEED"); return u; }
    snprintf(how, cap, "open: %s", strerror(errno));
#elif defined(__APPLE__)
    u->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (u->fd >= 0 && fcntl(u->fd, F_NOCACHE, 1) == 0) { snprintf(how, cap, "F_NOCACHE"); return u; }
    if (u->fd >= 0) close(u->fd);
    snprintf(how, cap, "open: %s", strerror(errno));
#else
    snprintf(how, cap, "no uncached read on this platform");
#endif
    free(u->bounce_raw);
    free(u);
    return NULL;
}

/** @brief Read [at, at + n) through the reader's handle into its bounce buffer; at and n are DFS_ALIGN multiples.
 *
 * @param u The reader.
 * @param at First byte.
 * @param n Byte count, at most DFS_BOUNCE.
 * @return Bytes read; fewer at end of file or on an error. */
static size_t raw_read(openpete_disc_uncached_t* u, uint64_t at, size_t n) {
    size_t done = 0;
    while (done < n) {
#if defined(_WIN32)
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)(at + done);
        ov.OffsetHigh = (DWORD)((at + done) >> 32);
        DWORD got = 0;
        if (!ReadFile(u->h, u->bounce + done, (DWORD)(n - done), &got, &ov) || got == 0) break;
        done += got;
#elif defined(__linux__) || defined(__APPLE__)
        ssize_t got = pread(u->fd, u->bounce + done, n - done, (off_t)(at + done));
        if (got <= 0) break;
        done += (size_t)got;
        if (done % DFS_ALIGN != 0) break;   /* end of file: an unaligned remainder ends the read */
#else
        (void)u; (void)at; break;
#endif
    }
    return done;
}

size_t openpete_disc_uncached_read(openpete_disc_uncached_t* u, uint64_t offset, void* dst,
                                   size_t len) {
    if (len > OPENPETE_DISC_PROBE_BLOCK) len = OPENPETE_DISC_PROBE_BLOCK;
    const uint64_t at = offset & ~(uint64_t)(DFS_ALIGN - 1u);
    const uint64_t end = (offset + len + DFS_ALIGN - 1u) & ~(uint64_t)(DFS_ALIGN - 1u);
#if defined(__linux__)
    if (u->evict) (void)posix_fadvise(u->fd, (off_t)at, (off_t)(end - at), POSIX_FADV_DONTNEED);
#endif
    const size_t got = raw_read(u, at, (size_t)(end - at));
    const size_t lead = (size_t)(offset - at);
    if (got <= lead) return 0;
    const size_t n = got - lead < len ? got - lead : len;
    memcpy(dst, u->bounce + lead, n);
    return n;
}

void openpete_disc_uncached_close(openpete_disc_uncached_t* u) {
    if (!u) return;
#if defined(_WIN32)
    if (u->h != INVALID_HANDLE_VALUE) CloseHandle(u->h);
#elif defined(__linux__) || defined(__APPLE__)
    if (u->fd >= 0) close(u->fd);
#endif
    free(u->bounce_raw);
    free(u);
}
