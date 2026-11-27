// core/record/savestate.c - save/load of openpete game state to slot files.
//
// v6 file layout (little-endian throughout, no padding inside the header
// thanks to fixed-size fields):
//
//   uint32_t  magic         = 'OSv1' (0x3176534F LE)
//   uint32_t  version       = 6
//   char      ident[32]     - content identity, see ident_fill()
//   uint32_t  frame         - openpete_frame (VBL counter) at save
//   uint32_t  cpu_bytes
//   uint32_t  ram_bytes
//   uint32_t  scratch_bytes
//   uint32_t  vram_bytes    (1 MiB - VRAM_WIDTH * VRAM_HEIGHT * 2)
//   uint32_t  vbl           - openpete_vbl (guest-visible VSync counter);
//                             restored so post-load wait loops don't see a
//                             wild delta (a one-VBL timeline skew)
//   uint32_t  spu_state_bytes  - PsyX SPU voice mirror + cursors
//   uint32_t  spu_mem_bytes    - SPU sample RAM
//   uint32_t  cd_state_bytes   - PsyCross CD head-position globals
//   uint32_t  game_extra_bytes - opaque per-game host blob (game_extras)
//   uint32_t  modset_tag_bytes - modset tag string (no NUL): the ordered
//                                form, or the set form in an older file
//   uint32_t  arena_bytes      - mod arena section (ledger rows + used bytes)
//   char      game[12]      - the saving game's id ("spyro-1"), NUL-padded;
//                             all zero in a file written before the field
//                             existed (it was reserved and written as 0)
//   uint32_t  vbl_device    - openpete_vbl_device (every VBLANK elapsed,
//                             held ones included); the drive head's clock
//   uint32_t  park_ra       - park site of the save: the guest ra of the
//   uint32_t  park_sp         psyq_VSync call whose final yield the fiber
//                             was parked at, and the guest sp there
//                             (openpete_yield_site); v5 and later
//   uint32_t  consume       - openpete_consume (entries of the game's
//                             pad-ring consumer, E) and openpete_consume_write
//   uint32_t  consume_write   (pad writes since that entry, j): a
//                             consume-keyed bk2's row position; v6 only
//   uint8_t   cpu_regs[cpu_bytes]
//   uint8_t   ram[ram_bytes]
//   uint8_t   scratch[scratch_bytes]
//   uint8_t   vram[vram_bytes]
//   uint8_t   spu_state[spu_state_bytes]
//   uint8_t   spu_mem[spu_mem_bytes]
//   uint8_t   cd_state[cd_state_bytes]
//   uint8_t   card[sizeof(state_card_t)] - card port FLAG bytes and the
//                             card each port held; sized by the ABI
//   uint8_t   game_extra[game_extra_bytes]
//   uint8_t   arena[arena_bytes]
//   char      modset_tag[modset_tag_bytes]
//
// Note on the libco game fiber: its host call stack is NOT snapshotted.
// On resume the fiber unwinds through whatever frames were live at load
// time. A load therefore applies only while the fiber is parked at the
// final yield of a psyq_VSync call with the save's park site (same ra,
// same sp), so the host frames directly below the yield are the recompiled
// continuation of the call the state was saved in; a load at another site
// returns 1 with nothing restored and the caller retries at a later final
// yield. The caller frames further out are the live run's; recompiled C
// reads cpu->fields fresh on every access, so after at most 1-2 outer-loop
// iterations execution stabilises against the loaded cpu+RAM.
//
// Compat (unsited load): a v4 file carries no park site. It is read with
// park_ra = park_sp = 0 and loads at any final yield, the rule before
// sites were recorded, with a warning. A v4 or v5 file carries no consume
// position and leaves openpete_consume and openpete_consume_write as they
// are. The header layout is versioned by `version` alone; OPENPETE_STATE_ABI is unchanged, so a v4 file keeps its
// identity. An attempt to memcpy the fiber memory was reverted
// because libco's Windows backend uses an opaque OS-fiber handle that
// can't be serialised, and the cross-platform split wasn't worth the
// small fidelity gain.
//
// The CPU portion is exactly the prefix of CPUState that comes before its
// memory-callback function pointers - those point at host code and must
// not survive a save/load roundtrip.
//
// Identity gating (v3): ident = sha256("stateabi:<N>|cg:<codegen_stamp>|
// rom:<rom_sha256>") hex-truncated to 30 chars. This covers what actually
// varies between installations - the recomp module (boot-compiled per
// machine under the distribution model) and the ROM - and pins the state
// LAYOUT via the curated OPENPETE_STATE_ABI number (bump it whenever any
// section layout changes). Unlike the old __DATE__/__TIME__ hash, dev
// rebuilds with unchanged layout keep their slots (the section-size checks
// below still catch gross drift). Residual dev-only risk: a layout-only
// change under an unbumped ABI on a "dev-static" codegen stamp - accepted,
// same curation contract as OPENPETE_RECOMP_ABI_VERSION.
//
// Game gating: the ident does not tell games apart when the module carries
// no ROM hash (a CMake-built module), so the header names the saving game
// and a load under another game is refused. The field took over bytes every
// earlier file wrote as zero, so the ABI is unchanged and an earlier file
// reads as naming no game (see state_game_check).

#include "openpete_state.h"
#include "openpete_discontinuity.h"
#include "openpete_fiber.h"  /* openpete_vbl - saved/restored via hdr.vbl */
#include "openpete_atomic_file.h"
#include "openpete_cdrom_mmio.h" /* the controller model joins the CD section */
#include "openpete_log.h"
#include "openpete_memcard.h" /* openpete_psx_bios_reseed_ram */
#include "openpete_psxtrace.h" /* openpete_sched_active */
#include "openpete_mods.h"   /* per-mod state participation + modset tag */
#include "openpete_paths.h"
#include "openpete_psx.h"
#include "openpete_recomp_module.h"  /* codegen_stamp / rom_sha256 identity */
#include "game_extras.h"
#include "openpete_modsave.h"
#include "sha256.h"

#include "PsyX/PsyX_public.h"

/* PsyX SPU state-export (PsyX_SPUAL.h is a PsyCross-internal header; the
 * thunks extern these the same way). */
extern int  PsyX_CD_QueueStateSize(void);
extern void PsyX_CD_QueueSnapshot(void* dst);
extern void PsyX_CD_QueueRestore(const void* src);
extern unsigned int PsyX_SPUAL_StateSize(void);
extern int PsyX_SPUAL_SnapshotState(void* dst, unsigned int size);
extern int PsyX_SPUAL_RestoreState(const void* src, unsigned int size);
extern int PsyX_SPUAL_RestoreStateEx(const void* src, unsigned int size,
                                     int quiet_al);
extern unsigned int PsyX_SPUAL_MemSize(void);
extern int PsyX_SPUAL_SnapshotMem(void* dst, unsigned int size);
extern int PsyX_SPUAL_RestoreMem(const void* src, unsigned int size);

/* PsyCross CD head-position globals (LIBCD.C; plain global-scope names,
 * identical symbols under C and C++ linkage). Saved so a state taken
 * between a guest CdlSetloc and its CdRead doesn't resume reading from
 * wherever the OTHER timeline left the head. The command QUEUE
 * (g_cdComQueue) is NOT saved: it holds callback pointers and is drained
 * synchronously in this port; a queued-command-in-flight save would need
 * a quiescence audit first. */
extern int g_cdCurrentSector;
extern int g_cdSectorSize;
extern int g_cdCurrentTrack;
extern int g_cdNumFrames;
extern int g_CD_com;
extern int g_cdReadDoneFlag;
extern int g_cdXaStartLBA;
extern int g_cdXaStartVBL;

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
  #include <unistd.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */
#endif

#define OPENPETE_STATE_MAGIC    0x3176534Fu  /* 'OSv1' little-endian */
#define OPENPETE_STATE_VERSION  7u
/* The oldest header version this build reads: v4, the v6 header without
 * the park site and the consume position. */
#define OPENPETE_STATE_VERSION_MIN 4u
#define OPENPETE_STATE_HASH_LEN 32
/* Bytes of the header's game id field; an id fills at most LEN - 1. */
#define OPENPETE_STATE_GAME_LEN 12

/* Curated state-layout ABI. Bump on ANY change to a section's layout
 * (header fields, SPUALVoiceState packing, cd_state_t, mods-section
 * framing, arena ledger, ...). Part of the ident hash, so a bump cleanly
 * orphans older files. 4 = arena section, MSv2 mods framing.
 * 5 = virtual XA music-head anchor joined the CD section. */
/* 6 = arena section v2: high-arena ledger + the arena's used bytes.
 * 7 = arena section v3: one ledger with allocation + stream-handle rows;
 *     the low-park ledger is gone with the low allocator.
 * 8 = SPUALVoiceState carries the voice head (consumed_dec_fp).
 * 9 = header v4 carries the device VBL count; the PSYQ latch pack v9 the
 *     critical-section flag, the latched VBLANK and the dropped count.
 * 10 = the CD section carries the CD-ROM controller model (registers,
 *     response FIFO, interrupt queue, VBL count).
 * 11 = the card section (port FLAG bytes, the card each port held).
 * 12 = SPUALVoiceState and the SPU common block hold the envelope and
 *     sweep generators of PsyX_SPUEnvelope.h and a latched key-off. */
#define OPENPETE_STATE_ABI 12u

/* Just the register file - the function-pointer tail of CPUState is
 * runtime-only, must not be saved or restored. */
#define CPU_REG_BYTES ((uint32_t)offsetof(CPUState, read_word))

typedef struct {
    uint32_t magic;
    uint32_t version;
    char     ident[OPENPETE_STATE_HASH_LEN];
    uint32_t frame;
    uint32_t cpu_bytes;
    uint32_t ram_bytes;
    uint32_t scratch_bytes;
    uint32_t vram_bytes;
    uint32_t vbl;
    uint32_t spu_state_bytes;
    uint32_t spu_mem_bytes;
    uint32_t cd_state_bytes;
    uint32_t game_extra_bytes;
    uint32_t modset_tag_bytes;
    uint32_t arena_bytes;      /* mod arena section: ledger rows + used bytes */
    char     game[OPENPETE_STATE_GAME_LEN]; /* saving game's id, NUL-padded */
    uint32_t vbl_device;       /* openpete_vbl_device: every VBLANK elapsed */
    uint32_t park_ra;          /* v5: openpete_yield_site.ra at the save */
    uint32_t park_sp;          /* v5: openpete_yield_site.sp at the save */
    uint32_t consume;          /* v6: openpete_consume (E) at the save */
    uint32_t consume_write;    /* v6: openpete_consume_write (j) at the save */
    uint32_t registry_bytes;   /* v7: mod registry section, after the modset tag */
} openpete_state_header_t;

/* The header's size is fixed per version: the game field replaced
 * reserved[3]; v4 appended vbl_device (104 bytes); v5 appended the park
 * site (112 bytes); v6 the consume position (120 bytes); v7 the registry
 * section's size (124 bytes). */
#define STATE_HEADER_V4_BYTES ((size_t)offsetof(openpete_state_header_t, park_ra))
#define STATE_HEADER_V5_BYTES ((size_t)offsetof(openpete_state_header_t, consume))
#define STATE_HEADER_V6_BYTES ((size_t)offsetof(openpete_state_header_t, registry_bytes))
typedef char state_header_size_check[sizeof(openpete_state_header_t) == 124 ? 1 : -1];
typedef char state_header_v4_size_check[STATE_HEADER_V4_BYTES == 104 ? 1 : -1];
typedef char state_header_v5_size_check[STATE_HEADER_V5_BYTES == 112 ? 1 : -1];
typedef char state_header_v6_size_check[STATE_HEADER_V6_BYTES == 120 ? 1 : -1];

/** @brief Return the byte size of a state header of a version this build reads.
 * @param version The header's version.
 * @return The size; a version outside the readable range returns the v4 size. */
static size_t state_header_bytes(uint32_t version) {
    if (version >= 7u && version <= OPENPETE_STATE_VERSION) return sizeof(openpete_state_header_t);
    if (version == 6u) return STATE_HEADER_V6_BYTES;
    if (version == 5u) return STATE_HEADER_V5_BYTES;
    return STATE_HEADER_V4_BYTES;
}

/** @brief Read a state header of any version this build reads, leaving the file just past it.
 *
 * Reads the v4 prefix, then the fields each later version appended (v5 the
 * park site, v6 the consume position, v7 the registry section's size); a
 * field the version lacks reads as zero. Magic and version are left to the caller's checks; a version
 * outside the readable range reads only the prefix.
 * @param fp The state file, at offset 0.
 * @param hdr Receives the header.
 * @return 0 when the header's bytes were read, -1 on a short read. */
static int state_header_read(FILE* fp, openpete_state_header_t* hdr) {
    memset(hdr, 0, sizeof *hdr);
    if (fread(hdr, 1, STATE_HEADER_V4_BYTES, fp) != STATE_HEADER_V4_BYTES) return -1;
    if (hdr->magic != OPENPETE_STATE_MAGIC || hdr->version < 5u ||
        hdr->version > OPENPETE_STATE_VERSION)
        return 0;
    const size_t tail = state_header_bytes(hdr->version) - STATE_HEADER_V4_BYTES;
    return fread((uint8_t*)hdr + STATE_HEADER_V4_BYTES, 1, tail, fp) == tail ? 0 : -1;
}

/* CD head-position section (fixed layout, ABI-covered). */
typedef struct {
    int32_t cur_sector;
    int32_t sector_size;
    int32_t cur_track;
    int32_t num_frames;
    int32_t cd_com;
    int32_t read_done;
    int32_t xa_start_lba;   /* virtual music-head anchor (LIBCD.C) - the */
    int32_t xa_start_vbl;   /* GetlocP reply is a pure function of these */
    /* The controller registers a game's own drive code polls (core/psx/
     * cdrom_mmio.c): an interrupt queued before the save is raised after
     * the load at the VBL count it was due. */
    openpete_cdrom_mmio_state_t mmio;
} state_cd_t;

/** @brief Capture the CD section: PsyCross's head position and music-head anchor, and the controller model.
 *
 * The file save and the in-memory snapshot both take the section here.
 * @param cd Receives the section; every byte is written. */
static void state_cd_capture(state_cd_t* cd) {
    memset(cd, 0, sizeof *cd);
    cd->cur_sector   = g_cdCurrentSector;
    cd->sector_size  = g_cdSectorSize;
    cd->cur_track    = g_cdCurrentTrack;
    cd->num_frames   = g_cdNumFrames;
    cd->cd_com       = g_CD_com;
    cd->read_done    = g_cdReadDoneFlag;
    cd->xa_start_lba = g_cdXaStartLBA;
    cd->xa_start_vbl = g_cdXaStartVBL;
    openpete_cdrom_mmio_state_save(&cd->mmio);
}

/** @brief Restore the CD section state_cd_capture took.
 *
 * The file load and the in-memory restore both apply the section here.
 * @param cd The section. */
static void state_cd_apply(const state_cd_t* cd) {
    g_cdCurrentSector = cd->cur_sector;
    g_cdSectorSize    = cd->sector_size;
    g_cdCurrentTrack  = cd->cur_track;
    g_cdNumFrames     = cd->num_frames;
    g_CD_com          = cd->cd_com;
    g_cdReadDoneFlag  = cd->read_done;
    g_cdXaStartLBA    = cd->xa_start_lba;
    g_cdXaStartVBL    = cd->xa_start_vbl;
    openpete_cdrom_mmio_state_load(&cd->mmio);
}

/* Card section (fixed layout, ABI-covered, no header field): each card
 * port's FLAG byte, which the card answers to every command, and the card
 * each port held at the save, by its resolved image path. The card image
 * is not carried; a load is refused while no port holds a card the state
 * held, since the state's kernel RAM caches that card's directory. */
#define STATE_CARD_PATH_LEN 1280
typedef struct {
    uint8_t flag[2];
    uint8_t held[2];
    char    path[2][STATE_CARD_PATH_LEN];
} state_card_t;

/** @brief Capture the card section: both ports' FLAG bytes and the card each port holds.
 *
 * @param c Receives the section; every byte is written. */
static void state_card_capture(state_card_t* c) {
    memset(c, 0, sizeof *c);
    openpete_psx_bios_card_flags_get(c->flag);
    for (int p = 0; p < 2; p++)
        c->held[p] = (uint8_t)openpete_memcard_held_path(p, c->path[p], sizeof c->path[p]);
}

/** @brief Refuse a state holding a card that no port of the running session holds.
 *
 * A card is matched by its resolved image path, in either port. The
 * refusal is logged naming the card's path.
 * @param path The state file, for the log line.
 * @param c The state's card section.
 * @return 0 when every card the state held is held now, -1 otherwise. */
static int state_card_check(const char* path, const state_card_t* c) {
    for (int p = 0; p < 2; p++) {
        if (!c->held[p]) continue;
        char want[STATE_CARD_PATH_LEN];
        memcpy(want, c->path[p], sizeof want);
        want[sizeof want - 1] = '\0';
        int found = 0;
        for (int q = 0; q < 2 && !found; q++) {
            char have[STATE_CARD_PATH_LEN];
            found = openpete_memcard_held_path(q, have, sizeof have) && !strcmp(have, want);
        }
        if (found) continue;
        OPL_ERROR_F("savestate", "%s: the state holds the card %s in slot %d, which no slot "
                    "holds; load refused - running state untouched", path,
                    want[0] ? want : "(in-memory card, no file)", p + 1);
        return -1;
    }
    return 0;
}

/* Chunked fwrite/fread. Some Windows stdio configurations short-write
 * multi-megabyte blobs in a single fwrite call (observed on the 8 MiB
 * fiber memory); chunking keeps each underlying _write under the
 * comfortable range and gives us a precise errno when a chunk fails. */
#define IO_CHUNK (64u * 1024u)

/** @brief Write a buffer to a state file in IO_CHUNK pieces.
 *
 * Stops at the first short write and logs the file path, the section name,
 * the byte offset within the section, the sizes and errno. A zero-length
 * buffer writes nothing and succeeds; the buffer is not read then.
 * @param fp Open destination file.
 * @param path File path, for the log line.
 * @param what Section name, for the log line.
 * @param buf Bytes to write.
 * @param n Byte count.
 * @return 0 when all bytes were written, -1 on a short write. */
static int write_section(FILE* fp, const char* path, const char* what,
                         const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t left = n;
    while (left > 0) {
        size_t want = left > IO_CHUNK ? IO_CHUNK : left;
        size_t got  = fwrite(p, 1, want, fp);
        if (got != want) {
            OPL_ERROR_F("savestate",
                        "%s: short write in %s (off=%zu want=%zu got=%zu errno=%d %s)",
                        path, what, n - left, want, got, errno, strerror(errno));
            return -1;
        }
        p += got; left -= got;
    }
    return 0;
}

/** @brief Read a state file section into a buffer in IO_CHUNK pieces.
 *
 * Stops at the first short read and logs the file path, the section name,
 * the byte offset within the section, the sizes and errno; the buffer then
 * holds a partial section. A zero-length read succeeds without touching
 * the buffer.
 * @param fp Open source file.
 * @param path File path, for the log line.
 * @param what Section name, for the log line.
 * @param buf Receives the bytes.
 * @param n Byte count.
 * @return 0 when all bytes were read, -1 on a short read. */
static int read_section(FILE* fp, const char* path, const char* what,
                        void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t left = n;
    while (left > 0) {
        size_t want = left > IO_CHUNK ? IO_CHUNK : left;
        size_t got  = fread(p, 1, want, fp);
        if (got != want) {
            OPL_ERROR_F("savestate",
                        "%s: short read in %s (off=%zu want=%zu got=%zu errno=%d %s)",
                        path, what, n - left, want, got, errno, strerror(errno));
            return -1;
        }
        p += got; left -= got;
    }
    return 0;
}

/** @brief Fill a 32-byte state identity field from the state ABI and the given codegen and ROM stamps.
 *
 * Hashes the string "stateabi:<ABI>|cg:<codegen_stamp>|rom:<rom_sha256>"
 * with SHA-256, where a NULL stamp reads as "?". The field holds the first
 * 15 digest bytes as 30 lowercase hex characters, then zero bytes to 32. A
 * source string longer than the 512-byte buffer is hashed as its first 512
 * bytes, the last being the terminating NUL.
 * @param out Receives OPENPETE_STATE_HASH_LEN bytes.
 * @param cg The codegen stamp, or NULL.
 * @param rom The ROM stamp, or NULL. */
static void ident_fill_terms(char out[OPENPETE_STATE_HASH_LEN], const char* cg, const char* rom) {
    char src[512];
    int n = snprintf(src, sizeof(src), "stateabi:%u|cg:%s|rom:%s",
                     OPENPETE_STATE_ABI, cg ? cg : "?", rom ? rom : "?");
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(src)) n = (int)sizeof(src);

    uint8_t digest[32];
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    openpete_sha256_update(&c, src, (size_t)n);
    openpete_sha256_final(&c, digest);

    memset(out, 0, OPENPETE_STATE_HASH_LEN);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < (OPENPETE_STATE_HASH_LEN - 1) / 2; ++i) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    /* 15 bytes of digest as 30 hex chars + NUL padding to 32. */
}

/** @brief Fill the 32-byte state identity field from the state ABI, the running module's codegen stamp and its ROM hash.
 *
 * A missing recomp module or a missing stamp reads as "?". Both the save
 * header and the load check use it.
 * @param out Receives OPENPETE_STATE_HASH_LEN bytes. */
static void ident_fill(char out[OPENPETE_STATE_HASH_LEN]) {
    const openpete_recomp_module* m = openpete_recomp_module_get();
    ident_fill_terms(out, m ? m->codegen_stamp : NULL, m ? m->rom_sha256 : NULL);
}

/** @brief Format a 32-byte identity field as 64 lowercase hex digits.
 * @param id The field.
 * @param out Receives 65 bytes. */
static void ident_hex(const char id[OPENPETE_STATE_HASH_LEN], char out[2 * OPENPETE_STATE_HASH_LEN + 1]) {
    for (int i = 0; i < OPENPETE_STATE_HASH_LEN; i++)
        snprintf(out + 2 * i, 3, "%02x", (unsigned char)id[i]);
}

int openpete_state_ident_from_terms(const char* cg, const char* rom, char* out, size_t cap) {
    if (cap < 2 * OPENPETE_STATE_HASH_LEN + 1) return -1;
    char id[OPENPETE_STATE_HASH_LEN];
    ident_fill_terms(id, cg, rom);
    ident_hex(id, out);
    return 0;
}

/* A foreign identity the load check admits besides this build's: a state
 * whose identity was recomputed from another module's stamps by the
 * caller (the bug-report replay). Empty when none. */
static char s_admit_ident[OPENPETE_STATE_HASH_LEN];
static int  s_admit_ident_set = 0;

int openpete_state_admit_ident(const char* hex) {
    s_admit_ident_set = 0;
    if (!hex || !*hex) return 0;
    if (strlen(hex) != 2 * OPENPETE_STATE_HASH_LEN) return -1;
    for (int i = 0; i < OPENPETE_STATE_HASH_LEN; i++) {
        unsigned v = 0;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        s_admit_ident[i] = (char)v;
    }
    s_admit_ident_set = 1;
    return 0;
}

/* Card names a load maps to the card a port holds now: a state whose card
 * section names remap_name[p] is checked against port p's held card. */
static char s_card_remap[2][STATE_CARD_PATH_LEN];

void openpete_state_card_remap(const char* port1, const char* port2) {
    snprintf(s_card_remap[0], sizeof s_card_remap[0], "%s", port1 ? port1 : "");
    snprintf(s_card_remap[1], sizeof s_card_remap[1], "%s", port2 ? port2 : "");
}

/** @brief Rewrite a card section path that names a remapped card to the path of the card its port holds now.
 *
 * A path naming no remapped card, or a port holding no card, is left.
 * @param c The card section, rewritten in place.
 * @param log Nonzero to log each rewrite. */
static void state_card_remap_apply(state_card_t* c, int log) {
    for (int p = 0; p < 2; p++) {
        if (!c->held[p]) continue;
        for (int q = 0; q < 2; q++) {
            if (!s_card_remap[q][0] || strncmp(c->path[p], s_card_remap[q], STATE_CARD_PATH_LEN) != 0)
                continue;
            char have[STATE_CARD_PATH_LEN];
            if (!openpete_memcard_held_path(q, have, sizeof have)) break;
            if (log) OPL_INFO_F("savestate", "card %s of slot %d maps to %s", s_card_remap[q], p + 1, have);
            memset(c->path[p], 0, sizeof c->path[p]);
            snprintf(c->path[p], sizeof c->path[p], "%s", have);
            break;
        }
    }
}

/** @brief Fill a header's game field with the running game's id, NUL-padded and truncated to OPENPETE_STATE_GAME_LEN - 1 bytes.
 *
 * @param out Receives OPENPETE_STATE_GAME_LEN bytes; all zero before a game
 *        is registered. */
static void game_fill(char out[OPENPETE_STATE_GAME_LEN]) {
    memset(out, 0, OPENPETE_STATE_GAME_LEN);
    const char* id = game_get_id();
    if (id) strncpy(out, id, OPENPETE_STATE_GAME_LEN - 1);
}

/** @brief Format "<dir>/<name>" with the platform separator.
 *
 * @param dir A directory.
 * @param name A file name inside it.
 * @param out Receives the path.
 * @param out_size Capacity of @p out.
 * @return 0 on success, -1 when the path does not fit. */
static int state_file_path(const char* dir, const char* name, char* out, size_t out_size) {
#ifdef _WIN32
    int n = snprintf(out, out_size, "%s\\%s", dir, name);
#else
    int n = snprintf(out, out_size, "%s/%s", dir, name);
#endif
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
}

/** @brief 1 when path names an existing regular file, else 0. */
static int state_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

/** @brief Decide whether a slot file in the shared states directory belongs to the running game.
 *
 * Applies the loader's game test (state_game_check) to the file's header
 * without logging: a header naming a game belongs to that game, whatever
 * its identity; a header naming none belongs to the running game only when
 * the running module carries a ROM hash and the file's identity equals the
 * module's, the one case in which it would load. A file whose magic or
 * version this build does not read belongs to no game. A file naming no game
 * under a module without a ROM hash is undecided: a later run under a
 * module with one can attribute it.
 * @param path The shared slot file.
 * @param why Receives the reason when the file does not belong.
 * @return 1 when it belongs to the running game, 0 when it does not, -1
 *         when this module cannot tell. */
static int shared_slot_belongs(const char* path, const char** why) {
    openpete_state_header_t hdr;
    FILE* fp = fopen(path, "rb");
    const int got = fp ? state_header_read(fp, &hdr) : -1;
    if (fp) fclose(fp);
    if (got != 0 || hdr.magic != OPENPETE_STATE_MAGIC ||
        hdr.version < OPENPETE_STATE_VERSION_MIN || hdr.version > OPENPETE_STATE_VERSION) {
        *why = "not a state this build reads";
        return 0;
    }
    char want[OPENPETE_STATE_GAME_LEN];
    game_fill(want);
    if (hdr.game[0]) {
        *why = "saved by another game";
        return memcmp(hdr.game, want, OPENPETE_STATE_GAME_LEN) == 0;
    }
    const openpete_recomp_module* m = openpete_recomp_module_get();
    if (!(m && m->rom_sha256 && *m->rom_sha256)) {
        *why = "names no game and this module carries no ROM hash to prove its game";
        return -1;
    }
    char ident[OPENPETE_STATE_HASH_LEN];
    ident_fill(ident);
    *why = "names no game and its identity is another game's or build's";
    return memcmp(hdr.ident, ident, OPENPETE_STATE_HASH_LEN) == 0;
}

/** @brief Report whether the shared states directory holds a slot's file, formatting its path.
 *
 * @param shared The shared states directory.
 * @param slot The slot number.
 * @param out Receives the path, or NULL to only test.
 * @param out_size Capacity of @p out.
 * @return 1 when slot<N>.state exists there as a regular file, else 0. */
static int shared_slot_src(const char* shared, unsigned slot, char* out, size_t out_size) {
    char name[32], path[1280];
    snprintf(name, sizeof name, "slot%u.state", slot);
    if (state_file_path(shared, name, path, sizeof path) != 0 || !state_file_exists(path))
        return 0;
    if (out) snprintf(out, out_size, "%s", path);
    return 1;
}

/** Tallies of one seeding pass over the shared slot files. */
typedef struct {
    unsigned found;   /* slot files present in the shared directory */
    unsigned copied;  /* copied into the game's directory */
    unsigned failed;  /* copies that failed; the pass leaves no marker */
    unsigned undecided; /* files this module cannot attribute; no marker */
} state_seed_counts_t;

/** @brief Copy one shared slot file into the game's directory when it belongs to the running game and the game's slot is empty.
 *
 * The copy goes through a temporary renamed into place, so an interrupted
 * copy leaves no slot file. The shared file is only read. Each file
 * present and not copied is logged with the reason.
 * @param shared The shared states directory.
 * @param game_dir The running game's states directory.
 * @param slot The slot number.
 * @param c Tallies, updated. */
static void seed_one_slot(const char* shared, const char* game_dir, unsigned slot,
                          state_seed_counts_t* c) {
    char name[32], src[1280], dst[1280];
    if (!shared_slot_src(shared, slot, src, sizeof src)) return;
    snprintf(name, sizeof name, "slot%u.state", slot);
    c->found++;
    const char* why = NULL;
    int belongs = shared_slot_belongs(src, &why);
    if (belongs != 1) {
        if (belongs < 0) c->undecided++;
        OPL_INFO_F("savestate", "%s left in place, not copied: %s", src, why);
        return;
    }
    if (state_file_path(game_dir, name, dst, sizeof dst) != 0) {
        c->failed++;
        return;
    }
    if (state_file_exists(dst)) {
        OPL_INFO_F("savestate", "%s left in place, not copied: %s already exists", src, dst);
        return;
    }
    if (op_atomic_copy_file(src, dst) != 0) {
        c->failed++;
        return;
    }
    c->copied++;
    OPL_INFO_F("savestate", "copied %s to %s", src, dst);
}

int openpete_state_seed_game_dir(void) {
    static int s_done;
    if (s_done) return 0;
    s_done = 1;
    const char* game = game_get_id();
    char shared[1024], game_dir[1024], marker[1280];
    if (!game || !*game ||
        openpete_paths_state_dir("states", shared, sizeof shared, 0) != 0 ||
        openpete_paths_game_state_dir("states", game, game_dir, sizeof game_dir, 0) != 0 ||
        state_file_path(game_dir, ".seeded", marker, sizeof marker) != 0)
        return -1;
    if (state_file_exists(marker)) return 0;

    unsigned any = 0;
    for (unsigned slot = 0; slot < OPENPETE_STATE_NUM_SLOTS; slot++)
        any |= (unsigned)shared_slot_src(shared, slot, NULL, 0);
    if (!any) return 0; /* nothing to seed: no marker, no directory */
    state_seed_counts_t c = {0, 0, 0, 0};
    if (openpete_paths_game_state_dir("states", game, game_dir, sizeof game_dir, 1) != 0)
        return -1;
    for (unsigned slot = 0; slot < OPENPETE_STATE_NUM_SLOTS; slot++)
        seed_one_slot(shared, game_dir, slot, &c);
    OPL_INFO_F("savestate", "seeding %s from %s: %u of %u slot files copied, %u failed, "
               "%u undecided; the originals stay in %s", game_dir, shared, c.copied, c.found,
               c.failed, c.undecided, shared);
    if (c.failed) return -1;
    if (c.undecided) return (int)c.copied; /* no marker: a later module may attribute them */
    static const char kMarker[] =
        "Slot files of the shared states directory were copied here once.\n";
    if (op_atomic_write_file(marker, kMarker, sizeof kMarker - 1) != 0) return -1;
    return (int)c.copied;
}

/** @brief Resolve the running game's states directory, creating it when missing.
 *
 * "<states>/<game id>", where <states> is a sibling of memcard.c's "cards"
 * directory under whichever layout openpete_paths_state_dir resolves: the
 * executable's folder for a portable install, the per-user state directory
 * otherwise. The cards stay shared by every game; savestates do not, so one
 * game's slots never list or load another's. The first call in a process
 * seeds the directory from the shared one (openpete_state_seed_game_dir).
 * No trailing separator.
 * @param out Receives the path.
 * @param out_size Capacity of @p out.
 * @return 0 on success, -1 when no base resolves or the path does not
 *         fit. */
static int resolve_state_dir(char* out, size_t out_size) {
    (void)openpete_state_seed_game_dir();
    return openpete_paths_game_state_dir("states", game_get_id(), out, out_size, 1);
}

/** @brief Build the path of a slot's state file, "<states dir>/slot<N>.state".
 *
 * The separator is a backslash on Windows and a slash elsewhere. Logs the
 * reason on failure.
 * @param slot Slot number, below OPENPETE_STATE_NUM_SLOTS.
 * @param out Receives the path.
 * @param out_size Capacity of @p out.
 * @return 0 on success, -1 on an out-of-range slot, an unresolved states
 *         directory or a path that does not fit. */
static int slot_path(unsigned slot, char* out, size_t out_size) {
    if (slot >= OPENPETE_STATE_NUM_SLOTS) {
        OPL_ERROR_F("savestate", "slot %u out of range (0..%u)",
                    slot, OPENPETE_STATE_NUM_SLOTS - 1);
        return -1;
    }
    char dir[1024], name[32];
    if (resolve_state_dir(dir, sizeof(dir)) != 0) return -1;
    snprintf(name, sizeof name, "slot%u.state", slot);
    if (state_file_path(dir, name, out, out_size) != 0) {
        OPL_ERROR_F("savestate", "slot path too long");
        return -1;
    }
    return 0;
}

int openpete_state_slot_present(unsigned slot) {
    char path[1280];
    if (slot_path(slot, path, sizeof(path)) != 0) return 0;
    struct stat st;
    return (stat(path, &st) == 0 && (st.st_mode & S_IFREG)) ? 1 : 0;
}

/** @brief Grow an image buffer to hold a byte count.
 * @param buf The buffer, reallocated.
 * @param cap Its capacity, updated.
 * @param need Bytes needed.
 * @return 0 on success, -1 when allocation failed. */
static int image_reserve(uint8_t** buf, size_t* cap, size_t need) {
    if (need <= *cap) return 0;
    uint8_t* g = (uint8_t*)realloc(*buf, need);
    if (!g) return -1;
    *buf = g;
    *cap = need;
    return 0;
}

int openpete_state_image_capture(uint8_t** buf, size_t* len, size_t* cap,
                                 const CPUState* cpu, unsigned frame) {
    const char* modset_tag = openpete_mods_modset_order_tag();
    if (!modset_tag) modset_tag = "";

    openpete_state_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic         = OPENPETE_STATE_MAGIC;
    hdr.version       = OPENPETE_STATE_VERSION;
    ident_fill(hdr.ident);
    game_fill(hdr.game);
    hdr.frame         = (uint32_t)frame;
    hdr.cpu_bytes     = CPU_REG_BYTES;
    hdr.ram_bytes     = OPENPETE_PSX_RAM_SIZE;
    hdr.scratch_bytes = OPENPETE_PSX_SCRATCH_SIZE;
    hdr.vram_bytes    = PsyX_VRAM_GetSize();
    /* Guest-visible VBL counter (VSync(-1) reads it; Spyro's wait loops
     * compare it against counters stored in the SAVED guest RAM). Without
     * restoring it, the first post-load wait loop sees a wild delta and
     * exits early - a one-VBL timeline skew (measured: post-load hash
     * timeline leads the post-save one by exactly one row). */
    hdr.vbl              = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    hdr.vbl_device       = (uint32_t)SDL_GetAtomicInt(&openpete_vbl_device);
    /* Saves run only at a final yield; the site is the one a load must
     * find the fiber parked at. */
    if (openpete_yield_final) {
        hdr.park_ra = openpete_yield_site.ra;
        hdr.park_sp = openpete_yield_site.sp;
    }
    hdr.consume          = openpete_consume;
    hdr.consume_write    = openpete_consume_write;
    hdr.spu_state_bytes  = PsyX_SPUAL_StateSize();
    hdr.spu_mem_bytes    = PsyX_SPUAL_MemSize();
    hdr.cd_state_bytes   = (uint32_t)sizeof(state_cd_t);
    hdr.game_extra_bytes = game_state_extra_size();
    hdr.modset_tag_bytes = (uint32_t)strlen(modset_tag);
    /* Immutable-row promises are checked once per file save (and once
     * per rewind keyframe), never per speculative tick. */
    openpete_mods_arena_immutable_check("file save");
    hdr.arena_bytes      = openpete_mods_arena_state_size();
    hdr.registry_bytes   = openpete_mods_registry_state_size();

    /* The file layout, in order: header, cpu, ram, scratch, vram, spu,
     * spumem, cd, card, game-extra, arena, modset-tag, registry. */
    const size_t total = sizeof(hdr) + CPU_REG_BYTES + OPENPETE_PSX_RAM_SIZE +
                         OPENPETE_PSX_SCRATCH_SIZE + hdr.vram_bytes + hdr.spu_state_bytes +
                         hdr.spu_mem_bytes + sizeof(state_cd_t) + sizeof(state_card_t) +
                         hdr.game_extra_bytes + hdr.arena_bytes + hdr.modset_tag_bytes +
                         hdr.registry_bytes;
    if (image_reserve(buf, cap, total) != 0) {
        OPL_ERROR_F("savestate", "out of memory for a %zu-byte state image", total);
        return -1;
    }
    uint8_t* p = *buf;
    memcpy(p, &hdr, sizeof hdr);                                  p += sizeof hdr;
    memcpy(p, cpu, CPU_REG_BYTES);                                p += CPU_REG_BYTES;
    memcpy(p, openpete_psx_get_ram(), OPENPETE_PSX_RAM_SIZE);     p += OPENPETE_PSX_RAM_SIZE;
    memcpy(p, openpete_psx_get_scratch(), OPENPETE_PSX_SCRATCH_SIZE); p += OPENPETE_PSX_SCRATCH_SIZE;
    if (PsyX_VRAM_Snapshot(p, hdr.vram_bytes) != 0 ||
        PsyX_SPUAL_SnapshotState(p + hdr.vram_bytes, hdr.spu_state_bytes) != 0 ||
        PsyX_SPUAL_SnapshotMem(p + hdr.vram_bytes + hdr.spu_state_bytes, hdr.spu_mem_bytes) != 0) {
        OPL_ERROR_F("savestate", "host-mirror snapshot failed");
        return -1;
    }
    p += hdr.vram_bytes + hdr.spu_state_bytes + hdr.spu_mem_bytes;
    state_cd_t cd;
    state_cd_capture(&cd);
    memcpy(p, &cd, sizeof cd);                                    p += sizeof cd;
    state_card_t card;
    state_card_capture(&card);
    memcpy(p, &card, sizeof card);                                p += sizeof card;
    if (hdr.game_extra_bytes) game_state_extra_save(p);
    p += hdr.game_extra_bytes;
    if (hdr.arena_bytes) openpete_mods_arena_state_save(p);
    p += hdr.arena_bytes;
    memcpy(p, modset_tag, hdr.modset_tag_bytes);
    p += hdr.modset_tag_bytes;
    if (hdr.registry_bytes) openpete_mods_registry_state_save(p);
    *len = total;
    return 0;
}

int openpete_state_image_sections(const uint8_t* img, size_t len,
                                  openpete_state_section_t* out, int max) {
    if (!img || len < STATE_HEADER_V4_BYTES) return -1;
    openpete_state_header_t h;
    memset(&h, 0, sizeof h);
    memcpy(&h, img, STATE_HEADER_V4_BYTES);
    const size_t hn = state_header_bytes(h.version);
    if (len < hn) return -1;
    memcpy(&h, img, hn);
    const struct { const char* name; uint64_t n; int guest; } sec[] = {
        {"header", hn, 0},                {"cpu", h.cpu_bytes, 0},
        {"ram", h.ram_bytes, 1},          {"scratch", h.scratch_bytes, 1},
        {"vram", h.vram_bytes, 1},        {"spu", h.spu_state_bytes, 1},
        {"spumem", h.spu_mem_bytes, 1},   {"cd", h.cd_state_bytes, 0},
        {"card", sizeof(state_card_t), 0}, {"game-extra", h.game_extra_bytes, 1},
        {"arena", h.arena_bytes, 1},      {"modset-tag", h.modset_tag_bytes, 0},
        {"registry", h.registry_bytes, 0},
    };
    const int nsec = (int)(sizeof sec / sizeof sec[0]);
    uint64_t off = 0;
    for (int i = 0; i < nsec; i++) {
        if (off + sec[i].n > len) return -1;
        if (i < max) out[i] = (openpete_state_section_t){sec[i].name, (size_t)off, (size_t)sec[i].n,
                                                         sec[i].guest};
        off += sec[i].n;
    }
    return off == len ? nsec : -1;
}

int openpete_state_image_card_paths_set(uint8_t* img, size_t len, const char* port1,
                                        const char* port2) {
    openpete_state_section_t s[16];
    const int n = openpete_state_image_sections(img, len, s, 16);
    for (int i = 0; i < n; i++) {
        if (strcmp(s[i].name, "card") != 0) continue;
        state_card_t c;
        memcpy(&c, img + s[i].off, sizeof c);
        const char* want[2] = {port1, port2};
        int changed = 0;
        for (int p = 0; p < 2; p++) {
            if (!c.path[p][0]) continue;
            memset(c.path[p], 0, sizeof c.path[p]);
            snprintf(c.path[p], sizeof c.path[p], "%s", want[p] ? want[p] : "");
            changed++;
        }
        memcpy(img + s[i].off, &c, sizeof c);
        return changed;
    }
    return -1;
}

int openpete_state_image_card_held(const uint8_t* img, size_t len, int held[2]) {
    openpete_state_section_t s[16];
    const int n = openpete_state_image_sections(img, len, s, 16);
    for (int i = 0; i < n; i++) {
        if (strcmp(s[i].name, "card") != 0 || s[i].len != sizeof(state_card_t)) continue;
        state_card_t c;
        memcpy(&c, img + s[i].off, sizeof c);
        held[0] = c.held[0] != 0;
        held[1] = c.held[1] != 0;
        return 0;
    }
    return -1;
}

int openpete_state_image_ident(const uint8_t* img, size_t len, char* out, size_t cap) {
    if (!img || len < sizeof(openpete_state_header_t) || cap < 2 * OPENPETE_STATE_HASH_LEN + 1)
        return -1;
    const openpete_state_header_t* h = (const openpete_state_header_t*)img;
    for (int i = 0; i < OPENPETE_STATE_HASH_LEN; i++)
        snprintf(out + 2 * i, 3, "%02x", (unsigned char)h->ident[i]);
    return 0;
}

int openpete_state_save_file(const char* path, const CPUState* cpu,
                             unsigned frame) {
    uint8_t* img = NULL;
    size_t len = 0, cap = 0;
    if (openpete_state_image_capture(&img, &len, &cap, cpu, frame) != 0) {
        free(img);
        OPL_ERROR_F("savestate", "%s: state capture failed", path);
        return -1;
    }
    op_atomic_file_t af;
    if (op_atomic_file_open(&af, path) != 0) {
        free(img);
        return -1;
    }
    if (write_section(af.f, path, "state", img, len) != 0) {
        free(img);
        op_atomic_file_abort(&af);
        return -1;
    }
    state_card_t card;
    memcpy(&card, img + len - sizeof(state_card_t) -
                  ((const openpete_state_header_t*)img)->game_extra_bytes -
                  ((const openpete_state_header_t*)img)->arena_bytes -
                  ((const openpete_state_header_t*)img)->modset_tag_bytes -
                  ((const openpete_state_header_t*)img)->registry_bytes, sizeof card);
    free(img);
    if (op_atomic_file_commit(&af) != 0) return -1;
    OPL_INFO_F("savestate", "saved %s (frame=%u) cards held %u/%u flags 0x%02X/0x%02X",
               path, frame, card.held[0], card.held[1], card.flag[0], card.flag[1]);
    return 0;
}

int openpete_state_save(unsigned slot, const CPUState* cpu, unsigned frame) {
    char path[1280];
    if (slot_path(slot, path, sizeof(path)) != 0) return -1;
    return openpete_state_save_file(path, cpu, frame);
}

/** @brief Check that the sections a state header declares add up to the bytes the file holds after the header.
 *
 * The fixed sections were already matched against this build; the four
 * variable sections (game extra, arena, modset tag, registry) are sized only by the
 * header, so a corrupt or hostile header could otherwise ask for a
 * multi-gigabyte allocation before any read fails. The sum is formed in
 * 64 bits, so no declared size can wrap it.
 * @param fp The state file, positioned just past the header.
 * @param path Its path, for the log line.
 * @param hdr The header read from it.
 * @return 0 when the declared sections fill the rest of the file exactly, -1 otherwise. */
static int state_sections_fit(FILE* fp, const char* path, const openpete_state_header_t* hdr) {
    long pos = ftell(fp);
    long end = (pos >= 0 && fseek(fp, 0, SEEK_END) == 0) ? ftell(fp) : -1;
    if (end < pos || fseek(fp, pos, SEEK_SET) != 0) {
        OPL_ERROR_F("savestate", "%s: cannot measure the file", path);
        return -1;
    }
    uint64_t want = (uint64_t)hdr->cpu_bytes + hdr->ram_bytes + hdr->scratch_bytes +
                    hdr->vram_bytes + hdr->spu_state_bytes + hdr->spu_mem_bytes +
                    hdr->cd_state_bytes + sizeof(state_card_t) + hdr->game_extra_bytes +
                    hdr->arena_bytes + hdr->modset_tag_bytes + hdr->registry_bytes;
    if (want == (uint64_t)(end - pos)) return 0;
    OPL_ERROR_F("savestate", "%s: header declares %llu section bytes, the file holds %ld "
                "(game-extra=%u arena=%u modset-tag=%u registry=%u)", path,
                (unsigned long long)want, end - pos, hdr->game_extra_bytes, hdr->arena_bytes,
                hdr->modset_tag_bytes, hdr->registry_bytes);
    return -1;
}

/** @brief Refuse a state saved by another game, or one naming no game whose identity does not prove the game.
 *
 * A header naming a game must name the running one. A header naming none
 * was written before the field existed; its identity check has already
 * passed, and that proves the same disc only when the running module
 * carries a ROM hash (a boot-compiled module), so it loads then and is
 * refused otherwise (a CMake-built module's identity is the same for every
 * game). Each refusal is logged naming both games.
 * @param path The state file, for the log line.
 * @param hdr The header read from it.
 * @return 0 when the state belongs to the running game, -1 otherwise. */
static int state_game_check(const char* path, const openpete_state_header_t* hdr) {
    char want[OPENPETE_STATE_GAME_LEN];
    game_fill(want);
    char have[OPENPETE_STATE_GAME_LEN + 1];
    memcpy(have, hdr->game, OPENPETE_STATE_GAME_LEN);
    have[OPENPETE_STATE_GAME_LEN] = '\0';
    if (have[0]) {
        if (!strncmp(have, want, OPENPETE_STATE_GAME_LEN)) return 0;
        OPL_ERROR_F("savestate", "%s: state saved by %s, running %s (%s); load refused "
                    "- running state untouched", path, have, want, game_get_display_name());
        return -1;
    }
    const openpete_recomp_module* m = openpete_recomp_module_get();
    if (m && m->rom_sha256 && *m->rom_sha256) return 0;
    OPL_ERROR_F("savestate", "%s: state names no game and this module's identity carries "
                "no ROM hash to prove it is %s (%s); load refused - running state untouched",
                path, want, game_get_display_name());
    return -1;
}

/** @brief Restore the bk2 row position (E, j) a state header carries.
 *
 * A header before v6 carries none; the live position stays.
 * @param hdr The loaded header. */
static void state_consume_restore(const openpete_state_header_t* hdr) {
    if (hdr->version < 6u) return;
    openpete_consume      = hdr->consume;
    openpete_consume_write = hdr->consume_write;
}

/** @brief Admit a state file's park site: the fiber must be parked at a final yield of the psyq_VSync call the state was saved in.
 *
 * A header with no park site (v4) is admitted at any final yield, with a
 * warning naming the site it loads at.
 * @param path The state file, for the log line.
 * @param hdr The header read from it.
 * @return 0 when the load may apply now, 1 when the fiber is parked elsewhere. */
static int state_site_check(const char* path, const openpete_state_header_t* hdr) {
    const openpete_park_site_t site = {hdr->park_ra, hdr->park_sp};
    if (!openpete_park_site_matches(&site)) return 1;
    if (site.ra == 0u)
        OPL_WARN_F("savestate", "%s: v%u state carries no park site; loaded at the current "
                   "final yield (ra=0x%08X sp=0x%08X) unchecked", path, hdr->version,
                   openpete_yield_site.ra, openpete_yield_site.sp);
    return 0;
}

/** @brief Check a state file's header against this build: magic, version, game, identity, the fixed section sizes, and the variable sections against the file's length.
 *
 * Each refusal is logged with the values that differ. The park site is
 * not checked here: it is the one verdict that changes while a load waits.
 * @param fp The state file, positioned just past the header.
 * @param path Its path, for the log lines.
 * @param hdr The header read from it.
 * @return 0 when this build can read the file, -1 otherwise. */
static int state_header_check(FILE* fp, const char* path, const openpete_state_header_t* hdr) {
    if (hdr->magic != OPENPETE_STATE_MAGIC) {
        OPL_ERROR_F("savestate", "%s: bad magic 0x%08X (expected 0x%08X)",
                    path, hdr->magic, OPENPETE_STATE_MAGIC);
        return -1;
    }
    if (hdr->version < OPENPETE_STATE_VERSION_MIN || hdr->version > OPENPETE_STATE_VERSION) {
        OPL_ERROR_F("savestate", "%s: version %u not supported (expected %u..%u)",
                    path, hdr->version, OPENPETE_STATE_VERSION_MIN, OPENPETE_STATE_VERSION);
        return -1;
    }
    /* Before the identity, so a state from another game is reported as
     * that rather than as an identity mismatch. */
    if (state_game_check(path, hdr) != 0) return -1;

    char want_ident[OPENPETE_STATE_HASH_LEN];
    ident_fill(want_ident);
    if (s_admit_ident_set && memcmp(hdr->ident, want_ident, OPENPETE_STATE_HASH_LEN) != 0 &&
        memcmp(hdr->ident, s_admit_ident, OPENPETE_STATE_HASH_LEN) == 0) {
        OPL_WARN_F("savestate", "%s: identity of another module admitted (recomputed from its "
                   "stamps by the caller)", path);
    } else if (memcmp(hdr->ident, want_ident, OPENPETE_STATE_HASH_LEN) != 0) {
        char hh[OPENPETE_STATE_HASH_LEN + 1];
        char wh[OPENPETE_STATE_HASH_LEN + 1];
        memcpy(hh, hdr->ident,  OPENPETE_STATE_HASH_LEN); hh[OPENPETE_STATE_HASH_LEN] = '\0';
        memcpy(wh, want_ident, OPENPETE_STATE_HASH_LEN); wh[OPENPETE_STATE_HASH_LEN] = '\0';
        OPL_ERROR_F("savestate",
                    "%s: identity mismatch (file=%s, this build=%s) - "
                    "state was made against a different ROM / recomp module "
                    "/ state ABI",
                    path, hh, wh);
        return -1;
    }

    if (hdr->cpu_bytes != CPU_REG_BYTES ||
        hdr->ram_bytes != OPENPETE_PSX_RAM_SIZE ||
        hdr->scratch_bytes != OPENPETE_PSX_SCRATCH_SIZE ||
        hdr->vram_bytes != PsyX_VRAM_GetSize() ||
        hdr->spu_state_bytes != PsyX_SPUAL_StateSize() ||
        hdr->spu_mem_bytes != PsyX_SPUAL_MemSize() ||
        hdr->cd_state_bytes != (uint32_t)sizeof(state_cd_t)) {
        OPL_ERROR_F("savestate",
                    "%s: section sizes mismatch (cpu=%u/%u ram=%u/%u "
                    "scratch=%u/%u vram=%u/%u spu=%u/%u spumem=%u/%u cd=%u/%u)",
                    path,
                    hdr->cpu_bytes, CPU_REG_BYTES,
                    hdr->ram_bytes, OPENPETE_PSX_RAM_SIZE,
                    hdr->scratch_bytes, OPENPETE_PSX_SCRATCH_SIZE,
                    hdr->vram_bytes, PsyX_VRAM_GetSize(),
                    hdr->spu_state_bytes, PsyX_SPUAL_StateSize(),
                    hdr->spu_mem_bytes, PsyX_SPUAL_MemSize(),
                    hdr->cd_state_bytes, (uint32_t)sizeof(state_cd_t));
        return -1;
    }
    return state_sections_fit(fp, path, hdr);
}

/** @brief Read a state file's arena, modset-tag and registry sections, the last three in the file.
 *
 * Seeks past every earlier section from the end of the header, so a load
 * judges the mod sections without reading the guest memory images.
 * @param fp The state file.
 * @param path Its path, for the log lines.
 * @param body Offset of the first section (the end of the header).
 * @param hdr The header, already checked against the file's length.
 * @param arena_out Receives the arena section (NULL when it is empty); the caller frees it.
 * @param tag_out Receives the NUL-terminated modset tag; the caller frees it.
 * @param reg_out Receives the registry section (NULL when it is empty); the caller frees it.
 * @return 0 when the sections were read, -1 otherwise (logged; nothing to free). */
static int state_mod_sections_read(FILE* fp, const char* path, long body,
                                   const openpete_state_header_t* hdr,
                                   uint8_t** arena_out, char** tag_out, uint8_t** reg_out) {
    const uint64_t skip = (uint64_t)hdr->cpu_bytes + hdr->ram_bytes + hdr->scratch_bytes +
                          hdr->vram_bytes + hdr->spu_state_bytes + hdr->spu_mem_bytes +
                          hdr->cd_state_bytes + sizeof(state_card_t) +
                          hdr->game_extra_bytes;
    uint8_t* arena = hdr->arena_bytes ? (uint8_t*)malloc(hdr->arena_bytes) : NULL;
    char*    tag   = (char*)malloc((size_t)hdr->modset_tag_bytes + 1);
    uint8_t* reg   = hdr->registry_bytes ? (uint8_t*)malloc(hdr->registry_bytes) : NULL;
    int rc = (!tag || (hdr->arena_bytes && !arena) || (hdr->registry_bytes && !reg)) ? -1 : 0;
    if (rc != 0) OPL_ERROR_F("savestate", "%s: out of memory", path);
    if (rc == 0 && fseek(fp, body + (long)skip, SEEK_SET) != 0) {
        OPL_ERROR_F("savestate", "%s: cannot seek to the arena section", path);
        rc = -1;
    }
    if (rc == 0 && hdr->arena_bytes)
        rc = read_section(fp, path, "arena", arena, hdr->arena_bytes);
    if (rc == 0 && hdr->modset_tag_bytes)
        rc = read_section(fp, path, "modset-tag", tag, hdr->modset_tag_bytes);
    if (rc == 0 && hdr->registry_bytes)
        rc = read_section(fp, path, "registry", reg, hdr->registry_bytes);
    if (rc != 0) {
        free(arena); free(tag); free(reg);
        return -1;
    }
    tag[hdr->modset_tag_bytes] = '\0';
    *arena_out = arena;
    *tag_out = tag;
    *reg_out = reg;
    return 0;
}

/** @brief Read a state file's card section, the one after the CD section.
 *
 * Seeks past the earlier sections from the end of the header, so a load
 * judges the card without reading the guest memory images.
 * @param fp The state file.
 * @param path Its path, for the log lines.
 * @param body Offset of the first section (the end of the header).
 * @param hdr The header, already checked against the file's length.
 * @param c Receives the section.
 * @return 0 when the section was read, -1 otherwise (logged). */
static int state_card_read(FILE* fp, const char* path, long body,
                           const openpete_state_header_t* hdr, state_card_t* c) {
    const uint64_t skip = (uint64_t)hdr->cpu_bytes + hdr->ram_bytes + hdr->scratch_bytes +
                          hdr->vram_bytes + hdr->spu_state_bytes + hdr->spu_mem_bytes +
                          hdr->cd_state_bytes;
    if (fseek(fp, body + (long)skip, SEEK_SET) != 0) {
        OPL_ERROR_F("savestate", "%s: cannot seek to the card section", path);
        return -1;
    }
    return read_section(fp, path, "card", c, sizeof *c);
}

/** @brief Read a state file's card section and refuse the state when no slot holds a card it held.
 *
 * @param fp The state file.
 * @param path Its path, for the log lines.
 * @param body Offset of the first section (the end of the header).
 * @param hdr The header, already checked against the file's length.
 * @param c Receives the section.
 * @return 0 when the state may load, -1 when the section cannot be read or the card is not held (logged). */
static int state_card_admit(FILE* fp, const char* path, long body,
                            const openpete_state_header_t* hdr, state_card_t* c) {
    if (state_card_read(fp, path, body, hdr, c) != 0) return -1;
    state_card_remap_apply(c, 1);
    return state_card_check(path, c);
}

/** @brief Refuse a state saved under another modset, naming whether the mods or only their order differ.
 *
 * Guest RAM and the arena carry mod-parked payloads and pointers into
 * them, and the order the mods entered and registered their overrides in
 * decides which mod's code runs first on every tick, so a file saved under another mutative
 * modset, or the same one in another order, restores a world the running
 * mods did not build. A file whose tag is in the set form (written before
 * the ordered form) is compared as a set and its order is not checked.
 * @param path The state file, for the log lines.
 * @param tag The modset tag the state was saved under.
 * @return 0 when the modset admits the file, -1 otherwise. */
static int state_modset_verdict(const char* path, const char* tag) {
    const char* now_order = openpete_mods_modset_order_tag();
    const char* now_set = openpete_mods_modset_tag();
    switch (openpete_modset_compare(tag, now_order, now_set)) {
    case OPENPETE_MODSET_MATCH:
        return 0;
    case OPENPETE_MODSET_SET_MATCH:
        if (tag[0])
            OPL_INFO_F("savestate", "%s: modset tag [%s] records no order; "
                       "compared as a set", path, tag);
        return 0;
    case OPENPETE_MODSET_ORDER_DIFFERS:
        OPL_ERROR_F("savestate",
                    "%s: MODSET MISMATCH (order) - state saved under [%s], "
                    "running [%s]: the same mods, entered or hooked in "
                    "another order (the hash after '#' differs); load "
                    "refused - running state untouched. Recreate the "
                    "enabled.txt order and the mods enabled or "
                    "hot-reloaded during the session the state was saved "
                    "in.",
                    path, tag, now_order);
        return -1;
    case OPENPETE_MODSET_SET_DIFFERS:
    default:
        OPL_ERROR_F("savestate",
                    "%s: MODSET MISMATCH (mods) - state saved under [%s], "
                    "running [%s]; load refused - running state "
                    "untouched. Enable the same mods (and versions) "
                    "the state was saved with.",
                    path, tag, now_order);
        return -1;
    }
}

/** @brief Refuse a state the running mods cannot restore: another modset, or an arena ledger that is not a prefix match of the live one.
 *
 * Both verdicts read only the file and the session's mod state, so they
 * run before the park-site wait: a file they refuse is refused at once
 * instead of after the wait. The modset verdict runs first, since a ledger
 * written by other mods or another order fails as a consequence of it.
 * The live ledger only grows and its rows only gain superseded marks, so
 * a refusal here is final; a pass is re-judged at the attempt that
 * commits. Each refusal is logged.
 * @param path The state file, for the log lines.
 * @param arena The arena section, NULL when the file has none.
 * @param arena_bytes Its size.
 * @param tag The modset tag the state was saved under.
 * @return 0 when the mod state admits the file, -1 otherwise. */
static int state_mod_verdict(const char* path, const uint8_t* arena, uint32_t arena_bytes,
                             const char* tag) {
    if (state_modset_verdict(path, tag) != 0) return -1;
    if (openpete_mods_arena_state_verdict(arena, arena_bytes) != 0) {
        OPL_ERROR_F("savestate", "%s: load refused - running state untouched", path);
        return -1;
    }
    return 0;
}

/** @brief Refuse a state load, with a logged error, while the run follows the reference's schedule.
 *
 * Under the reference's schedule (a --psxtrace-verify run with sidecars)
 * the thunk layer's position in the schedule's rows (VSync, read, seed,
 * CdSync, controller and interrupt cursors, the reference VBLANK reached)
 * is host state no state file carries; a load would replay from a RAM
 * image the schedule has moved past and desync every row after it.
 * @param what Name of the refused load, for the log line.
 * @return 1 when the load is refused, 0 when no schedule is active. */
static int sched_refuses_load(const char* what) {
    if (!openpete_sched_active()) return 0;
    OPL_ERROR_F("savestate", "%s refused: the run is under the reference's "
                "schedule, whose position no state carries", what);
    return 1;
}

int openpete_state_load_file(const char* path, CPUState* cpu,
                             unsigned* out_frame) OP_REQUIRES(sim) {
    if (sched_refuses_load(path)) return -1;
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        OPL_ERROR_F("savestate", "fopen %s for read failed: %s",
                    path, strerror(errno));
        return -1;
    }

    openpete_state_header_t hdr;
    if (state_header_read(fp, &hdr) != 0) {
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: short read on header", path);
        return -1;
    }

    /* Every verdict that reads only the file and the session runs before
     * the park-site check, so a file that can never load is refused at
     * the first attempt rather than after the site wait. */
    const long body = ftell(fp);
    if (body < 0 || state_header_check(fp, path, &hdr) != 0) {
        fclose(fp);
        return -1;
    }
    uint8_t* arena_buf = NULL;
    char*    tag_buf   = NULL;
    uint8_t* reg_buf   = NULL;
    if (state_mod_sections_read(fp, path, body, &hdr, &arena_buf, &tag_buf, &reg_buf) != 0) {
        fclose(fp);
        OPL_ERROR_F("savestate", "%s: load refused - running state untouched", path);
        return -1;
    }
    state_card_t card;
    int hrc = state_mod_verdict(path, arena_buf, hdr.arena_bytes, tag_buf);
    if (hrc == 0) hrc = state_card_admit(fp, path, body, &hdr, &card);
    if (hrc == 0) hrc = state_site_check(path, &hdr);
    if (hrc == 0 && fseek(fp, body, SEEK_SET) != 0) {
        OPL_ERROR_F("savestate", "%s: cannot seek to the cpu section", path);
        hrc = -1;
    }
    if (hrc != 0) {
        free(arena_buf); free(tag_buf); free(reg_buf);
        fclose(fp);
        return hrc;
    }

    /* Read into temp buffers first so a short read doesn't leave any
     * region half-overwritten. */
    uint8_t  cpu_buf[CPU_REG_BYTES];
    uint8_t* ram_buf    = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE);
    uint8_t* vram_buf   = (uint8_t*)malloc(hdr.vram_bytes);
    uint8_t* spu_buf    = (uint8_t*)malloc(hdr.spu_state_bytes);
    uint8_t* spumem_buf = (uint8_t*)malloc(hdr.spu_mem_bytes);
    uint8_t* extra_buf  = hdr.game_extra_bytes ? (uint8_t*)malloc(hdr.game_extra_bytes) : NULL;
    uint8_t  scratch_buf[OPENPETE_PSX_SCRATCH_SIZE];
    state_cd_t cd;

    int rc = 0;
    if (!ram_buf || !vram_buf || !spu_buf || !spumem_buf ||
        (hdr.game_extra_bytes && !extra_buf)) {
        OPL_ERROR_F("savestate", "%s: out of memory", path);
        rc = -1;
    }
    /* The file layout up to the mod sections, in order; a zero-length
     * section reads nothing. */
    const struct { const char* what; void* buf; size_t n; } sections[] = {
        { "cpu",        cpu_buf,     CPU_REG_BYTES },
        { "ram",        ram_buf,     OPENPETE_PSX_RAM_SIZE },
        { "scratch",    scratch_buf, OPENPETE_PSX_SCRATCH_SIZE },
        { "vram",       vram_buf,    hdr.vram_bytes },
        { "spu",        spu_buf,     hdr.spu_state_bytes },
        { "spumem",     spumem_buf,  hdr.spu_mem_bytes },
        { "cd",         &cd,         sizeof(cd) },
        { "card",       &card,       sizeof(card) },
        { "game-extra", extra_buf,   hdr.game_extra_bytes },
    };
    for (size_t i = 0; rc == 0 && i < sizeof sections / sizeof sections[0]; i++)
        rc = read_section(fp, path, sections[i].what, sections[i].buf, sections[i].n);
    fclose(fp);
    /* The section was read again with the rest; the modsave rebind below
     * reads the remapped paths the admission checked. */
    if (rc == 0) state_card_remap_apply(&card, 0);

    if (rc != 0) {
        free(ram_buf); free(vram_buf); free(spu_buf); free(spumem_buf);
        free(extra_buf); free(arena_buf); free(tag_buf); free(reg_buf);
        OPL_ERROR_F("savestate", "%s: load refused - running state untouched",
                    path);
        return -1;
    }

    /* Commit. Only the register file is overwritten on cpu - the
     * function-pointer tail (read_word/write_word/...) keeps its host
     * bindings from openpete_psx_runtime_init. */
    memcpy(cpu,                        cpu_buf,     CPU_REG_BYTES);
    memcpy(openpete_psx_get_ram(),     ram_buf,     OPENPETE_PSX_RAM_SIZE);
    openpete_guest_generation_bump();   /* guest RAM replaced */
    /* A state written before the boot seeded the kernel tables lacks them;
     * a state that carries them keeps the kernel RAM it saved. */
    openpete_psx_bios_reseed_ram(openpete_psx_get_ram());
    memcpy(openpete_psx_get_scratch(), scratch_buf, OPENPETE_PSX_SCRATCH_SIZE);
    (void)PsyX_VRAM_Restore(vram_buf, hdr.vram_bytes);
    (void)PsyX_SPUAL_RestoreMem(spumem_buf, hdr.spu_mem_bytes);
    (void)PsyX_SPUAL_RestoreState(spu_buf, hdr.spu_state_bytes);
    state_cd_apply(&cd);
    uint8_t live_flag[2];
    openpete_psx_bios_card_flags_get(live_flag);
    openpete_psx_bios_card_flags_set(card.flag);
    free(ram_buf); free(vram_buf); free(spu_buf); free(spumem_buf); free(tag_buf);

    /* Re-anchor the guest-visible VBL counter to the saved value (see the
     * save-side comment): without this the first post-load wait loop sees
     * a wild delta and the timeline leads by one VBL. */
    SDL_SetAtomicInt(&openpete_vbl, (int)hdr.vbl);
    SDL_SetAtomicInt(&openpete_vbl_device, (int)hdr.vbl_device);
    state_consume_restore(&hdr);

    /* Per-game post-load resync - restores any host-side state that
     * mirrors RAM (e.g. Spyro's active-overlay dispatcher pointer). */
    game_on_state_loaded(cpu);

    /* Game-side extra host state (e.g. Spyro's WAD-overlay append cursor).
     * The impl validates its own version/size and ignores foreign blobs. */
    if (hdr.game_extra_bytes)
        game_state_extra_load(extra_buf, hdr.game_extra_bytes);
    else
        openpete_modsave_block_absent();
    free(extra_buf);
    {
        const char* held[2] = { card.held[0] ? card.path[0] : "", card.held[1] ? card.path[1] : "" };
        openpete_modsave_rebind(held);
    }

    /* Arena apply (verdict passed pre-commit): restore the arena bytes
     * and re-anchor both cursors before anything downstream - a mod's
     * next tick hook may re-park against the restored layout. */
    openpete_mods_arena_state_apply(arena_buf, hdr.arena_bytes);
    free(arena_buf);
    /* The custom-level registry's host state describes the guest RAM just
     * restored (the shared slot's claim, the replace declarations); a
     * state without the section leaves it as it is. */
    openpete_mods_registry_state_apply(reg_buf, hdr.registry_bytes);
    free(reg_buf);

    /* Broadcast the discontinuity LAST, once engine + game + mod state is
     * fully committed: host caches derived from guest RAM (interp prev/curr
     * tables, texture-pack registry, bk2 recording, ...) resync here so
     * nothing carries a pre-load snapshot across the jump. */
    openpete_guest_discontinuity_notify();

    if (out_frame) *out_frame = hdr.frame;
    OPL_INFO_F("savestate", "loaded %s (frame=%u) cards held %u/%u flags 0x%02X/0x%02X "
               "-> 0x%02X/0x%02X", path, hdr.frame, card.held[0], card.held[1],
               live_flag[0], live_flag[1], card.flag[0], card.flag[1]);
    return 0;
}

int openpete_state_load(unsigned slot, CPUState* cpu,
                        unsigned* out_frame) OP_REQUIRES(sim) {
    char path[1280];
    if (slot_path(slot, path, sizeof(path)) != 0) return -1;
    return openpete_state_load_file(path, cpu, out_frame);
}

int openpete_state_resolve_dir(char* out, unsigned long out_size) {
    return resolve_state_dir(out, (size_t)out_size);
}

/* ── In-memory snapshot (see openpete_state.h for the contract) ──────── */

struct openpete_state_mem {
    int        valid;
    unsigned   frame;
    openpete_park_site_t site;  /* the park site at the save */
    uint32_t   vbl;
    uint32_t   vbl_device;
    uint32_t   consume;
    uint32_t   consume_write;
    uint8_t    cpu_buf[CPU_REG_BYTES];
    uint8_t    scratch_buf[OPENPETE_PSX_SCRATCH_SIZE];
    uint8_t*   ram;        /* 2 MiB */
    uint8_t*   spu;        /* PsyX_SPUAL_StateSize() */
    uint32_t   spu_bytes;
    state_cd_t cd;
    uint8_t    card_flag[2];  /* card port FLAG bytes; the card is never swapped mid-session */
    /* CD transport (command queue + drain-mode flags) and the game's
     * extra host-state blob. A speculative/rewound tick can run
     * load-transition code - CD commands enqueued, the WAD cursor moved,
     * the overlay dispatcher flipped - and restoring guest RAM without
     * these leaves the host machinery on the abandoned timeline: phantom
     * queued commands detonate the next CdSync drain, and a flipped
     * dispatcher routes overlay calls into the wrong level's functions.
     * cdq holds host pointers -> in-memory snapshots only, never disk. */
    uint8_t*   cdq;        /* PsyX_CD_QueueStateSize() */
    uint32_t   cdq_bytes;
    uint8_t*   gx;         /* game_state_extra_size() at the save (may be 0) */
    uint32_t   gx_bytes;
    uint32_t   gx_cap;     /* grow-only: the save-record trailer grows while a card write is pending */
    /* VRAM: a speculative tick that crosses into a transition uploads
     * fade/loading graphics (LoadImage) into VRAM, and the committed
     * entry then reads VRAM back into gameplay RAM (the framebuffer-
     * capture StoreImage the crossfade uses) - so VRAM is gameplay-
     * READABLE state at transitions and must roll back with guest RAM.
     * Restored via the row-delta path: a tick whose speculation never
     * touched VRAM costs one compare, no GPU traffic. */
    uint8_t*   vram;       /* PsyX_VRAM_GetSize() */
    uint32_t   vram_bytes;
    /* Mod arena: its three used ranges (upper and lower DMA-reachable
     * segment, general pool) back to back. The cursors only grow, so the
     * buffer is realloc'd grow-only at save time (the arena itself never
     * moves - only this snapshot copy). */
    uint8_t*   arena;
    uint32_t   arena_cap;
    uint32_t   arena_used[OPENPETE_ARENA_RANGES];
    int        arena_external;  /* the owner carries the arena itself */
    /* Mod registry section (openpete_mods_registry_state_save); grow-only. */
    uint8_t*   reg;
    uint32_t   reg_bytes;
    uint32_t   reg_cap;
};

openpete_state_mem_t* openpete_state_mem_new(void) {
    openpete_state_mem_t* m =
        (openpete_state_mem_t*)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->spu_bytes  = PsyX_SPUAL_StateSize();
    m->cdq_bytes  = (uint32_t)PsyX_CD_QueueStateSize();
    m->gx_bytes   = game_state_extra_size();
    m->gx_cap     = m->gx_bytes;
    m->vram_bytes = PsyX_VRAM_GetSize();
    m->ram  = (uint8_t*)malloc(OPENPETE_PSX_RAM_SIZE);
    m->spu  = (uint8_t*)malloc(m->spu_bytes);
    m->cdq  = (uint8_t*)malloc(m->cdq_bytes);
    m->gx   = m->gx_bytes ? (uint8_t*)malloc(m->gx_bytes) : NULL;
    m->vram = (uint8_t*)malloc(m->vram_bytes);
    if (!m->ram || !m->spu || !m->cdq || (m->gx_bytes && !m->gx) || !m->vram) {
        openpete_state_mem_free(m);
        return NULL;
    }
    return m;
}

void openpete_state_mem_free(openpete_state_mem_t* m) {
    if (!m) return;
    free(m->ram);
    free(m->spu);
    free(m->cdq);
    free(m->gx);
    free(m->vram);
    free(m->arena);
    free(m->reg);
    free(m);
}

int openpete_state_mem_save(openpete_state_mem_t* m, const CPUState* cpu,
                            unsigned frame) {
    if (!m) return -1;
    m->frame = frame;
    m->site  = openpete_yield_final ? openpete_yield_site : (openpete_park_site_t){0u, 0u};
    m->vbl   = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    m->vbl_device = (uint32_t)SDL_GetAtomicInt(&openpete_vbl_device);
    m->consume    = openpete_consume;
    m->consume_write = openpete_consume_write;
    memcpy(m->cpu_buf,     cpu,                        CPU_REG_BYTES);
    memcpy(m->ram,         openpete_psx_get_ram(),     OPENPETE_PSX_RAM_SIZE);
    memcpy(m->scratch_buf, openpete_psx_get_scratch(), OPENPETE_PSX_SCRATCH_SIZE);
    if (PsyX_SPUAL_SnapshotState(m->spu, m->spu_bytes) != 0) return -1;
    state_cd_capture(&m->cd);
    openpete_psx_bios_card_flags_get(m->card_flag);
    PsyX_CD_QueueSnapshot(m->cdq);
    m->gx_bytes = game_state_extra_size();
    if (m->gx_bytes > m->gx_cap) {
        uint8_t* grown = (uint8_t*)realloc(m->gx, m->gx_bytes);
        if (!grown) return -1;
        m->gx = grown;
        m->gx_cap = m->gx_bytes;
    }
    if (m->gx_bytes) game_state_extra_save(m->gx);
    if (PsyX_VRAM_Snapshot(m->vram, m->vram_bytes) != 0) return -1;
    if (!m->arena_external) {
        uint32_t used = openpete_mods_arena_used(NULL);
        if (used > m->arena_cap) {
            uint8_t* grown = (uint8_t*)realloc(m->arena, used);
            if (!grown) return -1;
            m->arena = grown;
            m->arena_cap = used;
        }
        openpete_mods_arena_mem_save(m->arena, m->arena_used);
    }
    m->reg_bytes = openpete_mods_registry_state_size();
    if (m->reg_bytes > m->reg_cap) {
        uint8_t* grown = (uint8_t*)realloc(m->reg, m->reg_bytes);
        if (!grown) return -1;
        m->reg = grown;
        m->reg_cap = m->reg_bytes;
    }
    if (m->reg_bytes) openpete_mods_registry_state_save(m->reg);
    m->valid = 1;
    return 0;
}

/** @brief Fault injection: OPENPETE_STATE_FAULT_SPU_RESTORE=card fails the SPU step of the first in-memory restore made while a speculative card write is pending, once.
 *
 * Exercises the partial restore (CPU, RAM and scratchpad replaced, the
 * rest not) that a real SPU restore failure leaves.
 * @return 1 on the faulted restore, else 0. */
static int state_fault_spu_restore(void) {
    static int s_armed = -1;
    if (s_armed < 0) {
        const char* e = getenv("OPENPETE_STATE_FAULT_SPU_RESTORE");
        s_armed = e && !strcmp(e, "card") ? 1 : 0;
    }
    if (!s_armed || !openpete_memcard_spec_pending()) return 0;
    s_armed = 0;
    OPL_WARN_F("savestate", "fault injected: SPU restore failed after CPU, RAM and scratchpad");
    return 1;
}

int openpete_state_mem_load(const openpete_state_mem_t* m, CPUState* cpu,
                            unsigned* out_frame, int quiet) OP_REQUIRES(sim) {
    if (!m || !m->valid) return -1;
    if (sched_refuses_load("in-memory state load")) return -1;
    if (!openpete_park_site_matches(&m->site)) return 1;
    memcpy(cpu,                        m->cpu_buf,     CPU_REG_BYTES);
    memcpy(openpete_psx_get_ram(),     m->ram,         OPENPETE_PSX_RAM_SIZE);
    memcpy(openpete_psx_get_scratch(), m->scratch_buf, OPENPETE_PSX_SCRATCH_SIZE);
    openpete_guest_generation_bump();   /* guest RAM replaced, quiet rollbacks included */
    if (PsyX_SPUAL_RestoreStateEx(m->spu, m->spu_bytes, quiet ? 1 : 0) != 0 ||
        state_fault_spu_restore())
        return -2;
    state_cd_apply(&m->cd);
    openpete_psx_bios_card_flags_set(m->card_flag);
    PsyX_CD_QueueRestore(m->cdq);
    /* Runs on the QUIET path too (runahead's per-tick rollback): a
     * speculative tick that crossed into a load moved the WAD cursor or
     * flipped the overlay dispatcher - host state the committed timeline
     * must never inherit from an abandoned branch. Cheap and change-only
     * (the dispatcher setter no-ops when the id already matches). */
    if (m->gx_bytes) game_state_extra_load(m->gx, m->gx_bytes);
    (void)PsyX_VRAM_RestoreDelta(m->vram, m->vram_bytes);
    if (!m->arena_external)
        openpete_mods_arena_mem_restore(m->arena, m->arena_used);
    openpete_mods_registry_state_apply(m->reg, m->reg_bytes);
    SDL_SetAtomicInt(&openpete_vbl, (int)m->vbl);
    SDL_SetAtomicInt(&openpete_vbl_device, (int)m->vbl_device);
    openpete_consume = m->consume;
    openpete_consume_write = m->consume_write;
    if (!quiet) {
        game_on_state_loaded(cpu);
        openpete_guest_discontinuity_notify();
    }
    if (out_frame) *out_frame = m->frame;
    return 0;
}

void openpete_state_mem_arena_external(openpete_state_mem_t* m) {
    if (!m) return;
    m->arena_external = 1;
    free(m->arena);
    m->arena = NULL;
    m->arena_cap = 0;
    memset(m->arena_used, 0, sizeof m->arena_used);
}

openpete_park_site_t openpete_state_mem_site(const openpete_state_mem_t* m) {
    return m->site;
}
