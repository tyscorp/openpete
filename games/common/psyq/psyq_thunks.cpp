/* games/common/psyq/psyq_thunks.cpp - PSYQ -> PsyCross ABI thunks, shared by every
 * game.
 *
 * The recompiler emits PSYQ-targeted JALs as `psyq_NAME(cpu)` (with
 * --external-prefix=psyq_). This file provides hand-written bodies for the
 * named PSYQ functions a game calls, translating between the MIPS calling
 * convention (args in a0..a3 + sp-relative slots, return in v0) and PsyCross's
 * native C signatures.
 *
 * Nothing here is specific to one game. What is - the PSYQ-internal structures
 * a thunk mirrors into PSX RAM at a game's own addresses, the stack frame each
 * real PSYQ function leaves behind, and couplings to a game's own features -
 * is reached through psyq_game_hooks.h, whose every hook has an inert default.
 * A game supplies only what it has.
 *
 * The auto-generated abort stubs in psyq_stubs.c are weak; these strong
 * definitions override them at link time. Anything *not* thunked here
 * still aborts - that's the development gate for "did Spyro just touch a
 * PSYQ surface we don't cover yet?"
 *
 * Compiled as C++ because PsyCross's libgpu.h uses `static_assert` on
 * struct sizes (C++11 keyword). Each thunk is wrapped in `extern "C"`
 * so the recompiler-emitted dispatch table still finds them.
 */

#include <cstdlib>
#include "openpete_thread_roles.h"

extern "C" {
#include "openpete.h"          /* openpete_shutdown_requested */
#include "openpete_fiber.h"
#include "openpete_guest_regs.h"
#include "openpete_psxtrace.h"
#include "openpete_log.h"
#include "openpete_ticker.h"
#include "openpete_psx.h"
#include "openpete_memcard.h"
#include "openpete_cdrom_mmio.h"
#include "openpete_crash.h"
#include "psyq/psyq_game_hooks.h"
#include "renderer/ui_game.h"   /* the 2D chokepoint's deferred feed */
#include "openpete_native_render.h"   /* DISPENV/DRAWENV and DrawOTag notes */
#include "openpete_fb_snapshot.h"     /* openpete_fb_snapshot_note_draw_env */
#include "openpete_run_capture.h"     /* openpete_capture_note_psyx_walk */

/* The PSYQ_OT_BASE_RING most recent distinct ordering-table bases handed to
 * DrawOTag (see the note at the walk). Newest first; a repeat of the newest
 * is ignored, so consecutive DrawOTag calls on one table take one slot. */
#define PSYQ_OT_BASE_RING 8
static uint32_t s_ot_base[PSYQ_OT_BASE_RING];
/// @brief Record a in the ring of recent DrawOTag bases, newest first, unless it repeats the newest.
static void ot_base_note(uint32_t a) {
    if (a == s_ot_base[0]) return;
    for (int i = PSYQ_OT_BASE_RING - 1; i > 0; i--) s_ot_base[i] = s_ot_base[i - 1];
    s_ot_base[0] = a;
}
extern "C" uint32_t psyq_ot_recent_base(int i) {
    return (i >= 0 && i < PSYQ_OT_BASE_RING) ? s_ot_base[i] : 0u;
}
}

#include <cstdio>
#include <cstring>
#include <stdint.h>

/* PsyCross PSYQ headers. Order matters: libgte.h defines SVECTOR before
 * libgpu.h references it. */
#include "psx/libgte.h"
#include "psx/libetc.h"
#include "psx/libgpu.h"
#include "psx/libcd.h"
#include "psx/libpad.h"
#include "psx/libspu.h"
#include <dirent.h>
#include "../../../third_party/PsyCross/src/audio/PsyX_SPUAL.h"
extern "C" void PsyX_SPUAL_TickEnvelope(int samples);
extern "C" int  PsyX_SPUAL_GetVoiceEnvelope(int voice, int* keyed_on);
extern "C" int  PsyX_SPUAL_CaptureDrain(int16_t* out, int max_frames);
extern "C" void PsyX_SPUAL_CaptureStats(unsigned long long* produced, unsigned long long* dropped);

/* OPENPETE_AUDIO_DUMP: set by the host when the env var names a file and no
 * recorder owns the capture ring; every VSync drains the ring into it. */
int openpete_audio_dump_armed = 0;
/** @brief Append the SPU capture ring to the OPENPETE_AUDIO_DUMP file, once per VBL.
 *
 * Does nothing unless openpete_audio_dump_armed is set. The first armed call
 * opens the file named by the variable once; a failed open is logged and the
 * dump stays off. Frames drain in chunks of 4096 as 16-bit stereo at 44100
 * Hz and the file is flushed on every call. Each call appends one line
 * `<vbl> <frames in the file> <frames the capture ring has produced>` to
 * PATH.idx, the dump's VBL axis at the rate the SPU clock actually charged
 * (the two frame counts differ by the frames rendered before the file
 * opened that the ring no longer held, constant unless the ring drops). A
 * rise in the ring's dropped-frame count logs an error, since a drop
 * shifts every later sample. */
static void audio_dump_drain(void) {
    static FILE* f = NULL;
    static FILE* idx = NULL;
    static int tried = 0;
    if (!openpete_audio_dump_armed) return;
    if (!f) {
        if (tried) return;
        tried = 1;
        const char* path = getenv("OPENPETE_AUDIO_DUMP");
        f = path ? fopen(path, "wb") : NULL;
        if (!f) { OPL_WARN_F("audio", "OPENPETE_AUDIO_DUMP: cannot open %s", path ? path : "(null)"); return; }
        char ipath[1024];
        snprintf(ipath, sizeof ipath, "%s.idx", path);
        idx = fopen(ipath, "w");
        if (idx) fputs("# vbl file_frames produced\n", idx);
        else OPL_WARN_F("audio", "OPENPETE_AUDIO_DUMP: cannot open %s", ipath);
        OPL_INFO_F("audio", "dump: %s (s16 stereo 44100) vbl_now=%d",
                   path, SDL_GetAtomicInt(&openpete_vbl_device));
    }
    int16_t buf[4096 * 2];
    int n;
    static unsigned long long total = 0;
    static int first = 1;
    while ((n = PsyX_SPUAL_CaptureDrain(buf, 4096)) > 0) {
        fwrite(buf, sizeof(int16_t) * 2, (size_t)n, f);
        total += (unsigned long long)n;
        if (n < 4096) break;
    }
    if (first && total > 0) {
        first = 0;
        OPL_INFO_F("audio", "dump: first drain %llu frames at vbl %d",
                   total, SDL_GetAtomicInt(&openpete_vbl_device));
    }
    /* A frame the ring dropped before this drain is a hole in the file:
     * every later sample is early by it. */
    unsigned long long produced = 0, dropped = 0;
    PsyX_SPUAL_CaptureStats(&produced, &dropped);
    if (idx) {
        fprintf(idx, "%d %llu %llu\n", SDL_GetAtomicInt(&openpete_vbl_device), total, produced);
        fflush(idx);
    }
    static unsigned long long reported = 0;
    if (dropped != reported) {
        reported = dropped;
        OPL_ERROR_F("audio", "dump: capture ring dropped %llu frames; the file is not a timeline", dropped);
    }
    fflush(f);
}

/* Saved-OT capture: each psyq_DrawOTag rebuilds the PSX OT chain into a
 * host scratch buffer (PsyCross P_TAG slots, guest->host translated)
 * and the display walk parses that: the reference walk shows the
 * just-built OT, bit-original.
 *
 * Sizing (measured over a 20k-frame demo): 4:3 peaks at 2,255 slots /
 * 94 KB. 1 MB scratch + 8192 slots is ample headroom. */
#define OT_SCRATCH_SZ  (1u * 1024u * 1024u)
#define OT_MAX_SLOTS   8192

typedef struct {
    uint8_t  scratch[OT_SCRATCH_SZ];
    uint8_t* slots[OT_MAX_SLOTS];
    int      slot_count;
    size_t   scratch_used;
} saved_ot_t;

static saved_ot_t  s_ot_buf[1];
static saved_ot_t* g_ot_cur  = &s_ot_buf[0];  /* built this DrawOTag */

/** @brief Draw the OT the last DrawOTag captured, through PsyCross's primitive parser.
 *
 * Clears PsyCross's draw splits, parses the captured chain from its first
 * slot, and draws the splits; nothing is drawn when the capture holds no
 * slot. The caller owns the PsyX BeginScene/EndScene framing, and the
 * DrawOTag thunk calls it only while PsyCross renders
 * (openpete_psycross_renders). */
extern "C" void openpete_render_walk_ot(void) {
    extern void ClearSplits(void);
    extern void ParsePrimitivesLinkedList(u_long* p, int singlePrimitive);
    extern void DrawAllSplits(void);

    if (g_ot_cur->slot_count <= 0) return;
    ClearSplits();
    ParsePrimitivesLinkedList((u_long*)g_ot_cur->slots[0], 0);
    DrawAllSplits();
}
#include "psx/libmcrd.h"
#include "psx/libapi.h"

/* XA streaming source (lives in PsyCross/audio/PsyX_XA.cpp). */
extern "C" {
    void PsyX_XA_Start(int sector, int file, int chan);
    void PsyX_XA_Stop(void);
    int  PsyX_XA_IsActive(void);
    void PsyX_XA_PumpTo(int end_lba, long long head_f, int frames_per_lba);
    int  PsyX_XA_GetCurrentSector(void);
    void PsyX_XA_Stats(unsigned long long* sectors, unsigned long long* silence,
                       unsigned long long* late, unsigned long long* refused,
                       unsigned long long* reseeks);
    void PsyX_XA_EndStats(unsigned long long* image_end_pumps, int* max_read_lba, int* image_sectors);
}

/* Music WAV override (core/host/) - plays assets/music/track_NN.wav in
 * place of the XA stream when present. */
#include "music_override.h"
#include "openpete_mods.h"          /* custom-level song lookup + SFX overlay */
#include "openpete_profile.h"       /* game-fiber cost split (--profile-out) */

/* PsyCross internals shared with the libcd thunks below. */
extern "C" int g_CD_com;
extern "C" int readMode;  /* 0=RM_DATA, 1=RM_XA_AUDIO */
extern "C" int g_cdXaStartLBA;   /* virtual music head anchor - LIBCD.C */
extern "C" int g_cdXaStartVBL;

/* Drive state mirrored from the guest's commands, on every entry (CdControl,
 * CdControlB, CdControlF, and the mode CdRead sets). libcd has no arm flag:
 * CdlSetmode writes the drive's mode byte, and ADPCM sectors route to the SPU
 * during any read while CdlModeRT is set, so a CdlReadS streams music iff the
 * mirrored mode has RT. CdlSetfilter records file/chan; CdlSetloc latches the
 * sector a later CdlReadS with no param reads from; CdlPause/Stop/ReadN stop
 * the stream. */
static uint8_t s_cd_mode          = 0;
static int     s_cd_setloc_sector = 0;
static uint32_t s_cd_reading      = 0;   /* bit 0: a CdlReadS runs; bit 1: it streams music */
static int     s_xa_filter_file   = 1;
static int     s_xa_filter_chan   = 0;

/* Engagement counters for the routing above, printed once at exit under the
 * `music` tag: mode writes that cleared RT while it was set, CdlReadS issued
 * with RT clear (a read that did not stream), and stream starts. */
static uint32_t s_cnt_mode_rt_cleared = 0;
static uint32_t s_cnt_reads_rt_clear  = 0;
static uint32_t s_cnt_stream_starts   = 0;

/* The drive head: one position serves the guest's position query
 * (CdlGetlocP), the seek bit of the drive status, and the XA decoder's
 * pump. Idle, it rests at s_head_lba. While a read or a play runs it
 * reaches s_head_lba at time s_head_t, after the seek, and advances from
 * there at 75 LBA per second times the speed factor: at double speed
 * 37609/15000 LBA per VBL (150 LBA per second at 2250000/37609 VBLs per
 * second). Time is in 1/CD_HEAD_TU VBL. The state is set by guest commands
 * and the guest VBL count, so it travels in the latch pack. */
#define CD_HEAD_TU 30000u
static uint32_t s_head_lba   = 0;   /* rest LBA, or the LBA a moving head reaches at s_head_t */
static uint64_t s_head_t     = 0;   /* when a moving head reaches s_head_lba */
static uint32_t s_head_rate  = 0;   /* 0 idle, 1 single speed, 2 double speed */
static uint32_t s_head_speed = 2;   /* speed factor of the last read or play */

/* Seek time of a CdlReadS, read off the reference drive's position answers
 * (the VBL each streaming run's least-squares fit reaches its start LBA;
 * 31 runs, seek distances 35174-141299 LBA, 23.7-36.0 VBLs): 16.645 +
 * 7.17627e-5 d + 0.0243477 sqrt(d) VBLs for a seek of d LBA, within 0.03
 * VBL of every run. A read at a speed other than the last read's or play's
 * adds CD_SEEK_SPEED_CHANGE: the one such run (a double-speed stream read
 * after single-speed CD-DA play, seek about 5900 LBA) took 36.5 VBLs, and
 * 17.0-17.7 VBLs keeps every answer of the run within 1 LBA. Seeks
 * outside 35174-141299 LBA are extrapolated. */
#define CD_SEEK_C0            499349u   /* 16.645 VBL, in CD_HEAD_TU */
#define CD_SEEK_C1_PER_1E5    215288u   /* per LBA, x 1e5 */
#define CD_SEEK_C2_PER_1E5     73043u   /* per 1/1000 sqrt(LBA), x 1e5 */
#define CD_SEEK_SPEED_CHANGE  520500u   /* 17.35 VBL */

/// @brief The device VBL count (every VBLANK elapsed, held ones included) as drive-head time (CD_HEAD_TU per VBL).
static uint64_t cd_head_now(void) {
    return (uint64_t)(uint32_t)SDL_GetAtomicInt(&openpete_vbl_device) * CD_HEAD_TU;
}

/** @brief The head's position at time `t` in units of 1/`per` LBA, floored.
 *
 * A moving head advances (t - s_head_t) * rate * 37609 / (CD_HEAD_TU * 30000)
 * LBA; the product is split by whole VBLs to stay within 64 bits.
 * @param t Head time (CD_HEAD_TU per VBL).
 * @param per Units per LBA: 1 for the LBA, 294 for output frames at double speed.
 * @return The position. */
static int64_t cd_head_at(uint64_t t, uint32_t per) {
    const int64_t base = (int64_t)s_head_lba * per;
    if (s_head_rate == 0u || t <= s_head_t) return base;
    const uint64_t dt = t - s_head_t;
    const uint64_t k = (uint64_t)s_head_rate * 37609u * per;
    const uint64_t a = (dt / CD_HEAD_TU) * k;
    return base + (int64_t)(a / 30000u + ((a % 30000u) * CD_HEAD_TU + (dt % CD_HEAD_TU) * k) / 900000000u);
}

/// @brief Floor of the square root of v.
static uint64_t cd_isqrt(uint64_t v) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/** @brief Start the head moving toward `lba` for a CdlReadS: it reaches it after the seek from its current position and then reads at `rate`.
 *
 * @param lba The read's first LBA.
 * @param rate 1 single speed, 2 double speed. */
static void cd_head_read(int lba, uint32_t rate) {
    const uint64_t now = cd_head_now();
    const int64_t from = cd_head_at(now, 1u);
    const uint64_t d = (uint64_t)(from > lba ? from - lba : lba - from);
    uint64_t seek = CD_SEEK_C0 + d * CD_SEEK_C1_PER_1E5 / 100000u +
                    cd_isqrt(d * 1000000u) * CD_SEEK_C2_PER_1E5 / 100000u;
    if (rate != s_head_speed) seek += CD_SEEK_SPEED_CHANGE;
    if (!openpete_speculative)
        OPL_INFO_F("cd", "head: read at LBA %d from LBA %lld (x%u), seek %llu/%u VBL (vbl %d)",
                   lba, (long long)from, rate, (unsigned long long)seek, CD_HEAD_TU,
                   SDL_GetAtomicInt(&openpete_vbl_device));
    s_head_lba = (uint32_t)lba;
    s_head_t = now + seek;
    s_head_rate = rate;
    s_head_speed = rate;
}

/** @brief Start the head playing CD-DA from `lba` at `rate` now, without a seek.
 *
 * @param lba The play's first LBA.
 * @param rate 1 single speed, 2 double speed. */
static void cd_head_play(int lba, uint32_t rate) {
    s_head_lba = (uint32_t)lba;
    s_head_t = cd_head_now();
    s_head_rate = rate;
    s_head_speed = rate;
}

/// @brief Stop a moving head where it is now (CdlPause, CdlStop).
static void cd_head_stop(void) {
    if (s_head_rate == 0u) return;
    s_head_lba = (uint32_t)cd_head_at(cd_head_now(), 1u);
    s_head_rate = 0u;
}

/** @brief Rest the head at `lba` after a data read or a seek, read at `rate`.
 *
 * @param lba The LBA after the last sector read, or the seek target.
 * @param rate 1 single speed, 2 double speed. */
static void cd_head_rest(int lba, uint32_t rate) {
    s_head_lba = (uint32_t)lba;
    s_head_rate = 0u;
    s_head_speed = rate;
}

/** @brief Whether the head is still seeking to a read's first LBA.
 *
 * @return 1 while a moving head has not reached s_head_lba, else 0. */
static int cd_head_seeking(void) {
    return s_head_rate != 0u && cd_head_now() < s_head_t;
}

/// @brief The speed factor a drive mode byte selects: 2 with CdlModeSpeed, else 1.
static uint32_t cd_mode_rate(uint8_t mode) { return (mode & CdlModeSpeed) ? 2u : 1u; }

/** @brief Write one byte of libcd's state block in guest RAM.
 *
 * libcd keeps the drive's last status byte (+0x0C), the last CdlSetloc
 * position (+0x18, 4 bytes), the mode byte (+0x1C) and the last command
 * (+0x1D) in a block of its own data that CdStatus, CdLastPos, CdMode and
 * CdLastCom read; a game that translates those getters reads the block
 * directly, so the thunks write it where the library would. Nothing is
 * written when psyq_game_libcd_block_addr() is 0.
 * @param off Byte offset in the block.
 * @param v The byte. */
static void cd_mirror_byte(uint32_t off, uint8_t v) {
    uint32_t base = psyq_game_libcd_block_addr();
    if (base == 0u) return;
    openpete_psx_get_ram()[(base + off) & 0x1FFFFFu] = v;
}
/// @brief Write libcd's last-status byte (block offset 0x0C).
static void cd_mirror_status(uint8_t st)  { cd_mirror_byte(0x0Cu, st); }
/* The reference's response to the last position query (eight bytes:
 * track, index, position, absolute position), when the golden's
 * controller sidecar carries it. */
static uint8_t s_getlocp_ref[16];
static int     s_getlocp_ref_valid = 0;
/// @brief Write libcd's drive-mode byte (block offset 0x1C).
static void cd_mirror_mode(uint8_t mode)  { cd_mirror_byte(0x1Cu, mode); }
/// @brief Write libcd's last-command byte (block offset 0x1D).
static void cd_mirror_com(uint8_t com)    { cd_mirror_byte(0x1Du, com); }
/** @brief Write the four bytes of a CdlLOC as libcd's last position (block offset 0x18).
 *
 * @param loc Host pointer to at least 4 bytes; not NULL. */
static void cd_mirror_pos(const u_char* loc) {
    for (uint32_t i = 0; i < 4u; i++) cd_mirror_byte(0x18u + i, loc[i]);
}
/** @brief Return the model's drive status for a command's first response: motor-on (0x02), plus seeking (0x40) or reading (0x20) while a streaming read runs.
 *
 * libcd stores the status from a command's first response for every command
 * except the two location queries, whose responses are the position. The
 * model never sets the shell-open or error bits; the seek bit is set until
 * the drive head reaches the read's first LBA (cd_head_seeking).
 * cd_command_host uses this value only when the golden's controller sidecar
 * holds no reference response. A CdlReadS is acknowledged before its
 * reading starts, so the seek and reading bits appear only through a later
 * command; a stream never polled with a status command keeps the idle
 * value.
 * @return The status byte. */
static uint8_t cd_live_status(void) {
    if (!s_cd_reading) return 0x02u;
    return (uint8_t)(0x02u | (cd_head_seeking() ? 0x40u : 0x20u));
}
/* Music level is the product of the two hardware stages the XA stream passes
 * through: its own CD input volume, then the SPU main volume applied to the
 * summed mix. Both arrive via separate SpuSetCommonAttr masks, so they are
 * tracked apart and recombined whenever either moves. */
static float s_cdVolume      = 1.0f;
static float s_spuMainVolume = 1.0f;

/* The latch pack these statics and the callback tables below serialize into
 * for the savestate; definitions follow the last table. */
#include "psyq/psyq_thunk_state.h"
#include "PsyX/PsyX_public.h"

/* Internal PsyCross helpers we need to reach for the OT thunk. They live
 * in src/gpu/PsyX_GPU.h, which isn't on the public include path; redeclare
 * the two we use. Both are linkable directly from the static lib. */
extern "C" {
    void ClearSplits();
    void DrawAllSplits();
}

extern "C" {

/* --- Helpers ------------------------------------------------------------- */

/** @brief Resolve a guest pointer argument to a host pointer valid for @p len bytes, warning once per call site on refusal.
 *
 * The span must lie inside one guest region (openpete_psx_to_host_span). A
 * refused span, guest NULL included, logs the call, the address and the
 * length the first time its site refuses one; the caller then skips the
 * access and returns the PSYQ call's failure value.
 * @param call The thunk's name, for the warning.
 * @param addr The guest address, as held in cpu->aN.
 * @param len The bytes the thunk or PsyCross reads or writes there.
 * @param warned The call site's once-flag.
 * @return The host pointer, or NULL when the span is refused. */
static void* psyq_span(const char* call, uint32_t addr, uint32_t len, std::atomic<bool>* warned) {
    void* p = openpete_psx_to_host_span(addr, len);
    if (p == nullptr && !warned->exchange(true, std::memory_order_relaxed)) {
        OPL_WARN_F("psyq", "%s: guest span 0x%08X+0x%X refused; access skipped", call, addr, len);
    }
    return p;
}

/** @brief Resolve a guest C string read up to its NUL or @p max bytes, warning once per call site on refusal.
 *
 * The NUL scan stops at the end of the string's region
 * (openpete_psx_to_host_str), so no host read built on the result passes
 * it.
 * @param call The thunk's name, for the warning.
 * @param addr The guest address of the string.
 * @param max The most bytes the reader consumes; UINT32_MAX for a string
 *        read to its terminator.
 * @param len_out Receives the string's length, the NUL excluded, capped at
 *        @p max; may be NULL.
 * @param warned The call site's once-flag.
 * @return The host pointer, or NULL when the string is refused. */
static const char* psyq_str(const char* call, uint32_t addr, uint32_t max, uint32_t* len_out, std::atomic<bool>* warned) {
    const char* p = openpete_psx_to_host_str(addr, max, len_out);
    if (p == nullptr && !warned->exchange(true, std::memory_order_relaxed)) {
        OPL_WARN_F("psyq", "%s: guest string at 0x%08X runs past its region; access skipped", call, addr);
    }
    return p;
}

/** @brief Clamp a 64-bit byte count to the span length argument; a count past 4 GiB becomes UINT32_MAX, which every span refuses. */
static inline uint32_t psyq_len(uint64_t n) {
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

/* Each expansion's lambda is its own type, so its static is a once-flag
 * private to the call site. */
#define PSYQ_SITE_ONCE() ([]() -> std::atomic<bool>* { static std::atomic<bool> s_warned; return &s_warned; }())
/* Guest pointer argument -> host pointer valid for len bytes, or NULL. */
#define SPAN(addr, len) psyq_span(__func__, (uint32_t)(addr), psyq_len(len), PSYQ_SITE_ONCE())
/* A guest NULL must stay NULL without a warning: an optional argument of 0
 * skips the access the PSYQ call makes for it. A nonzero address resolves
 * through SPAN; the site tells a refusal from an absent argument by the
 * address. */
#define SPAN_OPT(addr, len) ((addr) ? SPAN(addr, len) : nullptr)
/* Guest C string -> host pointer whose bytes to the NUL (or max) lie in one
 * region, or NULL; lenp may be NULL. */
#define GSTR(addr, max, lenp) psyq_str(__func__, (uint32_t)(addr), (uint32_t)(max), (lenp), PSYQ_SITE_ONCE())

/* libgpu's environment block, as the thunks touch it: VRAM size at +4, the
 * current DRAWENV (0x5C bytes) at +0x10, the current DISPENV (0x14 bytes)
 * at +0x6C, ending at +0x80. */
#define PSYQ_LIBGPU_ENV_SIZE 0x80u

/** @brief Bytes a LoadImage or StoreImage transfer moves for @p r: w * h 16-bit pixels, 0 when either side is not positive.
 *
 * LoadImage then moves nothing; StoreImage's VRAM read does not guard a
 * negative width. */
static inline uint64_t psyq_image_bytes(const RECT16* r) {
    return (r->w > 0 && r->h > 0) ? (uint64_t)r->w * (uint64_t)r->h * 2u : 0u;
}

/* PSYQ callbacks (CdReadCallback, VSyncCallback) take a PSX function
 * pointer that the compiled C wants invoked when the underlying event
 * fires. We can't call PSX code natively, so we save the address and
 * dispatch via the recompiled-function table when our thunk knows the
 * event has happened (e.g. right after PsyCross's synchronous CdRead).
 *
 * Each saved cb is dispatched on the *same* CPUState that registered it,
 * with its registers temporarily clobbered for the call. */
#include "openpete_recomp_module.h"

static uint32_t g_cd_read_cb     = 0;

/* libetc's VSyncCallbacks array (eight slots at 0x800665F8 in Spyro 2),
 * called in slot order from libetc's VBLANK handler. Slot 4 is the one
 * VSyncCallback writes and lives in g_vsync_cb; the others live in
 * g_intr_cb, whose eleven words the savestate latch pack carries (slots 8
 * to 10 are never written). A BIOS-driven card driver installs its
 * per-VBLANK step in slot 7. */
#define PSYQ_INTR_SLOTS 11
#define PSYQ_VSYNC_SLOTS 8u
#define PSYQ_VSYNC_SLOT_CB 4u
static uint32_t g_intr_cb[PSYQ_INTR_SLOTS];
static uint32_t g_vsync_cb       = 0;

/** @brief Run the compiled guest function at cb_addr with a0 and a1 set.
 *
 * Saves the general-purpose registers, hi and lo and restores them
 * afterwards, as the kernel's exception return does (openpete_guest_regs.h),
 * so the interrupted code's registers survive the call; the callee's return
 * value is discarded. ra is set to a sentinel, since the dispatcher runs the
 * body to its return. A zero cb_addr does nothing, and an address with no
 * compiled body logs a warning. The handler's gate hits are host-initiated
 * for the oracle's clock replay. OPENPETE_CB_DEBUG (read once) logs entry
 * and exit.
 * @param cpu Guest CPU state.
 * @param cb_addr Guest address of the handler.
 * @param a0 First argument.
 * @param a1 Second argument. */
static void invoke_psx_callback(CPUState* cpu, uint32_t cb_addr,
                                uint32_t a0, uint32_t a1) {
    if (cb_addr == 0) return;
    openpete_guest_regs_t saved;
    openpete_guest_regs_save(cpu, &saved);
    cpu->a0 = a0; cpu->a1 = a1; cpu->ra = 0xDEADBEEF;
    static int s_cbdbg = -1; if (s_cbdbg < 0) s_cbdbg = getenv("OPENPETE_CB_DEBUG") ? 1 : 0;
    if (s_cbdbg) OPL_INFO_F("thunks", "DBG callback 0x%08X enter pos=%u sp=0x%08X", cb_addr, openpete_sched_pos(), cpu->sp);
    openpete_psxtrace_gate_host_enter();
    if (!openpete_recomp_module_get()->dispatch(cpu, cb_addr)) {
        OPL_WARN_F("thunks", "callback 0x%08X has no compiled body", cb_addr);
    }
    openpete_psxtrace_gate_host_leave();
    if (s_cbdbg) OPL_INFO_F("thunks", "DBG callback 0x%08X leave pos=%u sp=0x%08X", cb_addr, openpete_sched_pos(), cpu->sp);
    openpete_guest_regs_restore(cpu, &saved);
}

/** @brief Run libetc's VSync slots 0 through 7 in order, once, for one VBLANK.
 *
 * libetc's line-0 handler calls the eight-entry VSyncCallbacks array in
 * slot order after incrementing its counter; slot 4 is the VSyncCallback
 * function (g_vsync_cb), the others are g_intr_cb. The Spyro games fill
 * slot 4 (the game's per-VBL function) and slot 7 (a BIOS card driver's
 * per-VBLANK step).
 * @param cpu Guest CPU state. */
static void psyq_run_vsync_slots(CPUState* cpu) {
    for (uint32_t i = 0; i < PSYQ_VSYNC_SLOTS; i++) {
        const uint32_t cb = (i == PSYQ_VSYNC_SLOT_CB) ? g_vsync_cb : g_intr_cb[i];
        if (cb) invoke_psx_callback(cpu, cb, 0, 0);
    }
}

/* --- libgcc/crt0 --------------------------------------------------------- */

/* Defined below, with the libgpu thunks it is mostly used by. */
static void psyq_mirror_stack_frame(CPUState* cpu, psyq_frame_site_t site);

/** @brief Thunk for libgcc __main: writes the guest RAM and stack effects of the real constructor runner.
 *
 * The real __main at 0x8005B988 sets the initialized flag at 0x800730BC,
 * walks an empty constructor list and stores ra in its own stack frame; the
 * game mirror and the stack-frame mirror reproduce those bytes so a RAM diff
 * against a reference run agrees from the first instruction of Initialize.
 * @param cpu Guest CPU state. */
void psyq___main(CPUState* cpu) {
    /* libgcc constructor-runner. Real PSYQ __main at 0x8005B988:
     *   - load *(0x800730BC) - "already initialized" flag
     *   - if zero: write 1 to it (mark initialized)
     *   - walks __CTOR_LIST__ at 0x80010000 calling each ctor (loop count
     *     in s1 - Spyro has 0 ctors so the loop is skipped)
     *   - in addition, push ra at sp+12 of __main's own frame (sp -= 16)
     *
     * Without these byte-level effects, --seed-ram + --halt-at-pc
     * comparison against pcsx-redux shows the 5 bytes as diffs at the very
     * first halt point inside Initialize(). Replicating them keeps the
     * "first frame == pcsx-redux RAM" invariant intact for the diff loop
     * (see docs/src/developer/ram-diff-vs-pcsxr.md). */
    psyq_game_mirror_main(cpu);
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_MAIN);
}

/* --- libetc -------------------------------------------------------------- */

/* Forward decls: defined later, called from inside psyq_VSync below. */
static void psyq_libpad_mirror_canonical_slot2(CPUState* cpu);
/* Who delivers a VBL (deliver_vbl): psyq_VSync's own wait and the VSync
 * and VSync(-1) charges are taken whatever the critical-section flag says;
 * every other source is held while it is set. */
enum vbl_src {
    VBL_SRC_VSYNC = 0,  /* psyq_VSync's wait, its VSync and VSync(-1) charges */
    VBL_SRC_CHARGE,     /* the schedule's other charges (registration, anchor, load window, clock gate, CdRead, CdControl, CdSync) */
    VBL_SRC_SPIN,       /* the dispatcher's spin tick */
    VBL_SRC_LOOP,       /* an emitted polling-loop back-edge */
    VBL_SRC_INCALL,     /* a device wait inside a kernel call */
    VBL_SRC_N
};
static void sched_charge_to(CPUState* cpu, uint32_t target, const char* what,
                            vbl_src src = VBL_SRC_CHARGE, int final_wait = 0);
static void deliver_vbl(CPUState* cpu, vbl_src src, int final_wait);
extern "C" uint32_t openpete_psxtrace_anchor_hits;
extern "C" uint32_t openpete_psxtrace_anchor_hit_target;
static void psyq_vbl_body(CPUState* cpu, int final_wait);
static void cd_stream_pump(int wait);
static void cd_defer_tick(CPUState* cpu, uint32_t vbls);
static void cd_defer_countdown(uint32_t vbls);
static void cd_defer_due(CPUState* cpu, uint32_t vbls, uint32_t left);
static void cd_defer_before_call(CPUState* cpu);
static void cd_defer_after_call(CPUState* cpu);
static void pad_busy_tick(uint32_t vbls);
static int pad_polls(uint32_t vbls);
static uint32_t s_pad_card_free_prev = 1u; /* the card driver left SIO0 free at the previous VBLANK */
static uint8_t  s_pad_last[8];            /* the first pad buffer as the last exchange with pad data left it */
static uint32_t s_pad_last_ok = 0u;       /* s_pad_last holds such an exchange */
static uint32_t s_pad_data_prev = 1u;     /* the previous vsync's controller command carried pad data */
static void pad_poll_census(CPUState* cpu, uint32_t key, int pad_cmd, int ref, int held, const uint8_t prev[8]);
static void cd_events_tick(uint32_t vbls);
static int s_cd_at_anchor = 0;           /* inside the anchor hook, before the record's hash */
static uint32_t s_pad_buf0 = 0;          /* the first port's pad buffer (PadInitDirect) */

/* The SPU clock of the oracle's clock replay: the reference's SPU sample
 * count (its exact CPU clock / 768, the SPU's sample period) that
 * openpete's SPU has been charged to. Unseeded, every waited VBL charges
 * the hardware's period, 527722650/715909 samples with the remainder
 * carried; seeded, the SPU advances only when an event with a reference
 * clock (a wait's return, a query, a CdSync poll, a libspu call) moves the
 * clock, except inside a load window, where the waits run on openpete's own
 * count and each VBL charges the same period. */
static int      s_spu_clk_seeded = 0;
static uint64_t s_spu_clk = 0;          /* reference SPU samples charged to */
static uint64_t s_spu_clk_frac = 0;     /* remainder of the samples per VBL, in 1/715909 samples */
static uint64_t s_spu_clk_charges = 0, s_spu_clk_samples = 0;

/** @brief Charge the SPU envelope to the reference clock tick: the samples from the SPU clock to tick / 768.
 *
 * Does nothing without the clock replay or for tick 0 (no reference
 * clock). The first tick after open and after every load-window skip
 * seeds the clock instead of charging. A tick behind the clock charges
 * nothing. The audio dump drains after each charge.
 * @param tick The reference's exact CPU clock at the event. */
static void spu_clock_charge(uint64_t tick) {
    if (tick == 0 || !openpete_sched_clock_replay()) return;
    const uint64_t s = tick / 768u;
    if (openpete_sched_clock_reseed_take() || !s_spu_clk_seeded) {
        if (!s_spu_clk_seeded) openpete_sched_clock_seeded(tick);
        s_spu_clk = s;
        s_spu_clk_seeded = 1;
        return;
    }
    if (s <= s_spu_clk) return;
    PsyX_SPUAL_TickEnvelope((int)(s - s_spu_clk));
    /* A load window can charge more than the capture ring holds between
     * two VBL bodies: the dump drains after every charge. */
    audio_dump_drain();
    s_spu_clk_charges++;
    s_spu_clk_samples += s - s_spu_clk;
    s_spu_clk = s;
}

/** @brief The SPU samples `wait` VBLs charge: the hardware's period until the SPU clock is seeded, then none outside a load window and the same period inside one.
 *
 * The period is 897487.5 GPU clocks per VBLANK (3412.5 clocks by 263
 * lines) at 33868800/53693175 CPU cycles per GPU clock, over 768 cycles per
 * sample: 527722650/715909 samples (737.136), carried as a remainder.
 * @param wait VBLs being run.
 * @return Samples to charge. */
static int spu_vbl_samples(int wait) {
    if (s_spu_clk_seeded && !openpete_sched_in_load()) return 0;
    const uint64_t acc = s_spu_clk_frac + (uint64_t)wait * 527722650u;
    s_spu_clk_frac = acc % 715909u;
    return (int)(acc / 715909u);
}

/** @brief Deliver the in-work VBLANK the clock replay schedules at this gate hit, if any.
 *
 * Called by the dispatcher on every counted gate hit while the replay is
 * on. The VBLs run through the VBL body (the game's own VSync callback
 * included) before the entered function's body, a call boundary where only
 * argument and return registers are live, which the callback invocation
 * preserves.
 * @param cpu Guest CPU state.
 * @param addr Guest address of the function entered. */
extern "C" void psyq_sched_clock_gate(CPUState* cpu, uint32_t addr) {
    uint32_t target = 0;
    if (!openpete_sched_vblpos_gate(addr, &target)) return;
    openpete_psxtrace_gate_host_enter();
    sched_charge_to(cpu, target, "in-work VBLANK");
    openpete_psxtrace_gate_host_leave();
}

/* The critical-section probe: VSync waits and VSync(-1) charges the game
 * made with interrupts masked (outside a delivery). Both are taken anyway;
 * on hardware a masked VSync wait spins until libetc's timeout. */
static uint64_t s_masked_vsync_waits = 0, s_masked_query_charges = 0;
static int s_dlv_depth = 0;              /* deliveries in progress */

/** @brief Deliver the in-work VBLANK the clock replay schedules before this guest load, if any.
 *
 * Called by the read gate (dispatch.c) on every guest load outside a
 * host-initiated call while the replay holds `read` rows; loads the VBL
 * body itself makes (the pad buffer's save and restore) are not counted.
 * The VBLs run through the VBL body before the load reads memory, with
 * every register preserved (deliver_vbl), since a load sits mid-block.
 * @param cpu Guest CPU state.
 * @param addr Guest address of the load. */
extern "C" void psyq_sched_clock_read_gate(CPUState* cpu, uint32_t addr) {
    uint32_t target = 0;
    if (s_dlv_depth || !openpete_sched_vblpos_read_gate(addr, &target)) return;
    openpete_psxtrace_gate_host_enter();
    sched_charge_to(cpu, target, "in-work VBLANK at a load");
    openpete_psxtrace_gate_host_leave();
}

/* VSync waits off the schedule by VBLs waited (1, 2, 3, 4 or more): the
 * waits whose earlier VBLs yield at an intermediate yield. */
static uint64_t s_vsync_waits_by_n[4];
/* The innermost psyq_VSync wait in progress: its call's guest ra and sp,
 * published as openpete_yield_site at the wait's final yield. */
static openpete_park_site_t s_wait_site = {0u, 0u};

/** @brief Thunk for libetc VSync: returns the VBL counter, waiting first when a0 is not negative.
 *
 * A0 below 0 only reads the counter. A0 of 0 waits one VBL and any other
 * value waits that many, each through deliver_vbl (one VBL body and one
 * yield per VBL), the last of them the wait's final yield. Under an active
 * schedule the wait charges VBLs up to the reference's return instead, and
 * the returned counter is the reference's value at that call site. The
 * wait is taken whether or not interrupts are masked. The call's ra and sp
 * are the park site its final yield publishes (openpete_yield_site).
 * @param cpu Guest CPU state; a0 is the mode, v0 receives the counter. */
void psyq_VSync(CPUState* cpu) {
    /* libetc semantics:
     *   mode  < 0 : query - return current vbl counter, no wait
     *   mode == 0 : wait for next vblank, return updated counter
     *   mode  > 0 : wait that many vblanks
     *
     * vbl is yield-driven: each waited VBL bumps the counter by one and
     * yields once. That keeps Spyro's wait-loop pattern
     * (`while VSync(-1)-s < 2 VSync(0)`) deterministic - the loop always
     * exits after exactly 2 yields per logical frame regardless of wall-
     * clock pacing, which is the invariant BK2 replay relies on.
     *
     * Wall-clock pacing lives elsewhere: the host loop (run.c) sleeps to
     * its own phase-locked deadline between game slices, so 1 yield is one
     * VBL period. Tab / --headless skip the host-loop wait, which
     * fast-forwards wall clock without changing yield-per-frame counts -
     * so BK2 state at any frame is identical regardless of pacing. */
    int mode = (int)cpu->a0;
    static uint32_t calls = 0u;
    int observed = SDL_GetAtomicInt(&openpete_vbl);
    if ((++calls & 0x3FFu) == 1u) {
        OPL_INFO_F("thunks", "VSync(%d) vbl=%d frame=%u (call #%u)",
                   mode, observed, openpete_frame, calls);
    }
    const int masked = openpete_crit_masked() && s_dlv_depth == 0;

    if (mode < 0) {
        /* Query: read the counter, wait for nothing. Every Spyro game's
         * frame wait is `VSync(0); VSync(-1); while (cur - ref < 2)`, so
         * VSync(0) advances the counter and this call only observes it.
         * Under the reference's schedule the game sees the reference's
         * answer to the same read, and the VBLs whose interrupts the
         * reference had seen by then are charged first. */
        uint32_t target = 0;
        int32_t sc = openpete_sched_query(cpu->ra, &target);
        if (sc >= 0 && target > openpete_sched_pos()) {
            if (masked) s_masked_query_charges++;
            sched_charge_to(cpu, target, "VSync(-1)", VBL_SRC_VSYNC);
        }
        spu_clock_charge(openpete_sched_query_tick());
        cpu->v0 = (sc >= 0) ? (uint32_t)sc : (uint32_t)observed;
        return;
    }
    if (masked) s_masked_vsync_waits++;
    const openpete_park_site_t outer_site = s_wait_site;
    s_wait_site = openpete_park_site_t{cpu->ra, cpu->sp};
    int wait = (mode == 0) ? 1 : mode;
    uint32_t sched_target = 0;
    if (openpete_sched_active() && openpete_sched_next_vsync(mode, &sched_target)) {
        cd_defer_before_call(cpu);
        /* Up to the reference's VBLANK at this wait's return; a wait the
         * reference returned from at once charges nothing. */
        if (sched_target <= openpete_sched_pos()) {
            static uint32_t s_nocharge = 0;
            if ((++s_nocharge & 0x3FFu) == 1u)
                OPL_INFO_F("thunks", "VSync(%d) charged nothing: pos=%u ra=0x%08X (#%u)", mode, openpete_sched_pos(), cpu->ra, s_nocharge);
        }
        (void)openpete_sched_vblpos_late(sched_target);
        sched_charge_to(cpu, sched_target, "VSync", VBL_SRC_VSYNC, 1);
        spu_clock_charge(openpete_sched_wait_tick());
    } else {
        /* One VBL body per waited VBL. Past the schedule's rows (the
         * trace's end) each advances the position, so the game-visible
         * counter keeps moving. */
        s_vsync_waits_by_n[(wait >= 4) ? 3 : wait - 1]++;
        if (wait > 1)
            OPL_INFO_F("delivery", "VSync(%d) off the schedule: %d VBLs from vbl %d frame %u (ra=0x%08X)",
                       mode, wait, SDL_GetAtomicInt(&openpete_vbl), openpete_frame, cpu->ra);
        for (int i = 0; i < wait; i++) deliver_vbl(cpu, VBL_SRC_VSYNC, i == wait - 1);
    }
    {
        /* A waited call's own value: the counter at its return. */
        int32_t sc = openpete_sched_counter();
        cpu->v0 = (sc >= 0) ? (uint32_t)sc : (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    }
    s_wait_site = outer_site;
    if (openpete_sched_active()) cd_defer_after_call(cpu);
}

/** @brief Deliver one VBL per VBLANK from the schedule position up to `target`.
 *
 * Under the schedule every VBL openpete charges is one the reference spent;
 * each runs through deliver_vbl, which advances the position by one whether
 * the VBL is taken or held. A target behind the position charges nothing;
 * once the first anchor hit is reached and outside a load window it records
 * a schedule failure (an extra wait, or a read the reference completed
 * sooner). Before that hit the boot runs under openpete's own count.
 * @param cpu Guest CPU state.
 * @param target The reference VBLANK to charge up to.
 * @param what The charging site, for the failure message.
 * @param src The delivery source: VBL_SRC_VSYNC for psyq_VSync's charges,
 *        whose VBLs are taken whatever the critical-section flag says.
 * @param final_wait Nonzero when the last VBL ends a psyq_VSync wait (its
 *        yield is the wait's final yield). */
static void sched_charge_to(CPUState* cpu, uint32_t target, const char* what,
                            vbl_src src, int final_wait) {
    uint32_t pos = openpete_sched_pos();
    if (target < pos) {
        /* Before the first anchor hit the boot runs under openpete's own
         * count and the first stamp reconciles it; after it, being ahead
         * is a desync. */
        if (openpete_psxtrace_anchor_hits >= openpete_psxtrace_anchor_hit_target && !openpete_sched_in_load())
            openpete_sched_fail("%s: openpete at reference VBLANK %u, target %u (ahead by %u)",
                                what, pos, target, pos - target);
        return;
    }
    while (pos < target) {
        pos++;
        deliver_vbl(cpu, src, final_wait && pos == target);
        if (!openpete_sched_active()) openpete_sched_advance_pos(1);
    }
}

/** @brief Charge, before the anchor's hash, the in-work VBLANKs of the ending record whose entry openpete did not reach.
 * @param cpu Guest CPU state. */
static void sched_charge_late(CPUState* cpu) {
    const uint32_t late = openpete_sched_vblpos_late(0u);
    if (late > openpete_sched_pos()) sched_charge_to(cpu, late, "late in-work VBLANK");
}

/** @brief Check the schedule position against the reference at an anchor hit, and deliver disc events due there.
 *
 * Called by the dispatcher at every anchor hit from the hit target on. Inside
 * the game's load window (psyq_game_load_window_word), leaving the window
 * skips the record index to the reference's window end and charges the VBLs
 * up to that record's stamp; nothing else runs at that hit. Otherwise
 * openpete_sched_anchor_target checks the position against the record's
 * stamp (ahead is a schedule failure, behind a long frame the next wait
 * charges), and the deferred-read tick runs with zero VBLs at the anchor.
 * @param cpu Guest CPU state.
 * @param record The hit's index from the hit target; unused, the current
 *        record comes from openpete_psxtrace_current_record(). */
extern "C" void psyq_sched_anchor(CPUState* cpu, uint32_t record) {
    uint32_t target;
    (void)record;
    /* The loader's window: leaving it re-aligns the record index with the
     * reference's window end, charging the VBLs its loader spent beyond
     * openpete's (callbacks included) before this hit's hash, and the
     * verifier skips the reference records in between. */
    uint32_t word = psyq_game_load_window_word();
    if (word && openpete_sched_active()) {
        uint32_t skip_to = 0;
        uint32_t rec_now = openpete_psxtrace_current_record();
        if (openpete_sched_anchor_window(rec_now, cpu->read_word(word), &skip_to)) {
            if (openpete_psxtrace_skip_current_to(skip_to) == 0 &&
                openpete_sched_anchor_target(skip_to, &target))
                sched_charge_to(cpu, target, "load window end");
            return;
        }
    }
    /* A long frame (openpete behind the record's stamp) is counted here
     * and charged by the next wait: the interrupt landed inside the tick's
     * work after the tick had consumed its counters, so a body run before
     * the hash would count it one tick early. */
    sched_charge_late(cpu);
    (void)openpete_sched_anchor_target(openpete_psxtrace_current_record(), &target);
    s_cd_at_anchor = 1;
    cd_defer_tick(cpu, 0u);
    s_cd_at_anchor = 0;
}


/** @brief Run the device half of one VBLANK, rows the drive, the SPU and the CD-ROM controller do whether or not the CPU takes it: the stream pump, the SPU envelope and the audio drain.
 *
 * Advances the device VBL count first, so the drive head reads the VBLANK
 * being run. Under the oracle's seeded clock replay the reference's clock
 * charges the SPU instead (spu_vbl_samples). */
static void vbl_device_head(void) {
    SDL_AddAtomicInt(&openpete_vbl_device, 1);
    openpete_vbl_leaf_begin();
    /* Run the SPU for the VBL's output samples: the voices' envelopes and
     * heads the guest reads back (key status, ENVX) and the mix, on the
     * same clock, with or without an audio device, after the XA decoder
     * has delivered the sectors the drive head reads in it. */
    cd_stream_pump(1);
    PsyX_SPUAL_TickEnvelope(spu_vbl_samples(1));
    audio_dump_drain();
    openpete_vbl_leaf_end("head");
}

/** @brief Step the CD-ROM controller model by one VBLANK: the interrupts it raises for a game that drives the controller's registers itself. */
static void vbl_device_mmio(void) {
    static int s_cd_raw_set = 0;
    if (!s_cd_raw_set) { s_cd_raw_set = 1; openpete_cdrom_mmio_set_raw_pc_min(psyq_game_cd_raw_pc_min()); }
    openpete_vbl_leaf_begin();
    openpete_cdrom_mmio_vbl(1u);
    openpete_vbl_leaf_end("mmio");
}

/* One vsync's pad verdicts, taken at its VBLANK before the card step. */
typedef struct {
    int card_free;      /* the card driver left SIO0 free at this VBLANK */
    int ref_known;      /* the schedule's controller log covers this VBLANK */
    int ref_cmd;        /* the controller log records a command at it (1 off the schedule) */
    int pad_cmd;        /* the controller takes a command on this vsync */
    int pad_data;       /* that command carries pad data (pad_polls) */
    int card_held_prev; /* the card held SIO0 at the previous VBLANK */
    int fresh;          /* the slots see this vsync's pad data */
    uint8_t prev[8];    /* the first pad buffer as the game left it */
} pad_vsync_t;

/** @brief Write the eight bytes at src to the first pad buffer.
 * @param cpu Guest CPU state.
 * @param src The bytes. */
static void pad_buf_put(CPUState* cpu, const uint8_t* src) {
    for (uint32_t k = 0; k < 8u; k++) cpu->write_byte(s_pad_buf0 + k, src[k]);
}

/** @brief Take a vsync's pad verdicts after the card step.
 *
 * When the card driver held SIO0 at the VBLANK, the controller is sent no
 * command on that vsync; nor on one the schedule's controller log records
 * none at. A command that carries pad data (pad_polls) makes the vsync
 * fresh.
 * @param cpu Guest CPU state.
 * @param key Reference VBLANK index of this VBLANK.
 * @param card_free Whether the card driver left SIO0 free at the VBLANK, read before the step.
 * @param pv Receives the verdicts and the pad buffer as the game left it. */
static void pad_vsync_begin(CPUState* cpu, uint32_t key, int card_free, pad_vsync_t* pv) {
    pv->card_free = card_free;
    pv->ref_known = 0;
    pv->ref_cmd = openpete_sched_padcmd_at(key, &pv->ref_known);
    pv->pad_cmd = pv->card_free && pv->ref_cmd;
    pv->card_held_prev = !s_pad_card_free_prev;
    memset(pv->prev, 0, sizeof pv->prev);
    if (s_pad_buf0) for (uint32_t k = 0; k < 8u; k++) pv->prev[k] = (uint8_t)cpu->read_byte(s_pad_buf0 + k);
    pv->pad_data = pv->pad_cmd ? pad_polls(1u) : 0;
    pv->fresh = pv->pad_data;
}

/** @brief Count the host's pad write of a yield and choose what the slots see.
 *
 * The pad data an exchange returns reaches the slots at the next vsync, so
 * a vsync after one the card held is not fresh. A fresh vsync keeps the
 * host's write and records it as the last pad data; any other shows the
 * slots the last pad data (libpad rewrites the buffer at every vsync from
 * the last exchange that returned data), or the buffer as the game left
 * it before any. At a vsync whose VBLANK finds the card holding SIO0, the
 * host's write is the data of the previous vsync's exchange: when that
 * exchange carried pad data, the write becomes the last pad data after
 * the slots are shown the previous one, so the next vsync, which has no
 * exchange of its own to show, rewrites the buffer from it.
 * @param cpu Guest CPU state; the pad buffer holds the host's write.
 * @param key Reference VBLANK index of this VBLANK.
 * @param pv The vsync's verdicts; fresh is updated. */
static void pad_vsync_after_yield(CPUState* cpu, uint32_t key, pad_vsync_t* pv) {
    pv->fresh = pv->fresh && !pv->card_held_prev;
    const uint8_t* shown = s_pad_last_ok ? s_pad_last : pv->prev;
    pad_poll_census(cpu, key, pv->pad_cmd, pv->ref_known ? pv->ref_cmd : -1, !pv->fresh, shown);
    openpete_feed_pad_write();
    if (!s_pad_buf0) return;
    uint8_t write[8];
    for (uint32_t k = 0; k < 8u; k++) write[k] = (uint8_t)cpu->read_byte(s_pad_buf0 + k);
    if (!pv->fresh) pad_buf_put(cpu, shown);
    if (pv->fresh || (!pv->card_free && s_pad_data_prev)) {
        memcpy(s_pad_last, write, sizeof s_pad_last);
        s_pad_last_ok = 1u;
    }
}

/** @brief Close a vsync's pad work after the slots ran.
 *
 * libpad's command sequence advances one command per vsync the controller
 * accepted one on. After a vsync with no pad data the buffer holds what
 * the game left in it before the VBLANK, except after one whose command
 * carries pad data following a vsync the card held: libpad's handler ran
 * and rewrote the buffer from the last exchange that returned data, which
 * stays.
 * @param cpu Guest CPU state.
 * @param pv The vsync's verdicts. */
static void pad_vsync_end(CPUState* cpu, const pad_vsync_t* pv) {
    if (pv->pad_cmd) pad_busy_tick(1u);
    s_pad_card_free_prev = pv->card_free ? 1u : 0u;
    s_pad_data_prev = pv->pad_data ? 1u : 0u;
    const int rewritten = pv->pad_data && pv->card_held_prev && s_pad_last_ok;
    if (s_pad_buf0 && !pv->fresh && !rewritten) pad_buf_put(cpu, pv->prev);
}

/** @brief Run the CPU half's interrupt work for one VBLANK: the yield, then libetc's VSync slots, with the pad buffer kept unless libpad polled.
 *
 * The kernel's card step runs first, keyed on reference VBLANK `key` (its
 * handler precedes libetc's in the kernel's chain, so an event it delivers
 * is visible to the slots of the same VBLANK). Under fast boot the slots
 * run without yielding; with no host fiber the wait goes through
 * PsyCross's VSync. The controller takes no command on a vsync whose
 * VBLANK finds the card driver holding SIO0 with an unfinished transfer,
 * nor on one the schedule records none at; a command advances libpad's
 * command sequence. The pad data a vsync's exchange returns reaches the
 * game's slots at the next one, so after a vsync the card held, the
 * host's write during the yield is taken back before the slots read it. A
 * vsync whose command carries no pad data (pad_polls) restores the first
 * pad buffer after the slots. A final yield publishes the innermost
 * wait's park site before it switches. The host's pad write during a
 * yield, and its first stamp at the first fast-boot VBL, advance the bk2
 * row position (openpete_feed_pad_write) whether or not the vsync polls.
 * Once the run is to stop (openpete_shutdown_requested) a fast-boot VBL
 * takes the yield, so the host loop sees the request and quits; a boot
 * that never draws a scene otherwise never returns to it.
 * @param cpu Guest CPU state.
 * @param key Reference VBLANK index the card step and pad command belong to.
 * @param final_wait Nonzero when this yield ends a psyq_VSync wait. */
static void vbl_cpu_interrupt(CPUState* cpu, uint32_t key, int final_wait) {
    /* A transfer the card driver left unfinished holds SIO0 at this VBLANK. */
    const int card_free = !openpete_psx_bios_card_busy();
    openpete_psx_bios_vbl_at(cpu, key);
    pad_vsync_t pv;
    pad_vsync_begin(cpu, key, card_free, &pv);

    if (openpete_fast_boot_active && !openpete_shutdown_requested()) {
        /* The host's first stamp, made before the fiber first ran, is the
         * pad write every fast-boot VBL repeats: one write, at (E, j) 0. */
        static int s_boot_pad_write = 0;
        if (!s_boot_pad_write) { s_boot_pad_write = 1; openpete_feed_pad_write(); }
        /* Fast-boot: don't yield to the host, so the boot spins run at full
         * CPU speed and openpete_frame stays anchored at 0 until the first
         * frame of real content. The game's own per-frame callbacks still
         * run: a loader that advances its state machine from a VSync slot
         * makes no progress without them, and would spin here forever. */
        psyq_run_vsync_slots(cpu);
        /* libpad rewrites the pad buffers from every vblank interrupt, so a
         * value the game stores into its buffer (its stick recentre on
         * detecting an analog pad) lasts one VBL on hardware. The host loop's
         * poll does that rewrite on the yield path; with no yield here the
         * override channel's stamp (neutral until the host publishes a
         * sample) is written directly. The live path is left alone: it reads
         * SDL, which this thread does not own. */
        if (g_PsyX_Pad_OverrideActive) PsyX_Pad_InternalPadUpdates();
    } else if (openpete_host_fiber) {
        if (final_wait) openpete_yield_site = s_wait_site;
        openpete_yield_final = final_wait ? 1 : 0;
        openpete_yield_to_host();
        openpete_yield_final = 0;
        pad_vsync_after_yield(cpu, key, &pv);
        psyq_run_vsync_slots(cpu);
    } else {
        /* No fiber loop (smoke-test path) - fall back to PsyCross's busy
         * wait so audio/timer semantics still match. */
        (void)VSync(0);
        psyq_run_vsync_slots(cpu);
    }

    /* PSYQ libpad parity: PsyCross's input poll (which fired during the
     * yield above, or via VSync(0) on the smoke-test path) wrote
     * "connected pad" bytes into slot 2 even though no controller is
     * present there. Mirror real PSYQ's empty-slot output before the
     * game's next read. */
    psyq_libpad_mirror_canonical_slot2(cpu);
    pad_vsync_end(cpu, &pv);
}

/** @brief Run one VBL: the counters, the device half, the CPU's interrupt work, the CD-ROM controller and the deferred disc read, in the console's order.
 *
 * The VSync slots are gameplay-visible (tick counters, the pad read), so
 * every VBLANK the CPU takes runs this, once.
 * @param cpu Guest CPU state.
 * @param final_wait Nonzero when the yield ends a psyq_VSync wait. */
static void psyq_vbl_body(CPUState* cpu, int final_wait) {
    SDL_AddAtomicInt(&openpete_vbl, 1);
    vbl_device_head();
    vbl_cpu_interrupt(cpu, openpete_sched_pos() + 1u, final_wait);
    vbl_device_mmio();
    /* The card transfer the step started completes in the SIO0 interrupt,
     * after the VBLANK handlers and, in most VBLs, after the disc's. */
    openpete_psx_bios_vbl_complete(cpu);
    /* A disc read the drive would still be busy with: its completion is the
     * last thing this VBL delivers, after the frame's own callbacks. */
    cd_defer_tick(cpu, 1u);
}

/* --- VBL delivery -------------------------------------------------------
 * deliver_vbl is the one place a VBLANK reaches the guest. A source other
 * than psyq_VSync's own wait is held while the critical-section flag is
 * set: the device half runs, the position counts the VBLANK and D the drop,
 * and at most one VBLANK latches, keyed on the first held VBLANK's index.
 * The CPU half runs once when the flag clears (psyq_crit_flush). A taken
 * VBL sets the flag for its duration, as the exception runs masked, so a
 * delivery falling due inside one is latched and flushed on return. */
static const char* const k_vbl_src_name[VBL_SRC_N] = {
    "vsync", "charge", "spin", "loop", "in-call"};
static uint64_t s_dlv_taken[VBL_SRC_N], s_dlv_held[VBL_SRC_N];
static uint64_t s_dlv_flushes = 0;
static uint32_t s_dlv_since_wait = 0, s_dlv_since_wait_max = 0;  /* taken VBLs between two VSync waits */
static uint32_t s_hold_run = 0, s_hold_run_max = 0;             /* held VBLs before a flush */
static int      s_latched = 0;
static uint32_t s_latch_key = 0;        /* reference VBLANK index of the first held VBLANK */
static int      s_dlv_outer_crit = 1;   /* the interrupted code's flag while a delivery runs */

/* OPENPETE_VBL_HOLD=<source>:<skip>:<count>[:dry]: a diagnostic critical
 * section over a window of gated deliveries. After `skip` deliveries of
 * <source> (a vbl_src name, or "any") the next `count` are held as if the
 * flag were set, and the window's end flushes as ExitCriticalSection
 * would. With `dry` the same deliveries are counted and taken, and the
 * window's end records the same drive-head and VBL counters: the no-hold
 * leg a hold leg's head is compared against. */
static int      s_hw_parsed = 0;
static int      s_hw_dry = 0;
static int      s_hw_src = -1;          /* -1 off, VBL_SRC_N any */
static uint32_t s_hw_skip = 0, s_hw_count = 0;
static uint64_t s_hw_held = 0, s_hw_flushes = 0;
static uint64_t s_hw_head_after = 0;    /* drive-head time after the window's flush */
static uint32_t s_hw_dev_vbl = 0;       /* device VBL count there */
static uint32_t s_hw_vbl_before = 0, s_hw_vbl_after = 0;   /* openpete_vbl across the window */
static uint32_t s_hw_dev_before = 0;    /* openpete_vbl_device before the window's first VBLANK */
static uint64_t s_hw_flushes_before = 0;   /* s_dlv_flushes there */
static int      s_hw_d_before = 0;      /* D there */

/** @brief Parse OPENPETE_VBL_HOLD once. */
static void hold_window_parse(void) {
    if (s_hw_parsed) return;
    s_hw_parsed = 1;
    const char* e = std::getenv("OPENPETE_VBL_HOLD");
    if (!e || !*e) return;
    char name[16] = {0}, mode[8] = {0};
    unsigned skip = 0, count = 0;
    const int n = std::sscanf(e, "%15[^:]:%u:%u:%7s", name, &skip, &count, mode);
    if (n < 3 || (n == 4 && std::strcmp(mode, "dry"))) {
        OPL_WARN_F("delivery", "OPENPETE_VBL_HOLD=%s: expected <source>:<skip>:<count>[:dry]", e);
        return;
    }
    s_hw_dry = (n == 4);
    if (!std::strcmp(name, "any")) s_hw_src = VBL_SRC_N;
    for (int i = 1; i < VBL_SRC_N; i++) if (!std::strcmp(name, k_vbl_src_name[i])) s_hw_src = i;
    if (s_hw_src < 0) { OPL_WARN_F("delivery", "OPENPETE_VBL_HOLD: unknown source %s", name); return; }
    s_hw_skip = skip;
    s_hw_count = count;
    OPL_INFO_F("delivery", "hold window: %s after %u, %u VBL(s)%s", name, skip, count,
               s_hw_dry ? ", dry (taken)" : "");
}

/** @brief Report whether the diagnostic hold window holds this gated delivery, consuming it.
 *
 * A dry window consumes the delivery the same way and returns 0.
 * @param src The delivery's source.
 * @return 1 when the delivery is held by the window. */
static int hold_window_takes(vbl_src src) {
    hold_window_parse();
    if (s_hw_src < 0 || !s_hw_count) return 0;
    if (s_hw_src != VBL_SRC_N && s_hw_src != (int)src) return 0;
    if (s_hw_skip) { s_hw_skip--; return 0; }
    if (!s_hw_held) {
        s_hw_vbl_before = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
        s_hw_dev_before = (uint32_t)SDL_GetAtomicInt(&openpete_vbl_device);
        s_hw_flushes_before = s_dlv_flushes;
        s_hw_d_before = openpete_sched_dropped();
    }
    s_hw_count--;
    s_hw_held++;
    return !s_hw_dry;
}

/** @brief Hold one VBLANK: run its device half, count it on the position and in D, latch it.
 * @param src The delivery's source. */
static void hold_vbl(vbl_src src) {
    vbl_device_head();
    vbl_device_mmio();
    openpete_vbl_leaf_begin();
    cd_defer_countdown(1u);
    openpete_vbl_leaf_end("countdown");
    if (!s_latched) { s_latched = 1; s_latch_key = openpete_sched_pos() + 1u; }
    if (openpete_sched_active()) { openpete_sched_advance_pos(1); openpete_sched_drop_vbl(); }
    s_dlv_held[src]++;
    if (++s_hold_run > s_hold_run_max) s_hold_run_max = s_hold_run;
}

/** @brief Take the VBLANK latched while interrupts were masked: the CPU half, once.
 *
 * The held VBLANKs already ran the device half and advanced the position,
 * so the flush advances `openpete_vbl` by one, runs the card step and the
 * pad command keyed on the first held VBLANK, the yield and the VSync
 * slots, and the drive events at the current position (cd_defer_due with 0
 * VBLs ahead); a completion that fell due during the hold is delivered
 * here and its closing CdlPause owes its full count. D drops by one. Every
 * register is preserved. Does nothing without a latched VBLANK.
 * @param cpu Guest CPU state at the call that cleared the flag. */
extern "C" void psyq_crit_flush(CPUState* cpu) {
    if (!s_latched) return;
    s_latched = 0;
    s_hold_run = 0;
    s_dlv_flushes++;
    openpete_guest_regs_t saved;
    openpete_guest_regs_save(cpu, &saved);
    openpete_crit_set(1);
    s_dlv_depth++;
    openpete_loop_vbl();
    SDL_AddAtomicInt(&openpete_vbl, 1);
    vbl_cpu_interrupt(cpu, s_latch_key, 0);
    openpete_psx_bios_vbl_complete(cpu);
    cd_defer_due(cpu, 0u, 0u);
    if (openpete_sched_active()) openpete_sched_take_dropped();
    s_dlv_depth--;
    openpete_crit_set(0);
    openpete_guest_regs_restore(cpu, &saved);
}

/** @brief Assert the clocks and the drive head at the hold window's end against a window of the same VBLANKs taken.
 *
 * Every VBLANK of the window ran the device half once, so the device VBL
 * count advanced by the window's length and the drive head, which reads
 * device time, sits at that count. A held window advanced `openpete_vbl`
 * by one (the flush), flushed once and, under the schedule, left D higher
 * by the window's length less one; a dry window took each VBLANK, so
 * `openpete_vbl` advanced by its length with no flush and D unchanged. A
 * held window whose end found the flag set has not flushed and is not
 * checked. Logs one verdict line. */
static void hold_window_check(void) {
    const uint32_t n = (uint32_t)s_hw_held;
    if (!s_hw_dry && openpete_crit_masked()) {
        OPL_INFO_F("delivery", "hold window check: SKIP, the flag is set at the window's end (no flush yet)");
        return;
    }
    const uint32_t dev_want = s_hw_dev_before + n;
    const uint64_t head_want = (uint64_t)dev_want * CD_HEAD_TU;
    const uint32_t vbl_want = s_hw_dry ? n : 1u;
    const uint64_t flush_want = s_hw_dry ? 0u : 1u;
    const int d_want = (s_hw_dry || !openpete_sched_active()) ? 0 : (int)n - 1;
    const uint32_t vbl_got = s_hw_vbl_after - s_hw_vbl_before;
    const uint64_t flush_got = s_dlv_flushes - s_hw_flushes_before;
    const int d_got = openpete_sched_dropped() - s_hw_d_before;
    const int ok = s_hw_dev_vbl == dev_want && s_hw_head_after == head_want && vbl_got == vbl_want &&
                   flush_got == flush_want && d_got == d_want;
    OPL_INFO_F("delivery", "hold window check: %s device vbl %u -> %u (want %u), head t=%llu (want %llu), "
               "vbl +%u (want +%u), flushes +%llu (want +%llu), D +%d (want +%d)",
               ok ? "PASS" : "FAIL", s_hw_dev_before, s_hw_dev_vbl, dev_want,
               (unsigned long long)s_hw_head_after, (unsigned long long)head_want, vbl_got, vbl_want,
               (unsigned long long)flush_got, (unsigned long long)flush_want, d_got, d_want);
}

/** @brief End the diagnostic hold window when its last VBLANK was consumed: flush as ExitCriticalSection would (not for a dry window), and record the drive head and the VBL counters there.
 *
 * The head line carries every input of the head's position (time, base
 * LBA, start time, rate) and the position, so a hold leg and its dry leg
 * compare field by field.
 * @param cpu Guest CPU state. */
static void hold_window_close(CPUState* cpu) {
    if (s_hw_src < 0 || s_hw_count || !s_hw_held || s_hw_flushes) return;
    if (!s_hw_dry && !openpete_crit_masked()) psyq_crit_flush(cpu);
    s_hw_flushes++;
    s_hw_vbl_after = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
    s_hw_dev_vbl = (uint32_t)SDL_GetAtomicInt(&openpete_vbl_device);
    s_hw_head_after = cd_head_now();
    OPL_INFO_F("delivery", "hold window closed: %llu %s, flush at device vbl %u, vbl %u -> %u, "
               "head t=%llu lba=%u, D=%d",
               (unsigned long long)s_hw_held, s_hw_dry ? "taken (dry)" : "held", s_hw_dev_vbl,
               s_hw_vbl_before, s_hw_vbl_after, (unsigned long long)s_hw_head_after, s_head_lba,
               openpete_sched_dropped());
    OPL_INFO_F("delivery", "hold window head: device vbl %u pos=%lld base=%u start=%llu rate=%u",
               s_hw_dev_vbl, (long long)cd_head_at(s_hw_head_after, 1u), s_head_lba,
               (unsigned long long)s_head_t, s_head_rate);
    hold_window_check();
}

/** @brief Deliver one VBLANK from `src`: taken (one VBL body, the position advanced under a schedule) or, for a gated source while interrupts are masked, held.
 *
 * Restarts the polling-loop accumulator either way. A taken VBL from a
 * source other than psyq_VSync's wait preserves every register, since it
 * can land inside a guest loop; psyq_VSync's wait is a call boundary and
 * its final yield is where the host may load a snapshot.
 * @param cpu Guest CPU state.
 * @param src The delivery's source.
 * @param final_wait Nonzero when the VBL ends a psyq_VSync wait. */
static void deliver_vbl(CPUState* cpu, vbl_src src, int final_wait) {
    openpete_loop_vbl();
    if (src != VBL_SRC_VSYNC && (openpete_crit_masked() || hold_window_takes(src))) {
        hold_vbl(src);
        hold_window_close(cpu);
        return;
    }
    /* The interrupted code's flag: the outermost delivery keeps it in
     * s_dlv_outer_crit, which a snapshot taken at this delivery's yield
     * carries and a load replaces. */
    const int crit_nested = openpete_crit_masked();
    if (s_dlv_depth == 0) s_dlv_outer_crit = crit_nested;
    openpete_guest_regs_t saved;
    if (src != VBL_SRC_VSYNC) openpete_guest_regs_save(cpu, &saved);
    openpete_crit_set(1);
    s_dlv_depth++;
    psyq_vbl_body(cpu, final_wait);
    if (openpete_sched_active()) openpete_sched_advance_pos(1);
    s_dlv_depth--;
    const int crit = (s_dlv_depth == 0) ? s_dlv_outer_crit : crit_nested;
    openpete_crit_set(crit);
    if (src != VBL_SRC_VSYNC) openpete_guest_regs_restore(cpu, &saved);
    s_dlv_taken[src]++;
    if (src == VBL_SRC_VSYNC) s_dlv_since_wait = 0;
    else if (++s_dlv_since_wait > s_dlv_since_wait_max) s_dlv_since_wait_max = s_dlv_since_wait;
    if (!crit) psyq_crit_flush(cpu);
    if (s_hw_dry) hold_window_close(cpu);
}

/* Polling-loop quanta off the schedule at counted polls: counted, not
 * delivered, until a guest cycle clock supplies their quantum. */
static uint64_t s_loop_counted_only = 0;
/* Polling-loop quanta under the schedule before the first anchor hit at a
 * pure spin or device-status poll: one VBL each, on openpete's own count. */
static uint64_t s_loop_pre_anchor = 0;

/** @brief Act on the quantum an emitted polling-loop back-edge reached.
 *
 * Under the schedule a quantum is a charge, never a count of its own: it
 * charges to the VBLANK the reference's rows show due before their next
 * row point (openpete_sched_loop_due), or delivers nothing. Before the
 * first anchor hit no row is due and the boot runs under openpete's own
 * count (sched_charge_to): a pure spin or a device-status poll there takes
 * one VBL, as off the schedule, since nothing the spin reads changes until
 * a VBLANK runs (a card driver's completion word, set by its interrupt
 * handler); a counted poll delivers nothing. Off the schedule a pure spin, where the
 * iteration a VBL lands on is unobservable, and a device-status poll, which
 * spins on a device the VBLs pass for, take one VBL; a counted poll, where
 * the static instruction count is not the CPU's time, is counted only.
 * @param cpu Guest CPU state, mid-loop.
 * @param pure The site kind: 1 pure spin, 2 device-status poll, 0 counted poll. */
static void loop_deliver(CPUState* cpu, uint32_t pure) {
    if (openpete_sched_active()) {
        uint32_t target = 0;
        if (openpete_sched_loop_due(openpete_loop_insns_since_anchor(), &target) &&
            target > openpete_sched_pos())
            sched_charge_to(cpu, target, "loop", VBL_SRC_LOOP);
        else if (pure && openpete_psxtrace_anchor_hits < openpete_psxtrace_anchor_hit_target) {
            s_loop_pre_anchor++;
            sched_charge_to(cpu, openpete_sched_pos() + 1u, "loop before anchor 0", VBL_SRC_LOOP);
        }
        return;
    }
    if (!pure) { s_loop_counted_only++; return; }
    deliver_vbl(cpu, VBL_SRC_LOOP, 0);
}

/** @brief Deliver the quantum an emitted polling-loop back-edge reached (psx_loop_poll), publishing the loop's header for the yields it takes.
 *
 * openpete_yield_loop_header names the header while loop_deliver runs and
 * returns to its previous value after, so a yield inside the delivery is
 * attributed to the loop and every other yield reads 0.
 * @param cpu Guest CPU state, mid-loop.
 * @param header The loop's header address.
 * @param pure The site kind: 1 pure spin, 2 device-status poll, 0 counted poll. */
extern "C" void psyq_deliver_loop_vbl(CPUState* cpu, uint32_t header, uint32_t pure) {
    const uint32_t outer_header = openpete_yield_loop_header;
    openpete_yield_loop_header = header;
    loop_deliver(cpu, pure);
    openpete_yield_loop_header = outer_header;
}

/** @brief Deliver one VBL for a device wait inside a kernel call.
 *
 * Held like any gated delivery while interrupts are masked; a synchronous
 * kernel wait entered masked waits forever on hardware too.
 * @param cpu Guest CPU state at the call. */
extern "C" void psyq_deliver_incall_vbl(CPUState* cpu) {
    deliver_vbl(cpu, VBL_SRC_INCALL, 0);
}

/** @brief Log the delivery census: taken and held VBLs per source, flushes, the per-run maxima, the critical-section probe, the loop accumulator, D and the hold window. */
extern "C" void psyq_vbl_delivery_report(void) {
    char taken[256], held[256];
    int nt = 0, nh = 0;
    for (int i = 0; i < VBL_SRC_N; i++) {
        nt += std::snprintf(taken + nt, sizeof taken - (size_t)nt, "%s%s=%llu", i ? " " : "",
                            k_vbl_src_name[i], (unsigned long long)s_dlv_taken[i]);
        nh += std::snprintf(held + nh, sizeof held - (size_t)nh, "%s%s=%llu", i ? " " : "",
                            k_vbl_src_name[i], (unsigned long long)s_dlv_held[i]);
    }
    OPL_INFO_F("delivery", "taken: %s", taken);
    OPL_INFO_F("delivery", "held: %s; flushes=%llu; max taken between waits=%u; max held run=%u",
               held, (unsigned long long)s_dlv_flushes, s_dlv_since_wait_max, s_hold_run_max);
    OPL_INFO_F("delivery", "VSync waits off the schedule by VBLs: 1=%llu 2=%llu 3=%llu 4+=%llu",
               (unsigned long long)s_vsync_waits_by_n[0], (unsigned long long)s_vsync_waits_by_n[1],
               (unsigned long long)s_vsync_waits_by_n[2], (unsigned long long)s_vsync_waits_by_n[3]);
    uint32_t ra = 0, enters = 0, exits = 0, leaves = 0; int32_t vbl = -1;
    openpete_crit_report(&ra, &vbl, &enters, &exits, &leaves);
    OPL_INFO_F("delivery", "crit: first enabled ra=0x%08X vbl=%d; enters=%u exits=%u kernel leaves=%u; "
               "masked VSync waits=%llu, masked VSync(-1) charges=%llu, masked at exit=%d",
               ra, vbl, enters, exits, leaves, (unsigned long long)s_masked_vsync_waits,
               (unsigned long long)s_masked_query_charges,
               s_dlv_depth ? s_dlv_outer_crit : openpete_crit_masked());
    uint64_t polls = 0, trips = 0, acc_max = 0; uint32_t qmax = 0, hdr = 0; int on = 1;
    openpete_loop_report(&polls, &trips, &acc_max, &qmax, &hdr, &on);
    OPL_INFO_F("delivery", "loop: polls=%llu quanta=%llu max insns between VBLs=%llu max quanta between VBLs=%u "
               "last header=0x%08X delivery=%s",
               (unsigned long long)polls, (unsigned long long)trips, (unsigned long long)acc_max, qmax, hdr,
               on ? "on" : "off");
    OPL_INFO_F("delivery", "loop quanta at counted polls off the schedule, counted only: %llu; "
               "at spins before anchor 0 under the schedule, one VBL each: %llu",
               (unsigned long long)s_loop_counted_only, (unsigned long long)s_loop_pre_anchor);
    openpete_vbl_leaf_report();
    openpete_loop_site_census_log();
    uint32_t hits[4], bad[4];
    openpete_sched_conv_report(hits, bad);
    OPL_INFO_F("delivery", "D=%d; conversions with D>0 (hits/bad): wait %u/%u, query %u/%u, row skip %u/%u, "
               "charge target %u/%u; vbl=%d device=%d",
               openpete_sched_dropped(), hits[0], bad[0], hits[1], bad[1], hits[2], bad[2], hits[3], bad[3],
               SDL_GetAtomicInt(&openpete_vbl), SDL_GetAtomicInt(&openpete_vbl_device));
    if (s_hw_src >= 0)
        OPL_INFO_F("delivery", "hold window%s: held=%llu flushes=%llu vbl %u -> %u device=%u head=%llu",
                   s_hw_dry ? " (dry, taken)" : "", (unsigned long long)s_hw_held, (unsigned long long)s_hw_flushes,
                   s_hw_vbl_before, s_hw_vbl_after, s_hw_dev_vbl, (unsigned long long)s_hw_head_after);
}

/** @brief Charge the VBLs the reference's tick spent before a callback registration at this call site, so the new callback does not run for them.
 *
 * Nothing is charged when the schedule has no registration row for the
 * caller's return address.
 * @param cpu Guest CPU state; `ra` names the registration site. */
static void sched_charge_registration(CPUState* cpu) {
    uint32_t target;
    if (openpete_sched_registration(cpu->ra, &target)) sched_charge_to(cpu, target, "callback registration");
}

/** @brief Thunk for libetc VSyncCallback: records the guest VSync callback address a0 for psyq_VSync to invoke.
 *
 * Charges the VBLs the schedule holds for the registration site first. The
 * callback is called once per VBL body from psyq_VSync; no host timer fires
 * it. v0 is 0.
 * @param cpu Guest CPU state; a0 is the callback address, 0 to clear. */
void psyq_VSyncCallback(CPUState* cpu) {
    /* Save the cb address; psyq_VSync invokes it once per call (there is
     * no real 60Hz timer - VSync()'s thunk fires it when the compiled main
     * loop yields). */
    sched_charge_registration(cpu);
    OPL_INFO_F("thunks", "VSyncCallback registered: 0x%08X", cpu->a0);
    g_vsync_cb = cpu->a0;
    cpu->v0 = 0u;
    psyq_game_mirror_vsync_callback(cpu, cpu->a0);
}

/** @brief Thunk for libetc StopCallback: calls PsyCross's StopCallback and returns its result in v0.
 *
 * The callback addresses the thunks recorded stay registered.
 * @param cpu Guest CPU state. */
void psyq_StopCallback(CPUState* cpu) {
    cpu->v0 = (uint32_t)StopCallback();
}

/** @brief Thunk for libetc ResetCallback: calls PsyCross's ResetCallback, and on the first call leaves the critical section.
 *
 * PsyCross's ResetCallback clears only its own host VSync callback pointer
 * and returns it truncated to 32 bits; the guest callbacks the thunks record
 * (the VSync, interrupt and DMA tables) are left registered. v0 is 0 unless
 * something set PsyCross's host pointer. libetc's ResetCallback runs
 * startIntr, which on its first call (interrupts not yet initialized) ends
 * in ExitCriticalSection (Spyro 2 0x800549F0, the `jal 0x8005AFF4` at
 * 0x80054AAC) and on later calls returns at once; the kernel enters the
 * executable masked, so this is where the game's interrupts are first
 * enabled. Its first call also runs _96_remove (A0 0x72), which closes the
 * five CD-ROM events the ROM's boot opened, freeing the event table's first
 * entries for the game's own events.
 * @param cpu Guest CPU state. */
void psyq_ResetCallback(CPUState* cpu) {
    static int s_intr_started = 0;
    cpu->v0 = (uint32_t)ResetCallback();
    if (!s_intr_started) {
        s_intr_started = 1;
        openpete_psx_bios_reset_callback_events();
        openpete_crit_leave(cpu);
    }
}

/* DMA completion handlers, by channel.
 *
 * A PSYQ transfer ends in a DMA interrupt, and libcd's read path finishes its
 * work in the channel-3 handler: it is what tells the game a read landed.
 * PsyCross performs the transfer host-side and raises no interrupt, so unless
 * the thunk layer records this handler and calls it when the transfer is done,
 * a game that waits on its own read-completion bookkeeping waits forever.
 *
 * PSX DMA has 7 channels; 3 is the CD-ROM. */
#define PSYQ_DMA_CHANNELS 7
static uint32_t g_dma_cb[PSYQ_DMA_CHANNELS];

/** @brief Thunk for libapi DMACallback: records the guest handler a1 for DMA channel a0.
 *
 * v0 is the handler the channel had, or 0 for a channel outside 0..6, which
 * records nothing. psyq_dma_complete calls the recorded handler.
 * @param cpu Guest CPU state. */
void psyq_DMACallback(CPUState* cpu) {
    uint32_t ch = cpu->a0;
    uint32_t prev = 0u;
    if (ch < PSYQ_DMA_CHANNELS) {
        prev = g_dma_cb[ch];
        g_dma_cb[ch] = cpu->a1;
        OPL_INFO_F("thunks", "DMACallback ch=%u -> 0x%08X", ch, cpu->a1);
    }
    cpu->v0 = prev;   /* PSYQ returns the handler it replaced */
}

/** @brief Call the guest's DMA completion handler for channel `ch`, if the game registered one.
 *
 * The handler receives the channel in a0 and 0 in a1.
 * @param cpu Guest CPU state.
 * @param ch DMA channel, 0..6; out of range calls nothing. */
static void psyq_dma_complete(CPUState* cpu, uint32_t ch) {
    if (ch < PSYQ_DMA_CHANNELS && g_dma_cb[ch]) {
        invoke_psx_callback(cpu, g_dma_cb[ch], ch, 0u);
    }
}

/* --- Level-load delay -----------------------------------------------------
 *
 * A data read lands in the frame that issued it; on hardware the drive holds
 * it in flight for many VBLs and the game waits, polling its own bookkeeping
 * from the main loop. openpete_cd_load_delay is the number of VBLs of guest
 * time every level load spends in that wait, the same on every machine: the
 * bytes still land at the read's issue unless the reference's per-sector
 * timing paces them (cd_land_sector), and what is deferred is the completion
 * the game waits for (the read callback, libcd's post-read status and the
 * channel-3 DMA handler), fired from the VBL body once the owed count has
 * elapsed.
 *
 * The budget opens at the first read of a load (psyq_game_cd_read_in_load
 * turning 1) and each read of the load takes up to the game's per-read cap
 * from it until it is spent; a game with no classifier leaves every read
 * instant. A second read issued while one is deferred (the game's retry
 * after its own timeout) completes the deferred one first, in order.
 *
 * The countdown, budget and pad state below is guest-timed and travels in the
 * latch pack: a rolled-back speculative tick must not leave a completion owed
 * on an abandoned branch. The sector-landing, first-data-response and
 * pending-command state (s_cd_land_*, s_cd_data_*, s_cd_pending_*) is not in
 * the pack. */
static uint32_t s_cd_defer_owed    = 0;  /* VBLs until the deferred read completes (fixed load delay) */
static uint32_t s_cd_defer_pending = 0;  /* 1 while a completion is deferred */
static uint32_t s_cd_defer_rec = 0, s_cd_defer_vbl = 0; /* the reference's completion, when scheduled */
static uint64_t s_cd_defer_clock = 0;    /* its clock value on the reference */
static uint32_t s_cd_load_budget   = 0;  /* VBLs the current load has left to spend */
static uint32_t s_cd_load_prev     = 0;  /* in_load of the previous read (edge) */
static uint32_t s_cd_defer_stream  = 0;  /* 1 while the deferred read is a stream read (drive-timed) */
static uint32_t s_cd_pause_owed    = 0;  /* VBLs until the closing CdlPause of a stream read completes */
static uint32_t s_cd_stream_reads  = 0;  /* stream reads timed by the drive model (engagement count) */

/* Stream reads (psyq_game_cd_read_streamed) run on a drive model instead of
 * the level-load budget: a read the game polls from its main loop while a
 * cutscene plays completes when a double-speed drive would deliver its last
 * sector, and libcd's closing CdlPause completes later still, which is what
 * the game's busy check (CdSync(1) == CdlComplete) waits for. The constants
 * are in CPU cycles (33868800 Hz) and are read off the reference's stream
 * reads (31-44 sectors, mode 0x80): one sector per 1/150 s after a fixed
 * 2.73 VBLs (the seek and rotational wait from a paused head, its mean;
 * the reference's spans 0.7-4.5), so the last sector's interrupt is seen
 * from the first VBL at or after that time; the CdlPause's second response
 * arrives 4 VBLs after the last sector's data-ready interrupt. */
#define CD_STREAM_SECTOR_CYCLES 225792u   /* 33868800 / 150 */
#define CD_STREAM_SEEK_CYCLES   1545510u  /* 2.73 VBL */
#define CD_STREAM_PAUSE_VBLS    4u

/** @brief Return the VBLs a stream read of `sectors` sectors stays in flight on the drive model, rounded up to a whole VBL.
 *
 * The VBL is 2250000/37609 Hz of the 33868800 Hz CPU clock, so a cycle
 * count c is c * 2250000 / (33868800 * 37609) VBLs.
 * @param sectors The read's sector count.
 * @return The VBLs from the issue to the last sector's arrival, at least 1. */
static uint32_t cd_stream_read_vbls(int sectors) {
    const uint64_t n = sectors > 0 ? (uint64_t)sectors : 0u;
    const uint64_t cycles = CD_STREAM_SEEK_CYCLES + n * CD_STREAM_SECTOR_CYCLES;
    const uint64_t den = 33868800ull * 37609ull;
    const uint64_t v = (cycles * 2250000ull + den - 1u) / den;
    return v ? (uint32_t)v : 1u;
}

/* libpad runs one controller command per vsync its handler runs on, so a
 * request that needs a command sequence keeps the pad in PadStateExecCmd
 * for the sequence's length and a second request meanwhile is refused.
 * The lengths are the DualShock's, in handler vsyncs from the request to
 * the first after which PadGetState reports PadStateStable again, read
 * off the reference's port: actuator alignment 0x43 (enter
 * configuration), 0x4D, 0x43 0x43 (leave), two polls; main-mode change
 * 0x43, 0x44, 0x4D, 0x43 0x43, two polls. The handler writes the pad
 * buffer at each vsync from the response to the previous vsync's
 * command, and only a command the pad answered in its normal mode (a
 * poll, or the 0x43 that enters configuration) carries pad data: the
 * vsyncs after the commands sent in configuration mode (all but the
 * first and the last two of a sequence) leave the buffer as it is. */
#define PAD_ACTALIGN_VBLS 6u
#define PAD_MAINMODE_VBLS 7u
static uint32_t s_pad_busy_vbls = 0;     /* vsyncs left in the current command sequence */
static uint32_t s_pad_seq_vbls  = 0;     /* the sequence's length */
/** @brief Whether a pad command sequence is pending or running, during which a new request is refused.
 *
 * @return 1 while vsyncs of the sequence remain, else 0. */
static int pad_busy(void) { return s_pad_busy_vbls != 0u; }
/** @brief Whether PadGetState reports a running command sequence (PadStateExecCmd).
 *
 * The handler sends a sequence's first command at the first vsync after the
 * request; until then a pending request leaves the pad stable.
 * @return 1 once the first command is sent and vsyncs remain, else 0. */
static int pad_exec(void) { return s_pad_busy_vbls != 0u && s_pad_busy_vbls != s_pad_seq_vbls; }
/// @brief Count vbls VBLs off the running pad command sequence, stopping at 0.
static void pad_busy_tick(uint32_t vbls) { s_pad_busy_vbls = (s_pad_busy_vbls > vbls) ? s_pad_busy_vbls - vbls : 0u; }
/// @brief Start a pad command sequence of vbls VBLs.
static void pad_seq_start(uint32_t vbls) { s_pad_busy_vbls = s_pad_seq_vbls = vbls; }
/** @brief Count how many of the next `vbls` handler vsyncs write the pad buffer.
 *
 * With no sequence running every vsync writes it. Inside a sequence a vsync
 * writes it when the remaining count at it is the sequence length or one
 * less, or 2 or fewer; the other vsyncs follow commands sent in
 * configuration mode, which carry no pad data.
 * @param vbls Handler vsyncs to count over.
 * @return The number that write the buffer. */
static int pad_polls(uint32_t vbls) {
    int n = 0;
    for (uint32_t i = 0; i < vbls; i++) {
        uint32_t rem = (s_pad_busy_vbls > i) ? s_pad_busy_vbls - i : 0u;
        if (rem <= 2u || rem + 2u > s_pad_seq_vbls) n++;
    }
    return n;
}

/* OPENPETE_PAD_POLL_CENSUS=1: per yield VBLANK, whether the controller took
 * a command and whether the slots see the host's write or the last pad
 * data; a line per host write replaced by the last pad data while their
 * buttons or sticks differ (input the game never sees), and under the
 * oracle schedule a line per VBLANK the card holds while the reference's
 * controller log records a command. The totals print at exit. */
static struct {
    int on;                 /* -1 unread, 0 off, 1 on */
    unsigned long vbls, no_cmd, held, lost, ref_known, ref_differ;
} s_pp = { -1, 0, 0, 0, 0, 0, 0 };

/** @brief Print the pad poll census totals. */
static void pad_poll_census_report(void) {
    OP_ROLE_ASSERT(sim);
    OPL_INFO_F("pad-poll", "census: vbls=%lu no_command=%lu no_data=%lu lost_writes=%lu "
               "reference_known=%lu card_held_against_reference=%lu",
               s_pp.vbls, s_pp.no_cmd, s_pp.held, s_pp.lost, s_pp.ref_known, s_pp.ref_differ);
}

/** @brief Count one yield VBLANK's pad verdict for the census, before the host write is taken back.
 * @param cpu Guest CPU state; the pad buffer holds the host's write.
 * @param key Reference VBLANK index of this VBLANK.
 * @param pad_cmd Whether the controller takes a command on this vsync.
 * @param ref Whether the reference's controller log records a command at `key`; -1 unknown.
 * @param held Whether this vsync's slots see the last pad data instead of the host's write.
 * @param prev The buffer the slots see when `held`: the last pad data, else the buffer before the yield. */
static void pad_poll_census(CPUState* cpu, uint32_t key, int pad_cmd, int ref, int held, const uint8_t prev[8]) {
    if (s_pp.on < 0) {
        const char* e = getenv("OPENPETE_PAD_POLL_CENSUS");
        s_pp.on = (e && *e && *e != '0') ? 1 : 0;
        if (s_pp.on) atexit(pad_poll_census_report);
    }
    if (!s_pp.on) return;
    s_pp.vbls++;
    if (!pad_cmd) s_pp.no_cmd++;
    if (ref >= 0) {
        s_pp.ref_known++;
        if (ref && !pad_cmd) {
            s_pp.ref_differ++;
            OPL_INFO_F("pad-poll", "card held against the reference: key=%u", key);
        }
    }
    if (!held) return;
    s_pp.held++;
    if (!s_pad_buf0) return;
    uint8_t w[8];
    int same = 1;
    for (uint32_t k = 2; k < 8u; k++) {
        w[k] = (uint8_t)cpu->read_byte(s_pad_buf0 + k);
        if (w[k] != prev[k]) same = 0;
    }
    if (same) return;
    s_pp.lost++;
    OPL_INFO_F("pad-poll", "lost: vbl=%d key=%u e=%u j=%u shown=%02x%02x/%02x%02x%02x%02x "
               "write=%02x%02x/%02x%02x%02x%02x",
               SDL_GetAtomicInt(&openpete_vbl), key, openpete_consume, openpete_consume_write,
               prev[2], prev[3], prev[4], prev[5], prev[6], prev[7],
               w[2], w[3], w[4], w[5], w[6], w[7]);
}

extern "C" uint32_t openpete_psxtrace_anchor_hits;

/* The reference's completion VBLANK of the last command issued through
 * libcd (its last recorded interrupt), 0 when none is pending. A blocking
 * CdControl, or the CdSync(0) after a CdControlF, spins in libcd until the
 * drive's second response arrives; the VBLs of that spin pass there. */
static uint32_t s_cd_pending_done = 0, s_cd_pending_rec = 0;
/** @brief Whether an event the reference recorded in record `rec` at VBLANK `vbl` has happened at openpete's position `vbls` VBLANKs ahead of the current one.
 *
 * Every event of an earlier record has happened (its hash saw it); within
 * the event's own record it has once the position reaches its VBLANK. At the
 * anchor the current record has already advanced to the one starting there,
 * whose events all follow the hash, so no event of it has happened.
 * @param rec The event's record.
 * @param vbl The event's VBLANK on the reference.
 * @param vbls VBLANKs ahead of the current position.
 * @return 1 when the event has happened, else 0. */
static int cd_ref_seen(uint32_t rec, uint32_t vbl, uint32_t vbls) {
    uint32_t cur = openpete_sched_current_record();
    return cur > rec || (!s_cd_at_anchor && cur == rec && openpete_sched_pos() + vbls >= vbl);
}
/** @brief Complete the pending libcd command: its status byte becomes the second response's.
 *
 * The status is the reference's second response to the last libcd command,
 * from the golden's controller sidecar. */
static void cd_pending_complete(void) {
    s_cd_pending_done = 0;
    cd_mirror_status(openpete_cdrom_mmio_libcd_last_done_status());
}
/** @brief Charge the VBLs a pending libcd command still needs, then complete it.
 *
 * With a pending completion and an active schedule whose position is short
 * of the completion's VBLANK, advances the schedule to it (sched_charge_to,
 * logged under `what`); then applies the completion's status byte.
 * @param cpu Guest CPU state.
 * @param what Label for the charge. */
static void cd_charge_pending(CPUState* cpu, const char* what) {
    if (!s_cd_pending_done) return;
    uint32_t done = s_cd_pending_done;
    if (openpete_sched_active() && done > openpete_sched_pos()) sched_charge_to(cpu, done, what);
    cd_pending_complete();
}

/** @brief Deliver a data read's completion, the tail the game waits on.
 *
 * Fires the read callback: the guest handler registered through the thunk
 * or, when libcd is translated in the image, the one in the slot at
 * psyq_game_cd_read_cb_addr(). Then lets
 * the game check its bookkeeping (psyq_game_cd_read_done), records libcd's
 * closing CdlPause as the last command with the reference's response status
 * and completion VBLANK or else the idle status, clears the streaming latch,
 * and fires the channel-3 DMA handler. An undeferred read completes at
 * issue; a deferred one completes when its owed VBLs or the reference's
 * completion VBLANK are reached.
 * @param cpu Guest CPU state. */
static void cd_read_complete(CPUState* cpu) {
    OPL_INFO_F("cd", "read complete (hit=%u vbl=%d)", openpete_psxtrace_anchor_hits,
               SDL_GetAtomicInt(&openpete_vbl));
    uint32_t cb = g_cd_read_cb;
    if (cb == 0u) {
        /* libcd is translated in this game's image: CdReadCallback stored
         * the handler in guest RAM instead of reaching the thunk. */
        uint32_t slot = psyq_game_cd_read_cb_addr();
        if (slot != 0u) cb = cpu->read_word(slot);
    }
    invoke_psx_callback(cpu, cb, /*intr=CdlComplete=*/2u, 0u);
    psyq_game_cd_read_done(cpu, g_cd_read_cb);
    /* libcd's ready callback issues CdlPause after the last sector; the
     * command byte and the idle status are what the game reads afterwards.
     * Under the golden's controller sidecar the pause's completion is the
     * reference's: CdSync reports the read chain complete only from that
     * VBLANK. */
    g_CD_com = CdlPause;
    s_cd_reading = 0u;
    cd_mirror_com(CdlPause);
    {
        uint8_t ref_resp[16]; uint32_t ref_n = 0;
        if (openpete_cdrom_mmio_libcd_response(CdlPause, ref_resp, &ref_n) && ref_n >= 1u) {
            cd_mirror_status(ref_resp[0]);
            s_cd_pending_done = openpete_cdrom_mmio_libcd_last_done_vbl();
            s_cd_pending_rec  = openpete_cdrom_mmio_libcd_last_done_rec();
        } else cd_mirror_status(0x02u);
    }

    /* The transfer is complete; on hardware that is a channel-3 DMA interrupt,
     * and libcd's read path does its bookkeeping in that handler. */
    psyq_dma_complete(cpu, 3u);
}

/** @brief Return the VBLs a data read stays in flight: the level-load budget, capped per read.
 *
 * A read the game classifies in-load (psyq_game_cd_read_in_load) after one
 * it did not opens the budget at openpete_cd_load_delay VBLs. Each in-load
 * read takes up to psyq_game_cd_read_delay_cap() VBLs from it, or all of it
 * when the cap is 0; a read not in load clears the budget and is not
 * deferred.
 * @param cpu Guest CPU state.
 * @param sectors Sector count of the read, for the log line.
 * @return VBLs to defer the read's completion. */
static uint32_t cd_read_delay_for(CPUState* cpu, int sectors) {
    const uint32_t in_load = psyq_game_cd_read_in_load(cpu) ? 1u : 0u;
    if (in_load && !s_cd_load_prev) {
        s_cd_load_budget = (uint32_t)(openpete_cd_load_delay > 0
                                          ? openpete_cd_load_delay : 0);
        if (s_cd_load_budget)
            OPL_INFO_F("cd", "load delay: load opened at a %d-sector read, "
                       "%u VBL(s) to spend", sectors, s_cd_load_budget);
    }
    s_cd_load_prev = in_load;
    if (!in_load) { s_cd_load_budget = 0u; return 0u; }
    uint32_t d = s_cd_load_budget;
    const uint32_t cap = psyq_game_cd_read_delay_cap();
    if (cap && d > cap) d = cap;
    s_cd_load_budget -= d;
    return d;
}

/** @brief Return the VBLs a data read stays in flight off the schedule: the level-load budget, else the drive model for a stream read.
 *
 * A read the level-load budget does not defer and the game classifies as a
 * stream read (psyq_game_cd_read_streamed) is deferred by
 * cd_stream_read_vbls and marked, so its delivery starts the closing
 * CdlPause.
 * @param cpu Guest CPU state at the CdRead call.
 * @param sectors The read's sector count.
 * @return VBLs to defer the read's completion. */
static uint32_t cd_read_or_stream_delay_for(CPUState* cpu, int sectors) {
    uint32_t delay = cd_read_delay_for(cpu, sectors);
    if (delay || !psyq_game_cd_read_streamed(cpu)) return delay;
    delay = cd_stream_read_vbls(sectors);
    s_cd_defer_stream = 1u;
    s_cd_stream_reads++;
    OPL_INFO_F("cd", "stream read %u: %d sectors at 0x%08X complete in %u VBL(s), "
               "CdlPause %u later (vbl=%d)", s_cd_stream_reads, sectors,
               cpu->a1, delay, CD_STREAM_PAUSE_VBLS, SDL_GetAtomicInt(&openpete_vbl_device));
    return delay;
}

/** @brief Charge one VBL to a guest spin that polls a deferred disc read without calling VSync.
 *
 * The reference spent the read's VBLs, interrupts included, inside such a
 * spin. The dispatcher calls this every 2048 dispatches without a VBL. On
 * the schedule it charges one VBL when a read's completion is deferred;
 * otherwise nothing. Off the schedule a spin on the drive receives its VBLs
 * at its classified back-edge (psyq_deliver_loop_vbl, device-status poll).
 * @param cpu Guest CPU state.
 * @param addr Unused. */
extern "C" void psyq_sched_spin_tick(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!openpete_sched_active() || !s_cd_defer_pending || !openpete_sched_spin_due()) return;
    OPL_DEBUG_F("cd", "schedule: deferred read polled in a spin, 1 VBL passes (%u owed)", s_cd_defer_owed);
    sched_charge_to(cpu, openpete_sched_pos() + 1u, "spin", VBL_SRC_SPIN);
}

/* One VBL elapsed: a deferred completion owed nothing more fires now. */
/* The drive's first data response to a deferred read on the reference:
 * libcd's status byte takes its value from that VBLANK until the read
 * completes. */
static uint32_t s_cd_data_vbl = 0, s_cd_data_rec = 0; static uint8_t s_cd_data_status = 0;

/* A deferred read's sectors land one by one at the VBLANK the drive
 * delivered each on the reference (its data-ready interrupts), visible
 * from the frame after; until then the destination keeps its previous
 * bytes. The drained bytes wait in a host copy, the previous bytes are
 * put back, and each landing copies its sector in. Whatever has not
 * landed by the completion lands with it. */
#define CD_LAND_MAX 1024u
static uint8_t* s_cd_land_buf = NULL; static uint32_t s_cd_land_cap = 0;
static uint32_t s_cd_land_dst = 0, s_cd_land_sectors = 0, s_cd_land_next = 0;
static uint32_t s_cd_land_vbl[CD_LAND_MAX], s_cd_land_rec[CD_LAND_MAX];
/** @brief Copy sector k of the deferred read's host buffer into its guest destination.
 *
 * A destination range outside main RAM is skipped.
 * @param k Sector index within the read, below s_cd_land_sectors. */
static void cd_land_sector(uint32_t k) {
    uint8_t* ram = openpete_psx_get_ram();
    uint32_t phys = (s_cd_land_dst & 0x1FFFFFFFu) + k * 2048u;
    if (phys + 2048u <= 0x200000u) memcpy(ram + phys, s_cd_land_buf + k * 2048u, 2048u);
}
/// @brief Land every sector of the deferred read not yet landed, and end the landing.
static void cd_land_rest(void) {
    while (s_cd_land_next < s_cd_land_sectors) cd_land_sector(s_cd_land_next++);
    s_cd_land_sectors = 0;
}
/** @brief Land the deferred read's next sectors whose reference data-ready interrupt has happened.
 *
 * @param vbls VBLANKs ahead of the current position the check applies to;
 *        0 checks the current one. */
static void cd_land_tick(uint32_t vbls) {
    while (s_cd_land_next < s_cd_land_sectors && cd_ref_seen(s_cd_land_rec[s_cd_land_next], s_cd_land_vbl[s_cd_land_next], vbls))
        cd_land_sector(s_cd_land_next++);
}

/** @brief Apply the reference's drive interrupts that openpete's position has reached.
 *
 * Lands the deferred read's sectors, mirrors the first data response's
 * status byte, and completes a pending command. cd_defer_tick runs it at
 * every waited VBLANK and at the anchor, before the record's hash.
 * @param vbls VBLANKs ahead of the current position the check applies to. */
static void cd_events_tick(uint32_t vbls) {
    cd_land_tick(vbls);
    if (s_cd_data_vbl && cd_ref_seen(s_cd_data_rec, s_cd_data_vbl, vbls)) {
        cd_mirror_status(s_cd_data_status);
        s_cd_data_vbl = 0;
    }
    if (s_cd_pending_done && cd_ref_seen(s_cd_pending_rec, s_cd_pending_done, vbls)) cd_pending_complete();
}

/* A deferred completion whose record has passed, or whose clock value on
 * the reference precedes the row the call being answered was answered
 * from (openpete_sched_last_tick), landed before that call: delivered
 * before the call's answer is used. The callback's place among the
 * tick's own reads is not recorded; a following row of the record at the
 * game's witness site (a poll it makes only after seeing the callback)
 * places the callback before them, and it is delivered after the call. */
static void cd_defer_tick(CPUState* cpu, uint32_t vbls);
/** @brief Write one stderr line for a deferred read's delivery when OPENPETE_CD_DEFER_LOG is set.
 *
 * The line names the deciding path (before a call, after the witness
 * poll, at a VBL or anchor tick), the schedule's record and VBL position,
 * the completion's reference record and VBLANK, and the two clock values
 * the path compared (`clock=` the completion's, `at=` the schedule's last
 * row); two runs over captures that differ only in tick precision deliver
 * at equal positions unless a clock comparison flipped.
 * @param path The deciding path's name. */
static void cd_defer_log(const char* path) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = std::getenv("OPENPETE_CD_DEFER_LOG"); s_on = (e && *e && *e != '0'); }
    if (!s_on) return;
    fprintf(stderr, "[cd-defer] path=%s rec=%u pos=%u defer_rec=%u defer_vbl=%u owed=%u clock=%llu at=%llu\n", path,
            openpete_sched_current_record(), openpete_sched_pos(), s_cd_defer_rec, s_cd_defer_vbl, s_cd_defer_owed,
            (unsigned long long)s_cd_defer_clock, (unsigned long long)openpete_sched_last_tick());
}
/// @brief Complete the deferred read now: clear its deferral, land the remaining sectors, fire the callbacks.
static void cd_defer_deliver(CPUState* cpu) {
    s_cd_defer_vbl = s_cd_defer_rec = 0u; s_cd_defer_clock = 0u;
    s_cd_defer_pending = 0u;
    cd_land_rest();
    cd_read_complete(cpu);
}
/** @brief Deliver a deferred read's completion before a call whose answer postdates it.
 *
 * Delivers when the schedule's current record is past the completion's, or
 * is the same record and the row the call is answered from is at or after
 * the completion's clock value. Nothing happens unless a completion is
 * deferred to a reference VBLANK.
 * @param cpu Guest CPU state. */
static void cd_defer_before_call(CPUState* cpu) {
    if (!s_cd_defer_pending || !s_cd_defer_vbl) return;
    uint32_t cur = openpete_sched_current_record();
    uint64_t at = openpete_sched_last_tick();
    if (cur > s_cd_defer_rec || (cur == s_cd_defer_rec && at && s_cd_defer_clock && s_cd_defer_clock <= at)) {
        cd_defer_log("before_call");
        cd_defer_deliver(cpu);
    }
}
/** @brief Deliver a deferred read's completion after the call that shows the game has seen it.
 *
 * Delivers when the last CdSync poll on the schedule came from the game's
 * witness site (psyq_game_cd_done_witness_ra) at or after the completion's
 * clock value, in the completion's record. Nothing happens unless a
 * completion is deferred to a reference VBLANK with a clock value.
 * @param cpu Guest CPU state. */
static void cd_defer_after_call(CPUState* cpu) {
    if (!s_cd_defer_pending || !s_cd_defer_vbl || !s_cd_defer_clock) return;
    if (openpete_sched_current_record() != s_cd_defer_rec) return;
    uint32_t witness = psyq_game_cd_done_witness_ra(), ra; uint64_t tick;
    if (witness && openpete_sched_cdsync_peek(&ra, &tick) && ra == witness && tick >= s_cd_defer_clock) {
        cd_defer_log("after_call");
        cd_defer_deliver(cpu);
    }
}

/* VBLs of the last countdown beyond the deferred read's owed count: how
 * far the read's closing CdlPause has already run when the read completes
 * in the same VBL body. */
static uint32_t s_cd_defer_left = 0u;

/** @brief Count vbls elapsed VBLANKs off the deferred read's owed VBLs, or off its closing CdlPause.
 *
 * The device half of the deferral: the drive keeps working through a
 * VBLANK the CPU does not take. A read deferred to a reference VBLANK is
 * not counted here (cd_defer_due compares the position).
 * @param vbls VBLANKs elapsed. */
static void cd_defer_countdown(uint32_t vbls) {
    s_cd_defer_left = 0u;
    if (!s_cd_defer_pending) {
        s_cd_pause_owed = (vbls >= s_cd_pause_owed) ? 0u : s_cd_pause_owed - vbls;
        return;
    }
    if (s_cd_defer_vbl) return;
    s_cd_defer_left = (vbls > s_cd_defer_owed) ? vbls - s_cd_defer_owed : 0u;
    s_cd_defer_owed = (vbls >= s_cd_defer_owed) ? 0u : s_cd_defer_owed - vbls;
}

/** @brief Apply the reference's drive events and deliver the deferred read's completion when it is due.
 *
 * The CPU half of the deferral (libcd's interrupt handler does this work):
 * cd_events_tick lands sectors and mirrors libcd's status, then a
 * completion deferred to a reference VBLANK is due when the position
 * reaches it, and one deferred by a VBL count when the countdown reached
 * 0. Delivery lands the remaining sectors and fires the read callbacks; a
 * stream read's closing CdlPause starts with `left` of its VBLs already run.
 * @param cpu Guest CPU state.
 * @param vbls VBLs the position check looks ahead; 0 checks the current one.
 * @param left VBLs of the countdown beyond the owed count. */
static void cd_defer_due(CPUState* cpu, uint32_t vbls, uint32_t left) {
    cd_events_tick(vbls);
    if (!s_cd_defer_pending) return;
    if (s_cd_defer_vbl) {
        /* The completion callback ran at the reference's VBLANK inside its
         * record; it is delivered when the position reaches it there. */
        if (!cd_ref_seen(s_cd_defer_rec, s_cd_defer_vbl, vbls)) return;
        s_cd_defer_vbl = s_cd_defer_rec = 0u; s_cd_defer_clock = 0u;
        left = 0u;
    } else if (s_cd_defer_owed) {
        return;
    }
    cd_defer_log("tick");
    s_cd_defer_pending = 0u;
    cd_land_rest();
    cd_read_complete(cpu);
    /* A stream read's closing CdlPause is in flight from here; VBLs of this
     * advance past the completion count toward it. */
    if (s_cd_defer_stream) {
        s_cd_defer_stream = 0u;
        s_cd_pause_owed = (left >= CD_STREAM_PAUSE_VBLS) ? 0u : CD_STREAM_PAUSE_VBLS - left;
    }
}

/** @brief Advance the deferred read by vbls VBLs and deliver its completion when due.
 *
 * The countdown (cd_defer_countdown) then the drive events and the due
 * check (cd_defer_due) with the countdown's excess.
 * @param cpu Guest CPU state.
 * @param vbls VBLs the position advances by; 0 checks the current one. */
static void cd_defer_tick(CPUState* cpu, uint32_t vbls) {
    cd_defer_countdown(vbls);
    cd_defer_due(cpu, vbls, s_cd_defer_left);
}

/* --- The latch pack (psyq_thunk_state.h) --------------------------------- */

/** @brief Store `v` as word `i` of the pack at `p`, in host byte order.
 *
 * @param p The pack.
 * @param i Word index.
 * @param v The value. */
static void put_word(uint8_t* p, uint32_t i, uint32_t v) {
    memcpy(p + 4u * i, &v, 4);
}
/// @brief Read word i of the latch pack at p in host byte order.
static uint32_t get_word(const uint8_t* p, uint32_t i) {
    uint32_t v;
    memcpy(&v, p + 4u * i, 4);
    return v;
}

/** @brief Store the pad words of the latch pack: the card's hold at the previous VBLANK and the last pad data.
 *
 * Word 55 holds the card's hold (bit 0), whether the last pad data is set
 * (bit 1) and whether the previous vsync's command carried no pad data
 * (bit 2); words 56-57 the last pad data.
 * @param p The pack. */
static void pad_state_save(uint8_t* p) {
    put_word(p, 55, (s_pad_card_free_prev ? 0u : 1u) | (s_pad_last_ok ? 2u : 0u) | (s_pad_data_prev ? 0u : 4u));
    uint32_t w0, w1;
    memcpy(&w0, s_pad_last, 4);
    memcpy(&w1, s_pad_last + 4, 4);
    put_word(p, 56, w0);
    put_word(p, 57, w1);
}

/** @brief Load the pad words of a latch pack of version ver.
 *
 * A pack before version 11 predates the card's hold on SIO0 and the last
 * pad data: the previous VBLANK left SIO0 free, and a vsync without data
 * keeps the buffer as the game left it until the next fresh one. A pack
 * without bit 2 of word 55 had pad data on its previous vsync's command.
 * @param p The pack.
 * @param ver Its pack_ver. */
static void pad_state_load(const uint8_t* p, uint32_t ver) {
    const uint32_t w = (ver >= 11u) ? get_word(p, 55) : 0u;
    s_pad_card_free_prev = (w & 1u) ? 0u : 1u;
    s_pad_last_ok = (w & 2u) ? 1u : 0u;
    s_pad_data_prev = (w & 4u) ? 0u : 1u;
    const uint32_t w0 = (ver >= 11u) ? get_word(p, 56) : 0u, w1 = (ver >= 11u) ? get_word(p, 57) : 0u;
    memcpy(s_pad_last, &w0, 4);
    memcpy(s_pad_last + 4, &w1, 4);
}

extern "C" uint32_t psyq_thunk_state_size(void) { return PSYQ_THUNK_STATE_BYTES; }

extern "C" void psyq_thunk_state_save(void* dst) {
    uint8_t* p = (uint8_t*)dst;
    put_word(p, 0, PSYQ_THUNK_STATE_PACK_VER);
    put_word(p, 1, g_cd_read_cb);
    put_word(p, 2, g_vsync_cb);
    put_word(p, 3, (uint32_t)s_cd_mode);
    put_word(p, 4, (uint32_t)s_xa_filter_file);
    put_word(p, 5, (uint32_t)s_xa_filter_chan);
    for (uint32_t i = 0; i < PSYQ_INTR_SLOTS; i++)
        put_word(p, 6u + i, g_intr_cb[i]);
    for (uint32_t i = 0; i < PSYQ_DMA_CHANNELS; i++)
        put_word(p, 6u + PSYQ_INTR_SLOTS + i, g_dma_cb[i]);
    put_word(p, 24, (uint32_t)s_cd_setloc_sector);
    put_word(p, 25, s_cd_reading);
    put_word(p, 26, s_cd_defer_owed);
    put_word(p, 27, s_cd_defer_pending);
    put_word(p, 28, s_cd_load_budget);
    put_word(p, 29, s_cd_load_prev);
    put_word(p, 30, s_pad_busy_vbls);
    for (uint32_t i = 0; i < PSX_BIOS_CARD_STATE_WORDS; i++) put_word(p, 31u + i, 0u);
    put_word(p, 38, s_pad_seq_vbls);
    put_word(p, 39, s_cd_defer_rec);
    put_word(p, 40, s_cd_defer_vbl);
    put_word(p, 41, (uint32_t)s_cd_defer_clock);
    put_word(p, 42, (uint32_t)(s_cd_defer_clock >> 32));
    put_word(p, 43, s_cd_defer_stream);
    put_word(p, 44, s_cd_pause_owed);
    put_word(p, 45, (uint32_t)s_spu_clk_frac);
    put_word(p, 46, s_head_lba);
    put_word(p, 47, (uint32_t)s_head_t);
    put_word(p, 48, (uint32_t)(s_head_t >> 32));
    put_word(p, 49, s_head_rate);
    put_word(p, 50, s_head_speed);
    put_word(p, 51, (uint32_t)(s_dlv_depth ? s_dlv_outer_crit : openpete_crit_masked()));
    put_word(p, 52, (uint32_t)s_latched);
    put_word(p, 53, s_latch_key);
    put_word(p, 54, (uint32_t)openpete_sched_dropped());
    pad_state_save(p);
}

extern "C" uint32_t psyq_thunk_state_bytes_for(uint32_t ver) {
    if (ver == 1u) return PSYQ_THUNK_STATE_V1_BYTES;
    if (ver == 2u) return PSYQ_THUNK_STATE_V2_BYTES;
    if (ver == 3u) return PSYQ_THUNK_STATE_V3_BYTES;
    if (ver == 4u) return PSYQ_THUNK_STATE_V4_BYTES;
    if (ver == 5u) return PSYQ_THUNK_STATE_V5_BYTES;
    if (ver == 6u) return PSYQ_THUNK_STATE_V6_BYTES;
    if (ver == 7u) return PSYQ_THUNK_STATE_V7_BYTES;
    if (ver == 8u) return PSYQ_THUNK_STATE_V8_BYTES;
    if (ver == 9u || ver == 10u) return PSYQ_THUNK_STATE_V10_BYTES;
    if (ver == 11u) return PSYQ_THUNK_STATE_BYTES;
    return 0u;
}

/** @brief Widen the XA armed bit of a state written before the mode byte was mirrored to the drive mode it stood for.
 *
 * A version-1 latch pack, and Spyro 1's pre-pack savestate words, hold an
 * armed bit in the mode field. Spyro 1 sets exactly two modes, SF|RT|Speed
 * to stream and Speed for data, so the bit widens to those.
 * @param armed Nonzero when the stream was armed.
 * @return The drive mode byte. */
static uint8_t cd_mode_from_armed_bit(uint32_t armed) {
    return armed ? (uint8_t)(CdlModeRT | CdlModeSF | CdlModeSpeed)
                 : (uint8_t)CdlModeSpeed;
}

/** @brief Load the drive head from pack `p` of version `ver`.
 *
 * A pack before version 8 holds no head: a running stream read (bit 0 of
 * the reading latch) under a CdlModeRT mode was music, and its head is
 * rebuilt at double speed from the stream's anchor in the core CD section
 * (loaded before the pack), without a seek; otherwise the head rests at
 * LBA 0.
 * @param p The pack.
 * @param ver Its version. */
static void cd_head_load(const uint8_t* p, uint32_t ver) {
    if (ver >= 8u) {
        s_head_lba   = get_word(p, 46);
        s_head_t     = (uint64_t)get_word(p, 48) << 32 | get_word(p, 47);
        s_head_rate  = get_word(p, 49);
        s_head_speed = get_word(p, 50);
        return;
    }
    s_head_speed = 2u;
    s_head_rate = 0u;
    s_head_lba = 0u;
    if (!(s_cd_reading & 1u) || !(s_cd_mode & CdlModeRT)) return;
    s_cd_reading |= 2u;
    s_head_lba = (uint32_t)g_cdXaStartLBA;
    s_head_t = (uint64_t)(uint32_t)g_cdXaStartVBL * CD_HEAD_TU;
    s_head_rate = 2u;
}

/** @brief Load the card driver words of a version 5-9 pack into the driver's guest RAM.
 *
 * From version 10 the driver lives in guest RAM and the state's RAM image
 * carries it; earlier packs leave that RAM as the image holds it.
 * @param p The pack.
 * @param ver Its version. */
static void card_state_load_legacy(const uint8_t* p, uint32_t ver) {
    if (ver < 5u || ver > 9u) return;
    uint32_t card[PSX_BIOS_CARD_STATE_WORDS];
    for (uint32_t i = 0; i < PSX_BIOS_CARD_STATE_WORDS; i++) card[i] = get_word(p, 31u + i);
    openpete_psx_bios_card_state_load_legacy(card);
}

extern "C" int psyq_thunk_state_load(const void* src, uint32_t size) {
    const uint8_t* p = (const uint8_t*)src;
    if (size < PSYQ_THUNK_STATE_V1_BYTES) return -1;
    const uint32_t ver = get_word(p, 0);
    if (ver < 1u || ver > PSYQ_THUNK_STATE_PACK_VER) return -1;
    if (size < psyq_thunk_state_bytes_for(ver)) return -1;
    g_cd_read_cb     = get_word(p, 1);
    g_vsync_cb       = get_word(p, 2);
    s_cd_mode        = (ver >= 2u) ? (uint8_t)get_word(p, 3)
                                   : cd_mode_from_armed_bit(get_word(p, 3));
    s_xa_filter_file = (int)get_word(p, 4);
    s_xa_filter_chan = (int)get_word(p, 5);
    for (uint32_t i = 0; i < PSYQ_INTR_SLOTS; i++)
        g_intr_cb[i] = get_word(p, 6u + i);
    for (uint32_t i = 0; i < PSYQ_DMA_CHANNELS; i++)
        g_dma_cb[i] = get_word(p, 6u + PSYQ_INTR_SLOTS + i);
    if (ver >= 2u) s_cd_setloc_sector = (int)get_word(p, 24);
    /* A pack without the latch predates the status mirror: idle is the value
     * that read the same as an unwritten block. */
    s_cd_reading = (ver >= 3u) ? get_word(p, 25) : 0u;
    /* A pack without the load-delay words predates it: nothing deferred. */
    s_cd_defer_owed    = (ver >= 4u) ? get_word(p, 26) : 0u;
    s_cd_defer_pending = (ver >= 4u) ? get_word(p, 27) : 0u;
    s_cd_load_budget   = (ver >= 4u) ? get_word(p, 28) : 0u;
    s_cd_load_prev     = (ver >= 4u) ? get_word(p, 29) : 0u;
    /* A pack without the pad and card countdowns predates them: idle. */
    s_pad_busy_vbls    = (ver >= 5u) ? get_word(p, 30) : 0u;
    s_pad_seq_vbls     = (ver >= 5u) ? get_word(p, 38) : 0u;
    s_cd_defer_rec     = (ver >= 5u) ? get_word(p, 39) : 0u;
    s_cd_defer_vbl     = (ver >= 5u) ? get_word(p, 40) : 0u;
    s_cd_defer_clock   = (ver >= 5u) ? ((uint64_t)get_word(p, 42) << 32 | get_word(p, 41)) : 0u;
    /* A pack without the stream-read words predates the drive model: none owed. */
    s_cd_defer_stream  = (ver >= 6u) ? get_word(p, 43) : 0u;
    s_cd_pause_owed    = (ver >= 6u) ? get_word(p, 44) : 0u;
    /* A pack without the SPU period's remainder predates the exact period:
     * the next VBL starts a fresh one. */
    s_spu_clk_frac     = (ver >= 7u) ? get_word(p, 45) : 0u;
    cd_head_load(p, ver);
    card_state_load_legacy(p, ver);
    /* A pack without the critical-section words predates the mask: the
     * interrupted code ran with interrupts enabled and nothing latched. */
    const int crit = (ver >= 9u) ? (get_word(p, 51) != 0u) : 0;
    if (s_dlv_depth) s_dlv_outer_crit = crit;
    else openpete_crit_set(crit);
    s_latched   = (ver >= 9u) ? (int)get_word(p, 52) : 0;
    s_latch_key = (ver >= 9u) ? get_word(p, 53) : 0u;
    openpete_sched_set_dropped((ver >= 9u) ? (int32_t)get_word(p, 54) : 0);
    pad_state_load(p, ver);
    return 0;
}

extern "C" void psyq_thunk_state_load_v4(uint32_t cd_cb, uint32_t vs_cb,
                                         uint32_t xa_word) {
    g_cd_read_cb     = cd_cb;
    g_vsync_cb       = vs_cb;
    s_cd_mode        = cd_mode_from_armed_bit(xa_word & 1u);
    s_xa_filter_file = (int)((xa_word >> 8) & 0xFF);
    s_xa_filter_chan = (int)((xa_word >> 16) & 0xFF);
}

extern "C" void psyq_thunk_state_scramble(void) {
    g_cd_read_cb     = 0xA5A50001u;
    g_vsync_cb       = 0xA5A50003u;
    s_cd_mode        = (uint8_t)~s_cd_mode;
    s_cd_setloc_sector = 0x00D5D5D5;
    s_cd_reading     = 0x3u;
    s_cd_defer_owed    = 0xD5D50005u;
    s_cd_defer_pending = 0x7u;
    s_cd_load_budget   = 0xD5D50009u;
    s_cd_load_prev     = 0xBu;
    s_pad_busy_vbls    = 0xD5D5000Du;
    s_pad_seq_vbls     = 0xD5D5000Fu;
    s_pad_card_free_prev     = s_pad_card_free_prev ? 0u : 1u;
    s_pad_data_prev          = s_pad_data_prev ? 0u : 1u;
    for (uint32_t k = 0; k < 8u; k++) s_pad_last[k] = (uint8_t)(s_pad_last[k] ^ (0x5Au + k));
    s_cd_defer_rec     = 0xD5D50011u;
    s_cd_defer_vbl     = 0xD5D50013u;
    s_cd_defer_clock   = 0xD5D50017D5D50015ull;
    s_cd_defer_stream  = 0x19u;
    s_cd_pause_owed    = 0xD5D5001Bu;
    s_spu_clk_frac     = 0xD5D5001Du;
    s_head_lba         = 0xD5D5001Fu;
    s_head_t           = 0xD5D50023D5D50021ull;
    s_head_rate        = 0x25u;
    s_head_speed       = 0x27u;
    if (s_dlv_depth) s_dlv_outer_crit = !s_dlv_outer_crit;
    else openpete_crit_set(!openpete_crit_masked());
    s_latched          = !s_latched;
    s_latch_key        = 0xD5D50029u;
    openpete_sched_set_dropped(openpete_sched_dropped() + 0x2B);
    s_xa_filter_file = 0x1FF;
    s_xa_filter_chan = 0x2FF;
    for (uint32_t i = 0; i < PSYQ_INTR_SLOTS; i++)
        g_intr_cb[i] = 0xB5B50001u + 2u * i;
    for (uint32_t i = 0; i < PSYQ_DMA_CHANNELS; i++)
        g_dma_cb[i] = 0xC5C50001u + 2u * i;
}

extern "C" int psyq_thunk_state_selftest(void) {
    uint8_t before[PSYQ_THUNK_STATE_BYTES], scrambled[PSYQ_THUNK_STATE_BYTES],
            after[PSYQ_THUNK_STATE_BYTES];
    psyq_thunk_state_save(before);
    psyq_thunk_state_scramble();
    psyq_thunk_state_save(scrambled);
    const int engaged = memcmp(before, scrambled, sizeof before) != 0;
    const int loaded = psyq_thunk_state_load(before, sizeof before) == 0;
    psyq_thunk_state_save(after);
    const int same = memcmp(before, after, sizeof before) == 0;
    /* The scrambled image must differ from the saved one in every field the
     * pack carries, or a field the scramble missed is one the check cannot
     * see. Field 0 is the version and is the same in both; the reserved
     * words are written as 0. */
    uint32_t fields_unchanged = 0;
    for (uint32_t i = 1; i < PSYQ_THUNK_STATE_WORDS; i++) {
        if (i >= PSYQ_THUNK_STATE_RESERVED_FIRST && i <= PSYQ_THUNK_STATE_RESERVED_LAST) continue;
        if (get_word(before, i) == get_word(scrambled, i)) fields_unchanged++;
    }
    const uint32_t fields = PSYQ_THUNK_STATE_WORDS - 1u
                          - (PSYQ_THUNK_STATE_RESERVED_LAST - PSYQ_THUNK_STATE_RESERVED_FIRST + 1u);
    const int ok = engaged && loaded && same && fields_unchanged == 0;
    OPL_INFO_F("savestate",
               "thunk-state selftest: %s (%u bytes, scramble changed %u/%u "
               "fields, load=%d, roundtrip=%s)",
               ok ? "PASS" : "FAIL", (unsigned)PSYQ_THUNK_STATE_BYTES,
               (unsigned)(fields - fields_unchanged), (unsigned)fields, loaded,
               same ? "identical" : "DIFFERS");
    return ok ? 0 : -1;
}

/** @brief Thunk for libetc InterruptCallback: records nothing and returns 0.
 *
 * PsyCross services those interrupt sources host-side and dispatches no
 * guest handler for them, so recording the address would promise a call that
 * never arrives. The 0 in v0 says there was no previous handler, which a
 * first registration also sees on hardware.
 * @param cpu Guest CPU state. */
void psyq_InterruptCallback(CPUState* cpu) { cpu->v0 = 0u; }
/** @brief Thunk for libetc VSyncCallbacks: installs a guest handler in one of the eight VSync slots.
 *
 * a0 selects the slot, a1 is the handler, and v0 is the handler it replaced.
 * Slot 4 is the VSyncCallback slot. The VBLs the reference spent before the
 * registration are charged first. A slot at or above 8 records nothing and
 * returns 0.
 * @param cpu Guest CPU state. */
void psyq_VSyncCallbacks(CPUState* cpu) {
    sched_charge_registration(cpu);
    uint32_t slot = cpu->a0;
    if (slot >= PSYQ_VSYNC_SLOTS) {
        OPL_WARN_F("thunks", "VSync callback slot %u out of range", slot);
        cpu->v0 = 0u;
        return;
    }
    /* The wording is matched by skip-target log tokens. */
    OPL_INFO_F("thunks", "interrupt callback line %u -> 0x%08X", slot, cpu->a1);
    uint32_t* const cell = (slot == PSYQ_VSYNC_SLOT_CB) ? &g_vsync_cb : &g_intr_cb[slot];
    const uint32_t prev = *cell;
    *cell = cpu->a1;
    cpu->v0 = prev;
}

/* --- libgpu -------------------------------------------------------------- */

/* psyq_libgpu_helpers ---------------------------------------------------- *
 * Real PSYQ libgpu MIPS code (statically linked into Spyro's EXE) writes to
 * a number of specific PSX-RAM addresses during ResetGraph / SetDef* /
 * PutDispEnv / etc. Our thunks delegate behaviour to PsyCross, which
 * manages host-side state without writing those PSX-side bytes. To keep the
 * --seed-ram + --halt-at-pc diff loop's "byte-identical with pcsx-redux"
 * invariant alive past GraphicsInitialize, we replicate the byte-level
 * writes here. Disassembly references in comments map to the real PSYQ at
 * 0x8005EA94+ in spyro-1/build/psx.elf. */

/** @brief Replay a real PSYQ function's prologue stores into the guest stack.
 *
 * A PSYQ function pushes a frame and stores ra plus some saved registers; an
 * empty C thunk leaves those slots at whatever was there before, which reads
 * as a RAM difference at every call. With the frame below sp, ra is written
 * at frame_size below sp plus ra_off and the first n_sregs of s0..s4 (at
 * most 5) at +16, +20, and so on; sp is left unchanged. The frame is per
 * function and per PSYQ revision, so the descriptor comes from the game
 * (psyq_game_stack_frame); a site the game declares no frame for mirrors
 * nothing.
 * @param cpu Guest CPU state.
 * @param site The PSYQ function whose prologue to replay. */
static void psyq_mirror_stack_frame(CPUState* cpu, psyq_frame_site_t site) {
    psyq_game_frame_t f;
    if (!psyq_game_stack_frame(site, &f)) return;

    const uint32_t sregs[5] = { cpu->s0, cpu->s1, cpu->s2, cpu->s3, cpu->s4 };
    uint32_t sp_b = cpu->sp;
    cpu->sp = sp_b - f.frame_size;
    cpu->write_word(cpu->sp + f.ra_off, cpu->ra);
    for (uint8_t i = 0; i < f.n_sregs && i < 5u; i++) {
        cpu->write_word(cpu->sp + 16u + 4u * i, sregs[i]);
    }
    cpu->sp = sp_b;
}

/** @brief Thunk for libgpu ResetGraph: PsyCross's ResetGraph, then the RAM writes of the real function.
 *
 * The game's psyq_game_mirror_resetgraph writes what libgpu's ResetGraph
 * leaves in its data (Spyro 1: ResetGraph at 0x8005F2A4 zeroes the
 * 128-byte environment block at 0x80074A64 and fills it for modes 0, 3 and
 * 5, gpu_type 0 for NTSC), and the prologue's stack stores are replayed. At
 * the first call, when OPENPETE_THUNK_STATE_SELFTEST is set and not "0", it
 * runs the latch-pack selftest and exits the process with 0 on pass, 3 on
 * failure.
 * @param cpu Guest CPU state; a0 is the mode, v0 receives PsyCross's
 *        result. */
void psyq_ResetGraph(CPUState* cpu) {
    int mode = (int)cpu->a0;
    cpu->v0 = (uint32_t)ResetGraph(mode);

    /* Harness: the latch-pack round trip runs once, at the first PSYQ boot
     * call every game makes, and the process exits on its verdict. */
    static bool s_selftest_done = false;
    if (!s_selftest_done) {
        s_selftest_done = true;
        const char* e = std::getenv("OPENPETE_THUNK_STATE_SELFTEST");
        if (e && *e && *e != '0') {
            const int rc = psyq_thunk_state_selftest();
            std::fflush(stdout); std::fflush(stderr);
            std::_Exit(rc == 0 ? 0 : 3);
        }
    }

    psyq_game_mirror_resetgraph(cpu, mode);

    /* ResetGraph's own MIPS prologue: sp -= 32; sw s0,16; sw s1,20; sw ra,24.
     * (sw s2 at +20 was overwritten by sw s1 at the same slot.) */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_RESETGRAPH);
}

/** @brief Thunk for libgpu SetDispMask: PsyCross's SetDispMask(a0), then the real function's stack stores.
 *
 * A0 of 0 blanks the display and any other value enables it. The stack
 * stores are those of the real function at 0x8005F6C8.
 * @param cpu Guest CPU state. */
void psyq_SetDispMask(CPUState* cpu) {
    SetDispMask((int)cpu->a0);
    /* Real SetDispMask @0x8005F6C8: sp -= 32; sw s0,16; sw s1,20; sw ra,24. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_SETDISPMASK);
}

/** @brief Thunk for libgpu SetGraphDebug: PsyCross's SetGraphDebug(a0) result in v0, then the real function's stack stores.
 *
 * PsyCross's SetGraphDebug does nothing and returns 0. The stack stores are
 * those of the real function at 0x8005F53C.
 * @param cpu Guest CPU state. */
void psyq_SetGraphDebug(CPUState* cpu) {
    cpu->v0 = (uint32_t)SetGraphDebug((int)cpu->a0);
    /* Real SetGraphDebug @0x8005F53C: sp -= 24; sw s0,16; sw ra,20. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_SETGRAPHDEBUG);
}

/** @brief Thunk for libgpu DrawSync: PsyCross's DrawSync(a0), then the real function's stack stores.
 *
 * @param cpu Guest CPU state; v0 receives the result. */
void psyq_DrawSync(CPUState* cpu) {
    cpu->v0 = (uint32_t)DrawSync((int)cpu->a0);
    /* Real DrawSync @0x8005F764: sp -= 24; sw s0,16; sw ra,20. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_DRAWSYNC);
}

/** @brief Replay libgpu ClearImage's writes to guest RAM: the clamped rectangle and the fill packet.
 *
 * libgpu's ClearImage clamps the rectangle's width and height in place to
 * 0..VRAM size - 1 from its environment block, then builds its fill packet
 * in the fill-packet block: for a rectangle whose x and width are multiples
 * of 64, a 5-word packet (E6, E1, the 02 fill, position, size); otherwise 8
 * words that open the whole draw area (E3, E4, E5, E6, E1), draw a 60 tile,
 * and chain to 3 words that restore the current draw area from the
 * environment block's DRAWENV. The restore words carry the same E3/E4/E5
 * payloads PutDrawEnv packs. The E1 payload is written as 0 (see the note in
 * the body). Nothing is written when the game names no environment or fill
 * block, or when the environment block's span runs past its region; when
 * only the fill block's span runs past its region the clamp is written and
 * the packet is not.
 * @param rect The guest rectangle, clamped in place.
 * @param color24 The fill colour, 0xBBGGRR. */
static void psyq_mirror_clear_image(RECT16* rect, uint32_t color24) {
    const uint32_t env = psyq_game_libgpu_env_addr();
    const uint32_t blk = psyq_game_libgpu_clear_block_addr();
    if (!env || !blk) return;
    const uint8_t* e = (const uint8_t*)SPAN(env, PSYQ_LIBGPU_ENV_SIZE);
    if (!e) return;
    int16_t vram_w, vram_h;
    memcpy(&vram_w, e + 4, 2);
    memcpy(&vram_h, e + 6, 2);
    if (rect->w < 0) rect->w = 0; else if (rect->w > vram_w - 1) rect->w = (short)(vram_w - 1);
    if (rect->h < 0) rect->h = 0; else if (rect->h > vram_h - 1) rect->h = (short)(vram_h - 1);
    /* libgpu takes the E1 payload from the GPU status register's low 11 bits
     * (the texture page the last E1 command set); openpete keeps no GPU
     * status word, and the reference reads zero at every anchor measured,
     * so zero is written. A ClearImage issued while those bits are set on
     * hardware is a named unknown here. */
    const uint32_t e1 = 0xE1000000u | ((color24 >> 31) << 10);
    const uint32_t xy = ((uint32_t)(uint16_t)rect->y << 16) | (uint16_t)rect->x;
    const uint32_t wh = ((uint32_t)(uint16_t)rect->h << 16) | (uint16_t)rect->w;
    uint32_t w[13];
    int n;
    if (((uint16_t)rect->x & 0x3Fu) == 0u && ((uint16_t)rect->w & 0x3Fu) == 0u) {
        w[0] = 0x05FFFFFFu; w[1] = 0xE6000000u; w[2] = e1;
        w[3] = 0x02000000u | (color24 & 0xFFFFFFu); w[4] = xy; w[5] = wh;
        n = 6;
    } else {
        const DRAWENV* cur = (const DRAWENV*)(e + 0x10);
        const int x1 = cur->clip.x + cur->clip.w - 1, y1 = cur->clip.y + cur->clip.h - 1;
        w[0] = 0x08000000u | ((blk + 0x24u) & 0xFFFFFFu);
        w[1] = 0xE3000000u; w[2] = 0xE4FFFFFFu; w[3] = 0xE5000000u; w[4] = 0xE6000000u;
        w[5] = e1; w[6] = 0x60000000u | (color24 & 0xFFFFFFu); w[7] = xy; w[8] = wh;
        w[9] = 0x03FFFFFFu;
        w[10] = 0xE3000000u | ((uint32_t)(cur->clip.y & 0x3FF) << 10) | (uint32_t)(cur->clip.x & 0x3FF);
        w[11] = 0xE4000000u | ((uint32_t)(y1 & 0x3FF) << 10) | (uint32_t)(x1 & 0x3FF);
        w[12] = 0xE5000000u | ((uint32_t)(cur->ofs[1] & 0x7FF) << 11) | (uint32_t)(cur->ofs[0] & 0x7FF);
        n = 13;
    }
    void* b = SPAN(blk, 4u * (uint32_t)n);
    if (b) memcpy(b, w, 4u * (uint32_t)n);
}

/** @brief Thunk for libgpu ClearImage: mirrors libgpu's RAM writes, then runs PsyCross's ClearImage on the guest RECT.
 *
 * a0 is the RECT, clamped in place; a1..a3 are r, g, b. A RECT span that
 * runs past its region returns -1 with nothing written.
 * @param cpu Guest CPU state. */
void psyq_ClearImage(CPUState* cpu) {
    RECT16* rect = (RECT16*)SPAN(cpu->a0, sizeof(RECT16));
    if (!rect) { cpu->v0 = (uint32_t)-1; return; }
    psyq_mirror_clear_image(rect, ((cpu->a3 & 0xFFu) << 16) | ((cpu->a2 & 0xFFu) << 8) | (cpu->a1 & 0xFFu));
    cpu->v0 = (uint32_t)ClearImage(rect,
                                   (u_char)cpu->a1,
                                   (u_char)cpu->a2,
                                   (u_char)cpu->a3);
    /* Real ClearImage @0x8005F8F8: sp -= 40; sw s0,16; s1,20; s2,24; s3,28; ra,32. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_CLEARIMAGE);
}

/** @brief Thunk for libgpu LoadImage: PsyCross's LoadImage from the guest RECT and the w * h 16-bit pixels at a1.
 *
 * v0 is -1 with no transfer when the RECT or the pixel span runs past its
 * region.
 * @param cpu Guest CPU state. */
void psyq_LoadImage(CPUState* cpu) {
    RECT16* r = (RECT16*)SPAN(cpu->a0, sizeof(RECT16));
    u_long* src = r ? (u_long*)SPAN(cpu->a1, psyq_image_bytes(r)) : nullptr;
    if (!src) { cpu->v0 = (uint32_t)-1; return; }
    cpu->v0 = (uint32_t)LoadImage(r, src);
}

/** @brief Thunk for libgpu StoreImage: PsyCross's StoreImage of the guest RECT into the w * h 16-bit pixels at a1.
 *
 * v0 is -1 with no transfer when the RECT or the pixel span runs past its
 * region.
 * @param cpu Guest CPU state. */
void psyq_StoreImage(CPUState* cpu) {
    RECT16* r = (RECT16*)SPAN(cpu->a0, sizeof(RECT16));
    u_long* dst = r ? (u_long*)SPAN(cpu->a1, psyq_image_bytes(r)) : nullptr;
    if (!dst) { cpu->v0 = (uint32_t)-1; return; }
    cpu->v0 = (uint32_t)StoreImage(r, dst);
}

/** @brief Thunk for libgpu MoveImage: PsyCross's MoveImage of the guest RECT to (a1, a2).
 *
 * v0 is -1 with no copy when the RECT runs past its region.
 * @param cpu Guest CPU state. */
void psyq_MoveImage(CPUState* cpu) {
    RECT16* r = (RECT16*)SPAN(cpu->a0, sizeof(RECT16));
    if (!r) { cpu->v0 = (uint32_t)-1; return; }
    cpu->v0 = (uint32_t)MoveImage(r, (int)cpu->a1, (int)cpu->a2);
}

/* The native renderer (--renderer vk) draws the scanned-out framebuffer; feed it
 * the display framebuffer (DISPENV disp) + the drawn region (DRAWENV clip, used
 * to crop the overscan). The calls are no-ops when it is disabled. */

/** @brief Thunk for libgpu PutDispEnv: copies the guest DISPENV into libgpu's environment block and hands it to PsyCross and the native renderer.
 *
 * A DISPENV span that runs past its region returns 0 with nothing done.
 * @param cpu Guest CPU state. */
void psyq_PutDispEnv(CPUState* cpu) OP_REQUIRES(sim) {
    DISPENV* env = (DISPENV*)SPAN(cpu->a0, sizeof(DISPENV));
    if (!env) { cpu->v0 = 0u; return; }
    /* libgpu keeps a copy of the current DISPENV in its environment block. */
    if (psyq_game_libgpu_env_addr()) {
        void* cur = SPAN(psyq_game_libgpu_env_addr() + 0x6Cu, 0x14u);
        if (cur) memcpy(cur, env, 0x14u);
    }
    PutDispEnv(env);
    openpete_native_set_display_rect(env->disp.x, env->disp.y, env->disp.w, env->disp.h);
    /* Real PutDispEnv @0x80060030: sp -= 40; sw s0,16; s1,20; s2,24; s3,28; ra,32. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_PUTDISPENV);
}

/** @brief Build the packet libgpu PutDrawEnv leaves in env->dr_env, in guest RAM.
 *
 * libgpu's PutDrawEnv packs the environment into env->dr_env before sending
 * it, and the packet stays in the caller's struct: a tag of 6 words, or 9
 * with the clear primitive when isbg is set, then E3 (draw area top-left),
 * E4 (bottom-right), E5 (offset), E1 (texture page, dither, draw-on-display),
 * E2 (texture window), E6 (mask), and the clear's colour, position and size.
 * PsyCross's PutDrawEnv copies the struct to a host variable and writes no
 * packet, so it is built here from the same fields.
 * @param env The guest DRAWENV. */
static void psyq_pack_draw_env(DRAWENV* env) {
    uint32_t code[9];
    int n = 0;
    const int x1 = env->clip.x + env->clip.w - 1;
    const int y1 = env->clip.y + env->clip.h - 1;
    code[n++] = 0xE3000000u | ((uint32_t)(env->clip.y & 0x3FF) << 10) | (uint32_t)(env->clip.x & 0x3FF);
    code[n++] = 0xE4000000u | ((uint32_t)(y1 & 0x3FF) << 10) | (uint32_t)(x1 & 0x3FF);
    code[n++] = 0xE5000000u | ((uint32_t)(env->ofs[1] & 0x7FF) << 11) | (uint32_t)(env->ofs[0] & 0x7FF);
    code[n++] = 0xE1000000u | (uint32_t)(env->tpage & 0x9FF) | ((uint32_t)(env->dtd & 1) << 9)
              | ((uint32_t)(env->dfe & 1) << 10);
    code[n++] = 0xE2000000u | ((uint32_t)((env->tw.y >> 3) & 0x1F) << 15)
              | ((uint32_t)((env->tw.x >> 3) & 0x1F) << 10)
              | ((uint32_t)(((-env->tw.h) >> 3) & 0x1F) << 5)
              | (uint32_t)(((-env->tw.w) >> 3) & 0x1F);
    code[n++] = 0xE6000000u;
    if (env->isbg) {
        code[n++] = 0x02000000u | ((uint32_t)env->b0 << 16) | ((uint32_t)env->g0 << 8) | env->r0;
        code[n++] = ((uint32_t)(env->clip.y & 0xFFFF) << 16) | (uint32_t)(env->clip.x & 0xFFFF);
        code[n++] = ((uint32_t)(env->clip.h & 0xFFFF) << 16) | (uint32_t)(env->clip.w & 0xFFFF);
    }
    const uint32_t tag = 0x00FFFFFFu | ((uint32_t)n << 24);
    uint8_t* p = (uint8_t*)&env->dr_env;
    memcpy(p, &tag, 4);
    for (int i = 0; i < n; i++) memcpy(p + 4 + 4 * i, &code[i], 4);
}

/** @brief Thunk for libgpu PutDrawEnv: packs the guest DRAWENV's packet, copies it into libgpu's environment block, and hands it to PsyCross and the native renderer.
 *
 * The span is PsyCross's DRAWENV, which PutDrawEnv copies whole. A span
 * that runs past its region returns 0 with nothing done.
 * @param cpu Guest CPU state. */
void psyq_PutDrawEnv(CPUState* cpu) OP_REQUIRES(sim) {
    DRAWENV* env = (DRAWENV*)SPAN(cpu->a0, sizeof(DRAWENV));
    if (!env) { cpu->v0 = 0u; return; }
    psyq_pack_draw_env(env);
    /* libgpu keeps a copy of the current DRAWENV in its environment block. */
    if (psyq_game_libgpu_env_addr()) {
        void* cur = SPAN(psyq_game_libgpu_env_addr() + 0x10u, 0x5Cu);
        if (cur) memcpy(cur, env, 0x5Cu);
    }
    PutDrawEnv(env);
    openpete_native_set_draw_clip(env->clip.x, env->clip.y, env->clip.w, env->clip.h);
    /* The draw area a framebuffer-window copy maps its source through: the clip
     * rect names the buffer, the drawing offset maps its texels to screen. */
    openpete_fb_snapshot_note_draw_env(env->clip.x, env->clip.y, env->clip.w, env->clip.h,
                                       env->ofs[0], env->ofs[1], env->isbg);
    /* The actually-cleared framebuffer background = the gamestate draw-env clear
     * colour (r0,g0,b0) when auto-clear (isbg) is on. This is the faithful bg for
     * the native renderer in EVERY state - unlike g_Cyclorama.m_BackgroundColor,
     * which equals the clear only on frames that draw the cyclorama and goes stale
     * during cyclorama-less states (e.g. the black level-name flyby, where the
     * clear is black but g_Cyclorama still holds the previous level's sky). */
    openpete_native_set_clear_color(env->isbg, env->r0, env->g0, env->b0);
    openpete_native_note_put_draw_env(env->isbg, env->r0, env->g0, env->b0);
    /* Real PutDrawEnv @0x8005FDD8: sp -= 32; sw s0,16; s1,20; s2,24; ra,28. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_PUTDRAWENV);
}

/* Hor+ widescreen (--aspect) and FOV are native-renderer display
 * policies (core/renderer/native_render.c); no GTE register is widened.
 *
 * The DISPENV / DRAWENV stay at their native PSX VRAM widths (typically
 * 512). Widening the VRAM-facing widths would make GR_StoreFrameBuffer
 * blit the GL render back into the right half of VRAM (x>=512), which
 * is where Spyro's texture pages live, causing texture corruption. The
 * GL ortho widening happens inside PsyCross via
 * g_psyx_render_aspect_factor so geometry projected to the wider
 * screen-X range is mapped across the full SDL window. */

/** @brief Thunk for libgpu SetDefDispEnv: PsyCross's SetDefDispEnv on the guest DISPENV.
 *
 * a0 is the DISPENV, a1..a3 x, y and w, and the word at sp+0x10 h. No
 * stack stores are replayed: the real SetDefDispEnv is a leaf.
 * @param cpu Guest CPU state. */
void psyq_SetDefDispEnv(CPUState* cpu) {
    DISPENV* env = (DISPENV*)SPAN(cpu->a0, sizeof(DISPENV));
    if (!env) { cpu->v0 = 0u; return; }
    SetDefDispEnv(env,
                  (int)cpu->a1, (int)cpu->a2, (int)cpu->a3,
                  (int)cpu->read_word(cpu->sp + 0x10));
    /* Real SetDefDispEnv @0x8005EB4C is a leaf - no stack pushes. */
}

/** @brief Thunk for libgpu SetDefDrawEnv: PsyCross's SetDefDrawEnv on the guest DRAWENV.
 *
 * a0 is the DRAWENV, a1..a3 x, y and w, and the word at sp+0x10 h. A
 * DRAWENV span that runs past its region returns 0 with nothing written.
 * @param cpu Guest CPU state. */
void psyq_SetDefDrawEnv(CPUState* cpu) {
    DRAWENV* env = (DRAWENV*)SPAN(cpu->a0, sizeof(DRAWENV));
    if (!env) { cpu->v0 = 0u; return; }
    SetDefDrawEnv(env,
                  (int)cpu->a1, (int)cpu->a2, (int)cpu->a3,
                  (int)cpu->read_word(cpu->sp + 0x10));
    /* Real SetDefDrawEnv @0x8005EA94: sp -= 40; sw s0,16; s1,20; s2,24;
     * s3,28; s4,32; ra,36. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_SETDEFDRAWENV);
}

/** @brief Thunk for libgpu SetDrawMode: builds the DR_MODE words host-side and stores them in native PSX layout.
 *
 * The texture-window RECT at sp+0x10 is optional; a nonzero one whose span
 * runs past its region leaves the primitive unwritten.
 * @param cpu Guest CPU state. */
void psyq_SetDrawMode(CPUState* cpu) {
    /* DR_MODE* p, int dfe, int dtd, int tpage, RECT16* tw - five args.
     *
     * The game's polybuf primitive is native PSX layout (4-byte P_TAG:
     * addr:24 | len:8, then code[] words). PsyCross is built with
     * USE_EXTENDED_PRIM_POINTERS, so calling SetDrawMode directly on the
     * game's memory writes `len` at offset 8 and code[0] at offset 12 (after
     * the 8-byte host-pointer header) - corrupting the PSX-layout DR_MODE:
     * its tag length stays stale and the mode word lands in the wrong place.
     * The OT walk then sees a garbage-length node, and when the stale code
     * byte happens to be 0x_1 it mis-runs as DR_MOVE -> out-of-bounds VRAM
     * copy. g4/poly prims are unaffected because the game builds those with
     * inline setXY/setRGB/setcode macros (correct PSX layout); only the
     * *function* call SetDrawMode is layout-wrong.
     *
     * So SetDrawMode runs into a host-local extended DR_MODE, and the
     * computed length + code words are written back in native PSX 4-byte-tag
     * layout. Only
     * the tag's len byte is touched; the addr/next 24 bits are set later by
     * the game's addPrim (recompiled inline -> already PSX layout). */
    const uint32_t tw_addr = cpu->read_word(cpu->sp + 0x10);
    RECT16* tw = (RECT16*)SPAN_OPT(tw_addr, sizeof(RECT16));
    if (tw_addr && !tw) return;
    DR_MODE tmp;
    SetDrawMode(&tmp, (int)cpu->a1, (int)cpu->a2, (int)cpu->a3, tw);
    uint32_t p = cpu->a0;
    /* libgpu's SetDrawMode stores a length of 2 (the E1 and E2 words);
     * PsyCross's setDrawMode macro stores 3, which the OT walk would send
     * as a third command word. */
    cpu->write_byte(p + 3, 2u);                     /* PSX tag len (high byte) */
    cpu->write_word(p + 4, (uint32_t)tmp.code[0]);  /* code[0] = draw mode */
    cpu->write_word(p + 8, (uint32_t)tmp.code[1]);  /* code[1] = tex window */
}

/* OT-translation scratch buffer.
 *
 * The recompiled MIPS code writes 4-byte PSX P_TAGs (`addr:24 | len:8`) and
 * 4-byte-aligned primitive payloads. PsyCross compiled with
 * USE_EXTENDED_PRIM_POINTERS=1 (the default on x86_64) expects 12-byte P_TAG
 * headers (`uintptr_t addr; uint16_t len; uint16_t pgxp_index`) followed by
 * the same payload - i.e. the payload format is identical, only the header
 * size differs. Walking the PSX chain with PsyCross macros dereferences a
 * 24-bit field as a host pointer and SEGVs.
 *
 * Each frame we rebuild the chain in host scratch memory: one PsyCross-style
 * P_TAG slot per PSX entry, payload memcpy'd verbatim, addr fields chained to
 * the next host slot, last slot pointed at PsyCross's `prim_terminator`. */
/* OT-walk scratch lives in the saved_ot_t buffer declared above; each
 * DrawOTag rebuilds it and the display walk parses it. */

/* Accumulating trace guard for the game-fiber thunks. The C header's
 * OP_PROF_ACCUM_SCOPE relies on the cleanup attribute, which is C-only; this is
 * the C++ equivalent. Inert (one load) unless a trace is open. */
namespace {
struct op_accum_scope {
    int id; int tok; uint64_t t0;
    /// @brief Open the region name when a trace is open; the destructor closes it with the elapsed microseconds.
    explicit op_accum_scope(const char* name) : id(-1), tok(0), t0(0) {
        if (op_prof_on()) { id = op_prof_intern(name); tok = op_prof_enter(id, 0u); t0 = op_prof_now_us(); }
    }
    /// @brief Close the region with the microseconds since construction, when a trace was open at construction.
    ~op_accum_scope() { if (id >= 0) op_prof_exit(tok, id, op_prof_now_us() - t0); }
};
}

/* ot_payload_bytes result for an entry whose data runs past its region. */
#define OT_PAYLOAD_UNMAPPED ((size_t)-1)

/** @brief Resolve one OT entry: read its P_TAG and map the tag with its length words.
 *
 * The entry's host pointer is valid for 4 + plen * 4 bytes, the tag and the
 * payload words its length byte names; an entry whose words run past the
 * end of RAM (or its presentation region) does not resolve.
 * @param addr Guest address of the entry.
 * @param tag Receives the P_TAG word.
 * @return The host pointer, or NULL when the entry does not map. */
static uint8_t* ot_entry_host(uint32_t addr, uint32_t* tag) {
    const uint8_t* t = (const uint8_t*)openpete_psx_to_host_span(addr, 4u);
    if (!t) return nullptr;
    std::memcpy(tag, t, 4);
    return (uint8_t*)openpete_psx_to_host_span(addr, 4u + ((*tag >> 24) & 0xFFu) * 4u);
}

/** @brief The payload bytes an OT entry carries after its tag.
 *
 * The PSX P_TAG length is 8-bit (at most 1020 bytes of payload), but a
 * variable-length primitive such as LoadImage (code 0xA0) embeds image data
 * after the struct, which the GPU reads by the rect's w x h. For a real
 * primitive (plen >= 3) whose code byte (offset 7: tag at 0..3, code at
 * 4..7 with the GP0 command in the high byte) is DR_LOAD, the payload
 * extends to 12 bytes of code plus w * h * 2 bytes of image. An OT anchor
 * (plen 0) carries stale bytes after its tag, which are not interpreted.
 * @param addr Guest address of the entry.
 * @param host The entry, valid for 4 + plen * 4 bytes.
 * @param plen The tag's length byte.
 * @return The payload bytes, or OT_PAYLOAD_UNMAPPED when a DR_LOAD's image
 *         data runs past the end of the entry's region. */
static size_t ot_payload_bytes(uint32_t addr, const uint8_t* host, uint8_t plen) {
    const size_t words = (size_t)plen * 4u;
    if (plen < 3 || (host[7] & 0xF0u) != 0xA0u) return words;
    uint32_t wh;
    std::memcpy(&wh, host + 4 + 8, 4);  /* code[2] @ offset tag(4)+code[0..1](8) */
    const size_t needed = 12u + (size_t)(wh & 0xFFFFu) * (size_t)((wh >> 16) & 0xFFFFu) * 2u;
    if (needed <= words) return words;
    if (needed > OPENPETE_PSX_RAM_SIZE) return OT_PAYLOAD_UNMAPPED;
    return openpete_psx_to_host_span(addr, (uint32_t)(4u + needed)) ? needed : OT_PAYLOAD_UNMAPPED;
}

/** @brief Report whether OPENPETE_FAST_BOOT_HOLD=1 keeps fast boot on past the OT that would end it.
 *
 * Harness only: holds the run in the fast-boot VBL path, where the host
 * loop never runs, so a quit request can be tested there. The variable is
 * read once; the first held OT logs one line.
 * @param prims The drawing primitives of the OT that would end fast boot.
 * @return Nonzero when the hold is set. */
static int fast_boot_hold(unsigned prims) {
    static int s_hold = -1;
    if (s_hold < 0) {
        const char* e = std::getenv("OPENPETE_FAST_BOOT_HOLD");
        s_hold = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        if (s_hold)
            OPL_WARN_F("boot", "OPENPETE_FAST_BOOT_HOLD: fast boot held at vbl=%d "
                       "(OT with %u drawing primitives)",
                       SDL_GetAtomicInt(&openpete_vbl), prims);
    }
    return s_hold;
}

/* The payload longs PsyCross's primitive parser consumes per fixed-size GP0
 * code, rows by the code's high nibble (0x00-0x70), columns by its low
 * nibble; 0 is an unknown code, submitted as an anchor. Row 0x00: NOP 3,
 * DR_MOVE 5. 0x20 flat polys: F3 4, FT3 7, F4 5, FT4 9. 0x30 gouraud
 * polys: G3 6, GT3 9, G4 8, GT4 12. 0x40 flat lines: F2 3, F3 5, F4 6.
 * 0x50 gouraud lines: G2 4, G3 7, G4 9. 0x60: TILE 3, SPRT 4, TILE_1 2.
 * 0x70: TILE_8 2, SPRT_8 3, TILE_16 2, SPRT_16 3. DR_LOAD (0xA0) and the
 * draw-environment packets (0xE0) are sized from the entry. */
static const uint8_t k_ot_prim_longs[8][16] = {
    /* 0x00 */ { 3, 5, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 },
    /* 0x10 */ { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 },
    /* 0x20 */ { 4, 4, 4, 4,  7, 7, 7, 7,  5, 5, 5, 5,  9, 9, 9, 9 },
    /* 0x30 */ { 6, 6, 6, 6,  9, 9, 9, 9,  8, 8, 8, 8, 12,12,12,12 },
    /* 0x40 */ { 3, 3, 0, 0,  0, 0, 0, 0,  5, 5, 0, 0,  6, 6, 0, 0 },
    /* 0x50 */ { 4, 4, 0, 0,  0, 0, 0, 0,  7, 7, 0, 0,  9, 9, 0, 0 },
    /* 0x60 */ { 3, 3, 3, 3,  4, 4, 4, 4,  2, 2, 2, 2,  0, 0, 0, 0 },
    /* 0x70 */ { 2, 2, 2, 2,  3, 3, 3, 3,  2, 2, 2, 2,  3, 3, 3, 3 },
};

/** @brief The env codes of a draw-environment packet (0xE0) that PsyCross's ProcessDrawEnv consumes.
 *
 * ProcessDrawEnv walks the tag's length in longs or stops at the first
 * word whose subtype is not 1..6, whichever comes first; PSYQ's macros
 * over-allocate the length (setDrawMode sets 3 and writes two words), so
 * the count of leading subtype-1..6 words, scanning at most 16, is the
 * length the outer walker lands on. The packets are kept rather than
 * dropped, so a DR_TPAGE or DR_MODE the game links mid-table still switches
 * the texture page.
 * @param host The entry, valid for 4 + plen * 4 bytes.
 * @param plen The tag's length byte.
 * @return The count. */
static uint16_t ot_env_longs(const uint8_t* host, uint8_t plen) {
    const uint32_t scan_max = plen > 16u ? 16u : (uint32_t)plen;
    uint16_t n = 0u;
    for (uint32_t i = 0; i < scan_max; ++i) {
        uint32_t code_word;
        std::memcpy(&code_word, host + 4 + i * 4u, 4);
        const uint32_t sub = (code_word >> 24) & 0x0Fu;
        if (sub == 0u || sub > 6u) break;
        ++n;
    }
    return n;
}

/** @brief The length field PsyCross's walker needs for an OT entry, derived from its GP0 code.
 *
 * The non-singlePrimitive ParsePrimitivesLinkedList advances by the
 * length its ProcessXxx returns and stops at the slot's length field; a
 * field that disagrees with the code's own length short-circuits the slot
 * or walks garbage as a fresh tag (a stray DR_LOAD crashes in LoadImage).
 * The game's tag length sometimes differs from PsyCross's fixed length, so
 * the field comes from the code byte: k_ot_prim_longs, the payload for
 * DR_LOAD, ot_env_longs for 0xE0, 0 (an anchor) for an anchor entry or an
 * unknown code.
 * @param host The entry, valid for 4 + plen * 4 bytes.
 * @param plen The tag's length byte.
 * @param payload_bytes The entry's payload bytes (ot_payload_bytes).
 * @return The length in longs, 0 for an anchor. */
static uint16_t ot_prim_longs(const uint8_t* host, uint8_t plen, size_t payload_bytes) {
    if (plen == 0u) return 0u;
    const uint8_t code = host[7];
    if (code < 0x80u) return k_ot_prim_longs[code >> 4][code & 0x0Fu];
    if ((code & 0xF0u) == 0xA0u) return (uint16_t)(payload_bytes / 4u);
    if ((code & 0xF0u) == 0xE0u) return ot_env_longs(host, plen);
    return 0u;
}

/* Primitive-code census over DrawOTag walks, logged at debug level and
 * reset once openpete_frame has advanced 60 since the last log: submitted
 * prims per (code & 0xF0) >> 4 (polys 0x20/0x30, lines 0x40/0x50, sprites
 * and tiles 0x60/0x70, DR_LOAD 0xA0, draw-env 0xE0), all submitted prims,
 * and anchor slots. */
static uint32_t s_ot_hist[16];
static uint32_t s_ot_anchor_slots, s_ot_total_slots, s_ot_hist_calls, s_ot_hist_logged;

/** @brief Logs the primitive-code census once 60 VBLs have passed since the last log, then resets it.
 *
 * Counts the call first. */
static void ot_hist_log(void) {
    s_ot_hist_calls++;
    if (openpete_frame < s_ot_hist_logged + 60u) return;
    s_ot_hist_logged = openpete_frame;
    OPL_DEBUG_F("thunks",
               "DrawOTag@f%u calls=%u prims=%u anchors=%u hist "
               "0x:%u 1x:%u 2x:%u 3x:%u 4x:%u 5x:%u 6x:%u 7x:%u "
               "8x:%u 9x:%u Ax:%u Bx:%u Cx:%u Dx:%u Ex:%u Fx:%u",
               openpete_frame, s_ot_hist_calls, s_ot_total_slots, s_ot_anchor_slots,
               s_ot_hist[0],  s_ot_hist[1],  s_ot_hist[2],  s_ot_hist[3],
               s_ot_hist[4],  s_ot_hist[5],  s_ot_hist[6],  s_ot_hist[7],
               s_ot_hist[8],  s_ot_hist[9],  s_ot_hist[10], s_ot_hist[11],
               s_ot_hist[12], s_ot_hist[13], s_ot_hist[14], s_ot_hist[15]);
    for (int i = 0; i < 16; i++) s_ot_hist[i] = 0;
    s_ot_anchor_slots = 0;
    s_ot_total_slots  = 0;
    s_ot_hist_calls   = 0;
}

/** @brief The OT-dump file for the current VBL, or NULL outside the dumped range.
 *
 * OPENPETE_OT_DUMP_FRAMES=A-B (or A) arms the dump for openpete_frame in
 * [A, B]; the file is OPENPETE_OT_DUMP_PATH (default /tmp/ot-dump.txt),
 * opened with a header line on the first dumped call. Each dumped call
 * steps *call. One row per slot: frame, slot, call, address, length, code
 * and up to 16 payload words.
 * @param call The dump's call counter.
 * @return The file, or NULL. */
static FILE* ot_dump_file(uint32_t* call) {
    static int   s_dump_lo = -2;
    static int   s_dump_hi = -1;
    static FILE* s_dump_fp = nullptr;
    if (s_dump_lo == -2) {
        const char* env = std::getenv("OPENPETE_OT_DUMP_FRAMES");
        s_dump_lo = -1;
        s_dump_hi = -1;
        int a = -1, b = -1;
        if (env && std::sscanf(env, "%d-%d", &a, &b) >= 1) {
            s_dump_lo = a;
            s_dump_hi = (b >= a) ? b : a;
        }
        if (s_dump_lo >= 0)
            OPL_INFO_F("thunks", "OT dump armed for frames [%d,%d]", s_dump_lo, s_dump_hi);
    }
    if (s_dump_lo < 0 || (int)openpete_frame < s_dump_lo || (int)openpete_frame > s_dump_hi)
        return nullptr;
    if (!s_dump_fp) {
        const char* path = std::getenv("OPENPETE_OT_DUMP_PATH");
        s_dump_fp = std::fopen(path ? path : "/tmp/ot-dump.txt", "w");
        OPL_INFO_F("thunks", "OT dump opened path=%s fp=%p frame=%u",
                   path ? path : "/tmp/ot-dump.txt", (void*)s_dump_fp, openpete_frame);
        if (s_dump_fp)
            std::fprintf(s_dump_fp, "# frame,slot,call,psx_addr,plen,code,payload_words(hex)\n");
    }
    if (s_dump_fp) ++*call;
    return s_dump_fp;
}

/** @brief Writes one OT-dump row: frame, slot, call, address, length, code and up to 16 payload words.
 *
 * @param dump The dump file.
 * @param slot The slot index.
 * @param call The dump's call counter.
 * @param addr The entry's guest address.
 * @param host The entry.
 * @param plen The tag's length byte.
 * @param payload_bytes The entry's payload bytes. */
static void ot_dump_row(FILE* dump, int slot, uint32_t call, uint32_t addr,
                        const uint8_t* host, uint8_t plen, size_t payload_bytes) {
    const uint8_t code_byte = (plen != 0u) ? host[7] : 0u;
    std::fprintf(dump, "%u,%d,%u,0x%08X,%u,0x%02X",
                 openpete_frame, slot, call, addr, (unsigned)plen, (unsigned)code_byte);
    uint32_t nwords = (uint32_t)(payload_bytes / 4u);
    if (nwords > 16u) nwords = 16u; /* cap; enough for poly_gt4 */
    for (uint32_t w = 0; w < nwords; ++w) {
        uint32_t word;
        std::memcpy(&word, host + 4 + w * 4u, 4);
        std::fprintf(dump, ",%08X", word);
    }
    std::fputc('\n', dump);
}

/** @brief Reports whether OPENPETE_OT_GARBAGE_TRACE enables the derailed-chain probe; read once.
 *
 * A chain that derails into non-OT memory yields a stray tag whose code
 * byte 0x_1 is misparsed as DR_MOVE, an unbounded VRAM copy; the probe logs
 * up to 80 tags longer than 32 longs (the largest real primitive, POLY_GT4,
 * is 12) with the link that led to them.
 * @return 1 when set and not "0". */
static int ot_garbage_trace(void) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* e = getenv("OPENPETE_OT_GARBAGE_TRACE");
        s_on = (e && *e && *e != '0') ? 1 : 0;
    }
    return s_on;
}

/** @brief The OPENPETE_OT_TRUNCATE slot limit, or -1 when unset; read once.
 *
 * With N set, a walk submits OT-dump slots 0..N-1 only, so an -og capture
 * shows what the PsyCross walk painted up to any slot.
 * @return The limit, or -1. */
static int ot_truncate_limit(void) {
    static int s_limit = -2;
    if (s_limit == -2) {
        const char* e = std::getenv("OPENPETE_OT_TRUNCATE");
        s_limit = (e && *e) ? std::atoi(e) : -1;
        if (s_limit >= 0)
            OPL_INFO_F("thunks", "OT truncate armed: submitting first %d slots only", s_limit);
    }
    return s_limit;
}

/** @brief Reports whether --render-paths drops an OT entry: a geometry prim of a path not selected.
 *
 * Presentation only: the prim is not submitted and the OT is not modified.
 * Control and environment entries (length 0, or codes outside 0x20-0x7F)
 * always pass, so the draw environment and image loads still apply.
 * @param addr The entry's guest address.
 * @param host The entry.
 * @param plen The tag's length byte.
 * @return 1 when dropped. */
static int ot_render_path_drops(uint32_t addr, const uint8_t* host, uint8_t plen) {
    if (plen == 0u || !psyq_game_render_paths_active()) return 0;
    const uint8_t hi = host[7] & 0xF0u;
    return hi >= 0x20u && hi <= 0x70u && !psyq_game_render_path_addr_visible(addr);
}

/** @brief Copies one OT entry into the next host scratch slot with the length field PsyCross's walker expects.
 *
 * The slot is a 12-byte PsyCross P_TAG (host link, filled by
 * ot_link_slots; length; PGXP index 0xFFFF, none) followed by the payload
 * verbatim. A known code counts toward openpete_primitives_drawn, the
 * census and, for codes 0x20-0x7F, *draw_prims; an unknown code or an
 * empty payload is an anchor.
 * @param B The scratch being built.
 * @param addr The entry's guest address.
 * @param host The entry.
 * @param plen The tag's length byte.
 * @param payload_bytes The entry's payload bytes.
 * @param draw_prims Counts the drawing prims submitted.
 * @return 0 when the scratch is full, else 1. */
static int ot_copy_slot(saved_ot_t* B, uint32_t addr, const uint8_t* host, uint8_t plen,
                        size_t payload_bytes, unsigned* draw_prims) {
    const size_t slot_size = 12u + payload_bytes;
    if (B->scratch_used + slot_size > OT_SCRATCH_SZ) {
        OPL_WARN_F("thunks", "DrawOTag scratch overflow at slot %d (PSX 0x%08X)",
                   B->slot_count, addr);
        return 0;
    }
    uint8_t* out = B->scratch + B->scratch_used;
    B->scratch_used += slot_size;
    B->slots[B->slot_count++] = out;

    const uint16_t lenField = ot_prim_longs(host, plen, payload_bytes);
    /* No PGXP per-primitive index is supplied; 0xFFFF marks none. A
     * content-keyed (sx,sy)->index map collides for vertices at the same
     * pixel, so an index needs a per-vertex PGXP memory. */
    const uint16_t pgxpField = 0xFFFFu;
    std::memcpy(out + 8,  &lenField,  2);
    std::memcpy(out + 10, &pgxpField, 2);
    if (payload_bytes == 0u || lenField == 0u) {
        if (payload_bytes != 0u) std::memcpy(out + 12, host + 4, payload_bytes);
        s_ot_anchor_slots++;
        return 1;
    }
    std::memcpy(out + 12, host + 4, payload_bytes);
    const uint8_t code_byte = host[7];
    openpete_primitives_drawn++;
    s_ot_hist[(code_byte & 0xF0u) >> 4u]++;
    s_ot_total_slots++;
    if (code_byte >= 0x20u && code_byte < 0x80u) (*draw_prims)++;
    return 1;
}

/** @brief Links each host slot to the next one's host address, the last to PsyCross's prim_terminator.
 *
 * @param B The scratch built this walk. */
static void ot_link_slots(saved_ot_t* B) {
    for (int i = 0; i < B->slot_count; i++) {
        uintptr_t next_host = (i + 1 < B->slot_count)
            ? (uintptr_t)B->slots[i + 1]
            : (uintptr_t)&prim_terminator;
        std::memcpy(B->slots[i], &next_host, sizeof(uintptr_t));
    }
}

/** @brief Logs the high-water slot count and scratch use at most once per 1000 VBLs, for sizing saved_ot_t.
 *
 * @param B The scratch built this walk. */
static void ot_high_water(const saved_ot_t* B) {
    static int      s_max_slot_count = 0;
    static size_t   s_max_scratch    = 0;
    static unsigned s_last_log_frame = 0;
    if (B->slot_count > s_max_slot_count) s_max_slot_count = B->slot_count;
    if (B->scratch_used > s_max_scratch)  s_max_scratch    = B->scratch_used;
    if (openpete_frame >= s_last_log_frame + 1000u) {
        s_last_log_frame = openpete_frame;
        OPL_INFO_F("thunks", "OT high-water: max_slots=%d max_scratch=%zu KB",
                   s_max_slot_count, s_max_scratch / 1024);
    }
}

/** @brief Walks the guest OT chain from addr into the host scratch, one slot per entry.
 *
 * Stops at the terminator (next 0xFFFFFF), a zero address, an entry that
 * does not map, a DR_LOAD whose image runs past its region, a full scratch,
 * OT_MAX_SLOTS, or the OPENPETE_OT_TRUNCATE limit. Guest RAM is only read.
 * @param B The scratch, emptied by the caller.
 * @param addr The first entry's guest address.
 * @param draw_prims Counts the drawing prims submitted.
 * @param filtered_prims Counts the geometry prims --render-paths dropped. */
static void ot_walk(saved_ot_t* B, uint32_t addr, unsigned* draw_prims, unsigned* filtered_prims) {
    static uint32_t s_dump_call = 0;
    static int s_otg_logs = 0;
    FILE* dump = ot_dump_file(&s_dump_call);
    const int truncate = ot_truncate_limit();
    uint32_t prev_addr = 0u, prev_tag = 0u;
    while (B->slot_count < (int)OT_MAX_SLOTS && !(truncate >= 0 && B->slot_count >= truncate) && addr != 0u) {
        uint32_t tag;
        const uint8_t* host = ot_entry_host(addr, &tag);
        if (!host) break;
        const uint8_t  plen = (uint8_t)((tag >> 24) & 0xFFu);
        const uint32_t next = tag & 0x00FFFFFFu;
        if (plen > 32u && ot_garbage_trace() && s_otg_logs < 80) {
            s_otg_logs++;
            OPL_WARN_F("thunks",
                "OT-garbage f=%u slot=%d addr=0x%08X tag=0x%08X plen=%u "
                "code=0x%02X next=0x%06X <- prev addr=0x%08X tag=0x%08X",
                openpete_frame, B->slot_count, addr, tag, (unsigned)plen,
                (unsigned)host[7], next, prev_addr, prev_tag);
        }
        if (ot_render_path_drops(addr, host, plen)) {
            (*filtered_prims)++;
        } else {
            const size_t payload_bytes = ot_payload_bytes(addr, host, plen);
            if (payload_bytes == OT_PAYLOAD_UNMAPPED) break;
            if (!ot_copy_slot(B, addr, host, plen, payload_bytes, draw_prims)) break;
            if (dump) ot_dump_row(dump, B->slot_count - 1, s_dump_call, addr, host, plen, payload_bytes);
            prev_addr = addr;
            prev_tag  = tag;
        }
        if (next == 0x00FFFFFFu) break;
        addr = next | 0x80000000u;
    }
    if (dump) std::fflush(dump);
}

/** @brief DrawOTag thunk: copy the guest ordering-table chain into host slots and submit it to PsyCross.
 *
 * Walks the chain from a0 (ot_walk), one slot per entry, each a host copy
 * of the tag and its payload with the length field PsyCross's walker
 * expects for the primitive code (ot_prim_longs), links the slots, and,
 * while PsyCross renders, walks them through its parser. Ends fast boot at
 * the first table with more than one drawing primitive. Guest RAM is only
 * read.
 * @param cpu Guest CPU; a0 holds the first OT entry. */
void psyq_DrawOTag(CPUState* cpu) OP_REQUIRES(sim) {
    /* The OT rebuild is openpete's own work on the game fiber, so it lands
     * inside the sim-tick bracket rather than any render-side phase. */
    op_accum_scope prof_("psyq.drawotag");
    /* Content fed since the last canonical capture is drawn now, under the
     * last PutDrawEnv's env. */
    openpete_native_note_draw_otag();
    saved_ot_t* B = g_ot_cur;
    B->scratch_used = 0;
    B->slot_count = 0;

    /* The 2D chokepoint: verify the batch already released for this table, and
     * release one still pending (see ui_sprite_ot_draw_begin). */
    ui_sprite_ot_verify();
    ui_sprite_ot_draw_begin();

    static uint32_t s_calls = 0;
    ++s_calls;
    if ((s_calls & 0xFFFu) == 1u) {
        OPL_INFO_F("thunks", "DrawOTag(#%u) ot=0x%08X ra=0x%08X",
                   s_calls, cpu->a0, cpu->ra);
    }

    /* The ordering table this walk starts from. The 2D feeder needs the same
     * traversal to rank its primitives, and it runs a tick before the walk of
     * the table it is feeding, so the last PSYQ_OT_BASE_RING bases are kept,
     * newest first. The tables alternate, and the pending batch is linked
     * into one the newest walk did not use. The feeder walks every kept base
     * and ranks its primitives by first sighting across the chains. */
    const uint32_t addr = cpu->a0;
    ot_base_note(addr);
    /* Drawing primitives (codes 0x20-0x7F) this walk submitted, and the
     * geometry prims --render-paths dropped; a loading screen emits exactly
     * one, its background fill. */
    unsigned draw_prims_this_call = 0, filtered_prims_this_call = 0;
    ot_walk(B, addr, &draw_prims_this_call, &filtered_prims_this_call);

    /* --render-paths: the per-path polybuf intervals were recorded during this
     * frame's render pass and consumed by the walk above. The "canon drawn" mask
     * latch + the per-frame interval clear are NOT done here - they live in the
     * host present loop (core/runtime/run.c), once per canonical present, so they
     * survive the planned removal of this PsyCross OT submission (the feeder hooks
     * that record the intervals fire on the render functions, independent of
     * DrawOTag). */

    ot_link_slots(B);

    /* Walk the host-side chain in non-singlePrimitive mode and let PsyCross
     * batch splits across slots; a DrawPrim per slot would force
     * `g_DrawPrimMode=1` on every slot and fragment the vertex batches.
     *
     * Each slot's `len` field is the primitive's PSX-side P_TAG length, which
     * matches the primLength returned by ProcessGouraudPoly/ProcessFlatPoly,
     * so the inner `(primLength + P_LEN) * 4` advance lands cleanly on the
     * next slot - no over-walk into scratch. */
    if (B->slot_count > 0) {
        ot_high_water(B);
        /* When the native Vulkan renderer is the sole display (pure --renderer vk),
         * PsyCross does not rasterize the OT at all - the native renderer draws
         * everything from gamestate. Skip the GL walk entirely (and don't open a
         * PsyX scene, so PsyX_EndScene auto-no-ops via begin_scene_flag -> no
         * GR_StoreFrameBuffer, no second vsync swap on the hidden window). */
        extern int openpete_psycross_renders;
        if (openpete_psycross_renders) {
            if (PsyX_BeginScene()) {
                ClearSplits();
            }
            /* Sim-due display walk of the OT just captured. */
            extern void openpete_render_walk_ot(void);
            openpete_render_walk_ot();
            /* The capture unit pairs the -og leg by the scene this OT drew. */
            openpete_capture_note_psyx_walk();
        }
        /* Tell the host loop this iter's GL backbuffer holds fresh game pixels.
         * Spyro is a 30fps game: it calls VSync(0) twice per logical frame, so
         * if the host loop swapped on every yield we'd flash the bare clear-
         * colour every other frame. The host now only EndScene/SwapWindow when
         * this flag is set. */
        openpete_present_pending = 1;
    }

    /* The transition from "boot init" to "game is rendering" is the first OT
     * that draws a scene, not the first DrawOTag call. A game whose loading
     * screen is a single background fill issues DrawOTag every frame while it
     * reads from the disc; anchoring on that runs the whole load at wall-clock
     * pace behind a blank screen. Requiring more than one drawing primitive
     * holds fast boot until real content, and is inert for a game whose first
     * OT is already a scene. The count includes the prims --render-paths
     * dropped, so a filter that selects no path the first scene draws does not
     * hold fast boot. */
    const unsigned guest_draw_prims = draw_prims_this_call + filtered_prims_this_call;
    if (openpete_fast_boot_active && guest_draw_prims > 1u && !fast_boot_hold(guest_draw_prims)) {
        openpete_fast_boot_active = 0;
        OPL_INFO_F("boot",
                   "fast-boot ended at vbl=%d (first OT with %u drawing "
                   "primitives); openpete_frame anchored to 0",
                   SDL_GetAtomicInt(&openpete_vbl), guest_draw_prims);
    }

    ot_hist_log();
}

/* --- libpad -------------------------------------------------------------- */

/** @brief Thunk for libpad PadInitDirect: PsyCross's PadInitDirect on the two guest pad buffers, plus the game's RAM mirror.
 *
 * a0 is the first port's buffer, a1 the second's. The first buffer's address
 * is kept for psyq_vbl_body, which restores its bytes across a VBL that
 * polls no pad data. psyq_game_mirror_pad_init writes the rest of what the
 * real PadInitDirect leaves in PSX RAM.
 * @param cpu Guest CPU state. */
void psyq_PadInitDirect(CPUState* cpu) {
    uint32_t buf0 = cpu->a0;  /* &g_PadBuffer */
    uint32_t buf1 = cpu->a1;  /* &g_PadBufferSecondController */
    /* PsyCross writes one PADRAW per port each VBL and takes NULL as no
     * buffer; a refused buffer is passed as NULL. */
    PadInitDirect((unsigned char*)SPAN_OPT(buf0, sizeof(PADRAW)),
                  (unsigned char*)SPAN_OPT(buf1, sizeof(PADRAW)));
    s_pad_buf0 = buf0;

    /* The rest of what real PadInitDirect leaves in PSX RAM -- libpad's
     * pad-state dispatch table and its per-slot bookkeeping -- is PSYQ's own
     * data at this game's addresses, and the table entries are pointers into
     * PSYQ code, so both are specific to a game and a PSYQ revision. PsyCross
     * tracks pad state host-side and writes none of it. */
    psyq_game_mirror_pad_init(cpu, buf0, buf1);
}

/** @brief Rewrite libpad's second-slot state to what PSYQ libpad holds with no controller there, through psyq_game_mirror_pad_slot2.
 *
 * PsyCross's pad poll stamps every `g_controllers[i].padData` slot with
 * connected-pad bytes (status 0, id 0x41/0x73, buttons, neutral analog).
 * Real libpad with slot 2 empty leaves status 0xFF and zeros in that slot's
 * buffer; Spyro 1 also holds 0x00000190 in the libpad word at 0x80078E9C
 * from PadInitDirect on (buffer at 0x80078E50,
 * g_PadBufferSecondController). PsyCross overwrites the buffer at every
 * poll, so psyq_vbl_body runs this every VBL rather than once at
 * PadInitDirect. A game that provides no mirror writes nothing.
 * @param cpu Guest CPU state. */
static void psyq_libpad_mirror_canonical_slot2(CPUState* cpu) {
    psyq_game_mirror_pad_slot2(cpu);
}

/** @brief Thunk for libpad PadStartCom: calls PsyCross's PadStartCom.
 *
 * @param cpu Guest CPU state, unused. */
void psyq_PadStartCom(CPUState* cpu) {
    (void)cpu;
    PadStartCom();
}

/** @brief Thunk for libpad PadStopCom: calls PsyCross's PadStopCom.
 *
 * @param cpu Guest CPU state, unused. */
void psyq_PadStopCom(CPUState* cpu) {
    (void)cpu;
    PadStopCom();
}

/** @brief Thunk for libpad PadGetState: the state of the pad on port a0, in v0.
 *
 * PsyCross answers PadStateStable for a connected pad and PadStateDiscon
 * otherwise. A stable pad reports PadStateExecCmd while a pad command
 * sequence (PadSetMainMode) is past its first VBL.
 * @param cpu Guest CPU state; a0 is the port, v0 receives the state. */
void psyq_PadGetState(CPUState* cpu) {
    int st = PadGetState((int)cpu->a0);
    if (st == PadStateStable && pad_exec()) st = PadStateExecCmd;
    cpu->v0 = (uint32_t)st;
}

/** @brief Thunk for libpad PadInfoMode: PsyCross's PadInfoMode(a0, a1, a2) result in v0.
 *
 * The answer describes a connected analog controller: id 7 and an id table
 * of 4 and 7; an empty slot answers 0.
 * @param cpu Guest CPU state; a0 is the port, a1 the term, a2 the table
 *     offset. */
void psyq_PadInfoMode(CPUState* cpu) {
    cpu->v0 = (uint32_t)PadInfoMode((int)cpu->a0, (int)cpu->a1, (int)cpu->a2);
}

/** @brief Thunk for libpad PadInfoAct: PsyCross's PadInfoAct(a0, a1, a2) result in v0.
 *
 * The answer describes the two actuators of a connected controller; an empty
 * slot answers 0.
 * @param cpu Guest CPU state; a0 is the port, a1 the actuator number, a2 the
 *     term. */
void psyq_PadInfoAct(CPUState* cpu) {
    cpu->v0 = (uint32_t)PadInfoAct((int)cpu->a0, (int)cpu->a1, (int)cpu->a2);
}

/** @brief Thunk for libpad PadSetMainMode: starts a pad command sequence and returns 1, or returns 0 while one runs.
 *
 * The arguments are ignored: the pad already presents analog mode. The
 * sequence lasts PAD_MAINMODE_VBLS VBLs and is what PadGetState reports as
 * PadStateExecCmd.
 * @param cpu Guest CPU state; v0 receives 1 when accepted, else 0. */
void psyq_PadSetMainMode(CPUState* cpu) {
    /* The pad is already in analog mode (PsyCross presents id 0x73); the
     * request is accepted when no sequence is running and refused
     * otherwise, as libpad does. */
    if (pad_busy()) { cpu->v0 = 0u; return; }
    pad_seq_start(PAD_MAINMODE_VBLS);
    cpu->v0 = 1u;
}

/** @brief Thunk for libpad PadSetActAlign: accepts the actuator alignment table when no pad sequence runs.
 *
 * v0 is 0 while a sequence runs or when the six-byte table runs past its
 * region, else PsyCross's return.
 * @param cpu Guest CPU state. */
void psyq_PadSetActAlign(CPUState* cpu) {
    if (pad_busy()) { cpu->v0 = 0u; return; }
    /* libpad reads the six-byte actuator alignment table. */
    unsigned char* table = (unsigned char*)SPAN(cpu->a1, 6u);
    if (!table) { cpu->v0 = 0u; return; }
    cpu->v0 = (uint32_t)PadSetActAlign((int)cpu->a0, table);
    if (cpu->v0) pad_seq_start(PAD_ACTALIGN_VBLS);
}

/** @brief Thunk for libpad PadSetAct: registers the guest actuator buffer of a2 bytes for port a0.
 *
 * A buffer span that runs past its region is not registered.
 * @param cpu Guest CPU state. */
void psyq_PadSetAct(CPUState* cpu) {
    /* The registered actuator buffer is read len bytes at a time. */
    const int len = (int)cpu->a2;
    unsigned char* table = (unsigned char*)SPAN(cpu->a1, len > 0 ? (uint32_t)len : 0u);
    if (!table) return;
    PadSetAct((int)cpu->a0, table, len);
}

/* --- libcd --------------------------------------------------------------- */

/** @brief Thunk for libcd CdInit: PsyCross's CdInit, and libcd's status byte set to motor on, idle (0x02).
 *
 * @param cpu Guest CPU state; v0 receives PsyCross's result. */
void psyq_CdInit(CPUState* cpu) {
    int rc = CdInit();
    OPL_INFO_F("thunks", "CdInit -> %d", rc);
    cd_mirror_status(0x02u);   /* motor on, idle */
    cpu->v0 = (uint32_t)rc;
}

/* The music the host plays: the stream (its CdlReadS anchor LBA and VBL)
 * whose audio the host started, LBA 0 when none. The routes the game chose
 * for the last streams anchored, keyed by anchor, let cd_stream_host_sync
 * start a stream whose CdlReadS ran in a speculation the timeline adopted,
 * or that a rewind returned to. Host state. */
static int s_host_stream_lba = 0, s_host_stream_vbl = 0;
#define CD_ROUTE_HISTORY 8
static struct { int lba, vbl; psyq_music_route_t route; } s_routes[CD_ROUTE_HISTORY];
static uint32_t s_route_next = 0;

/** @brief Stop the host's music: the XA decoder and a WAV override. */
static void cd_stream_host_stop(void) {
    if (PsyX_XA_IsActive()) PsyX_XA_Stop();
    music_override_stop();
    s_host_stream_lba = 0;
    s_host_stream_vbl = 0;
}

/** @brief Start the host's music for the stream anchored at (`lba`, `vbl`) on `route`.
 *
 * Exactly one of a custom song by path, a WAV override by track, or the XA
 * decoder at `lba` then owns playback, tried in that order; starting an
 * override stops XA, and starting XA stops the override.
 * @param lba The stream's start LBA.
 * @param vbl The VBL of its CdlReadS.
 * @param route The game's route for it. */
static void cd_stream_host_start(int lba, int vbl, const psyq_music_route_t* route) {
    s_host_stream_lba = lba;
    s_host_stream_vbl = vbl;
    /* One line per track start / loop (every couple of minutes), so a
     * music report can be read off an ordinary log: the gap between two
     * of these is the loop period the guest measured. */
    s_cnt_stream_starts++;
    OPL_INFO_F("music", "stream start: track %d LBA %d chan %d vbl %d (at vbl %d)",
               route->track, lba, s_xa_filter_chan, vbl, SDL_GetAtomicInt(&openpete_vbl_device));
    if (route->custom_path && music_override_try_start_path(route->custom_path, lba)) {
        if (PsyX_XA_IsActive()) PsyX_XA_Stop();
        return;
    }
    if (music_override_try_start(route->track, lba)) {
        if (PsyX_XA_IsActive()) PsyX_XA_Stop();
        return;
    }
    music_override_stop();
    PsyX_XA_Start(lba, s_xa_filter_file, s_xa_filter_chan);
}

/** @brief Route a streaming read issued under an XA mode to one music source.
 *
 * Anchors the music stream (g_cdXaStartLBA, g_cdXaStartVBL), marks the
 * stream running (bit 1 of s_cd_reading) and lets the game fill the route
 * (its track index, a custom song's path); these run during runahead
 * speculation too, since they are guest-deterministic or roll back with
 * the tick. The route is recorded under the anchor; on a committed tick the
 * host's music then starts (cd_stream_host_start).
 * @param sector The read's start LBA. */
static void start_music_stream(int sector) {
    g_cdXaStartLBA = sector;
    g_cdXaStartVBL = SDL_GetAtomicInt(&openpete_vbl_device);
    s_cd_reading |= 2u;

    /* The game's route runs before the speculation check: its guest RAM
     * writes are part of the tick and roll back with it. */
    psyq_music_route_t route;
    route.track = -1;
    route.custom_path = NULL;
    psyq_game_music_route(sector, &route);
    s_routes[s_route_next].lba = sector;
    s_routes[s_route_next].vbl = g_cdXaStartVBL;
    s_routes[s_route_next].route = route;
    s_route_next = (s_route_next + 1u) % CD_ROUTE_HISTORY;

    /* Runahead lookahead: starting host audio (WAV override or XA) from a
     * speculative tick would be audible and then re-fired by the committed
     * re-run. Guest-side effects of the read roll back regardless. */
    if (openpete_speculative) return;
    cd_stream_host_start(sector, g_cdXaStartVBL, &route);
}

/** @brief Make the host's music follow the guest's stream on a committed tick.
 *
 * A stop or a CdlReadS issued in a speculation the timeline adopted, or a
 * rewind to another stream, leaves the host on a stream the guest is not
 * reading: the host stops when the guest reads none and starts the guest's
 * stream on its recorded route. A stream with no recorded route stops the
 * host. */
static void cd_stream_host_sync(void) {
    if (!(s_cd_reading & 2u)) {
        if (s_host_stream_lba) cd_stream_host_stop();
        return;
    }
    if (s_host_stream_lba == g_cdXaStartLBA && s_host_stream_vbl == g_cdXaStartVBL) return;
    cd_stream_host_stop();
    for (uint32_t i = 0; i < CD_ROUTE_HISTORY; i++)
        if (s_routes[i].lba == g_cdXaStartLBA && s_routes[i].vbl == g_cdXaStartVBL) {
            cd_stream_host_start(g_cdXaStartLBA, g_cdXaStartVBL, &s_routes[i].route);
            return;
        }
    /* Held as the host's stream with nothing playing, so the warning is
     * written once. */
    s_host_stream_lba = g_cdXaStartLBA;
    s_host_stream_vbl = g_cdXaStartVBL;
    OPL_WARN_F("music", "stream at LBA %d (vbl %d) has no recorded route; host music stopped",
               g_cdXaStartLBA, g_cdXaStartVBL);
}

/** @brief Start the drive head on a CdlReadS at `sector` and, under an XA mode, the music stream; return whether the command was consumed.
 *
 * @param sector The read's first LBA.
 * @return 1 when the read streams music (consumed here), else 0. */
static int cd_read_stream(int sector) {
    s_cd_reading = 1u;
    cd_head_read(sector, cd_mode_rate(s_cd_mode));
    if (!(s_cd_mode & CdlModeRT)) {
        s_cnt_reads_rt_clear++;
        return 0;
    }
    start_music_stream(sector);
    return 1;
}

/// @brief Log the music routing and XA delivery counters; registered with atexit by cd_set_mode.
static void cd_exit_summary(void) {
    OP_ROLE_ASSERT(sim);
    unsigned long long cd_frames = 0, cd_starved = 0;
    unsigned long long xa_sectors = 0, xa_silence = 0, xa_late = 0, xa_refused = 0, xa_reseeks = 0;
    PsyX_SPUAL_CDInputStats(&cd_frames, &cd_starved);
    PsyX_XA_Stats(&xa_sectors, &xa_silence, &xa_late, &xa_refused, &xa_reseeks);
    unsigned long long xa_image_end = 0;
    int xa_max_lba = -1, image_sectors = 0;
    PsyX_XA_EndStats(&xa_image_end, &xa_max_lba, &image_sectors);
    OPL_INFO_F("music", "summary: stream_starts=%u mode_rt_cleared=%u "
               "reads_rt_clear=%u cd_input_frames=%llu cd_input_starved=%llu "
               "xa_sectors=%llu xa_silence_frames=%llu xa_late_frames=%llu "
               "xa_refused_frames=%llu xa_reseeks=%llu xa_image_end_pumps=%llu "
               "xa_max_read_lba=%d image_sectors=%d",
               s_cnt_stream_starts, s_cnt_mode_rt_cleared,
               s_cnt_reads_rt_clear, cd_frames, cd_starved,
               xa_sectors, xa_silence, xa_late, xa_refused, xa_reseeks,
               xa_image_end, xa_max_lba, image_sectors);
}

/** @brief Set the drive's mode byte and mirror it into libcd's state block.
 *
 * CdlSetmode writes it on every CdControl entry, and psyq_CdRead writes the
 * mode libcd's CdRead sets before its own read. A write that clears
 * CdlModeRT while it was set is counted, and the first call registers the
 * exit summary of the music counters.
 * @param mode The mode byte. */
static void cd_set_mode(uint8_t mode) {
    static bool s_summary_armed = false;
    if (!s_summary_armed) {
        s_summary_armed = true;
        std::atexit(cd_exit_summary);
    }
    if ((s_cd_mode & CdlModeRT) && !(mode & CdlModeRT))
        s_cnt_mode_rt_cleared++;
    s_cd_mode = mode;
    cd_mirror_mode(mode);
}

/** @brief Run the host body shared by CdControl, CdControlB and CdControlF: mirror the drive state the command writes, and own a streaming CdlReadS.
 *
 * A command consumed here must not reach PsyCross: a CdlReadS that started
 * the music stream (PsyCross's body would seek the image file the XA pump
 * owns, or on the F path start a spooler thread over it), CdlSetmode and
 * CdlNop.
 * @param com The command byte.
 * @param param The command's parameter bytes as a host pointer, or NULL.
 * @return 1 when the command was consumed here, 0 when PsyCross still runs
 *         it. */
static int cd_command_host(u_char com, u_char* param) {
    s_cd_pause_owed = 0u;  /* the new command supersedes a stream read's closing CdlPause */
    g_CD_com = com;   /* CdLastCom() reports the command as issued */
    cd_mirror_com(com);
    if (com != CdlGetlocP && com != CdlGetlocL) {
        /* The status byte the drive answered this command with on the
         * reference (seek and error bits included), under the golden's
         * controller sidecar; the model's own otherwise. */
        uint8_t ref_resp[16]; uint32_t ref_n = 0;
        if (openpete_cdrom_mmio_libcd_response(com, ref_resp, &ref_n) && ref_n >= 1u) {
            cd_mirror_status(ref_resp[0]);
            s_cd_pending_done = openpete_cdrom_mmio_libcd_last_done_vbl();
            s_cd_pending_rec  = openpete_cdrom_mmio_libcd_last_done_rec();
        } else cd_mirror_status(cd_live_status());
    }
    /* libcd keeps the CdlLOC parameter of a positional command: CdlReadS,
     * CdlReadN, CdlSeekL and CdlSeekP here, CdlSetloc below. Without a
     * parameter the command acts at the drive's last Setloc. */
    int loc = s_cd_setloc_sector;
    if (param && (com == CdlReadS || com == CdlReadN || com == CdlSeekL ||
                  com == CdlSeekP)) {
        cd_mirror_pos(param);
        loc = CdPosToInt((CdlLOC*)param);
    }
    switch (com) {
        case CdlSetloc:
            if (param) {
                s_cd_setloc_sector = CdPosToInt((CdlLOC*)param);
                cd_mirror_pos(param);
                /* Head position for the level-segment asset overlay: CdRead
                 * has no location arg. */
                psyq_game_cd_setloc(param);
            }
            return 0;
        case CdlSetmode:
            cd_set_mode(param ? param[0] : 0u);
            return 1;
        case CdlNop:
            /* Status request: the status byte was mirrored above; nothing
             * else to do. Neither this nor CdlSetmode reaches PsyCross,
             * whose CdControlF has no arm for them. */
            return 1;
        case CdlSetfilter: {
            CdlFILTER* f = (CdlFILTER*)param;
            if (f) {
                s_xa_filter_file = (int)f->file;
                s_xa_filter_chan = (int)f->chan;
            }
            return 0;
        }
        case CdlReadS:
            return cd_read_stream(loc);
        case CdlPlay:
            cd_head_play(loc, cd_mode_rate(s_cd_mode));
            return 0;
        case CdlSeekL:
        case CdlSeekP:
            cd_head_rest(loc, s_head_speed);
            return 0;
        case CdlPause:
        case CdlStop:
        case CdlReadN:
            /* A data read or a stop ends the stream; PsyCross's read would
             * seek the same image file. Host audio only, so not under
             * speculation (see start_music_stream); a stop a speculation
             * adopted into the timeline is applied by cd_stream_host_sync. */
            if (!openpete_speculative) cd_stream_host_stop();
            cd_head_stop();
            /* Pause and Stop complete with a second response whose status is
             * idle; the motor-off bit a CdlStop clears on hardware is not
             * measured. A CdlReadN's completion is written by psyq_CdRead. */
            s_cd_reading = 0u;
            if (com != CdlReadN) cd_mirror_status(0x02u);
            return 0;
        default:
            return 0;
    }
}

/** @brief Run a CdControl command: the shared host body, then PsyCross's CdControl when the host body did not consume it.
 *
 * psyq_CdControl calls it with the parameter and response buffers already
 * translated from guest addresses to host pointers.
 * @param com The command byte.
 * @param param The parameter bytes, or NULL.
 * @param result The response buffer, or NULL.
 * @return 1 when the host body consumed the command, else CdControl's
 *         return. */
static int cdcontrol_host(u_char com, u_char* param, u_char* result) {
    if (cd_command_host(com, param)) return 1;
    return CdControl(com, param, result);
}

/* libcd's response buffer: a command's response is at most 8 bytes. */
#define PSYQ_CD_RESULT_SIZE 8u

/** @brief Bytes of a libcd command's parameter that the host and PsyCross read.
 *
 * A positional command (CdlSetloc, CdlReadN, CdlReadS, CdlSeekL, CdlSeekP)
 * reads a CdlLOC, whose four bytes cd_mirror_pos mirrors; CdlSetfilter
 * reads a CdlFILTER; CdlSetmode reads the mode byte. Any other command's
 * parameter is read by neither, and one byte is resolved.
 * @param com The command byte.
 * @return The byte count. */
static uint32_t cd_param_len(u_char com) {
    switch (com) {
        case CdlSetloc: case CdlReadN: case CdlReadS: case CdlSeekL: case CdlSeekP:
            return sizeof(CdlLOC);
        case CdlSetfilter:
            return sizeof(CdlFILTER);
        default:
            return 1u;
    }
}

/** @brief Thunk for libcd CdControl: the shared host body on the guest parameter and response buffers.
 *
 * The parameter span is the command's (cd_param_len). v0 is 0 with no
 * command issued when a nonzero parameter runs past its region.
 * @param cpu Guest CPU state. */
void psyq_CdControl(CPUState* cpu) {
    u_char com = (u_char)cpu->a0;
    u_char* param = (u_char*)SPAN_OPT(cpu->a1, cd_param_len(com));
    if (cpu->a1 && !param) { cpu->v0 = 0u; return; }
    /* Neither the host body nor PsyCross writes the response; a refused
     * buffer is passed as absent. */
    u_char* result = (u_char*)SPAN_OPT(cpu->a2, PSYQ_CD_RESULT_SIZE);
    OPL_INFO_F("thunks", "CdControl com=0x%02X param=0x%08X result=0x%08X",
               (unsigned)com, cpu->a1, cpu->a2);
    cpu->v0 = (uint32_t)cdcontrol_host(com, param, result);
    cd_charge_pending(cpu, "CdControl");
}

/** @brief Thunk for libcd CdControlB: the host body, then PsyCross's CdControlB when the host body did not consume the command.
 *
 * v0 is 0 with no command issued when a nonzero parameter runs past its
 * region.
 * @param cpu Guest CPU state. */
void psyq_CdControlB(CPUState* cpu) {
    u_char com = (u_char)cpu->a0;
    u_char* param = (u_char*)SPAN_OPT(cpu->a1, cd_param_len(com));
    if (cpu->a1 && !param) { cpu->v0 = 0u; return; }
    if (cd_command_host(com, param)) {
        cpu->v0 = 1u;
        cd_charge_pending(cpu, "CdControlB");
        return;
    }
    cpu->v0 = (uint32_t)CdControlB(com, param, (u_char*)SPAN_OPT(cpu->a2, PSYQ_CD_RESULT_SIZE));
    cd_charge_pending(cpu, "CdControlB");
}

/** @brief Thunk for libcd's CdControlF, which issues a drive command without waiting for its acknowledgement.
 *
 * CdlGetlocP is answered here and never reaches PsyCross, whose CdControlF
 * treats it as the start of CDDA/XA streaming and sets `readMode`
 * (PsyCross's LIBCD.C) to RM_XA_AUDIO; Spyro's music loop issues
 * CdControlF(CdlGetlocP, NULL) only to poll the playback position, and that
 * mode makes the next data CdRead copy 2304-byte audio sectors over the
 * game's 2048-byte buffer. For CdlGetlocP the thunk records the command in
 * `g_CD_com` (what CdLastCom returns) and libcd's block and, under the
 * golden's controller sidecar, latches the reference's 8-byte response for
 * the CdSync that follows. Other commands run cd_command_host, then
 * PsyCross's CdControlF when not consumed.
 * @param cpu Guest CPU state: a0 the command, a1 the parameter address or
 *        0; v0 receives 0 for CdlGetlocP, 1 when consumed, else PsyCross's
 *        return. */

void psyq_CdControlF(CPUState* cpu) {
    u_char com = (u_char)cpu->a0;
    u_char* param = (u_char*)SPAN_OPT(cpu->a1, cd_param_len(com));
    if (cpu->a1 && !param) { cpu->v0 = 0u; return; }
    OPL_DEBUG_F("thunks", "CdControlF com=0x%02X param=0x%08X",
               (unsigned)com, cpu->a1);
    /* PsyCross's CdControlF treats CdlGetlocP as "the caller is starting
     * CDDA/XA streaming" and flips readMode to RM_XA_AUDIO. Spyro's music
     * loop calls CdControlF(CdlGetlocP, NULL) just to poll the current
     * playback position - it doesn't switch mode. The bogus mode flip
     * then makes the next data CdRead read AudioSector-sized chunks
     * (2304B vs 2048B) and overrun the game's buffer. Track g_CD_com
     * ourselves for CdLastCom and skip PsyCross's switch entirely. */
    if (com == CdlGetlocP) {
        g_CD_com = com;
        cd_mirror_com(com);
        /* Under the golden's controller sidecar the drive's answer to this
         * query is the reference's, read back by the CdSync that follows. */
        uint32_t n = 0;
        s_getlocp_ref_valid = openpete_cdrom_mmio_libcd_response(CdlGetlocP, s_getlocp_ref, &n) && n == 8u;
        cpu->v0 = 0u;  /* CdControlF returns int (0) for the GetlocP case */
        return;
    }
    if (cd_command_host(com, param)) {
        cpu->v0 = 1u;   /* libcd: 1 on success; PsyCross's body returns 0 */
        return;
    }
    cpu->v0 = (uint32_t)CdControlF(com, param);
}

/* Active-overlay tracker (spyro_overlay_table.h). Each overlay's
 * `LEVEL_BALLOON(id, ...)` (overlays.ld) burns the overlay-id as the first
 * word at +0x00 of the game's overlay load address
 * (psyq_game_overlay_vaddr). After a CdRead settles bytes there, the word is
 * read and the dispatch state updated. Cutscene-style loads to the same
 * vaddr write LevelHeader / VRAM bytes whose first word is never one of
 * the overlay IDs, so they leave the tracker alone, but they overwrite the
 * resident overlay's bytes. */
#include "spyro_overlay_table.h"

/** @brief Copy the guest bytes a read of @p sectors sectors at @p dst will overwrite, for the sector-paced landing.
 *
 * Sizes the landing buffer to the read and returns a malloc'd copy of the
 * destination's current bytes, which the caller restores after the drain
 * and frees; the read's own bytes then land sector by sector.
 * @param dst The read's guest destination.
 * @param sectors The read's sector count.
 * @return The copy, or NULL when @p sectors is not positive or the range
 *         leaves main RAM. */
static uint8_t* cd_land_snapshot(uint32_t dst, int sectors) {
    if (sectors <= 0) return NULL;
    uint32_t bytes = (uint32_t)sectors * 2048u;
    if (s_cd_land_cap < bytes) { s_cd_land_buf = (uint8_t*)realloc(s_cd_land_buf, bytes); s_cd_land_cap = bytes; }
    uint32_t phys = (dst & 0x1FFFFFFFu);
    if (phys + bytes > 0x200000u) return NULL;
    uint8_t* old = (uint8_t*)malloc(bytes);
    memcpy(old, openpete_psx_get_ram() + phys, bytes);
    return old;
}

/** @brief Thunk for libcd CdRead: reads a0 2048-byte sectors into the guest buffer at a1 and schedules the completion.
 *
 * v0 is 0 with no read issued when the buffer span runs past its region.
 * @param cpu Guest CPU state. */
void psyq_CdRead(CPUState* cpu) {
    int sectors = (int)cpu->a0;
    /* CdReadSync drains 2048-byte data sectors into the buffer. */
    u_long* buf = (u_long*)SPAN(cpu->a1, sectors > 0 ? (uint64_t)sectors * 2048u : 0u);
    if (!buf) { cpu->v0 = 0u; return; }
    int     mode = (int)cpu->a2;
    OPL_INFO_F("thunks", "CdRead sectors=%d buf=0x%08X mode=0x%02X ra=0x%08X hit=%u vbl=%d",
               sectors, cpu->a1, mode, cpu->ra, openpete_psxtrace_anchor_hits,
               SDL_GetAtomicInt(&openpete_vbl));

    /* libcd's CdRead ORs 0x20 into the caller's mode (its read state word,
     * `ori v0, v0, 0x20` before the CdlSetmode it issues), so the drive's
     * mode byte reads mode | 0x20 after a read; PsyCross's CdRead only
     * enqueues, so the mirror is written here. */
    cd_set_mode((uint8_t)(mode | 0x20));
    /* The head comes to rest after the read's last sector. */
    cd_head_rest(s_cd_setloc_sector + sectors, cd_mode_rate((uint8_t)mode));
    /* libcd's CdRead issues CdlSetmode and CdlReadN; the command byte and
     * the drive's first response are what the game reads while the read
     * is in flight, its first data response from the VBLANK it arrived
     * on the reference. The data-ready interrupts that follow, one per
     * sector, are the VBLANKs the sectors land at. */
    uint32_t land_n = 0;
    {
        uint8_t ref_resp[16]; uint32_t ref_n = 0;
        (void)openpete_cdrom_mmio_libcd_response(CdlSetmode, ref_resp, &ref_n);
        g_CD_com = CdlReadN;
        cd_mirror_com(CdlReadN);
        s_cd_data_vbl = 0;
        if (openpete_cdrom_mmio_libcd_response(CdlReadN, ref_resp, &ref_n) && ref_n >= 1u) {
            cd_mirror_status(ref_resp[0]);
            uint8_t irq, st; uint32_t rec, vbl;
            if (openpete_cdrom_mmio_libcd_last_irq(1u, &rec, &vbl, &irq, &st)) { s_cd_data_rec = rec; s_cd_data_vbl = vbl; s_cd_data_status = st; }
            for (uint32_t i = 1; land_n < CD_LAND_MAX && openpete_cdrom_mmio_libcd_last_irq(i, &rec, &vbl, &irq, &st); i++)
                if (irq == 1u) { s_cd_land_rec[land_n] = rec; s_cd_land_vbl[land_n] = vbl; land_n++; }
        }
    }
    uint8_t* land_old = (land_n == (uint32_t)sectors) ? cd_land_snapshot(cpu->a1, sectors) : NULL;
    cpu->v0 = (uint32_t)CdRead(sectors, buf, mode);
    /* PsyCross's CdRead only enqueues; CdReadSync drains the queue into
     * the buffer. Spyro polls via CdSync (which doesn't drain), so we
     * drain inline here and then fire the cb so g_CdState.m_IsReading
     * clears.
     *
     * Force readMode = RM_DATA before draining: Spyro's CdRead path is
     * always data sectors (mode bits 0x80 = CdlModeSpeed). XA streaming
     * uses CdControlF(CdlReadS, ...) instead. Without this, a stale
     * RM_XA_AUDIO from prior music polling makes CdReadSync read the
     * wrong sector size and trip the assert(0) else-branch. */
    readMode = 0;  /* RM_DATA */
    extern int CdReadSync(int mode, u_char* result);
    int remaining = CdReadSync(/*mode=*/0, /*result=*/NULL);
    if (remaining > 0) {
        OPL_WARN_F("thunks", "CdReadSync left %d sectors unread", remaining);
    }
    /* Level-segment asset overlay: serve mod-supplied bytes over
     * the just-drained read BEFORE the game's loader parses them. Also
     * advances the tracked head position. */
    psyq_game_cd_apply_buf((uint8_t*)buf, sectors);
    uint8_t* ram = openpete_psx_get_ram();
    if (land_old) {
        uint32_t bytes = (uint32_t)sectors * 2048u, phys = cpu->a1 & 0x1FFFFFFFu;
        memcpy(s_cd_land_buf, ram + phys, bytes);
        memcpy(ram + phys, land_old, bytes);
        free(land_old);
        s_cd_land_dst = cpu->a1; s_cd_land_sectors = (uint32_t)sectors; s_cd_land_next = 0;
    } else {
        s_cd_land_sectors = 0;
    }

    /* A new read supersedes the previous stream read's closing CdlPause. */
    s_cd_pause_owed = 0u;
    /* A completion still owed from the previous read fires before this
     * read's, in issue order: the game re-issued after its own timeout. */
    s_cd_defer_stream = 0u;
    if (s_cd_defer_pending) {
        OPL_WARN_F("cd", "load delay: read issued with %u VBL(s) still owed; "
                   "completing the deferred read first", s_cd_defer_owed);
        s_cd_defer_pending = 0u;
        s_cd_defer_owed = 0u;
        s_cd_defer_vbl = s_cd_defer_rec = 0u; s_cd_defer_clock = 0u;
        cd_read_complete(cpu);
    }
    uint32_t delay;
    uint32_t sched_issue = 0, sched_done = 0, sched_done_rec = 0; int sched_sync = 0;
    if (openpete_sched_active() && openpete_sched_read(sectors, cpu->a1, &sched_issue, &sched_done, &sched_done_rec, &sched_sync)) {
        /* The VBLs the drive commands before the read spend pass in
         * their own calls (cd_charge_pending); a read a time-sliced loader
         * reaches earlier than the reference did is not charged to the
         * reference's issue, since the loader's intermediate state is CPU
         * time there and its rows are cd_load_transient. */
        if (sched_issue != openpete_sched_pos())
            OPL_INFO_F("cd", "schedule: read issued at reference VBLANK %u, openpete at %u",
                       sched_issue, openpete_sched_pos());
        if (sched_sync) {
            /* The reference completed this read before its next anchor hit:
             * the VBLs it spent pass here, callbacks included, and the read
             * completes instantly as the game's spin observed it. */
            sched_charge_to(cpu, sched_done, "CdRead");
            delay = 0;
            OPL_INFO_F("cd", "schedule: %d-sector read charged to reference VBLANK %u synchronously",
                       sectors, sched_done);
        } else {
            /* The completion callback ran at the reference's VBLANK inside
             * its record: delivered when the position reaches it there. */
            delay = cd_ref_seen(sched_done_rec, sched_done, 0u) ? 0u : 1u;
            s_cd_defer_rec = sched_done_rec; s_cd_defer_vbl = sched_done;
            s_cd_defer_clock = openpete_sched_read_done_tick();
        }
        (void)cd_read_delay_for(cpu, sectors);
    } else {
        delay = cd_read_or_stream_delay_for(cpu, sectors);
    }
    if (delay) {
        cd_land_tick(0u);
        s_cd_defer_pending = 1u;
        s_cd_defer_owed = s_cd_defer_vbl ? 0u : delay;
        if (s_cd_defer_vbl)
            OPL_INFO_F("cd", "schedule: %d-sector read completes at reference record %u, VBLANK %u (hit=%u vbl=%d)",
                       sectors, s_cd_defer_rec, s_cd_defer_vbl, openpete_psxtrace_anchor_hits, SDL_GetAtomicInt(&openpete_vbl));
        else
            OPL_INFO_F("cd", "load delay: %d-sector read deferred %u VBL(s) "
                       "(hit=%u vbl=%d)", sectors, delay, openpete_psxtrace_anchor_hits,
                       SDL_GetAtomicInt(&openpete_vbl));
    } else {
        s_cd_defer_vbl = s_cd_defer_rec = 0u; s_cd_defer_clock = 0u;
        OPL_INFO_F("cd", "read instant: %d sectors (hit=%u vbl=%d)", sectors,
                   openpete_psxtrace_anchor_hits, SDL_GetAtomicInt(&openpete_vbl));
        cd_land_rest();
        cd_read_complete(cpu);
    }

    /* Once the bytes have settled, the first word at the overlay vaddr is the
     * overlay id, so re-point the dispatcher at whichever overlay is now
     * resident. A write to that vaddr that is not an overlay carries a
     * different first word and leaves the tracker unchanged. */
    uint32_t ovl = psyq_game_overlay_vaddr();
    if (ovl != 0u && cpu->a1 == ovl) {
        uint32_t magic = 0;
        memcpy(&magic, ram + (ovl & 0x1FFFFFu), 4);
        spyro_set_active_overlay_by_magic(magic);
    }
}

void psyq_overlay_resync_from_ram(void) {
    uint32_t ovl = psyq_game_overlay_vaddr();
    uint8_t* ram = openpete_psx_get_ram();
    if (ovl == 0u || !ram) return;
    uint32_t magic = 0;
    memcpy(&magic, ram + (ovl & 0x1FFFFFu), 4);
    if (magic != spyro_get_active_overlay_magic())
        spyro_set_active_overlay_by_magic(magic);
}

/* libcd's internals, as distinct from the API above.
 *
 * PSYQ reaches the drive through the BIOS jump tables with an indirect call,
 * so these cannot be translated: the recompiler has no BIOS, and the symptom
 * is a status poll that never completes rather than a crash. They map onto
 * PsyCross's equivalents, which complete synchronously.
 *
 * The names are those of the SDK's own export tables; a decomp that splats
 * the PSYQ text without symbols has none of them.
 */
/** @brief Thunk for libcd's internal CD_sync: PsyCross's CdSync, with a scratch buffer standing in for a NULL result pointer.
 *
 * @param cpu Guest CPU state: a0 the mode, a1 the result address or 0; v0
 *        receives CdSync's return. */
void psyq_CD_sync(CPUState* cpu) {
    u_char scratch[PSYQ_CD_RESULT_SIZE];
    u_char* result = (u_char*)SPAN_OPT(cpu->a1, PSYQ_CD_RESULT_SIZE);
    if (!result) result = scratch;
    cpu->v0 = (uint32_t)CdSync((int)cpu->a0, result);
}

/** @brief Thunk for libcd's internal CD_datasync: PsyCross's CdReadSync, with a scratch buffer standing in for a NULL or refused result pointer.
 *
 * @param cpu Guest CPU state. */
void psyq_CD_datasync(CPUState* cpu) {
    u_char scratch[PSYQ_CD_RESULT_SIZE];
    u_char* result = (u_char*)SPAN_OPT(cpu->a1, PSYQ_CD_RESULT_SIZE);
    if (!result) result = scratch;
    cpu->v0 = (uint32_t)CdReadSync((int)cpu->a0, result);
}

/** @brief Thunk for libcd's internal CD_getsector: copies a1 words of the current sector to the guest address a0.
 *
 * v0 is 0 with nothing copied when the span runs past its region.
 * @param cpu Guest CPU state. */
void psyq_CD_getsector(CPUState* cpu) {
    /* The size argument counts 32-bit words. */
    const int words = (int)cpu->a1;
    void* dst = SPAN(cpu->a0, words > 0 ? (uint64_t)words * 4u : 0u);
    if (!dst) { cpu->v0 = 0u; return; }
    cpu->v0 = (uint32_t)CdGetSector(dst, words);
}

/** @brief Thunk for libcd's internal CD_init: PsyCross's CdInit, with libcd's status byte left alone.
 *
 * @param cpu Guest CPU state; v0 receives the result. */
void psyq_CD_init(CPUState* cpu) {
    cpu->v0 = (uint32_t)CdInit();
}

/** @brief Thunk for libcd's internal CD_flush, which returns 0: PsyCross owns the device and keeps no drive-side queue to drain.
 *
 * @param cpu Guest CPU state; v0 receives 0. */
void psyq_CD_flush(CPUState* cpu)   { cpu->v0 = 0u; }
/** @brief Thunk for libcd's internal CD_initvol, which returns 0: PsyCross applies the CD volume host-side and has no register to program.
 *
 * @param cpu Guest CPU state; v0 receives 0. */
void psyq_CD_initvol(CPUState* cpu) { cpu->v0 = 0u; }

/** @brief Answer a CdSync poll as libcd does while a stream read's closing CdlPause is in flight: no command complete.
 *
 * @param mode The CdSync mode.
 * @param rc The answer otherwise.
 * @return CdlNoIntr for a poll off the schedule while the CdlPause is owed, else rc. */
static int cd_stream_pause_answer(int mode, int rc) {
    return (mode != 0 && s_cd_pause_owed && !openpete_sched_active()) ? CdlNoIntr : rc;
}

/** @brief The drive head's LBA at the current guest VBL, the position a CdlGetlocP answers outside sidecar replay.
 *
 * Guest-deterministic: the head moves on the game's own commands and the
 * VBL count (cd_head_at).
 * @return The head's LBA. */
static int cd_getlocp_model_lba(void) {
    return (int)cd_head_at(cd_head_now(), 1u);
}

/** @brief Under OPENPETE_XA_LOG=<path>, write the XA decoder's sector head once per guest VBL while a stream plays.
 *
 * Line `h <vbl> <lba>`: the decoder's next LBA after the VBL's pump
 * (cd_stream_pump). Line `x <vbl>`: the decoder stopped. Speculative ticks
 * write nothing. The file is truncated on open and starts with a version
 * line.
 */
static void xa_head_log(void) {
    static FILE* s_f = NULL;
    static int s_tried = 0, s_last_vbl = -1, s_was_active = 0;
    if (!s_tried) {
        s_tried = 1;
        const char* path = std::getenv("OPENPETE_XA_LOG");
        if (path && *path) {
            s_f = std::fopen(path, "w");
            if (s_f) std::fputs("# xa-head-log 1\n", s_f);
            else OPL_WARN_F("audio", "OPENPETE_XA_LOG: cannot open %s", path);
        }
    }
    if (!s_f || openpete_speculative) return;
    const int vbl = (int)SDL_GetAtomicInt(&openpete_vbl_device);
    const int active = PsyX_XA_IsActive();
    if (!active) {
        if (s_was_active) { std::fprintf(s_f, "x %d\n", vbl); std::fflush(s_f); }
        s_was_active = 0;
        return;
    }
    s_was_active = 1;
    if (vbl == s_last_vbl) return;
    s_last_vbl = vbl;
    std::fprintf(s_f, "h %d %d\n", vbl, PsyX_XA_GetCurrentSector());
    std::fflush(s_f);
}

/** @brief Advance the XA decoder to the drive head over the `wait` VBLs the SPU is about to render, after reconciling the host's music with the guest's stream on a committed tick.
 *
 * The decoder is pumped only while the guest streams music and the host
 * plays that same stream: a CdlReadS a speculation issued has not started
 * host audio, and its head must not drive the decoder of the stream before
 * it. The head's position at the render's current sample is the position
 * `wait` VBLs ago; a matching sector plays from when the head has read it.
 * @param wait VBLs the VBL body is about to render. */
static void cd_stream_pump(int wait) {
    if (!openpete_speculative) cd_stream_host_sync();
    if ((s_cd_reading & 2u) && s_host_stream_lba == g_cdXaStartLBA &&
        s_host_stream_vbl == g_cdXaStartVBL && s_head_rate != 0u) {
        const uint64_t now = cd_head_now();
        const uint32_t per = 588u / s_head_rate;   /* output frames per LBA at the head's speed */
        PsyX_XA_PumpTo((int)cd_head_at(now, 1u),
                       (long long)cd_head_at(now - (uint64_t)wait * CD_HEAD_TU, per), (int)per);
    }
    xa_head_log();
}

/// @brief Exit line of the OPENPETE_GETLOCP_AB comparison: answers compared, answers over 2 LBA apart, the largest difference.
static uint32_t s_getlocp_ab_n, s_getlocp_ab_over;
static int s_getlocp_ab_max;
/// @brief Print the OPENPETE_GETLOCP_AB census line; registered with atexit on the first comparison.
static void getlocp_ab_summary(void) {
    OP_ROLE_ASSERT(sim);
    fprintf(stderr, "[getlocp-ab] compared=%u over2=%u max_abs_diff=%d verdict=%s\n",
            s_getlocp_ab_n, s_getlocp_ab_over, s_getlocp_ab_max,
            s_getlocp_ab_n == 0 ? "VACUOUS" : (s_getlocp_ab_over ? "FAIL" : "PASS"));
}

/** @brief Under OPENPETE_GETLOCP_AB, compare the replayed reference GetlocP answer with cd_getlocp_model_lba and count differences over 2 LBA.
 *
 * Runs only where the sidecar answers the query, the path on which the
 * model is otherwise never evaluated. A query before any streaming read has
 * anchored the model is not compared. Speculative ticks are not counted.
 * Each comparison writes one `[getlocp-ab]` line with the VBL, both LBAs
 * and the difference (model minus reference).
 * @param result The 8-byte GetlocP answer; bytes 5..7 are the absolute
 *     position in BCD minute, second, sector. */
static void getlocp_ab_compare(const u_char* result) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = std::getenv("OPENPETE_GETLOCP_AB"); s_on = (e && *e && *e != '0'); }
    if (!s_on || openpete_speculative || g_cdXaStartLBA <= 0) return;
    CdlLOC loc;
    loc.minute = result[5];
    loc.second = result[6];
    loc.sector = result[7];
    loc.track = 0;
    const int ref = CdPosToInt(&loc);
    const int model = cd_getlocp_model_lba();
    const int d = model - ref;
    const int ad = d < 0 ? -d : d;
    if (s_getlocp_ab_n++ == 0) std::atexit(getlocp_ab_summary);
    if (ad > 2) s_getlocp_ab_over++;
    if (ad > s_getlocp_ab_max) s_getlocp_ab_max = ad;
    fprintf(stderr, "[getlocp-ab] vbl=%d anchor=%d@%d model=%d ref=%d diff=%+d\n",
            (int)SDL_GetAtomicInt(&openpete_vbl_device), g_cdXaStartLBA, g_cdXaStartVBL, model, ref, d);
}

/** @brief Write the drive head's position into a CdlGetlocP answer: the reference drive's answer under the golden's sidecar, else the head model's once a music stream has been anchored.
 *
 * Spyro's track-end detection compares the absolute position (bytes 5..7,
 * BCD minute, second, sector) with the track's end in its music table, so
 * the answer is the drive head (cd_getlocp_model_lba), never the XA
 * decoder's position. Before the first music stream PsyCross's answer
 * stands.
 * @param result The 8-byte answer buffer. */
static void cd_getlocp_answer(u_char* result) {
    if (s_getlocp_ref_valid) {
        /* The reference drive's answer to the last position query, under
         * the golden's controller sidecar: the head's position as the drive
         * reported it, seek and read timing included. */
        memcpy(result, s_getlocp_ref, 8);
        getlocp_ab_compare(result);
        return;
    }
    if (g_cdXaStartLBA <= 0) return;
    CdlLOC abs;
    CdIntToPos(cd_getlocp_model_lba(), &abs);
    result[5] = abs.minute;
    result[6] = abs.second;
    result[7] = abs.sector;
}

/** @brief Complete the pending libcd command for a blocking CdSync, as libcd's CD_sync mode 0 waits for it.
 *
 * Mode 0 blocks in CD_sync until the status is CdlComplete or CdlDiskError
 * (the `beqz $s5` back-edge at 0x800649EC in Spyro 1's libcd); a poll
 * returns at once. No CdSync site in the three games passes mode 0. The
 * pending command completes, charged to its completion VBLANK under the
 * schedule, and the caller answers with PsyCross's poll (CdlComplete).
 * The VBLs libcd would spend waiting out a stream read's closing CdlPause
 * off the schedule are not passed; the first such call is warned.
 * @param cpu Guest CPU state; ra names the call site in the warning. */
static void cd_sync_blocking(CPUState* cpu) {
    cd_charge_pending(cpu, "CdSync");
    static int s_sync0_warned = 0;
    if (!s_cd_pause_owed || openpete_sched_active() || s_sync0_warned) return;
    s_sync0_warned = 1;
    OPL_WARN_F("cd", "CdSync(0) ra=0x%08X with a stream CdlPause owed (%u VBLs): "
               "answered CdlComplete without passing them", cpu->ra, s_cd_pause_owed);
}

/** @brief CdSync thunk: pumps the music override, then returns PsyCross's CD command status, with an optional per-VBL spin limit.
 *
 * A call past OPENPETE_PSYQ_CDSYNC_LIMIT calls at one `openpete_frame` runs the registered
 * exit flush (openpete_crash_run_exit_flush) and exits with code 3.
 * @param cpu Guest CPU state; a0 is the mode, a1 the result buffer, v0 receives the status. */
void psyq_CdSync(CPUState* cpu) {
    int mode = (int)cpu->a0;
    /* PsyCross's CdSync writes into result[0..7] whenever g_CD_com ==
     * CdlGetlocP, regardless of whether the caller asked for the data.
     * Spyro's music loop sets that com via CdControlF(CdlGetlocP, NULL),
     * then non-music code calls CdSync(1, NULL) just for the return
     * value (LoadCutscene, CDLoadSync, CDLoadAsync, CDLoadTime). With
     * a NULL result PsyCross then SIGSEGVs on result[0] = ...
     *
     * Pass a stack scratch buffer when the caller didn't supply one;
     * the writes are harmless and PsyCross still returns CdlComplete. */
    u_char  scratch[PSYQ_CD_RESULT_SIZE];
    u_char* result = (u_char*)SPAN_OPT(cpu->a1, PSYQ_CD_RESULT_SIZE);
    if (!result) result = scratch;

    /* The WAV override's stream refills on every CdSync; XA is pumped by
     * the drive head from the VBL body (cd_stream_pump). */
    music_override_pump();

    /* PsyCross's mode 0 is unimplemented: a blocking call is completed
     * first (cd_sync_blocking) and then answered by the poll. */
    if (mode == 0) cd_sync_blocking(cpu);
    int rc = cd_stream_pause_answer(mode, CdSync(1, result));
    /* A poll answers as the reference's poll at this call site did, from
     * the golden's CdSync sidecar, the VBLs to that poll's VBLANK passing
     * first (a game spinning on the drive spent them in the spin).
     * Without rows for this record, a poll before the pending command's
     * completion answers "in progress", as libcd did until the drive's
     * second response arrived; a game that polls again with no VBL passed
     * in between is spinning, and each such poll passes one VBL. */
    int32_t ref_rc; uint32_t ref_vbl;
    if (mode != 0 && openpete_sched_active() && openpete_sched_cdsync(mode, cpu->ra, &ref_rc, &ref_vbl)) {
        cd_defer_before_call(cpu);
        if (ref_vbl > openpete_sched_pos()) sched_charge_to(cpu, ref_vbl, "CdSync poll");
        spu_clock_charge(openpete_sched_last_tick());
        rc = (int)ref_rc;
        /* The reference's poll saw the completion: the drive's second
         * response had arrived (a poll answered complete before that
         * response's VBLANK saw an earlier command's). */
        if (rc == CdlComplete && s_cd_pending_done && cd_ref_seen(s_cd_pending_rec, s_cd_pending_done, 0u))
            cd_pending_complete();
        cd_defer_after_call(cpu);
    } else if (mode != 0 && s_cd_pending_done && openpete_sched_active()) {
        static uint32_t s_poll_pos = 0xFFFFFFFFu;
        if (openpete_sched_pos() < s_cd_pending_done) {
            if (openpete_sched_pos() == s_poll_pos) sched_charge_to(cpu, s_poll_pos + 1u, "CdSync poll");
            s_poll_pos = openpete_sched_pos();
            rc = (openpete_sched_pos() < s_cd_pending_done) ? CdlNoIntr : rc;
        }
        if (openpete_sched_pos() >= s_cd_pending_done) { cd_pending_complete(); s_poll_pos = 0xFFFFFFFFu; }
    }

    /* Native-side spin guard for CDLoadSync's `while (CdSync(1, NULL) !=
     * CdlComplete)` polls. The host hang watchdog in core/psx/dispatch.c
     * only catches recomp dispatches; a fully-native spin via this host
     * thunk -> host C goes undetected. Per-frame counter resets on frame
     * advance. Default off; set OPENPETE_PSYQ_CDSYNC_LIMIT to a positive
     * count to enable. */
    static int s_cs_read = 0;
    static uint32_t s_cs_limit = 0u;   /* debug knob: read once */
    if (!s_cs_read) {
        const char* env = std::getenv("OPENPETE_PSYQ_CDSYNC_LIMIT");
        s_cs_limit = (env && *env) ? (uint32_t)std::strtoul(env, NULL, 0) : 0u;
        s_cs_read = 1;
    }
    if (s_cs_limit) {
        const uint32_t limit = s_cs_limit;
        static uint32_t cs_calls_this_frame = 0u;
        static uint32_t cs_last_frame = 0u;
        if (openpete_frame != cs_last_frame) {
            cs_last_frame = openpete_frame;
            cs_calls_this_frame = 0u;
        }
        if (limit && ++cs_calls_this_frame > limit) {
            OPL_ERROR_F("thunks",
                        "psyq_CdSync spin guard: %u calls in frame=%u "
                        "(rc=%d s0=0x%08X ra=0x%08X) - exiting",
                        cs_calls_this_frame, openpete_frame, rc,
                        cpu->s0, cpu->ra);
            openpete_crash_run_exit_flush("cdsync-spin");
            std::fflush(stdout); std::fflush(stderr);
            openpete_crash_terminate_now(3);
        }
    }

    if (g_CD_com == CdlGetlocP && mode == 1) cd_getlocp_answer(result);
    /* Don't log every CdSync - busy-loops would flood. Sample every 2^22
     * calls. */
    static uint32_t calls = 0;
    if ((++calls & 0x3FFFFFu) == 1u) {
        OPL_INFO_F("thunks",
                   "CdSync(%d) -> %d (call #%u) s0=0x%08X ra=0x%08X",
                   mode, rc, calls, cpu->s0, cpu->ra);
    }
    cpu->v0 = (uint32_t)rc;
}

/** @brief Thunk for libcd CdLastCom: returns the last command byte.
 *
 * cd_command_host and psyq_CdRead record the command as issued in g_CD_com.
 * @param cpu Guest CPU state; v0 receives the byte. */
void psyq_CdLastCom(CPUState* cpu) {
    cpu->v0 = (uint32_t)CdLastCom();
}

/** @brief Thunk for libcd CdMix: returns 1 without applying the CD-XA mix volume.
 *
 * PsyCross declares CdMix and does not define it.
 * @param cpu Guest CPU state; v0 receives 1. */
void psyq_CdMix(CPUState* cpu) {
    /* PsyCross declares CdMix but never defines it. Spyro calls it from
     * CDInitialize() to set CD-XA mix volume; without real audio routing,
     * treat as success. */
    (void)cpu;
    cpu->v0 = 1u;
}

/** @brief Thunk for libcd CdReadCallback: records the guest read-completion handler a0.
 *
 * v0 is the handler it replaced. cd_read_complete calls the recorded
 * handler.
 * @param cpu Guest CPU state. */
void psyq_CdReadCallback(CPUState* cpu) {
    /* Save the recompiled cb address; our psyq_CdRead thunk fires it
     * after PsyCross finishes the (synchronous) read. */
    OPL_INFO_F("thunks", "CdReadCallback registered: 0x%08X", cpu->a0);
    uint32_t prev = g_cd_read_cb;
    g_cd_read_cb = cpu->a0;
    cpu->v0 = prev;  /* PSYQ returns the previous handler. */
}

/** @brief Thunk for libcd CdPosToInt: the sector number of the guest CdlLOC.
 *
 * v0 is 0 when the CdlLOC runs past its region.
 * @param cpu Guest CPU state. */
void psyq_CdPosToInt(CPUState* cpu) {
    CdlLOC* loc = (CdlLOC*)SPAN(cpu->a0, sizeof(CdlLOC));
    if (!loc) { cpu->v0 = 0u; return; }
    cpu->v0 = (uint32_t)CdPosToInt(loc);
}

/** @brief Thunk for libcd CdIntToPos: writes sector a0 as a CdlLOC at a1 and returns a1.
 *
 * v0 is 0 with nothing written when the CdlLOC runs past its region.
 * @param cpu Guest CPU state. */
void psyq_CdIntToPos(CPUState* cpu) {
    CdlLOC* loc = (CdlLOC*)SPAN(cpu->a1, sizeof(CdlLOC));
    if (!loc) { cpu->v0 = 0u; return; }
    CdIntToPos((int)cpu->a0, loc);
    cpu->v0 = cpu->a1;  /* PSYQ returns the same pointer. */
}

/* --- libspu -------------------------------------------------------------- */

#include "openpete_mods.h" /* SFX overlay service (mods/mod_sfx.c) */
#include "sfx_pack.h"      /* drop-in WAV replacement (core/host/sfx_pack.c) */
extern "C" void PsyX_SPUAL_StopOrphans(void);

/* ── SFX overlay: Spyro-1 glue ─────────────────────────────────────────
 * Names for the game's SoundTable slots (spyro-1/include/sound_table.h;
 * slot value = index into the per-level SoundDefinition array). The
 * resolver maps a keyed voice's sample addr back to the first named
 * slot whose definition owns that addr; the uploader stages replacement
 * ADPCM into the free SPU tail after the level bank. Everything here is
 * host-side: guest RAM and the SPU bank image stay byte-stock.
 */
static uint32_t s_spu_xfer_addr; /* SpuSetTransferStartAddr, advanced by SpuWrite */

/** @brief Thunk for libspu SpuInit: PsyCross's SpuInit, then registers the game's sound tables.
 *
 * libspu's SpuInit ends in SpuStart, whose first call opens and enables
 * the DMA-completion event (HwSPU, EvSpCOMP, EvMdNOINTR) in the kernel's
 * event table; the thunk opens it there once.
 * @param cpu Guest CPU state, unused. */
void psyq_SpuInit(CPUState* cpu) {
    static int s_spu_started = 0;
    (void)cpu;
    SpuInit();
    if (!s_spu_started) {
        s_spu_started = 1;
        openpete_psx_bios_spu_start_events();
    }
    psyq_game_sfx_arm();
}

/** @brief Set the music override's gain to the CD volume times the SPU main volume.
 *
 * XA reaches the SPU's CD input, where the SPU applies both volumes from its
 * registers; the override's WAV plays outside the SPU and takes them here. */
static void spyro_apply_music_gain(void) {
    music_override_set_gain(s_cdVolume * s_spuMainVolume);
}

/** @brief Thunk for libspu SpuSetCommonAttr: PsyCross's SpuSetCommonAttr, and the music gain from the main and CD volumes.
 *
 * An attribute span that runs past its region applies nothing.
 * @param cpu Guest CPU state. */
void psyq_SpuSetCommonAttr(CPUState* cpu) {
    SpuCommonAttr* attr = (SpuCommonAttr*)SPAN(cpu->a0, sizeof(SpuCommonAttr));
    if (!attr) return;
    SpuSetCommonAttr(attr);
    /* Spyro's music fader writes mask=SPU_COMMON_CDVOLL|SPU_COMMON_CDVOLR
     * with values in 0..0x3FFF. Route that to the XA streaming source's
     * gain (avg the two channels for a single mono gain). The WAV
     * override gets the same value so the in-game music slider and
     * track-transition fades cover both paths. */
    /* SPU volumes are 15-bit: hardware mixes as (sample * volume) >> 15, so
     * full scale is 32768. Main volume applies to the summed mix, and CD/XA
     * input is summed *before* it, so the music gain carries both stages. */
    if (attr && (attr->mask & (SPU_COMMON_MVOLL | SPU_COMMON_MVOLR))) {
        int l = attr->mvol.left, r = attr->mvol.right;
        if (l < 0) l = 0;
        if (r < 0) r = 0;
        /* Main volume lands in a sweep-capable register whose fixed mode
         * carries the level halved; the SPU doubles it on application, so
         * 0x3CCC is applied as 31128 and full scale here is 16384. CD input
         * volume below has no sweep mode and is applied as written, making
         * it full scale at 32768 - the two are not the same scale. */
        s_spuMainVolume = (float)((l + r) / 2) / 16384.0f;
        spyro_apply_music_gain();
    }
    if (attr && (attr->mask & (SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR))) {
        int l = attr->cd.volume.left;
        int r = attr->cd.volume.right;
        if (l < 0) l = 0;
        if (r < 0) r = 0;
        s_cdVolume = (float)((l + r) / 2) / 32768.0f;
        spyro_apply_music_gain();
    }
}

/** @brief The OPENPETE_AUDIO_HEAD_LOG file, opened on the first call; NULL when the variable is unset or empty or the open failed.
 *
 * The file is truncated on open and starts with a version line.
 * @return The open file or NULL. */
static FILE* audio_head_log(void) {
    static FILE* s_f = NULL;
    static int s_tried = 0;
    if (!s_tried) {
        s_tried = 1;
        const char* path = getenv("OPENPETE_AUDIO_HEAD_LOG");
        if (path && *path) {
            s_f = fopen(path, "w");
            if (s_f) fputs("# audio-head-log 1\n", s_f);
            else OPL_WARN_F("audio", "OPENPETE_AUDIO_HEAD_LOG: cannot open %s", path);
        }
    }
    return s_f;
}

/** @brief Frames the SPU capture ring has produced, the sample index of the mix being rendered next in an OPENPETE_AUDIO_DUMP; -1 without a capture.
 * @return The produced frame count, or -1. */
static long long audio_head_log_out(void) {
    if (!PsyX_SPUAL_CaptureActive()) return -1;
    unsigned long long produced = 0;
    PsyX_SPUAL_CaptureStats(&produced, NULL);
    return (long long)produced;
}

/** @brief Under OPENPETE_AUDIO_HEAD_LOG, write one `w` line per running voice whose decode window overlaps the SPU RAM span [lo, hi) about to be written.
 *
 * Called before the write reaches PsyCross, so the line holds the voice's
 * guest head (decoder samples since key-on, the block it is in) and its
 * OpenAL source offset as they were when the bytes landed.
 * @param lo First SPU RAM byte written.
 * @param hi One past the last byte written. */
static void audio_head_log_write(unsigned int lo, unsigned int hi) {
    FILE* f = audio_head_log();
    if (!f || hi <= lo) return;
    const int vbl = SDL_GetAtomicInt(&openpete_vbl_device);
    const long long out = audio_head_log_out();
    for (int v = 0; v < 24; v++) {
        PsyX_SPUAL_VoiceHead h;
        if (!PsyX_SPUAL_GetVoiceHead(v, &h)) continue;
        if (h.decode_hi <= h.decode_lo || hi <= h.decode_lo || lo >= h.decode_hi) continue;
        fprintf(f, "w vbl=%d v=%d lo=%#x hi=%#x addr=%#x loop=%#x win=%#x-%#x pitch=%u dec=%lld "
                   "head=%d hb=%#x ib=%u kp=%d al_state=%#x al_so=%d out=%lld\n",
                vbl, v, lo, hi, h.addr, h.loop_addr, h.decode_lo, h.decode_hi, h.pitch, h.dec,
                h.head, h.head_block, h.head_in_block, h.key_on_pending, (unsigned)h.al_state,
                h.al_offset, out);
    }
    fflush(f);
}

/** @brief Under OPENPETE_AUDIO_HEAD_LOG, write one `k` line per voice in `mask` keyed on or off.
 *
 * Called after the key reaches PsyCross: a key-on is pending until the
 * next output sample, so `out` is the sample before the one it starts on.
 * @param on 1 for a key-on, 0 for a key-off.
 * @param mask The voice mask. */
static void audio_head_log_key(int on, unsigned int mask) {
    FILE* f = audio_head_log();
    if (!f) return;
    const int vbl = SDL_GetAtomicInt(&openpete_vbl_device);
    const long long out = audio_head_log_out();
    for (int v = 0; v < 24; v++) {
        if (!(mask & (1u << v))) continue;
        PsyX_SPUAL_VoiceHead h;
        if (!PsyX_SPUAL_GetVoiceHead(v, &h)) { h.addr = 0; h.pitch = 0; }
        fprintf(f, "k vbl=%d v=%d on=%d addr=%#x pitch=%u out=%lld\n", vbl, v, on, h.addr, h.pitch, out);
    }
    fflush(f);
}

/** @brief Under OPENPETE_AUDIO_HEAD_LOG, write one `p` line per running voice of `attr` whose pitch the set changes.
 *
 * The body check resamples a chunk at the run's key-on pitch, so a pitch
 * change (0 stops the head, psx-spx "0=stop") ends the part it can judge.
 * @param attr A mapped guest voice attribute record, not NULL. */
static void audio_head_log_pitch(const SpuVoiceAttr* attr) {
    FILE* f = audio_head_log();
    if (!f || !(attr->mask & SPU_VOICE_PITCH)) return;
    const int vbl = SDL_GetAtomicInt(&openpete_vbl_device);
    const long long out = audio_head_log_out();
    for (int v = 0; v < 24; v++) {
        if (!(attr->voice & (1u << v))) continue;
        PsyX_SPUAL_VoiceHead h;
        if (!PsyX_SPUAL_GetVoiceHead(v, &h) || h.pitch == attr->pitch) continue;
        fprintf(f, "p vbl=%d v=%d pitch=%u out=%lld\n", vbl, v, (unsigned)attr->pitch, out);
    }
    fflush(f);
}

/** @brief Whether OPENPETE_SFX_END_LOG is set to a nonempty value not starting with '0', read once.
 *
 * Gates the stderr lines of the SPU thunks: voice attributes, key on/off,
 * key-status transitions, and SPU RAM reads and writes.
 * @return 1 when the log is on, else 0. */
static int sfx_key_log_on(void) {
    static int s_log = -1;
    if (s_log < 0) { const char* e = getenv("OPENPETE_SFX_END_LOG"); s_log = (e && *e && *e != '0'); }
    return s_log;
}
/** @brief Log one stderr line per voice in `attr` when OPENPETE_SFX_END_LOG is on and `attr` writes a start address.
 *
 * Each line gives openpete_frame, the voice, the address, the pitch and the
 * sample's slot name, or `-` when no slot owns the address.
 * @param attr A mapped guest voice attribute record, not NULL. */
static void sfx_key_log_attr(const SpuVoiceAttr* attr) {
    if (!sfx_key_log_on() || !(attr->mask & SPU_VOICE_WDSA)) return;
    const char* names[1];
    int n = psyq_game_sfx_slot_names(attr->addr, names, 1);
    for (int v = 0; v < 24; v++)
        if (attr->voice & (1u << v))
            fprintf(stderr, "[sfx-attr f=%u v=%d addr=%#x pitch=%#x name=%s]\n",
                    openpete_frame, v, attr->addr, attr->pitch, n ? names[0] : "-");
}

/* SpuVoiceAttr mask fields that move envelope timing: the pitch fields
 * (PITCH, NOTE, SAMPLE_NOTE) and the ADSR fields (AMODE..SL, ADSR1,
 * ADSR2). Only calls with one of them carry a `.spuq` row. */
#define SPU_VOICE_ENVELOPE_FIELDS (0x70u | 0xFF00u | 0x60000u)

/** @brief Charge the SPU clock to the reference's clock at this libspu call, when the clock replay matches it to a `.spuq` row.
 * @param kind 0 SpuSetKey, 1 SpuGetKeyStatus, 2 SpuGetAllKeysStatus, 3 SpuSetVoiceAttr.
 * @param ra The call's return address.
 * @param a0 First matched argument.
 * @param a1 Second matched argument.
 * @return 1 when the call matched a row, else 0. */
static int spu_clock_at_call(int kind, uint32_t ra, uint32_t a0, uint32_t a1) {
    uint64_t tick = 0;
    if (!openpete_sched_spuq(kind, ra, a0, a1, &tick)) return 0;
    spu_clock_charge(tick);
    return 1;
}

/** @brief Charge the SPU clock to the reference's tick of one SPU register access inside the call spu_clock_at_call last matched.
 *
 * The SPU state a libspu call moves or reads changes at the register
 * access (the KON store, a voice's sample-rate store, a voice's ENVX
 * load), a varying number of cycles after the call's entry. Does nothing
 * when the reference recorded no such access for the call.
 * @param kind 0 KON store, 1 sample-rate store, 2 ENVX load.
 * @param unit The KON half (0 voices 0-15, 1 voices 16-23) for kind 0, else the voice. */
static void spu_clock_at_reg(int kind, int unit) {
    uint64_t tick = 0;
    if (openpete_sched_spureg(kind, unit, &tick)) spu_clock_charge(tick);
}

/** @brief Key on the voices of mask, each KON half after charging the SPU clock to the reference's store of that half.
 * @param mask Voice bits 0-23. */
static void spu_key_on_at_kon(uint32_t mask) {
    for (int half = 0; half < 2; half++) {
        const uint32_t voices = mask & (half ? 0xFF0000u : 0xFFFFu);
        spu_clock_at_reg(0, half);
        if (voices) SpuSetKey(SPU_ON, voices);
    }
}

/** @brief Apply attr with its pitch written per voice after charging the SPU clock to the reference's sample-rate store for that voice.
 *
 * The other fields apply first, at the clock of the call's entry; PsyCross
 * applies the pitch after them within one attribute set as well.
 * @param attr The guest's attribute set; its mask has SPU_VOICE_PITCH. */
static void spu_voice_attr_at_pitch(const SpuVoiceAttr* attr) {
    SpuVoiceAttr a = *attr;
    a.mask = attr->mask & ~(unsigned long)SPU_VOICE_PITCH;
    if (a.mask) SpuSetVoiceAttr(&a);
    a.mask = SPU_VOICE_PITCH;
    for (int v = 0; v < 24; v++) {
        if (!(attr->voice & SPU_VOICECH(v))) continue;
        spu_clock_at_reg(1, v);
        a.voice = SPU_VOICECH(v);
        SpuSetVoiceAttr(&a);
    }
}

/** @brief libspu's key status of one voice: from the software key mask and the voice's ENVX register.
 *
 * Keyed on: 3 (SPU_RESET) when ENVX is 0, else 1 (SPU_ON); keyed off: 2
 * (SPU_OFF_ENV_ON) when ENVX is not 0, else 0 (SPU_OFF), the expression of
 * libspu's SpuGetKeyStatus and SpuGetAllKeysStatus. The key flag and ENVX
 * are PsyCross's software key bit and envelope level.
 * @param voice Voice index, 0..23.
 * @return The status. */
static int spu_libspu_key_status(int voice) {
    int keyed = 0;
    const int envx = PsyX_SPUAL_GetVoiceEnvelope(voice, &keyed);
    if (keyed) return envx == 0 ? SPU_RESET : SPU_ON;
    return envx != 0 ? SPU_OFF_ENV_ON : SPU_OFF;
}

/** @brief Thunk for libspu SpuSetVoiceAttr: passes the voice attributes to PsyCross as written.
 *
 * Sound replacement (the sfx pack, including the mod API's sfx_replace)
 * happens below this mirror, at the voice's PCM upload inside PsyCross, so
 * the address and pitch the one-shot duration is derived from, and with them
 * the tick on which the guest sees the voice end, are the game's own. Under
 * the oracle's clock replay a call that sets a pitch or ADSR field first
 * charges the envelope to the reference's clock at the call, and each
 * voice's pitch to the reference's sample-rate store for it. Under
 * OPENPETE_SFX_END_LOG an attribute set that writes a start address is
 * logged per voice with its sample's slot name; under
 * OPENPETE_AUDIO_HEAD_LOG a pitch change of a running voice logs a `p`
 * line before it applies.
 * @param cpu Guest CPU state; a0 is the SpuVoiceAttr. */
void psyq_SpuSetVoiceAttr(CPUState* cpu) {
    const SpuVoiceAttr* attr = (const SpuVoiceAttr*)SPAN(cpu->a0, sizeof(SpuVoiceAttr));
    if (!attr) return;
    sfx_key_log_attr(attr);
    audio_head_log_pitch(attr);
    const int matched = (attr->mask & SPU_VOICE_ENVELOPE_FIELDS) &&
                        spu_clock_at_call(3, cpu->ra, (uint32_t)attr->voice, (uint32_t)attr->mask);
    if (matched && (attr->mask & SPU_VOICE_PITCH)) spu_voice_attr_at_pitch(attr);
    else SpuSetVoiceAttr((SpuVoiceAttr*)attr);
}

/** @brief Thunk for libspu SpuSetVoiceVolume: sets the left and right volume of voice a0 to a1 and a2.
 *
 * The volumes are taken as signed 16-bit values.
 * @param cpu Guest CPU state. */
void psyq_SpuSetVoiceVolume(CPUState* cpu) {
    SpuSetVoiceVolume((int)cpu->a0,
                      (short)cpu->a1, (short)cpu->a2);
}

/** @brief Thunk for libspu SpuGetVoiceVolume: stores voice a0's left and right volume at the guest shorts a1 and a2.
 *
 * A side whose address is 0 or runs past its region is not stored.
 * @param cpu Guest CPU state. */
void psyq_SpuGetVoiceVolume(CPUState* cpu) {
    /* PsyCross skips the store for a NULL side; a refused side is passed
     * as NULL. */
    SpuGetVoiceVolume((int)cpu->a0,
                      (short*)SPAN_OPT(cpu->a1, sizeof(short)),
                      (short*)SPAN_OPT(cpu->a2, sizeof(short)));
}

/** @brief Thunk for libspu SpuSetKey: keys on or off the voices in mask a1, by a0 (SPU_ON or SPU_OFF).
 *
 * Under the oracle's clock replay the envelope is first charged to the
 * reference's clock at the call, and a key-on to the reference's KON store
 * of each half. Under OPENPETE_SFX_END_LOG a line with
 * openpete_frame, the VBL count, a0, the mask and ra is written to stderr;
 * under OPENPETE_AUDIO_HEAD_LOG a `k` line per voice in the mask.
 * @param cpu Guest CPU state. */
void psyq_SpuSetKey(CPUState* cpu) {
    if (spu_clock_at_call(0, cpu->ra, cpu->a0, cpu->a1) && cpu->a0 == SPU_ON) spu_key_on_at_kon(cpu->a1);
    else SpuSetKey((int)cpu->a0, (unsigned int)cpu->a1);
    audio_head_log_key(cpu->a0 == SPU_ON, cpu->a1);
    if (sfx_key_log_on())
        fprintf(stderr, "[spu-key f=%u vbl=%d on=%d mask=%#x ra=%#x rec=%u]\n",
                openpete_frame, SDL_GetAtomicInt(&openpete_vbl), (int)cpu->a0,
                cpu->a1, cpu->ra, openpete_psxtrace_current_record());
}

/** @brief Thunk for libspu SpuGetKeyStatus: PsyCross's key status for the voice mask in a0.
 *
 * Under the oracle's clock replay the envelope is first charged to the
 * reference's clock at the call and at its ENVX load, and the status is
 * libspu's expression for the mask's lowest voice (-1 for an empty mask),
 * read from the envelope level instead of PsyCross's ADSR phase.
 * Under OPENPETE_SFX_END_LOG a single-voice query whose result differs from
 * that voice's previous result logs one line with openpete_frame and the
 * trace record index when the new status is SPU_ON or SPU_RESET. The RESET read is the tick the game
 * frees the sound slot and writes the sound-ref byte, so two runs whose
 * logs match played every voice to the same guest-visible end.
 * @param cpu Guest CPU state; v0 receives the status. */
void psyq_SpuGetKeyStatus(CPUState* cpu) {
    unsigned int mask = (unsigned int)cpu->a0;
    if (spu_clock_at_call(1, cpu->ra, mask, 0u) && (mask & 0xFFFFFFu)) spu_clock_at_reg(2, __builtin_ctz(mask));
    if (openpete_sched_clock_replay()) cpu->v0 = (mask & 0xFFFFFFu) ? (uint32_t)spu_libspu_key_status(__builtin_ctz(mask)) : 0xFFFFFFFFu;
    else cpu->v0 = (uint32_t)SpuGetKeyStatus(mask);
    if (sfx_key_log_on() && mask && !(mask & (mask - 1u))) {
        static uint8_t s_last[24];
        int v = __builtin_ctz(mask);
        if (v < 24 && s_last[v] != (uint8_t)cpu->v0) {
            s_last[v] = (uint8_t)cpu->v0;
            if (cpu->v0 == SPU_ON || cpu->v0 == SPU_RESET)
                fprintf(stderr, "[sfx-key f=%u v=%d %s rec=%u]\n", openpete_frame, v,
                        cpu->v0 == SPU_ON ? "on" : "reset", openpete_psxtrace_current_record());
        }
    }
}

/** @brief Thunk for libspu SpuGetAllKeysStatus: stores one key-status byte per voice, 24 in all, at the guest address a0.
 *
 * A span that runs past its region stores nothing. Under the oracle's clock
 * replay the envelope is first charged to the reference's clock at the
 * call, and each byte is libspu's expression over the envelope level after
 * charging to the reference's ENVX load of that voice. Under
 * OPENPETE_SFX_END_LOG a voice whose byte differs from its previous read
 * logs one line with openpete_frame and the trace record index when the
 * new status is SPU_ON or SPU_RESET, the same line SpuGetKeyStatus writes.
 * @param cpu Guest CPU state. */
void psyq_SpuGetAllKeysStatus(CPUState* cpu) {
    /* One status byte per SPU voice. */
    char* status = (char*)SPAN(cpu->a0, 24u);
    if (!status) return;
    spu_clock_at_call(2, cpu->ra, cpu->a0, 0u);
    SpuGetAllKeysStatus(status);
    if (openpete_sched_clock_replay()) {
        for (int v = 0; v < 24; v++) {
            spu_clock_at_reg(2, v);
            status[v] = (char)spu_libspu_key_status(v);
        }
    }
    if (sfx_key_log_on()) {
        static uint8_t s_last[24];
        for (int v = 0; v < 24; v++) {
            const uint8_t st = (uint8_t)status[v];
            if (s_last[v] == st) continue;
            s_last[v] = st;
            if (st == SPU_ON || st == SPU_RESET)
                fprintf(stderr, "[sfx-key f=%u v=%d %s rec=%u]\n", openpete_frame, v,
                        st == SPU_ON ? "on" : "reset", openpete_psxtrace_current_record());
        }
    }
}

/** @brief Thunk for libspu SpuRead: copies a1 bytes of SPU RAM from the transfer address to the guest address a0.
 *
 * v0 is 0 with nothing copied when the span runs past its region.
 * @param cpu Guest CPU state. */
void psyq_SpuRead(CPUState* cpu) {
    unsigned char* dst = (unsigned char*)SPAN(cpu->a0, cpu->a1);
    if (!dst) { cpu->v0 = 0u; return; }
    cpu->v0 = SpuRead(dst, (unsigned int)cpu->a1);
    if (sfx_key_log_on())
        fprintf(stderr, "[spu-read f=%u spu=%#x dst=%#x len=%#x ra=%#x]\n",
                openpete_frame, s_spu_xfer_addr, cpu->a0, cpu->a1, cpu->ra);
    s_spu_xfer_addr += (unsigned int)cpu->a1;
}

/** @brief Write one SpuWrite payload to OPENPETE_SPU_WRITE_DUMP as DIR/<vbl>_<spu addr>.bin, the input of tools/audio_ring_check.py.
 *
 * The variable is read on the first call, which also deletes the
 * directory's .bin files: an earlier run's files would be read as chunks
 * this run never wrote.
 * @param src The payload's host bytes.
 * @param len The payload's byte count. */
static void spu_write_dump(const unsigned char* src, unsigned int len) {
    static const char* dir = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        dir = getenv("OPENPETE_SPU_WRITE_DUMP");
        if (dir && !*dir) dir = NULL;
        /* The directory holds this run's payloads only: an earlier run's
         * files would be read as chunks this run never wrote. */
        if (dir) {
            DIR* d = opendir(dir);
            if (d) {
                struct dirent* e;
                while ((e = readdir(d)) != NULL) {
                    size_t n = strlen(e->d_name);
                    if (n > 4 && strcmp(e->d_name + n - 4, ".bin") == 0) {
                        char old[1024];
                        snprintf(old, sizeof old, "%s/%s", dir, e->d_name);
                        remove(old);
                    }
                }
                closedir(d);
            }
        }
    }
    if (dir) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%08d_%06x.bin", dir,
                 SDL_GetAtomicInt(&openpete_vbl), s_spu_xfer_addr);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(src, 1, len, f); fclose(f); }
    }
}

/** @brief Thunk for libspu SpuWrite: uploads a1 bytes at the guest address a0 to SPU RAM at the transfer address.
 *
 * The sfx pack's staged replacements are invalidated over the written
 * range, or all of them on a whole-bank write. v0 is 0 with nothing
 * uploaded when the span runs past its region. Under
 * OPENPETE_AUDIO_HEAD_LOG the running voices whose window the write
 * overlaps are logged before it lands.
 * @param cpu Guest CPU state. */
void psyq_SpuWrite(CPUState* cpu) {
    /* The level-load bank blast overwrites all of SPU RAM including any
     * staged SFX replacements - invalidate them (re-staged lazily). The
     * sfx pack's identity cache follows the same two-tier rule: a blast
     * is a new generation, any other write (dragon speech lands over the
     * top of the bank mid-level) forgets just the addresses it covers. */
    unsigned int len = (unsigned int)cpu->a1;
    unsigned char* src = (unsigned char*)SPAN(cpu->a0, len);
    if (!src) { cpu->v0 = 0u; return; }
    psyq_game_sfx_arm();
    if (len != 0u && len == psyq_game_spu_bank_blast_len()) {
        sfx_pack_generation_bump();
        PsyX_SPUAL_StopOrphans(); /* a new level: no tails from the old one */
    } else {
        sfx_pack_invalidate_range(s_spu_xfer_addr, len);
    }
    audio_head_log_write(s_spu_xfer_addr, s_spu_xfer_addr + len);
    cpu->v0 = SpuWrite(src, len);
    if (sfx_key_log_on())
        fprintf(stderr, "[spu-write f=%u vbl=%d spu=%#x src=%#x len=%#x ra=%#x]\n",
                openpete_frame, SDL_GetAtomicInt(&openpete_vbl), s_spu_xfer_addr,
                cpu->a0, len, cpu->ra);
    spu_write_dump(src, len);
    s_spu_xfer_addr += len;
}

/** @brief Thunk for libspu SpuSetTransferMode: PsyCross's SpuSetTransferMode(a0), the normalized mode in v0.
 *
 * PsyCross maps 0 to 0 and every other mode to 1.
 * @param cpu Guest CPU state; v0 receives the mode. */
void psyq_SpuSetTransferMode(CPUState* cpu) {
    cpu->v0 = (uint32_t)SpuSetTransferMode((int)cpu->a0);
}

/** @brief Thunk for libspu SpuSetTransferStartAddr: sets the SPU RAM transfer address to a0.
 *
 * Also records a0 as the address the next SpuWrite uploads to. v0 is
 * PsyCross's return.
 * @param cpu Guest CPU state. */
void psyq_SpuSetTransferStartAddr(CPUState* cpu) {
    s_spu_xfer_addr = (unsigned int)cpu->a0;
    cpu->v0 = SpuSetTransferStartAddr((unsigned int)cpu->a0);
}

/** @brief Thunk for libspu SpuIsTransferCompleted: PsyCross's SpuIsTransferCompleted(a0) result in v0.
 *
 * @param cpu Guest CPU state; a0 is the wait flag. */
void psyq_SpuIsTransferCompleted(CPUState* cpu) {
    cpu->v0 = (uint32_t)SpuIsTransferCompleted((int)cpu->a0);
}

/* --- libmcrd ------------------------------------------------------------- */

/* PsyCross's libmcrd is broken for our needs (MemCardAccept fopen()s
 * "0.MCD" and fread()s an unopened FILE*; it indexes memoryCards[chan]
 * with the raw port encoding 0x10, out-of-bounds). We ignore it entirely
 * and marshal into openpete's host card module (core/host/memcard.c),
 * which owns two real 128 KiB .mcr images persisted under the per-user
 * state dir. */

/* The on-card file name field: libmcrd compares and stores 20 bytes. */
#define PSYQ_MC_NAME_MAX 20u
/* libmcrd McErrFileNotExist. */
#define PSYQ_MC_ERR_FILE_NOT_EXIST 5u

/** @brief Copy a guest memory-card file name into a host buffer, NUL-terminated.
 *
 * The card module reads the name with strncmp and strncpy over 20 bytes,
 * so the copy carries the name's bytes up to its NUL or the 20th byte;
 * the guest bytes read lie inside one region.
 * @param addr The guest address of the name.
 * @param out Receives the name; PSYQ_MC_NAME_MAX + 1 bytes.
 * @return 1 when copied, 0 when the name is refused. */
static int psyq_mc_name(uint32_t addr, char* out) {
    uint32_t n = 0;
    const char* s = GSTR(addr, PSYQ_MC_NAME_MAX, &n);
    if (!s) return 0;
    memcpy(out, s, n);
    out[n] = '\0';
    return 1;
}

/** @brief Thunk for libmcrd MemCardInit: initializes the host card module and replays the real function's RAM writes.
 *
 * psyq_game_mirror_memcard_init writes the libmcrd state the real function
 * leaves in PSX RAM, and the prologue's stack stores are replayed.
 * @param cpu Guest CPU state. */
void psyq_MemCardInit(CPUState* cpu) {
    openpete_memcard_init();

    psyq_game_mirror_memcard_init(cpu);

    /* MemCardInit's MIPS prologue: sp -= 0x18; sw ra,0x10(sp). */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_MEMCARDINIT);
}

/** @brief Thunk for libmcrd MemCardStart: calls openpete_memcard_start, which does nothing because the host card is always ready.
 *
 * @param cpu Guest CPU state, unused. */
void psyq_MemCardStart(CPUState* cpu) {
    (void)cpu;
    openpete_memcard_start();
}

/** @brief Thunk for libmcrd MemCardStop: calls openpete_memcard_stop, which clears the host card's pending command, result and status.
 *
 * @param cpu Guest CPU state, unused. */
void psyq_MemCardStop(CPUState* cpu) {
    (void)cpu;
    openpete_memcard_stop();
}

/** @brief Thunk for libmcrd MemCardExist: probes the card in the port a0 names.
 *
 * v0 is 1 when a card is present and 0 when the slot is empty; the libmcrd
 * result code reaches the game through the latch MemCardSync reads, which
 * openpete_memcard_exist sets.
 * @param cpu Guest CPU state. */
void psyq_MemCardExist(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_exist((int)cpu->a0, nullptr, nullptr);
}

/** @brief Thunk for libmcrd MemCardAccept: probes the card in the port a0 names.
 *
 * v0 is 1 when a card is present and 0 for an empty slot; the result code
 * reaches the game through the latch MemCardSync reads.
 * @param cpu Guest CPU state. */
void psyq_MemCardAccept(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_accept((int)cpu->a0, nullptr, nullptr);
}

/** @brief Thunk for libmcrd MemCardReadFile(chan, name, adrs, ofs, bytes): copies part of a card file into guest RAM.
 *
 * a0 is the port, a1 the file name, a2 the RAM destination, a3 the file
 * offset and the word at sp+0x10 the byte count; v0 is
 * openpete_memcard_read's libmcrd result code, which it also latches for
 * MemCardSync. A name that runs past its region returns McErrFileNotExist
 * unlatched.
 * @param cpu Guest CPU state. */
void psyq_MemCardReadFile(CPUState* cpu) {
    char name[PSYQ_MC_NAME_MAX + 1];
    if (!psyq_mc_name(cpu->a1, name)) { cpu->v0 = PSYQ_MC_ERR_FILE_NOT_EXIST; return; }
    const int bytes = (int)cpu->read_word(cpu->sp + 0x10);
    /* A refused destination is passed as NULL, which the card module
     * answers McErrFileNotExist and latches for MemCardSync. */
    void* dst = SPAN(cpu->a2, bytes > 0 ? (uint32_t)bytes : 0u);
    cpu->v0 = (uint32_t)openpete_memcard_read((int)cpu->a0, name, dst,
                                              (int)cpu->a3, bytes);
}

/** @brief Thunk for libmcrd MemCardWriteFile(chan, name, adrs, ofs, bytes): copies guest RAM into part of a card file.
 *
 * The argument layout is MemCardReadFile's; a refused source is passed as
 * NULL, which the card module answers McErrFileNotExist.
 * @param cpu Guest CPU state. */
void psyq_MemCardWriteFile(CPUState* cpu) {
    char name[PSYQ_MC_NAME_MAX + 1];
    if (!psyq_mc_name(cpu->a1, name)) { cpu->v0 = PSYQ_MC_ERR_FILE_NOT_EXIST; return; }
    const int bytes = (int)cpu->read_word(cpu->sp + 0x10);
    const void* src = SPAN(cpu->a2, bytes > 0 ? (uint32_t)bytes : 0u);
    cpu->v0 = (uint32_t)openpete_memcard_write((int)cpu->a0, name, src,
                                               (int)cpu->a3, bytes);
}

/** @brief Thunk for libmcrd MemCardCreateFile(chan, name, blocks): allocates a card file.
 *
 * @param cpu Guest CPU state; v0 receives the card module's libmcrd
 *        result, or McErrFileNotExist when the name runs past its region. */
void psyq_MemCardCreateFile(CPUState* cpu) {
    char name[PSYQ_MC_NAME_MAX + 1];
    if (!psyq_mc_name(cpu->a1, name)) { cpu->v0 = PSYQ_MC_ERR_FILE_NOT_EXIST; return; }
    cpu->v0 = (uint32_t)openpete_memcard_create((int)cpu->a0, name, (int)cpu->a2);
}

/** @brief Thunk for libmcrd MemCardFormat: formats the card in the port a0 names.
 *
 * v0 is openpete_memcard_format's result: 0 on success, an error code for an
 * empty slot or an image that did not reach the disk.
 * @param cpu Guest CPU state. */
void psyq_MemCardFormat(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_format((int)cpu->a0);
}

/** @brief Thunk for libmcrd MemCardSync: reports the card module's latched command and result to the guest out-slots a1 and a2.
 *
 * An out-slot whose address is 0 or runs past its region is skipped.
 * @param cpu Guest CPU state. */
void psyq_MemCardSync(CPUState* cpu) {
    /* mode 0 = Sync, 1 = Async; host I/O completes instantly so both report
     * Finished once per op that has run, and NothingRegistered (-1) when none
     * is pending. Pre-read the guest out-slots so a field the module leaves
     * unset round-trips unchanged. */
    /* A refused out-slot is skipped like an absent one. */
    int* cmds_out = (int*)SPAN_OPT(cpu->a1, sizeof(int));
    int* rslt_out = (int*)SPAN_OPT(cpu->a2, sizeof(int));
    int c = cmds_out ? *cmds_out : 0;
    int r = rslt_out ? *rslt_out : 0;
    int st = openpete_memcard_sync((int)cpu->a0, &c, &r);
    if (cmds_out) *cmds_out = c;
    if (rslt_out) *rslt_out = r;
    cpu->v0 = (uint32_t)st;
}

/* --- libapi -------------------------------------------------------------- */

/** @brief Thunk for libapi LoadExec: logs a warning and returns; openpete cannot load a second EXE.
 *
 * In Spyro 1 the one caller is CrashDemoLaunch (title-screen L1+Triangle
 * boots the demo CRASH.EXE in the disc's S0 directory), which
 * features/crash_demo_woah.c swallows before its engine teardown.
 * @param cpu Guest CPU state; unused. */
void psyq_LoadExec(CPUState* cpu) {
    (void)cpu;
    OPL_WARN_F("thunks", "%s", "LoadExec called - ignoring");
}

/* --- C runtime (printf/sprintf/exit/rand/srand/strlen/memcpy) ------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These PSYQ-name thunks shadow nothing host-side because of the psyq_
 * prefix. They translate the PSYQ ABI into libc. */

/** @brief Thunk for the C library's memcpy: copies a2 bytes from guest a1 to guest a0 with the host memcpy.
 *
 * Nothing is copied when either a2-byte span runs past its region; v0 is
 * the destination, as PSYQ's memcpy returns.
 * @param cpu Guest CPU state. */
void psyq_memcpy(CPUState* cpu) {
    /* PSYQ memcpy returns dest. */
    size_t n = (size_t)cpu->a2;
    void* dst = SPAN(cpu->a0, n);
    const void* src = dst ? SPAN(cpu->a1, n) : nullptr;
    if (dst && src) memcpy(dst, src, n);
    cpu->v0 = cpu->a0;
}

/** @brief Thunk for the C library's strlen: the length of the guest string at a0.
 *
 * v0 is 0 when the string runs past its region with no NUL.
 * @param cpu Guest CPU state. */
void psyq_strlen(CPUState* cpu) {
    uint32_t n = 0;
    const char* s = GSTR(cpu->a0, 0xFFFFFFFFu, &n);
    cpu->v0 = s ? n : 0u;
}

/* PSYQ rand/srand: the library's LCG, with the seed kept in the game's seed
 * slot in PSX RAM (psyq_game_rand_seed_addr) so it is part of the
 * gameplay-state hash and mutates exactly as the library's does:
 *     seed = seed * 0x41C64E6D + 0x3039
 *     return (seed >> 16) & 0x7FFF */
extern "C" uint8_t* openpete_psx_get_ram(void);

/* A game whose seed slot is not located (psyq_game_rand_seed_addr() == 0)
 * keeps the LCG state here; masking 0 into RAM offset 0 would write the
 * kernel's exception-vector area. */
static uint32_t psyq_seed_host;

/** @brief Read the rand seed word from guest RAM, or from host state when the game has no seed slot.
 *
 * The slot is the address psyq_game_rand_seed_addr names, masked into main
 * RAM and read as four little-endian bytes.
 * @return The seed. */
static inline uint32_t psyq_seed_load(void) {
    uint32_t addr = psyq_game_rand_seed_addr();
    if (!addr) return psyq_seed_host;
    const uint8_t* ram = openpete_psx_get_ram();
    uint32_t off = addr & 0x1FFFFFu;
    return  (uint32_t)ram[off]
         | ((uint32_t)ram[off + 1] << 8)
         | ((uint32_t)ram[off + 2] << 16)
         | ((uint32_t)ram[off + 3] << 24);
}
/** @brief Write the rand seed word to guest RAM, or to host state when the game has no seed slot.
 *
 * The slot is the address psyq_game_rand_seed_addr names, masked into main
 * RAM and written as four little-endian bytes.
 * @param v The new seed. */
static inline void psyq_seed_store(uint32_t v) {
    uint32_t addr = psyq_game_rand_seed_addr();
    if (!addr) { psyq_seed_host = v; return; }
    uint8_t* ram = openpete_psx_get_ram();
    uint32_t off = addr & 0x1FFFFFu;
    ram[off    ] = (uint8_t)(v        & 0xFF);
    ram[off + 1] = (uint8_t)((v >> 8 ) & 0xFF);
    ram[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    ram[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}
/** @brief Thunk for libc rand: advances the seed by the PSYQ LCG and returns 15 bits of it in v0.
 *
 * The seed becomes seed * 0x41C64E6D + 0x3039 (mod 2^32) and v0 is
 * (seed >> 16) & 0x7FFF, as in the real function at 0x8006272C.
 * @param cpu Guest CPU state; v0 receives the value. */
void psyq_rand(CPUState* cpu) {
    uint32_t seed = psyq_seed_load();
    seed = seed * 0x41C64E6Du + 0x3039u;
    psyq_seed_store(seed);
    cpu->v0 = (seed >> 16) & 0x7FFFu;
}

/** @brief Thunk for libc srand: stores a0 as the rand seed.
 *
 * Under an active schedule the reference's next srand seed replaces a0, and
 * a difference is logged. Does not set v0.
 * @param cpu Guest CPU state; a0 is the seed, and is overwritten with the
 *     reference's seed when the schedule supplies one. */
void psyq_srand(CPUState* cpu) {
    uint32_t seed;
    if (openpete_sched_active() && openpete_sched_next_srand(&seed)) {
        /* The reference's seed: a game that seeds from a root counter reads
         * hardware time, which openpete answers with 0. */
        if (seed != cpu->a0)
            OPL_INFO_F("thunks", "schedule: srand(0x%08X) replaces the game's 0x%08X", seed, cpu->a0);
        cpu->a0 = seed;
    }
    psyq_seed_store(cpu->a0);
}

/** @brief Thunk for libc exit: logs the call and ends the host process with status a0.
 *
 * @param cpu Guest CPU state; a0 is the exit status. */
void psyq_exit(CPUState* cpu) {
    OPL_INFO_F("thunks", "exit(%u) from compiled code", cpu->a0);
    exit((int)cpu->a0);
}

/* printf / sprintf reconstruct MIPS o32 varargs: first 4 slots in
 * a0..a3, then sp+0x10, sp+0x14, ... The first slot is the format
 * string (or, for sprintf, dst+fmt occupy slots 0/1). Numeric args
 * are fetched as 32-bit words; %s dereferences a PSX vaddr.
 *
 * Supported conversions: d, i, u, x, X, o, s, c, p, %. Flags '-' and '0',
 * a decimal width, and a '.precision' are honored; flags '+', ' ' and '#'
 * parse but are ignored. Length modifiers h, l, z, j, t (in any repeat)
 * parse but are ignored - the original PSX ABI passes everything as a
 * 32-bit slot anyway. */

namespace {

struct MipsArgs {
    CPUState* cpu;
    int       slot;        /* next slot index (0 = a0, 4 = sp+0x10, ...) */

    /// @brief Return the next vararg slot: a0..a3, then the stack words from sp+0x10.
    uint32_t fetch() {
        uint32_t v;
        switch (slot) {
            case 0: v = cpu->a0; break;
            case 1: v = cpu->a1; break;
            case 2: v = cpu->a2; break;
            case 3: v = cpu->a3; break;
            default: v = cpu->read_word(cpu->sp + (uint32_t)(slot * 4));
                     break;
        }
        ++slot;
        return v;
    }
};

/** @brief Write the digits of `v` in `base` to `out`, most significant first, with no NUL.
 *
 * @param v The value.
 * @param base 2..16.
 * @param upper Use upper-case hex digits.
 * @param out Receives the digits; 32 bytes hold any value.
 * @return The number of digits written, 1 for 0. */
size_t render_uint(uint32_t v, unsigned base, bool upper, char* out) {
    if (v == 0u) { out[0] = '0'; return 1u; }
    char tmp[32];
    size_t n = 0;
    const char* digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v) { tmp[n++] = digs[v % base]; v /= base; }
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

/** @brief Resolve a `%s` argument to host bytes and the count printed: to its NUL, or at most `precision` bytes when one is given.
 *
 * A guest NULL prints as "(null)"; a string that runs past its region
 * prints as empty.
 * @param addr The argument's guest address.
 * @param precision The conversion's precision, or -1 for none.
 * @param n_out Receives the byte count to print.
 * @return The bytes to print. */
const char* format_str_arg(uint32_t addr, int precision, size_t* n_out) {
    const uint32_t max = precision >= 0 ? (uint32_t)precision : 0xFFFFFFFFu;
    if (!addr) {
        *n_out = max < 6u ? max : 6u;
        return "(null)";
    }
    uint32_t len = 0;
    const char* s = GSTR(addr, max, &len);
    *n_out = s ? len : 0u;
    return s ? s : "";
}

/** @brief Format `fmt` into `dst` from MIPS o32 variadic slots, with snprintf's truncation and return semantics.
 *
 * Conversions `d`, `i`, `u`, `x`, `X`, `o`, `s`, `c`, `p` and `%` are
 * rendered; an unknown conversion is emitted verbatim after a `%`. `dst` is
 * NUL-terminated whenever `cap` is nonzero.
 * @param dst The output buffer.
 * @param cap Capacity of `dst` in bytes, the NUL included.
 * @param fmt The format string, as a host pointer.
 * @param args The slot cursor, advanced once per argument consumed.
 * @return The length the whole output would have had, or 0 when `cap` is
 *         0. */
size_t format_from_mips(char* dst, size_t cap, const char* fmt, MipsArgs& args) {
    size_t pos = 0;
    if (cap == 0) return 0;
    /* Reserve one byte for NUL - put() guards against pos >= cap-1 below. */
    size_t writable = cap - 1u;
    auto put = [&](char c) {
        if (pos < writable) dst[pos] = c;
        ++pos;
    };
    auto put_str = [&](const char* s, size_t n,
                       int width, bool left_align, bool zero_pad) {
        size_t pad = (width > 0 && (size_t)width > n) ? (size_t)width - n : 0u;
        char pc = zero_pad ? '0' : ' ';
        if (!left_align) for (size_t i = 0; i < pad; ++i) put(pc);
        for (size_t i = 0; i < n; ++i) put(s[i]);
        if (left_align) for (size_t i = 0; i < pad; ++i) put(' ');
    };

    while (*fmt) {
        char c = *fmt++;
        if (c != '%') { put(c); continue; }

        bool left_align = false, zero_pad = false;
        for (;;) {
            if (*fmt == '-') { left_align = true; ++fmt; }
            else if (*fmt == '0') { zero_pad = true; ++fmt; }
            else if (*fmt == '+' || *fmt == ' ' || *fmt == '#') { ++fmt; }
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        int precision = -1;
        if (*fmt == '.') {
            ++fmt;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') { precision = precision * 10 + (*fmt++ - '0'); }
        }
        /* Skip length modifiers: h, hh, l, ll, z, j, t. PSX ABI passes
         * everything as a 32-bit slot, so they don't change behavior. */
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') ++fmt;

        char conv = *fmt ? *fmt++ : '\0';
        char numbuf[40];
        switch (conv) {
            case '%': put('%'); break;
            case 'c': {
                char ch = (char)args.fetch();
                put_str(&ch, 1, width, left_align, false);
                break;
            }
            case 's': {
                size_t n = 0;
                const char* s = format_str_arg(args.fetch(), precision, &n);
                put_str(s, n, width, left_align, false);
                break;
            }
            case 'd':
            case 'i': {
                int32_t sv = (int32_t)args.fetch();
                bool neg = sv < 0;
                uint32_t uv = neg ? (uint32_t)(-(int64_t)sv) : (uint32_t)sv;
                size_t n = render_uint(uv, 10, false, numbuf);
                if (neg) {
                    /* Zero-pad after sign. */
                    if (zero_pad && !left_align && width > (int)(n + 1)) {
                        put('-');
                        for (size_t i = 0; i < (size_t)width - n - 1; ++i) put('0');
                        for (size_t i = 0; i < n; ++i) put(numbuf[i]);
                    } else {
                        char tmp[41];
                        tmp[0] = '-';
                        for (size_t i = 0; i < n; ++i) tmp[i + 1] = numbuf[i];
                        put_str(tmp, n + 1, width, left_align, false);
                    }
                } else {
                    put_str(numbuf, n, width, left_align, zero_pad && !left_align);
                }
                break;
            }
            case 'u': {
                uint32_t uv = args.fetch();
                size_t n = render_uint(uv, 10, false, numbuf);
                put_str(numbuf, n, width, left_align, zero_pad && !left_align);
                break;
            }
            case 'x':
            case 'X': {
                uint32_t uv = args.fetch();
                size_t n = render_uint(uv, 16, conv == 'X', numbuf);
                put_str(numbuf, n, width, left_align, zero_pad && !left_align);
                break;
            }
            case 'o': {
                uint32_t uv = args.fetch();
                size_t n = render_uint(uv, 8, false, numbuf);
                put_str(numbuf, n, width, left_align, zero_pad && !left_align);
                break;
            }
            case 'p': {
                uint32_t uv = args.fetch();
                numbuf[0] = '0'; numbuf[1] = 'x';
                size_t n = render_uint(uv, 16, false, numbuf + 2);
                put_str(numbuf, n + 2, width, left_align, false);
                break;
            }
            default:
                /* Unknown - emit the '%' and the conversion char verbatim. */
                put('%');
                if (conv) put(conv);
                break;
        }
    }
    if (pos < cap) dst[pos] = '\0';
    else dst[cap - 1] = '\0';
    return pos;
}

}  /* namespace */

/** @brief Thunk for the C library's printf: formats from the MIPS variadic slots into the log.
 *
 * v0 is the formatted length, or 0 when the format string runs past its
 * region.
 * @param cpu Guest CPU state. */
void psyq_printf(CPUState* cpu) {
    const char* fmt = GSTR(cpu->a0, 0xFFFFFFFFu, nullptr);
    if (!fmt) { cpu->v0 = 0u; return; }
    char buf[512];
    MipsArgs args{cpu, 1};
    size_t n = format_from_mips(buf, sizeof(buf), fmt, args);
    OPL_INFO_F("psx-printf", "%s", buf);
    cpu->v0 = (uint32_t)n;
}

/** @brief Thunk for the C library's sprintf: formats from the MIPS variadic slots into the guest buffer at a0.
 *
 * The output is capped at 4095 bytes plus the NUL. v0 is the formatted
 * length, or 0 with nothing written when the format string or the output
 * span runs past its region.
 * @param cpu Guest CPU state. */
void psyq_sprintf(CPUState* cpu) {
    const char* fmt = GSTR(cpu->a1, 0xFFFFFFFFu, nullptr);
    if (!fmt) { cpu->v0 = 0u; return; }
    /* sprintf has no length argument on the PSX side: the output is
     * formatted into a host buffer with a generous cap, then its bytes and
     * the NUL are copied to a destination span of exactly that length. */
    static char out[4096];
    MipsArgs args{cpu, 2};
    size_t n = format_from_mips(out, sizeof out, fmt, args);
    const size_t written = (n < sizeof out - 1u ? n : sizeof out - 1u) + 1u;
    char* dst = (char*)SPAN(cpu->a0, written);
    if (!dst) { cpu->v0 = 0u; return; }
    memcpy(dst, out, written);
    cpu->v0 = (uint32_t)n;
}

}  /* extern "C" */
