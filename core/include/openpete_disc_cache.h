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
/** @brief Report whether the cache is attached.
 *
 * Stays 1 after openpete_disc_cache_shutdown, since resident sectors still serve.
 * @return 1 after a successful attach, else 0. */
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
#ifdef __cplusplus
}
#endif
#endif
