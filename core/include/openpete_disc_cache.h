/* core/include/openpete_disc_cache.h - the disc image resident in host memory.
 *
 * The cache is disc_image's backing store. On attach it allocates the whole
 * image and a background thread fills it, tracking residency per 2352-byte
 * sector, in runs of missing sectors: first the chunk (8 MB rounded down to
 * whole sectors) a game read, then an XA read, last missed on; then the
 * game's boot-order profile ranges in order (openpete_disc_cache_set_boot_order,
 * generated from games/<game>/manifest/disc_boot_order.toml); then every
 * chunk in file order. A disc_image read of resident sectors is a memcpy (a
 * hit); a read touching any other sector is served from the file at once (a
 * miss), counted with its read path, emitted as a pacelog CD_MISS event,
 * and logged once per chunk. No read waits on the loader. Bytes are
 * identical to the file path.
 *
 * Harness knobs, read at attach: OPENPETE_DISC_CACHE_BPS=<bytes per second>
 * holds each loader run until that many bytes per second of VBL time
 * (2250000/37609 Hz, counted from VBL 0) cover every byte the loader and
 * the misses have read; OPENPETE_DISC_BOOT_ORDER=0 drops the profile (file
 * order).
 *
 * Under --disc-cache auto the cache may detach after measuring the file
 * fast and attach again when the watchdog sees slow demand reads
 * (openpete_disc_cache_attach_auto); a read of a detached image returns -1
 * from openpete_disc_cache_copy and goes to the file. Every module state
 * except the attach state is read and written under one lock; a hit's copy
 * runs under it, since the loader frees the image under it.
 *
 * PsyCross reads the image through openpete_disc_psycross_bind in both
 * cache modes, so libcd and XA reads take the same path as every host
 * reader. */
#ifndef OPENPETE_DISC_CACHE_H
#define OPENPETE_DISC_CACHE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct disc_image;

/** One boot-order profile range: @c count sectors from @c lba. */
typedef struct {
    uint32_t lba;
    uint32_t count;
} openpete_disc_range_t;

/** @brief Install the game's boot-order profile, read by the next attach.
 *
 * The loader reads the missing sectors of each range in order after the
 * steered chunks and before file order. Ranges past the image are skipped.
 * @param ranges The profile, kept by reference for the session; NULL or
 *        @p n 0 means file order.
 * @param n Range count. */
void openpete_disc_cache_set_boot_order(const openpete_disc_range_t* ranges, size_t n);

/** @brief Allocate the image and start the background loader.
 *
 * When the thread cannot be created the image loads inline before the call
 * returns. A call while already attached changes nothing.
 * @param bin_path Path to the sector image, as disc_image_bin_path reports
 *        it; disc_image handles on the same path read through the cache.
 * @return 1 when the cache is attached (or already was); 0 when
 *         @p bin_path is NULL or the file cannot be opened, sized (larger
 *         than 0x7FFFFFFF bytes) or allocated, and readers use the file. */
int  openpete_disc_cache_attach(const char* bin_path);
/** @brief Attach the cache measuring (--disc-cache auto on a filesystem the classifier calls local).
 *
 * The loader reads the boot-order profile uncached and timed
 * (openpete_disc_uncached_open) and, once openpete_disc_probe_slow has
 * enough reads, logs the numbers and either keeps filling (slow) or frees
 * the image (fast). A fast file is then watched: each demand read served
 * from it is timed (openpete_disc_cache_direct_served), and a tick whose
 * demand reads pass OPENPETE_DISC_BURST_NS attaches the cache again, the
 * loader starting at the chunk of the read that passed it. When no
 * uncached handle opens, the cache starts detached and watched.
 * @param bin_path Path to the sector image.
 * @return 1 when the cache is attached (measuring), else 0. */
int  openpete_disc_cache_attach_auto(const char* bin_path);
/** @brief Report whether the cache is attached.
 *
 * 1 while auto measures; 0 once auto frees the image, and 1 again if the
 * watchdog attaches it. Stays 1 after openpete_disc_cache_shutdown, since
 * resident sectors still serve.
 * @return 1 while attached, else 0. */
int  openpete_disc_cache_active(void);
/** @brief Copy image bytes [offset, offset + len) from resident sectors.
 *
 * On a miss the chunk of the first sector not resident becomes the
 * loader's next target for a game read (libcd, extract, WAD overlay) or an
 * XA read; the EXE read and a host read (woah NSF, library scan, ingest,
 * sfx census, mods, untagged) steer nothing. The caller then reads the
 * file and reports it with
 * openpete_disc_cache_miss_served.
 * @param bin_path The reader's sector image path.
 * @param offset First byte.
 * @param dst Receives @p len bytes on a hit.
 * @param len Byte count.
 * @param tag The OPENPETE_DISC_TAG_* path issuing the read.
 * @return 1 on a hit; 0 on a miss; -1 when the cache is not attached,
 *         holds another file, or the range passes the end of the image. */
int  openpete_disc_cache_copy(const char* bin_path, uint64_t offset, void* dst,
                              size_t len, int tag);
/** @brief Count a miss the reader served from the file.
 *
 * Adds to the miss and file-time totals and the tag's miss count, emits a
 * pacelog CD_MISS event, and logs a warning on the first miss in the chunk.
 * @param tag The OPENPETE_DISC_TAG_* path that missed.
 * @param offset First byte of the read.
 * @param len Byte count.
 * @param ns Time the file read took. */
void openpete_disc_cache_miss_served(int tag, uint64_t offset, size_t len, int64_t ns);
/** @brief Time a read a cached handle served from the file without the cache: the watchdog's input.
 *
 * Acts only while auto has freed the image after measuring it fast, and
 * only on a demand read (libcd, extract, WAD overlay, XA) of the watched
 * image, once the runtime counts VBLs (openpete_disc_trace_vbl not -1).
 * The read's time is added to its tick's sum (two VBLs per tick);
 * the first tick whose sum passes OPENPETE_DISC_BURST_NS logs once and
 * attaches the cache, the loader starting at this read's chunk. The read
 * itself was already served; nothing here waits on the loader.
 * @param bin_path The reader's sector image path.
 * @param tag The OPENPETE_DISC_TAG_* path issuing the read.
 * @param offset First byte of the read.
 * @param len Byte count.
 * @param ns Time the file read took. */
void openpete_disc_cache_direct_served(const char* bin_path, int tag, uint64_t offset,
                                       size_t len, int64_t ns);
/** @brief Bind PsyCross's CD image to a disc_image handle; its libcd and XA reads then go through it.
 *
 * Installs the read callback with PsyX_CDFS_SetMemRead and declares an
 * image of the handle's size, with no buffer, through PsyX_CDFS_Init_Mem.
 * Works in both cache modes; the handle decides between the cache and the
 * file.
 * @param img The booted disc's handle, kept for the session.
 * @return 1 when bound; 0 for a NULL handle or an image larger than
 *         0x7FFFFFFF bytes. */
int  openpete_disc_psycross_bind(struct disc_image* img);
/** @brief Install PsyCross's read observer when OPENPETE_DISC_TRACE is set, so its libcd and XA reads are traced.
 *
 * A read is traced with the result its disc_image read returned: hit or
 * miss with the cache attached, direct without it. Call before the first
 * PsyCross read. */
void openpete_disc_cache_trace_install(void);
/** @brief Stop the loader and join its thread.
 *
 * The image buffer stays allocated and its resident sectors keep serving;
 * sectors the loader had not read stay misses. No effect when the cache was
 * never attached. */
void openpete_disc_cache_shutdown(void);
/** @brief Report the misses one read path took.
 *
 * @param tag An OPENPETE_DISC_TAG_* value.
 * @return The count; 0 for an unknown tag. */
uint64_t openpete_disc_cache_tag_misses(int tag);
/** @brief Report the cache's counters for the exit census.
 *
 * Each output pointer may be NULL.
 * @param misses Receives the number of reads served from the file while
 *        the cache was attached.
 * @param direct_ns Receives the total time those file reads took, in ns.
 * @param bytes Receives the image size in bytes.
 * @param load_ns Receives the loader's wall time from attach to the last
 *        run, in ns; 0 until the loader finishes. */
void openpete_disc_cache_census(uint64_t* misses, int64_t* direct_ns,
                                size_t* bytes, int64_t* load_ns);

/* --disc-cache auto attaches the cache only when the image sits on a slow
 * filesystem: one whose reads cross a network or a VM boundary. */

/** @brief Classify a Linux statfs f_type as slow (9p, NFS, CIFS, SMB, SMB2, FUSE) or local.
 *
 * @param f_type The statfs f_type value.
 * @param why Receives the filesystem name, prefixed "local " when local; an
 *        unlisted value is local and named by its hex value.
 * @param cap Size of @p why.
 * @return 1 when slow, else 0. */
int openpete_disc_fs_classify_ftype(uint64_t f_type, char* why, size_t cap);
/** @brief Classify a Windows image location as slow (a UNC path, a remote or CD-ROM drive) or local.
 *
 * A UNC path (two leading separators naming a server share, the long
 * form with the UNC prefix, and the wsl$ and wsl.localhost shares of a WSL
 * distribution) is slow whatever @p drive_type says.
 * @param drive_type The GetDriveTypeW value of the image's volume root.
 * @param path The image path as given, or NULL to classify by drive type alone.
 * @param why Receives the reason, prefixed "local" when local.
 * @param cap Size of @p why.
 * @return 1 when slow, else 0. */
int openpete_disc_fs_classify_windows(unsigned drive_type, const char* path,
                                      char* why, size_t cap);
/** @brief Query the filesystem holding a path and classify it.
 *
 * Linux: statfs and openpete_disc_fs_classify_ftype. Windows: the full
 * path's volume root, GetDriveTypeW and openpete_disc_fs_classify_windows.
 * macOS: statfs, slow when MNT_LOCAL is clear. A failed query, or another
 * platform, is local.
 * @param path The image path.
 * @param why Receives the reason the boot log prints.
 * @param cap Size of @p why.
 * @return 1 when slow, else 0. */
int openpete_disc_fs_slow(const char* path, char* why, size_t cap);

/* --disc-cache auto on a filesystem the classifier calls local measures it:
 * the loader reads the boot-order profile, OPENPETE_DISC_PROBE_BLOCK bytes
 * at a time, with the OS file cache bypassed, and times each read. The
 * thresholds put the game's largest demand burst inside a quarter tick
 * (tick 37609/1125000 s = 33.43 ms; quarter 8.36 ms). The largest burst
 * is 1.5 MiB: Spyro 1's attract demo reads at most 1.46 MB of libcd and XA
 * sectors in one tick (OPENPETE_DISC_TRACE over 8000 ticks with four level
 * loads), in at most 6 reads of OPENPETE_DISC_PROBE_BLOCK. So a file is slow
 * when its throughput is under 1.5 MiB per quarter tick (188 MB/s) or its
 * p99 read latency is over a sixth of a quarter tick (1.39 ms), the time
 * six reads may each take. A hard disk (seek 4-12 ms) and a USB 2 or
 * optical drive fail one or both; an NVMe or SATA SSD passes both. */

/** Nanoseconds in one tick: 37609/1125000 s. */
#define OPENPETE_DISC_TICK_NS       (37609000000000ll / 1125000ll)
/** Byte count of one timed probe read. */
#define OPENPETE_DISC_PROBE_BLOCK   (256u * 1024u)
/** Latencies kept for the p99; reads past it count toward the totals only. */
#define OPENPETE_DISC_PROBE_SAMPLES 1024u
/** Probe bytes after which the decision is taken once the profile is read: 32 reads. */
#define OPENPETE_DISC_PROBE_MIN     (8u * 1024u * 1024u)
/** Probe bytes after which the decision is taken whatever the profile: 128 reads. */
#define OPENPETE_DISC_PROBE_BUDGET  (32u * 1024u * 1024u)
/** The largest demand burst in one tick, bytes. */
#define OPENPETE_DISC_BURST_BYTES   (1536ull * 1024u)
/** File-read time of the game's demand reads within one tick past which the burst is slow: a quarter tick, ns. */
#define OPENPETE_DISC_BURST_NS      (OPENPETE_DISC_TICK_NS / 4)
/** Throughput floor: OPENPETE_DISC_BURST_BYTES per OPENPETE_DISC_BURST_NS, bytes per second. */
#define OPENPETE_DISC_MIN_BPS       (OPENPETE_DISC_BURST_BYTES * 1000000000ull / (uint64_t)OPENPETE_DISC_BURST_NS)
/** p99 latency ceiling of one probe read: a sixth of OPENPETE_DISC_BURST_NS, ns. */
#define OPENPETE_DISC_MAX_P99_NS    (OPENPETE_DISC_BURST_NS / 6)

/** Timed probe reads. Zero-initialise before the first add. */
typedef struct {
    uint64_t bytes;     /* bytes read */
    int64_t  ns;        /* summed read time */
    uint32_t n;         /* reads */
    int64_t  lat[OPENPETE_DISC_PROBE_SAMPLES];   /* the first reads' times, ns */
} openpete_disc_probe_t;

/** @brief Add one timed read to a probe.
 *
 * @param p The probe.
 * @param bytes Bytes the read returned.
 * @param ns Time the read took. */
void openpete_disc_probe_add(openpete_disc_probe_t* p, size_t bytes, int64_t ns);
/** @brief Return the 99th-percentile read time of the kept samples, ns; 0 with none.
 *
 * @param p The probe.
 * @return The p99 latency in ns. */
int64_t openpete_disc_probe_p99(const openpete_disc_probe_t* p);
/** @brief Decide whether the measured image is slow: throughput under OPENPETE_DISC_MIN_BPS or p99 over OPENPETE_DISC_MAX_P99_NS.
 *
 * A probe with no timed bytes is fast.
 * @param p The probe.
 * @param why Receives the measured numbers and the thresholds.
 * @param cap Size of @p why.
 * @return 1 when slow, else 0. */
int openpete_disc_probe_slow(const openpete_disc_probe_t* p, char* why, size_t cap);

/** The watchdog's running sum: file-read time of demand reads in one tick. */
typedef struct {
    int64_t tick;   /* tick the sum belongs to */
    int64_t ns;     /* summed read time in it */
} openpete_disc_burst_t;

/** @brief Add one demand read's file time to the burst of its tick.
 *
 * A read in another tick than the last restarts the sum.
 * @param b The running sum.
 * @param tick The tick the read happened in.
 * @param ns Time the read took.
 * @return 1 when the tick's sum passes OPENPETE_DISC_BURST_NS. */
int openpete_disc_burst_add(openpete_disc_burst_t* b, int64_t tick, int64_t ns);

/** A handle reading the image with the OS file cache bypassed. */
typedef struct openpete_disc_uncached openpete_disc_uncached_t;
/** @brief Open an image for uncached reads.
 *
 * Linux: O_DIRECT, else (a filesystem that refuses it) a plain handle
 * whose reads first drop their pages with POSIX_FADV_DONTNEED. Windows:
 * FILE_FLAG_NO_BUFFERING. macOS: F_NOCACHE.
 * @param path The image path.
 * @param how Receives the method, or the failure.
 * @param cap Size of @p how.
 * @return The handle, or NULL. */
openpete_disc_uncached_t* openpete_disc_uncached_open(const char* path, char* how, size_t cap);
/** @brief Read up to OPENPETE_DISC_PROBE_BLOCK bytes at any offset, bypassing the OS file cache.
 *
 * The read covers the enclosing 4096-byte-aligned range through a bounce
 * buffer and copies the requested bytes out.
 * @param u The handle.
 * @param offset First byte.
 * @param dst Receives the bytes.
 * @param len Byte count; clipped to OPENPETE_DISC_PROBE_BLOCK.
 * @return Bytes copied; fewer than @p len at end of file or on an error. */
size_t openpete_disc_uncached_read(openpete_disc_uncached_t* u, uint64_t offset, void* dst,
                                   size_t len);
/** @brief Close an uncached handle; NULL is accepted.
 *
 * @param u The handle. */
void openpete_disc_uncached_close(openpete_disc_uncached_t* u);
#ifdef __cplusplus
}
#endif
#endif
