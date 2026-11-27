/* core/psx/disc_image.c - see openpete_disc_image.h. */
#include "openpete_disc_image.h"
#include "openpete_disc_cache.h"
#include "openpete_disc_trace.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "openpete_time.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ECMA-119 layout. The volume descriptor set starts at logical sector 16;
 * a descriptor's type byte is 1 for the primary volume descriptor and 255
 * for the set terminator, followed by the standard identifier "CD001". The
 * root directory record sits at byte 156 of the primary descriptor. A
 * directory record holds its length at byte 0, the extent's LBA (both-
 * endian, little-endian half first) at byte 2, the data length at byte 10,
 * the file flags at byte 25 (bit 1: directory), the identifier length at
 * byte 32 and the identifier from byte 33. Records never cross a sector;
 * a zero length byte ends a sector's records. */
#define ISO_VD_FIRST_LBA   16u
#define ISO_VD_MAX         32u
#define ISO_VD_PRIMARY     1u
#define ISO_VD_TERMINATOR  255u
#define ISO_PVD_ROOT       156u
#define ISO_DR_LEN         0u
#define ISO_DR_LBA         2u
#define ISO_DR_SIZE        10u
#define ISO_DR_FLAGS       25u
#define ISO_DR_NAME_LEN    32u
#define ISO_DR_NAME        33u
#define ISO_DR_FLAG_DIR    0x02u
#define ISO_NAME_MAX       64u
#define ISO_DEPTH_MAX      8

/* Raw sector header: 12 sync bytes, 3 address bytes, the mode byte; a Mode 2
 * sector carries an 8-byte subheader before its user data. */
#define RAW_MODE_BYTE   15u
#define RAW_USER_MODE1  16u
#define RAW_USER_MODE2  24u

/* Sectors disc_image_read_file reads per file read (and per trace row). */
#define READ_BATCH      64u

struct disc_image {
    FILE*    fp;
    char*    bin;
    uint64_t size;
    uint32_t sector_bytes;
    int      mode;
    int      tag;
    int      have_root;        /* root_lba / root_size read from the PVD */
    uint32_t root_lba, root_size;
};

static disc_image_t* s_game;

/** @brief Read a little-endian 32-bit value.
 *
 * @param p Four bytes.
 * @return The value. */
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** @brief Read up to @p len bytes at @p offset without moving any shared cursor.
 *
 * @param fp The open image.
 * @param offset First byte.
 * @param dst Receives the bytes.
 * @param len Byte count.
 * @return Bytes read; fewer than @p len at end of file or on an error. */
static size_t file_pread(FILE* fp, uint64_t offset, void* dst, size_t len) {
    size_t done = 0;
    while (done < len) {
#ifdef _WIN32
        HANDLE h = (HANDLE)_get_osfhandle(_fileno(fp));
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t at = offset + done;
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        size_t want = len - done;
        if (want > 0x40000000u) want = 0x40000000u;
        DWORD got = 0;
        if (!ReadFile(h, (uint8_t*)dst + done, (DWORD)want, &got, &ov) || got == 0) break;
        done += got;
#else
        ssize_t got = pread(fileno(fp), (uint8_t*)dst + done, len - done, (off_t)(offset + done));
        if (got <= 0) break;
        done += (size_t)got;
#endif
    }
    return done;
}

/** @brief Return the size of an open file.
 *
 * @param fp The file.
 * @return The size in bytes, or -1 when it cannot be read. */
static int64_t file_size(FILE* fp) {
#ifdef _WIN32
    if (_fseeki64(fp, 0, SEEK_END) != 0) return -1;
    int64_t n = _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0) return -1;
    int64_t n = (int64_t)ftello(fp);
#endif
    rewind(fp);
    return n;
}

disc_image_t* disc_image_open(const char* path, int mode, int tag) {
    char bin[4096], tried[4096];
    tried[0] = '\0';   /* the resolver writes it; a NULL path never reaches the resolver */
    if (!path || openpete_psx_resolve_disc_bin(path, bin, sizeof bin, tried, sizeof tried) != 0) {
        OPL_ERROR_F("disc", "no disc image at %s (tried: %s)", path ? path : "(null)",
                    tried[0] ? tried : "(nothing)");
        return NULL;
    }
    FILE* fp = fopen(bin, "rb");
    if (!fp) { OPL_ERROR_F("disc", "cannot open disc image %s", bin); return NULL; }
    int64_t size = file_size(fp);
    if (size <= 0 || size % DISC_IMAGE_RAW_SECTOR != 0) {
        if (size > 0 && size % DISC_IMAGE_USER_SECTOR == 0)
            OPL_ERROR_F("disc", "%s holds 2048-byte sectors: an image of user data only has lost "
                        "the Mode 2 Form 2 sectors that carry the game's XA audio. Rip the disc "
                        "as raw 2352-byte sectors (BIN/CUE).", bin);
        else
            OPL_ERROR_F("disc", "%s: size %lld is not a whole number of 2352-byte sectors",
                        bin, (long long)size);
        fclose(fp);
        return NULL;
    }
    disc_image_t* img = (disc_image_t*)calloc(1, sizeof *img);
    char* copy = (char*)malloc(strlen(bin) + 1u);
    if (!img || !copy) { free(img); free(copy); fclose(fp); return NULL; }
    memcpy(copy, bin, strlen(bin) + 1u);
    img->fp = fp;
    img->bin = copy;
    img->size = (uint64_t)size;
    img->sector_bytes = DISC_IMAGE_RAW_SECTOR;
    img->mode = mode;
    img->tag = tag;
    return img;
}

void disc_image_close(disc_image_t* img) {
    if (!img) return;
    if (s_game == img) s_game = NULL;
    fclose(img->fp);
    free(img->bin);
    free(img);
}

const char* disc_image_bin_path(const disc_image_t* img) { return img->bin; }

uint64_t disc_image_size(const disc_image_t* img) { return img->size; }

uint32_t disc_image_sector_bytes(const disc_image_t* img) { return img->sector_bytes; }

int disc_image_read_bytes(disc_image_t* img, uint64_t offset, void* dst,
                          size_t len, int tag) {
    uint8_t* out = (uint8_t*)dst;
    size_t in_image = offset >= img->size ? 0
                    : (img->size - offset < len ? (size_t)(img->size - offset) : len);
    if (in_image < len) memset(out + in_image, 0, len - in_image);
    int result = OPENPETE_DISC_DIRECT;
    const int cached = img->mode == DISC_READ_CACHED && in_image;
    if (cached) {
        int c = openpete_disc_cache_copy(img->bin, offset, out, in_image, tag);
        if (c == 1) return in_image == len ? OPENPETE_DISC_HIT : -1;
        if (c == 0) result = OPENPETE_DISC_MISS;
    }
    const int64_t t0 = cached ? op_now() : 0;
    size_t got = file_pread(img->fp, offset, out, in_image);
    if (got < in_image) memset(out + got, 0, in_image - got);
    if (result == OPENPETE_DISC_MISS)
        openpete_disc_cache_miss_served(tag, offset, in_image, op_now() - t0);
    else if (cached)
        openpete_disc_cache_direct_served(img->bin, tag, offset, in_image, op_now() - t0);
    return got == len ? result : -1;
}

/** @brief Read @p count consecutive raw sectors in one read, traced as one row under the handle's tag.
 *
 * @param img The handle.
 * @param lba First sector.
 * @param count Sectors.
 * @param out Receives count * 2352 bytes.
 * @return 0, or -1 when a sector lies past the end or the file came up short. */
static int read_raw_run(disc_image_t* img, uint32_t lba, uint32_t count, uint8_t* out) {
    int r = disc_image_read_bytes(img, (uint64_t)lba * DISC_IMAGE_RAW_SECTOR, out,
                                  (size_t)count * DISC_IMAGE_RAW_SECTOR, img->tag);
    openpete_disc_trace_sectors(img->tag, lba, count, r < 0 ? OPENPETE_DISC_DIRECT : r);
    return r < 0 ? -1 : 0;
}

/** @brief Locate a raw sector's user data from its mode byte.
 *
 * @param raw One raw sector.
 * @return The first of its 2048 user-data bytes: byte 16 for Mode 1, byte
 *         24 (after the subheader) otherwise. */
static const uint8_t* user_data(const uint8_t* raw) {
    return raw + (raw[RAW_MODE_BYTE] == 1u ? RAW_USER_MODE1 : RAW_USER_MODE2);
}

int disc_image_read_sector(disc_image_t* img, uint32_t lba, uint8_t* out2352) {
    return read_raw_run(img, lba, 1, out2352);
}

int disc_image_read_user(disc_image_t* img, uint32_t lba, uint8_t* out2048) {
    uint8_t raw[DISC_IMAGE_RAW_SECTOR];
    if (read_raw_run(img, lba, 1, raw) != 0) return -1;
    memcpy(out2048, user_data(raw), DISC_IMAGE_USER_SECTOR);
    return 0;
}

/** @brief Read the root directory's extent from the primary volume descriptor, once per handle.
 *
 * @param img The handle.
 * @return 0, or -1 when no primary descriptor precedes the terminator. */
static int read_root(disc_image_t* img) {
    if (img->have_root) return 0;
    uint8_t vd[DISC_IMAGE_USER_SECTOR];
    for (uint32_t i = 0; i < ISO_VD_MAX; i++) {
        if (disc_image_read_user(img, ISO_VD_FIRST_LBA + i, vd) != 0) return -1;
        if (memcmp(vd + 1, "CD001", 5) != 0) return -1;
        if (vd[0] == ISO_VD_TERMINATOR) return -1;
        if (vd[0] != ISO_VD_PRIMARY) continue;
        img->root_lba = le32(vd + ISO_PVD_ROOT + ISO_DR_LBA);
        img->root_size = le32(vd + ISO_PVD_ROOT + ISO_DR_SIZE);
        img->have_root = 1;
        return 0;
    }
    return -1;
}

/** @brief Copy a name for comparison: up to the version separator ';', without a trailing '.', upper-cased.
 *
 * @param src The name.
 * @param n Its length.
 * @param out Receives the comparison form, NUL-terminated.
 * @return The comparison form's length. */
static size_t name_key(const char* src, size_t n, char out[ISO_NAME_MAX]) {
    size_t k = 0;
    for (size_t i = 0; i < n && src[i] != ';' && k + 1 < ISO_NAME_MAX; i++)
        out[k++] = (char)toupper((unsigned char)src[i]);
    if (k && out[k - 1] == '.') k--;
    out[k] = '\0';
    return k;
}

/** @brief Find one name among a directory extent's records.
 *
 * @param img The handle.
 * @param dir_lba The directory's first sector.
 * @param dir_size The directory's length in bytes.
 * @param key The name in name_key form.
 * @param entry Receives the matching record.
 * @return 0 when found, -1 when absent or a sector cannot be read. */
static int find_in_dir(disc_image_t* img, uint32_t dir_lba, uint32_t dir_size,
                       const char* key, disc_image_entry_t* entry) {
    uint8_t sec[DISC_IMAGE_USER_SECTOR];
    uint32_t sectors = (dir_size + DISC_IMAGE_USER_SECTOR - 1u) / DISC_IMAGE_USER_SECTOR;
    for (uint32_t s = 0; s < sectors; s++) {
        if (disc_image_read_user(img, dir_lba + s, sec) != 0) return -1;
        for (uint32_t at = 0; at + ISO_DR_NAME < DISC_IMAGE_USER_SECTOR;) {
            uint32_t len = sec[at + ISO_DR_LEN];
            if (len == 0 || at + len > DISC_IMAGE_USER_SECTOR) break;
            uint32_t nlen = sec[at + ISO_DR_NAME_LEN];
            char rec_key[ISO_NAME_MAX];
            /* Identifiers 0x00 and 0x01 name the directory itself and its parent. */
            if (ISO_DR_NAME + nlen <= len && !(nlen == 1 && sec[at + ISO_DR_NAME] <= 1u) &&
                name_key((const char*)sec + at + ISO_DR_NAME, nlen, rec_key) &&
                strcmp(rec_key, key) == 0) {
                entry->lba = le32(sec + at + ISO_DR_LBA);
                entry->size = le32(sec + at + ISO_DR_SIZE);
                entry->is_dir = (sec[at + ISO_DR_FLAGS] & ISO_DR_FLAG_DIR) != 0;
                return 0;
            }
            at += len;
        }
    }
    return -1;
}

int disc_image_find(disc_image_t* img, const char* path, disc_image_entry_t* entry) {
    if (!img || !path || !entry || read_root(img) != 0) return -1;
    disc_image_entry_t cur = { img->root_lba, img->root_size, 1 };
    const char* p = path;
    for (int depth = 0; depth < ISO_DEPTH_MAX; depth++) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        size_t n = strcspn(p, "/\\");
        char key[ISO_NAME_MAX];
        if (!cur.is_dir || name_key(p, n, key) == 0) return -1;
        if (find_in_dir(img, cur.lba, cur.size, key, &cur) != 0) return -1;
        p += n;
        while (*p == '/' || *p == '\\') p++;
        if (!*p) { *entry = cur; return 0; }
    }
    return -1;
}

size_t disc_image_read_file_range(disc_image_t* img, const disc_image_entry_t* entry,
                                  uint32_t offset, void* buf, size_t len) {
    if (offset >= entry->size) return 0;
    size_t avail = entry->size - offset;
    size_t want = avail < len ? avail : len;
    uint8_t* raw = (uint8_t*)malloc((size_t)READ_BATCH * DISC_IMAGE_RAW_SECTOR);
    if (!raw) return 0;
    size_t done = 0;
    uint32_t skip = offset % DISC_IMAGE_USER_SECTOR;   /* bytes before offset in the first sector */
    for (uint32_t lba = entry->lba + offset / DISC_IMAGE_USER_SECTOR; done < want;) {
        size_t left = (skip + want - done + DISC_IMAGE_USER_SECTOR - 1u) / DISC_IMAGE_USER_SECTOR;
        uint32_t n = left < READ_BATCH ? (uint32_t)left : READ_BATCH;
        if (read_raw_run(img, lba, n, raw) != 0) break;
        for (uint32_t i = 0; i < n && done < want; i++) {
            size_t room = DISC_IMAGE_USER_SECTOR - skip;
            size_t m = want - done < room ? want - done : room;
            memcpy((uint8_t*)buf + done,
                   user_data(raw + (size_t)i * DISC_IMAGE_RAW_SECTOR) + skip, m);
            done += m;
            skip = 0;
        }
        lba += n;
    }
    free(raw);
    return done;
}

size_t disc_image_read_file(disc_image_t* img, const disc_image_entry_t* entry,
                            void* buf, size_t max) {
    return disc_image_read_file_range(img, entry, 0, buf, max);
}

void disc_image_set_game(disc_image_t* img) { s_game = img; }

disc_image_t* disc_image_game(void) { return s_game; }

int disc_image_game_read_sector(uint32_t lba, uint8_t* out2352) {
    disc_image_t* img = s_game;
    if (!img || !out2352) return -1;
    int r = disc_image_read_bytes(img, (uint64_t)lba * DISC_IMAGE_RAW_SECTOR, out2352,
                                  DISC_IMAGE_RAW_SECTOR, OPENPETE_DISC_TAG_MOD);
    openpete_disc_trace_sectors(OPENPETE_DISC_TAG_MOD, lba, 1, r < 0 ? OPENPETE_DISC_DIRECT : r);
    return r < 0 ? -1 : 0;
}
