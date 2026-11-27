// openpete_mods.h - ENGINE-facing mod framework API (loader + chains).
//
// The mod-facing surface lives in openpete_mod_api.h; this header is what
// the runtime and per-game code call.
//
// Interposition rides the recompiler's universal function-entry
// chokepoint: every recompiled function's prologue calls
// psx_override_dispatch(cpu, addr) (code_generator.cpp), which fans into
// the registered game's override_dispatch, which calls openpete_mods_dispatch()
// below. Direct jal calls, indirect calls, and dispatched calls all pass
// through the callee's own prologue, so a per-address chain here
// intercepts every call to a chosen function with zero generated-C edits.
// Cost when no mods are loaded: one global-flag branch
// (openpete_mods_active == 0).

#ifndef OPENPETE_MODS_H
#define OPENPETE_MODS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* FILE - savestate mods-section hooks */

#include "openpete.h" /* openpete_options_t */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* ── Parked former-ABI types ─────────────────────────────────────────
 * These left the mod-facing header at the api-v1 cut but their ENGINE
 * machinery survives (custom pipelines, materials,
 * SoundDefinition plumbing, level-ready driving, state blobs). They
 * live here, engine-internal, until their surfaces re-land as
 * append-only pillars. */

typedef void (*openpete_mod_level_fn)(struct CPUState* cpu, int level_id);

/* Engine-side spelling of openpete_mod_refine_fn (openpete_mod_api.h,
 * included further down - the struct is forward-declared so the two
 * typedefs stay compatible without reordering this header). No
 * CPUState: refine runs in tick context and reads guest RAM through the
 * mod api's own accessors, which keeps an engine-internal type out of
 * the public ABI. */
struct openpete_mod_material_key;
typedef int (*openpete_mod_material_fn)(
        const struct openpete_mod_material_key* key,
        void* uniform_out, uint32_t cap);
typedef struct CPUState CPUState;

/* Name->vaddr symbol table (generated from manifest/symbols.toml by
 * tools/gen_mod_sdk.py into sdk_symbols_table.c, game-side). The game
 * registers it through game_mod_desc_t. Entries sorted by name, then
 * overlay id; one row per (name, overlay), so a name whose rows lie in
 * several overlays occupies adjacent rows. */
typedef struct {
    const char* name;
    uint32_t    addr;
    uint8_t     legacy; /* 1 = historical raw alias (func_XXXXXXXX for a
                         * since-renamed function) - resolvable forever
                         * (decomp promotion contract), but the loader
                         * warns so mods migrate to the stable name. */
    uint32_t    overlay; /* overlay table id of the row's function; 0 = EXE */
} openpete_sdk_symbol_t;

/** @brief Name the guest function that contains a guest address.
 *
 * Picks the registered symbol with the largest address <= addr by a
 * linear scan (the table is sorted by name); on an address tie a
 * non-legacy row wins, since legacy rows are raw-name aliases of the same
 * address. The table carries no sizes, so a symbol 0x10000 bytes or more
 * below addr (SYM_ADDR_SPAN_MAX in mod_loader.c) is rejected as an
 * unrelated function. Read-only over a static table, so the crash handler
 * (core/diag/crash.c) may call it.
 * @param addr Guest address; 0 returns NULL.
 * @param off_out Receives addr minus the symbol address on a hit; may be
 *        NULL.
 * @return The symbol name, or NULL when no table is registered or no
 *         symbol is near enough. */
const char* openpete_mods_symbol_for_addr(uint32_t addr, uint32_t* off_out);

/** @brief Discover, build, load and enter mods, once at boot.
 *
 * core/runtime/run.c calls it after options parsing. The mods root is
 * opts->mods_dir, else env OPENPETE_MODS_DIR, else the writable data
 * location openpete_paths_data_dir resolves for "mods" (next to the exe in
 * a portable install, the per-user data directory otherwise). The enabled
 * set is opts->mods_enabled (--mods), else env OPENPETE_MODS, else
 * enabled.txt, else every discovered mod in lexicographic order; the order
 * is the priority (later is higher). With enabled.txt present, no --mods
 * list and a windowed run, every other discovered mod also loads with its
 * entry deferred to its first enable. Manifests are parsed, topo-sorted by
 * hard deps, then built and loaded in that order; a failed load disables
 * its dependents. A missing or empty directory loads nothing, leaves
 * openpete_mods_active at 0 and keeps the sim path identical to a modless
 * build. A broken mod is disabled with a log line and never fails the
 * launch.
 * @param opts Parsed options.
 * @return The number of mods loaded, including those loaded disabled. */
int openpete_mods_init(const openpete_options_t* opts);

/** @brief The mods root directory the boot discovery resolved.
 *
 * The directory openpete_mods_init scans (--mods-dir, OPENPETE_MODS_DIR, or
 * the data location), whether or not it exists or holds a mod.
 * @return The path, or NULL before discovery ran or when no root resolved. */
const char* openpete_mods_root_dir(void);

/** @brief Open the mods root directory in the platform's file manager, creating it first when it is missing.
 *
 * The directory is the one mods_root resolves from @p opts (--mods-dir,
 * OPENPETE_MODS_DIR, else the data location), or with NULL the one the boot
 * discovery resolved (openpete_mods_root_dir). SDL_OpenURL hands a plain
 * path to the shell on Windows and to the desktop portal or xdg-open on
 * Linux; macOS takes a file:// URL. Under the harness variable
 * OPENPETE_LAUNCHER_OPEN_URL_STUB=1 it logs the directory and opens nothing.
 * @param opts The options, or NULL for the discovered root.
 * @return 0 when opened (or stubbed), -1 when no root resolves or the open failed. */
int openpete_mods_open_folder(const openpete_options_t* opts);

/** @brief Register every discovered mod's [[binding]] defaults with the keymap without loading any mod.
 *
 * Discovers mods the way openpete_mods_init does, parses each mod.toml,
 * registers each binding through openpete_keymap_set_mod_default (rank 0
 * under the mod.<id> domain) and records the mod's boot enabled state with
 * openpete_keymap_set_mod_enabled. A manifest that fails to parse is
 * skipped. The Spyro 1 main calls it for --keys-report, which prints the
 * keymap and exits before openpete_mods_init runs; init registers the same
 * rows itself.
 * @param opts Parsed options; opts->mods_dir overrides the mods root. */
void openpete_mods_register_key_defaults(const openpete_options_t* opts);

/* Callbacks openpete_mods_prepare takes: a progress sink, the compile task,
 * and a runner that executes the task (on a worker or inline). */
typedef void (*openpete_mods_progress_fn)(const char* stage, int done,
                                          int total);
typedef int (*openpete_mods_task_fn)(void* userdata);
typedef int (*openpete_mods_run_task_fn)(openpete_mods_task_fn fn,
                                         void* userdata);
/** @brief Compile every discovered source mod whose build key is stale, before openpete_mods_init.
 *
 * The game calls it while its splash is up, so the compiles show as a
 * progress stage. Returns 0 at once when no compiler resolves, no mod is
 * discovered, or the SDK is missing (a notice is logged when a source mod
 * exists). Pass 1 hashes each discovered mod's sources, skipping
 * assets-only and binary-only mods and mods whose cached build matches the
 * key; when nothing is stale neither callback runs. Otherwise the stale
 * set compiles inside one @p run_task call, reporting
 * progress("COMPILING MODS", done, total). A compile failure is logged and
 * left for openpete_mods_init to disable the mod. Init re-checks the cache
 * and compiles anything still stale inline, so whether prepare ran changes
 * only when the compile time is spent.
 * @param opts Parsed options; opts->mods_dir overrides the mods root.
 * @param progress Progress sink called from the task, or NULL.
 * @param run_task Runs the compile task, or NULL to run it inline.
 * @return The number of stale mods found. */
int openpete_mods_prepare(const openpete_options_t* opts,
                          openpete_mods_progress_fn progress,
                          openpete_mods_run_task_fn run_task);

/** @brief Compile every source mod in the resolved mods root, for the --compile-mods utility mode.
 *
 * Runs before any game is identified and reads no game descriptor. The SDK
 * is <exe>/sdk, or OPENPETE_MOD_SDK_DIR when set. For each game staged
 * under the SDK's games/ (in id order; logged "--compile-mods: staged
 * <id>") it scans every candidate, ignoring enabled.txt so a disabled
 * mod's breakage still surfaces, keeps the mods whose [mod] game selects
 * that id, and compiles them against games/<id>/ into .build/<id>/ through
 * the same key, stamp and compile path boot uses: a mod whose cached build
 * matches its key is skipped, so a boot after a clean pass recompiles
 * nothing. Assets-only and binary-only mods are skipped. The CMake target
 * spyro1_mods runs it on native presets; cross presets compile the staged
 * mods in CMake instead (games/spyro-1/CMakeLists.txt).
 * @param opts Parsed options; opts->mods_dir overrides the mods root.
 * @return The number of failed compiles over every staged game (0 on
 *         success), or -1 when no compiler resolves, no mods root
 *         resolves, or no game is staged. */
int openpete_mods_compile_all(const openpete_options_t* opts);

/** @brief Run the override chain registered for a guest function at its entry.
 *
 * Called from every game's dispatch preamble (game_dispatch_preamble)
 * behind the openpete_mods_active gate. Returns 0 without running anything
 * when a base() fallthrough armed a bypass for addr (the bypass is
 * consumed and the recompiled body runs), when no link on any chain is
 * enabled, when addr has no chain, or when every link on its chain is
 * disabled or, for an address inside the game's overlay window, bound to
 * an overlay other than the resident one. Otherwise runs the first such
 * link.
 * @param cpu the guest CPU.
 * @param addr the entered function's guest vaddr.
 * @return 1 when a chain ran (skip the recompiled body), 0 to run the
 *         body. */
int openpete_mods_dispatch(CPUState* cpu, uint32_t addr);

/* 1 while at least one override chain is registered. Hot-path gate. */
extern int openpete_mods_active;

/* The overlay id of an unscoped override link: it runs whichever overlay
 * is resident. 0 binds an EXE target; any other value is an overlay id. */
#define OPENPETE_MODS_OVERLAY_ANY 0xFFFFFFFFu

/** @brief Report whether base() armed the one-shot bypass for an address.
 *
 * The dispatch preamble reads it before the mod arm consumes the bypass,
 * so the re-entry a base() fallthrough makes is not noted twice by the
 * write census.
 * @param addr the entered function's vaddr.
 * @return 1 while the bypass is armed for addr, else 0. */
int openpete_mods_bypass_pending(uint32_t addr);

/* A game's guard over guest bytes a mod may not change. A mod writes guest
 * RAM through api->guest() host pointers, which no guest store hook sees, so
 * the guard brackets every stretch of mod code instead: begin() before it
 * runs, end() after it returns or descends into guest code (base(), call()).
 * end() compares the guarded bytes against their state at begin(), restores
 * any changed byte and names the mod. NULL members are not allowed.
 *
 * The bracketed stretches: an override link, each half of it around base()
 * and call(), a mod's entry and hot-reload entry, its toggle hook, its
 * level-ready hooks, and its material refine callbacks. One stretch is open
 * at a time: entering a stretch inside an open one closes the outer stretch
 * first (its changes are restored and named against its owner) and reopens
 * it when the inner one leaves, so no stretch adopts another's change as its
 * baseline. Stretches run only on the thread openpete_mods_guard_claim_thread
 * names; a bracket on any other thread is skipped.
 *
 * Outside the guard's reach: a store through a pointer kept from an earlier
 * stretch and used from a present hook, a UI body, a refine callback the
 * renderer's present thread runs, or a thread the mod started. The next
 * begin() takes such a change as its baseline and nothing restores it. */
typedef struct openpete_mods_guest_guard {
    void (*begin)(void);
    void (*end)(const char* mod_id);
} openpete_mods_guest_guard_t;

/** @brief Install the game's guest-byte guard, or remove it with NULL.
 *
 * openpete_mods_init installs game_mod_desc_t.guest_guard before any mod's
 * entry runs.
 * @param guard The guard; the pointer is kept, so it must outlive the run. */
void openpete_mods_set_guest_guard(const openpete_mods_guest_guard_t* guard);

/** @brief Name the calling thread as the one guarded stretches run on.
 *
 * Called on the thread that runs the game's ticks, before the game fiber
 * first runs. Until then every thread's bracket runs, which covers the
 * entries openpete_mods_init calls before the host loop's threads start. */
void openpete_mods_guard_claim_thread(void);

/** @brief Report whether the guard compares and restores guarded bytes, which --scan-guard 0 turns off.
 *
 * With compares off a guard still sees every bracket, does no comparison
 * and restores nothing, so a mod's stores into the guarded bytes persist.
 * @return 0 under --scan-guard 0 (or OPENPETE_SCAN_GUARD=0), else 1. */
int openpete_mods_guest_guard_compares(void);

/** @brief Open a guarded stretch of a mod's code and push its id as the stretch's owner.
 *
 * No effect with no guard installed or off the claimed thread. An open
 * outer stretch is closed first and reopened by the matching leave. Every
 * call pairs with one openpete_mods_guard_leave.
 * @param mod_id The id of the mod whose code is about to run. */
void openpete_mods_guard_enter(const char* mod_id);

/** @brief Close the innermost guarded stretch, attributing any guarded-byte change to its owner, and pop it, reopening an outer stretch its enter closed. */
void openpete_mods_guard_leave(void);

/** @brief Close the innermost guarded stretch without popping it, before mod code descends into guest code. */
void openpete_mods_guard_suspend(void);

/** @brief Reopen the innermost guarded stretch after the guest code a suspend bracketed returns. */
void openpete_mods_guard_resume(void);

/* One per-link call counter row: calls of the link (owner, addr, overlay)
 * made while `resident` was the resident overlay
 * (OPENPETE_MODS_OVERLAY_ANY for an address outside the overlay window). */
typedef void (*openpete_mods_link_calls_fn)(void* ctx, const char* owner,
                                            uint32_t addr, uint32_t overlay,
                                            uint32_t resident,
                                            uint64_t calls);

/** @brief Visit every per-link call counter row of the override chains.
 *
 * A row is (owner, chain address, the link's bound overlay, the resident
 * overlay of the counted calls, calls); it is added on the link's first
 * call under that resident overlay and outlives the link. With
 * OPENPETE_MOD_LINK_CALLS set the rows are also printed to stderr at
 * process exit, one "[mods] link-calls owner=... addr=0x... overlay=...
 * resident=... calls=..." line each.
 * @param fn called once per row; NULL only counts.
 * @param ctx passed to fn.
 * @return The number of rows. */
int openpete_mods_link_calls(openpete_mods_link_calls_fn fn, void* ctx);

/* bk2 input-movie status (read-only v1 of the bk2 access service):
 * run.c publishes once per sim frame while a movie is loaded; mods
 * the host reads it back through openpete_mods_bk2_query. Record/replay
 * CONTROL stays engine-owned (mid-session anchoring is TAS infra). */
typedef struct {
    int active;      /* a movie is loaded this session               */
    int frame;       /* current movie frame (<0 before the offset)   */
    int frame_count; /* total frames in the movie                    */
    int overriding;  /* this tick's input actually came from it      */
} openpete_mods_bk2_status_t;
/** @brief Store the movie status the run loop reports for the current tick.
 *
 * The run loop calls it while a bk2 movie is loaded, including before the
 * movie's input offset and past its end (overriding 0). The status is
 * copied by value with no locking. A NULL @p st is ignored.
 * @param st The status to store. */
void openpete_mods_bk2_publish(const openpete_mods_bk2_status_t* st);
/** @brief Copy out the last published movie status.
 *
 * Reads all zero (active 0) until a movie publishes. Host-side only; the
 * mod API has no accessor for it.
 * @param out Receives the status; NULL does nothing. */
void openpete_mods_bk2_query(openpete_mods_bk2_status_t* out);

/* The game-neutral state the mod api's game services return
 * (openpete_mod_api::gameplay_live, player_pose, level_id). */
typedef struct {
    int32_t  live;     /* game_is_gameplay_live                           */
    int32_t  pose_rc;  /* game_read_player_pose: 1 placeable, 0 live, -1 none */
    int32_t  x, y, z;  /* player position; valid when pose_rc >= 0        */
    int32_t  yaw;      /* 12-bit facing, -1 when not located              */
    int32_t  level;    /* level id, -1 when pose_rc < 0                   */
    uint32_t tick;     /* canonical tick sampled at; 0 for a live read    */
} openpete_mods_game_state_t;
/** @brief Read the game-neutral state from guest RAM at the call.
 *
 * Reads only; tick context.
 * @param cpu The game CPU.
 * @param out Receives the state, tick 0. */
void openpete_mods_game_state_read(CPUState* cpu, openpete_mods_game_state_t* out);
/** @brief Sample the game-neutral state for calls from outside tick context.
 *
 * openpete_mods_tick_boundary calls it on the sim thread. The record is
 * published under a sequence counter, so a reader on the compose thread
 * never sees a mix of two ticks.
 * @param cpu The game CPU.
 * @param canonical_tick The canonical tick the sample belongs to. */
void openpete_mods_game_state_sample(CPUState* cpu, unsigned canonical_tick);
/** @brief Copy out the last sampled game-neutral state.
 *
 * Before the first sample it reads as no level live (live 0, pose_rc -1,
 * level -1). Legal from any thread.
 * @param out Receives the state. */
void openpete_mods_game_state_sampled(openpete_mods_game_state_t* out);

/* Guest scratch arena (core/mods/mod_arena.c): session-lifetime bump
 * allocator above the 2 MB of RAM in two pools, the DMA-reachable pool
 * [0x80200000, 0x81000000) and the general pool [0x81000000, 0x9F000000),
 * each segment registered as a psx_runtime region at its committed
 * extent - guest-addressable through the standard accessors, host-
 * addressable via *host_view. flags = OP_GALLOC_*. Every allocation is ledgered by
 * (owner, per-owner call index); the ledger is what the carry gate,
 * re-entry replay and the savestate verdict all consume. Returns the
 * guest vaddr, 0 on failure (exhausted, ledger full, bad args). */
#define OPENPETE_ARENA_OWNER_MAX 64 /* == the loader's MOD_ID_MAX */
/** @brief Allocate a block in the guest arena, or replay the owner's ledger row for this call.
 *
 * Runs the owner's next call index. When the owner's live row at that
 * index is an allocation with the same size, align and flags, that row is
 * served again: same vaddr, same host view, bytes as last written. A row
 * of another shape is marked superseded and the call takes a fresh block,
 * aligned up and zeroed. Without OP_GALLOC_NO_DMA the block goes to the
 * DMA-reachable pool: the upper segment [0x80700000, 0x81000000) when it
 * fits at that segment's cursor, else the lower segment
 * [0x80200000, 0x80700000) when it fits there. With the flag it goes to
 * the general pool [0x81000000, 0x9F000000). A segment registers as a
 * psx_runtime region on its first block and grows in 64 KiB granules. A
 * failure logs and returns 0, except a bad argument, which returns 0
 * silently; an exhausted pool logs every owner's use of it.
 * @param size the block size in bytes; 0 fails, as does more than 9 MiB
 *        in the DMA-reachable pool or 480 MiB in the general pool.
 * @param align the alignment, a power of two; 0 selects 8, any other
 *        value fails.
 * @param flags OP_GALLOC_* flags, stored in the row as passed.
 * @param host_view receives the block's host address, or NULL on failure;
 *        may be NULL.
 * @param owner the calling mod's id; NULL or empty fails.
 * @return the block's guest vaddr, or 0 on failure: a bad argument, a
 *         host reservation, commit or region registration failure, an
 *         exhausted pool, or an owner or ledger table allocation failure. */
uint32_t openpete_mods_guest_alloc(uint32_t size, uint32_t align,
                                   uint32_t flags, void** host_view,
                                   const char* owner);

/** @brief Reset an owner's ledger call cursor so its next calls replay from its first ledger row.
 *
 * The cursor is shared by openpete_mods_guest_alloc and
 * openpete_mods_vstream_handle calls. The loader calls this immediately
 * before running openpete_mod_entry at load and on hot reload, so a
 * matching call sequence gets back the same vaddrs and handles.
 * @param owner the mod id; NULL, empty, or an owner with no ledger calls
 *        does nothing. */
void openpete_mods_arena_entry_begin(const char* owner);

/** @brief Close an owner's entry run, logging the owner's pool use once per session when it holds any.
 *
 * The loader calls this immediately after openpete_mod_entry returns, at
 * load and on hot reload. The line names the bytes the owner's ledger rows
 * hold in each pool, mutable and IMMUTABLE. An owner whose first block
 * comes later, from a tick or toggle hook, is logged after that block.
 * @param owner the mod id; NULL or empty does nothing. */
void openpete_mods_arena_entry_end(const char* owner);

/** @brief One owner's bytes in each pool, from its ledger rows. */
typedef struct {
    uint32_t dma_bytes;          /* DMA-reachable pool, every row */
    uint32_t dma_immutable;      /* of which OP_GALLOC_IMMUTABLE rows */
    uint32_t general_bytes;      /* general pool, every row */
    uint32_t general_immutable;  /* of which OP_GALLOC_IMMUTABLE rows */
} openpete_mods_arena_use_t;

/** @brief Read an owner's pool use, safe from any thread.
 *
 * The figures are the sizes of the owner's allocation rows, superseded
 * rows included (their bytes stay allocated), alignment padding excluded.
 * They are published per owner with atomic stores by the allocating
 * thread; an owner past the first 256 that allocated reads as zero.
 * @param owner the mod id.
 * @param out receives the use; all zero when the owner holds nothing.
 * @return 1 when the owner holds a block, else 0. */
int  openpete_mods_arena_owner_use(const char* owner,
                                   openpete_mods_arena_use_t* out);

/** @brief Record one runahead tick's in-memory snapshot cost, save plus restore.
 *
 * Called by the runahead driver on the simulation thread; the figure is an
 * exponential mean (weight 1/16) published with an atomic store.
 * @param us the tick's save plus restore time in microseconds. */
void     openpete_mods_runahead_snapshot_note(uint64_t us);
/** @brief Return the mean runahead snapshot cost per tick, safe from any thread.
 *
 * @return microseconds, save plus restore; 0 before the first speculation. */
uint32_t openpete_mods_runahead_snapshot_us(void);

/* Snapshot support: the arena's three used ranges are timeline state,
 * in the order upper segment, lower segment, general pool. save copies
 * each [base, base + used); restore is a RE-ANCHOR - bytes allocated
 * after the snapshot keep their contents and no cursor moves backwards.
 * immutable_check verifies every IMMUTABLE row's checksum (captured on
 * first check) and warns once per broken promise; call it once per
 * keyframe / file save, never per speculative tick. */
enum {
    OPENPETE_ARENA_RANGE_UPPER = 0,   /* DMA-reachable [0x80700000, 0x81000000) */
    OPENPETE_ARENA_RANGE_LOWER,       /* DMA-reachable [0x80200000, 0x80700000) */
    OPENPETE_ARENA_RANGE_GENERAL,     /* general [0x81000000, 0x9F000000) */
    OPENPETE_ARENA_RANGES
};
/** @brief Return the arena's used bytes per range and in total.
 *
 * A range's used bytes run from its guest base to the end of its last
 * allocation, alignment padding included. Snapshots copy these ranges and
 * the runahead hash covers them; a restore raises a range's cursor, never
 * lowers it.
 * @param used receives each range's used bytes, indexed by
 *        OPENPETE_ARENA_RANGE_*; may be NULL.
 * @return the sum over the three ranges. */
uint32_t       openpete_mods_arena_used(uint32_t used[OPENPETE_ARENA_RANGES]);
/** @brief Return a range's guest base vaddr.
 *
 * @param range OPENPETE_ARENA_RANGE_*.
 * @return the base, or 0 for a range out of bounds. */
uint32_t       openpete_mods_arena_range_vaddr(unsigned range);
/** @brief Return a range's host bytes: its first byte, valid for the process lifetime.
 *
 * Offsets [0, used) of the range hold allocated data (see
 * openpete_mods_arena_used).
 * @param range OPENPETE_ARENA_RANGE_*.
 * @return the first byte, or NULL while the range's pool has no host
 *         reservation (it then has no used bytes) or for a range out of
 *         bounds. */
const uint8_t* openpete_mods_arena_range_bytes(unsigned range);
/** @brief Copy the three used ranges, upper, lower then general, into one buffer.
 *
 * @param dst a buffer of at least openpete_mods_arena_used(NULL) bytes.
 * @param used receives each range's byte count, as
 *        openpete_mods_arena_used reports it. */
void           openpete_mods_arena_mem_save(void* dst,
                                            uint32_t used[OPENPETE_ARENA_RANGES]);
/** @brief Copy snapshot bytes over each range's prefix and raise each cursor to cover them.
 *
 * Bytes past a range's saved count keep their contents and no cursor
 * decreases, so a handle a mod took after the snapshot stays valid. A
 * range whose saved count exceeds its committed extent is committed (and
 * registered) first; a range that cannot be is skipped with an error.
 * @param bytes the ranges as openpete_mods_arena_mem_save wrote them; NULL
 *        does nothing.
 * @param used each range's byte count; a count above the range's capacity
 *        is clamped to it. */
void           openpete_mods_arena_mem_restore(const void* bytes,
                                               const uint32_t used[OPENPETE_ARENA_RANGES]);
/** @brief Commit a range's first n bytes, raise its cursor to cover them, and return its host bytes for a snapshot restore to fill.
 *
 * The re-anchor step of openpete_mods_arena_mem_restore for one range:
 * bytes past n keep their contents and the cursor never decreases. A
 * count above the range's capacity is clamped to it.
 * @param range OPENPETE_ARENA_RANGE_*.
 * @param n in: the snapshot's byte count for the range; out: the count
 *        clamped to the range's capacity.
 * @return the range's first host byte, writable for the clamped count, or
 *         NULL for a zero count, a range out of bounds, or a commit
 *         failure (logged). */
uint8_t*       openpete_mods_arena_range_reanchor(unsigned range, uint32_t* n);
/** @brief Check every live IMMUTABLE row's bytes against its first-seen checksum.
 *
 * The first check of a row records the FNV-1a 64-bit hash of its bytes; a
 * later mismatch logs one warning naming the owner and call index and
 * stops checking that row. Superseded rows are skipped.
 * @param when the caller's context, named in the warning; NULL reads as
 *        "snapshot". */
void           openpete_mods_arena_immutable_check(const char* when);

/** @brief Sample the arena's changed 4 KiB pages at a final yield when OPENPETE_ARENA_PAGE_CENSUS is set.
 *
 * Diagnostic: compares the used prefix with a shadow copy, groups the
 * changed pages by game tick (openpete_canon_tick), attributes them to
 * every owner with a live allocation row on them, and reports per-owner
 * mean, p99 and max pages per tick and per 30-tick window at exit
 * (core/mods/arena_page_census.c). Off, it reads the variable once and
 * returns. Call at each final yield on the simulation thread. */
void           openpete_mods_arena_page_census_tick(void);

/** @brief Return the stream handle for a stream vaddr, the index a mod writes into a frame word's 21-bit field in place of the vaddr.
 *
 * The same vaddr, from any owner, gets the same handle; handle 0 is never
 * issued. Every call is a ledger row keyed by (owner, call index), sharing
 * the call cursor with openpete_mods_guest_alloc: on re-entry a call whose
 * row holds the same vaddr gets that row's handle back and rebinds it in
 * the vstream table; a different vaddr or an allocation row at that index
 * marks the row superseded and takes a fresh handle, and a savestate
 * written against a superseded row is refused on load.
 * @param vaddr the stream's guest vaddr.
 * @param owner the calling mod's id.
 * @return the handle, or 0 when refused: vaddr 0, a vaddr in the kernel
 *         area (physical address below 0x10000), a NULL or empty owner,
 *         the handle table full (VSTREAM_HANDLE_MAX), or a ledger or
 *         owner-table allocation failure. */
uint32_t openpete_mods_vstream_handle(uint32_t vaddr, const char* owner);

/** @brief Play the PCM WAV at path once as a host-side sound.
 *
 * The sound plays on one of the SPU render's host clip voices, added after
 * the SPU's main volume in guest time, so the device, --record and the
 * audio dump carry it; it writes no SPU register or guest state and is
 * legal from tick and present hooks. A play from a speculated tick is
 * rewound with it; one from a present hook persists. The WAV is loaded
 * through op_wav_load on its first play and its clip is cached by path.
 * While muted the call plays nothing and returns 0.
 * @param path WAV file path.
 * @param gain Linear gain, clamped to [0, 4].
 * @return 0 when played or muted; -1 when path is NULL, or the load or
 *         upload fails (logged). */
int openpete_mod_sound_play(const char* path, float gain);

/** @brief Play in-memory interleaved s16 PCM once as a host-side sound.
 *
 * The counterpart of openpete_mod_sound_play for audio the caller already
 * holds, so nothing is staged on disk. The first call for a key copies
 * the samples into a clip cached under the key; later calls with the same
 * key play the cached clip and ignore pcm, frame_count, channels and
 * sample_rate. The caller keeps ownership of pcm.
 * @param key Cache label for the sound, not a path.
 * @param pcm Interleaved samples.
 * @param frame_count Sample frames in pcm.
 * @param channels 1 or 2.
 * @param sample_rate Sample rate in Hz.
 * @param gain Linear gain, clamped to [0, 4].
 * @return 0 when played or muted; -1 when key is NULL, an uncached key
 *         comes with NULL pcm, zero frames or a channel count other than 1
 *         or 2, or the upload fails. */
int openpete_mod_sound_play_pcm(const char* key, const int16_t* pcm,
                                uint32_t frame_count, int channels,
                                uint32_t sample_rate, float gain);

/** @brief Set the engine mute for mod sounds; muting stops every playing clip.
 *
 * Called by openpete_set_mute(). While muted, play calls return 0 without
 * playing.
 * @param on Nonzero mutes. */
void openpete_mod_sound_set_mute(int on);

/* Live modset toggle: queue an enable/disable; it applies at the
 * NEXT tick boundary by flipping owner-keyed disabled flags across all
 * registration surfaces (nothing is torn down - re-enable restores the
 * mod exactly where it was). Asset overlays follow the toggle at their
 * consumers' own boundaries: textures at the next resolve, music at the
 * next track start, level data at the next level load.
 * openpete_mods_tick_boundary is the apply point, called at the
 * canonical present commit with the running canonical-tick counter;
 * OPENPETE_MODS_TOGGLE="tick:id:0|1[,...]" schedules for CI/dev. */
/* Loaded-mod enumeration (the mod menu's data source). */
/** @brief Return the number of loaded mods, enabled or disabled.
 *
 * @return The row count of the loaded mod table. */
int         openpete_mods_count(void);
/** @brief Return the id of loaded mod i.
 *
 * The string lives in the loader's mod table for the session.
 * @param i Index into the loaded mod table, 0 <= i < openpete_mods_count().
 * @return The id, or NULL for an index out of range. */
const char* openpete_mods_id(int i);
/** @brief Report whether the loaded mod with an id is currently enabled.
 *
 * Reads the live-toggle flag by a linear scan of the mod table by id.
 * @param id Mod id, non-NULL.
 * @return 1 when enabled, 0 when disabled or when no loaded mod has the id. */
int         openpete_mods_is_enabled(const char* id);

/* Per-mod row for the dev overlay's Mods panel. Strings are loader-
 * owned and live for the session. */
typedef struct {
    const char* id;
    const char* name;    /* manifest display name (falls back to id) */
    const char* version;
    int enabled;         /* live-toggle state */
    int entered;         /* openpete_mod_entry has run (deferred-entry
                          * load-all: boot-disabled mods enter on first
                          * enable) */
    int sim_mutative;    /* effective verdict: does the mod write guest
                          * RAM? Declared, or derived by the engine. */
    int has_ui;          /* a UI section (register_ui / section descriptor)
                          * or a live ui_status block */
    int ui_always;       /* the section carries OPENPETE_MOD_UI_ALWAYS: its
                          * body draws from the overlay's top-level pass on
                          * every present, not inside this mod's node */
    int has_assets;      /* assets/ overlay root (boot-scoped tier) */
    int has_src;
    int has_cfg;         /* [[config]] schema rows in the manifest */
    uint64_t imgui_calls; /* ImGui calls the section's last body
                           * made; 0 = the mod does not use ImGui,
                           * or its section has not drawn yet */
} openpete_mods_menu_info_t;
/** @brief Fill the Mods panel row for loaded mod i.
 *
 * The name falls back to the id when the manifest gives none, has_ui is
 * set for a UI body or a live ui_status block, has_cfg is the number of
 * [[config]] rows, and sim_mutative is the effective verdict.
 * @param i Index into the loaded mod table.
 * @param out Row to fill; its strings point into the mod table.
 * @return 1 when filled, 0 for an index out of range or a NULL @p out. */
int openpete_mods_menu_info(int i, openpete_mods_menu_info_t* out);

/** @brief Return loaded mod i's directory and the compiled object it was loaded from, for a bug report's copy.
 *
 * @param i Index into the loaded mod table.
 * @param dir Receives the mod's directory; may be NULL.
 * @param obj Receives the object path, "" when no object was loaded; may be NULL.
 * @return 1 when filled, 0 for an index out of range. Strings live in the mod table. */
int openpete_mods_bundle_paths(int i, const char** dir, const char** obj);

/** @brief Return the number of mods refused at load for a game their manifest requires.
 *
 * A refused mod is not in the loaded table; the Mods panel lists these
 * rows beside it, each with its reason.
 * @return The row count. */
int openpete_mods_refused_count(void);
/** @brief Read refused row i: the mod id and the reason it did not load.
 *
 * @param i Index, 0 <= i < openpete_mods_refused_count().
 * @param id Receives the mod id, owned by the loader for the session; may be NULL.
 * @param reason Receives the reason, owned likewise; may be NULL.
 * @return 1 when filled, 0 for an index out of range. */
int openpete_mods_refused_at(int i, const char** id, const char** reason);

/** @brief Callback of openpete_mods_missing_games: one enabled mod and one game it requires with no disc in the library.
 *
 * @param ud The caller's pointer.
 * @param mod_id The mod id.
 * @param game The game id. */
typedef void (*openpete_mods_missing_game_fn)(void* ud, const char* mod_id,
                                              const char* game);
/** @brief Report each enabled mod's required games that have no disc in the library, before any game boots.
 *
 * The launcher's note on its library screen. Reads the enabled order
 * (--mods, else enabled.txt; with neither nothing is reported) and each
 * listed mod's manifest; compiles and loads nothing. A mod whose `game`
 * key does not select @p running is skipped, as is a required game equal
 * to @p running, or, while @p running is NULL, equal to the mod's one
 * `game`. Ids that name no registered game are skipped: no disc supplies
 * them.
 * @param opts The options naming the mods root and the enabled list.
 * @param running The game about to run, or NULL while the launcher names none.
 * @param fn Called once per mod and missing game.
 * @param ud Passed to @p fn.
 * @return The number of calls made. */
int openpete_mods_missing_games(const openpete_options_t* opts, const char* running,
                                openpete_mods_missing_game_fn fn, void* ud);

/* ── Config schema ([[config]] rows in mod.toml) ─────────────────────
 * The author declares what is configurable; the ENGINE renders each
 * mod's settings section in the Mods panel from these rows. Values
 * live in the user's config.toml (written back with a comment-
 * preserving surgical edit) and reach the mod through the config_*
 * getters when its entry re-runs after the queued reload. */
typedef struct {
    const char* key;    /* dotted config path ("gems.sfx") */
    char        type;   /* 'b' bool, 'i' int, 'd' float, 's' string */
    const char* label;  /* row label (falls back to key) */
    const char* group;  /* section header within the mod's block, "" = none */
    const char* help;   /* tooltip text, "" = none */
    double      min, max;   /* numeric range; min >= max = free input */
    const char* const* items; /* string type: combo choices (else NULL) */
    int         n_items;
    /* Current effective value: config.toml if present, else the schema
     * default. sval is loader-owned and valid until the next row call. */
    int64_t     ival;   /* bool/int */
    double      fval;   /* float */
    const char* sval;   /* string */
    int         secret; /* `secret = true`: a bug report lists the key, never its value */
} openpete_mods_cfg_row_t;

typedef struct {
    const char* key;
    char        type;   /* same encoding as the row */
    int64_t     ival;
    double      fval;
    const char* sval;
} openpete_mods_cfg_edit_t;

/** @brief Return the number of [[config]] rows loaded mod mod_index declares.
 *
 * @param mod_index Index into the loaded mod table.
 * @return The row count, 0 for an index out of range. */
int openpete_mods_cfg_row_count(int mod_index);
/** @brief Fill a config row with its schema and current effective value.
 *
 * The value comes from the mod's config getters (config.toml, else the
 * schema default) into ival for bool and int rows, fval for float rows,
 * and sval for string rows; the other value fields are 0 or "". The label
 * falls back to the key. The combo choices in items and the string in sval
 * live in buffers the function reuses, so they stay valid only until the
 * next call, and a string longer than 127 bytes is truncated. Not safe to
 * call from two threads.
 * @param mod_index Index into the loaded mod table.
 * @param row Row index, 0 <= row < openpete_mods_cfg_row_count().
 * @param out Row to fill.
 * @return 1 when filled, 0 for an index out of range or a NULL @p out. */
int openpete_mods_cfg_row(int mod_index, int row,
                          openpete_mods_cfg_row_t* out);
/** @brief Write config edits into a mod's config.toml and queue its reload for the next tick boundary.
 *
 * openpete_mods_config_write writes the file. openpete_mods_tick_boundary
 * then reloads a source mod whose entry has run, so the entry re-reads
 * the values through config_*, and re-parses the file for any other mod.
 * The dev overlay's Mods panel and the OPENPETE_MODS_CFG_APPLY schedule
 * call it.
 * @param mod_index Index into the loaded mod table.
 * @param n Number of edits.
 * @param edits The edits.
 * @return 0 when written (the reload is asynchronous), -1 for an index
 *         out of range, n <= 0, NULL edits, or a failed write. */
int openpete_mods_cfg_apply(int mod_index, int n,
                            const openpete_mods_cfg_edit_t* edits);

struct openpete_mod_ui;
/** @brief Draw mod i's ui_status lines and run its UI section body with the overlay's widget table.
 *
 * Draws the status lines first, then the body under the render-purity
 * guard (openpete_mods_in_present), unless the section is flagged
 * OPENPETE_MOD_UI_ALWAYS: that body belongs to the always pass below and
 * would otherwise run twice per present. A mod without a body draws its
 * status lines only.
 * @param i Index into the loaded mod table.
 * @param ui The overlay's widget vtable. A NULL table, a disabled mod, an
 *        index out of range, or a call after openpete_mods_shutdown draws
 *        nothing. */
void openpete_mods_ui_draw(int i, const struct openpete_mod_ui* ui);

/** @brief Run a mod's UI body in the always pass when its section is flagged OPENPETE_MOD_UI_ALWAYS.
 *
 * No-op for an index out of range, a NULL @p ui, a disabled mod, an
 * unflagged section, a mod without a body, or a call after
 * openpete_mods_shutdown. Draws no ui_status lines;
 * those stay in the mod's node of the Mods panel. The body runs under the
 * render-purity guard (openpete_mods_in_present).
 * @param i Index into the loaded mod table.
 * @param ui The overlay's widget vtable.
 * @param no_input Nonzero raises the guard bit that puts
 *        ImGuiWindowFlags_NoInputs on every window the body opens; the
 *        caller sets it while the dev overlay is closed and the game owns
 *        the mouse. */
void openpete_mods_ui_draw_always(int i, const struct openpete_mod_ui* ui,
                                  int no_input);

/** @brief Count the enabled mods whose UI section is flagged OPENPETE_MOD_UI_ALWAYS and still has a body.
 *
 * One scan of the mod table. The overlay asks per present whether an
 * ImGui frame is wanted at all; a count derived here cannot drift out of
 * step with the loader's enable flips and body clears (a registration
 * strip, a refused section) the way a cross-thread counter would.
 * @return The count. */
int openpete_mods_ui_always_count(void);

/** @brief Stop drawing a mod's UI section for the rest of the run.
 *
 * Clears the mod's ui_body and its per-present ImGui call count and logs
 * one error line naming @p why; the mod keeps running. The overlay calls it
 * after a mod's body raises an ImGui error on three consecutive presents.
 * A section already refused returns 0 with no log line.
 * @param i Index into the loaded mod table.
 * @param why Reason text for the log line, or NULL.
 * @return 0, or -1 for an index out of range. */
int openpete_mods_ui_refuse_section(int i, const char* why);

/** @brief Queue enabling or disabling a mod, applied at the next tick boundary.
 *
 * Copies the id into the request queue, truncated to the loader's id
 * width, and treats any nonzero @p on as 1. Nothing is validated here: an
 * id that names no loaded mod is logged as an error when the queue drains,
 * and a disable that would strand the player in the mod's level stays
 * queued until the player leaves it. The queue has no lock.
 * @param id Mod id.
 * @param on Nonzero to enable, 0 to disable.
 * @return 0 when queued, -1 for a NULL or empty id or a failed queue
 *         allocation. */
int  openpete_mods_set_mod_enabled(const char* id, int on);
/** @brief Apply the queued mod state changes at a canonical tick boundary.
 *
 * core/mods/mod_draw.c calls it at the canonical present commit, when any
 * mod is loaded, with the running canonical-tick counter. In order: fires
 * this tick's OPENPETE_MODS_TOGGLE and OPENPETE_MODS_CFG_APPLY schedule
 * entries, polls hot reload, applies pending material selector changes,
 * runs the moby class census, reloads mods with a pending config apply,
 * and drains the toggle queue. An applied toggle flips owner-keyed
 * disabled flags on the chain, draw, registry and keymap surfaces without
 * tearing anything down, bumps openpete_mods_toggle_epoch, then calls the
 * mod's toggle hook. A disable of a mod that owns the level the player is
 * in stays queued until openpete_mods_registry_owner_busy returns 0; an
 * enable of a mod whose entry never ran runs the entry first and leaves
 * the mod disabled when it fails.
 * @param cpu Live game CPU; the toggle hook runs in tick context, where
 *        guest access is legal.
 * @param canonical_tick The running canonical-tick counter. */
void openpete_mods_tick_boundary(CPUState* cpu, unsigned canonical_tick);
/** @brief Run the exit pass: every mod's shutdown hook in reverse load order, then close the mod api.
 *
 * Runs once; a later call returns at once. For each mod, last-loaded
 * first, runs its registered shutdown hook when its entry ran (timed;
 * over 500 ms logs a warning), marks the mod closed so every api entry
 * it calls from any thread is refused with one logged warning, and calls
 * @p progress. Then closes the api for every caller, waits up to 1000 ms
 * for counted api calls in progress on other threads to return, and
 * logs the hook count. After it, tick boundaries, present hooks and UI
 * bodies call no mod code. run.c calls it on the thread that runs ticks,
 * after the host loop ends and the present thread is joined, before any
 * teardown, and again (a no-op then) after a host loop or host init that
 * returned early.
 * @param progress Called after each mod; NULL for none. The host passes
 *        its teardown heartbeat, which bounds each hook by the pump's
 *        teardown grace. */
void openpete_mods_shutdown(void (*progress)(void));
/** @brief Report whether openpete_mods_shutdown has closed the mod api.
 *
 * Callable from any thread.
 * @return 1 after the exit pass, else 0. */
int  openpete_mods_api_closed(void);
/** @brief Return 1 while the player is inside, or entering, a level that @p mod_id owns.
 *
 * True when one of the mod's claimed level slots, or its active custom
 * level, is the resident level or the level being entered. A disabled slot
 * refuses entry but stays claimed, so the mod cannot be turned off from
 * under the running level: the live-toggle apply loop keeps such a disable
 * queued until this returns 0, and the M overlay greys the mod's switch.
 * The resident level is the last live level the per-tick registry driver
 * observed, so a pause or a level teardown does not end residency. Reads
 * host-side statics only, so the menu thread may call it without guest
 * access.
 * @param mod_id Mod id; NULL returns 0.
 * @return 1 when busy, else 0. */
int  openpete_mods_registry_owner_busy(const char* mod_id);
/** @brief Return the toggle epoch, bumped once per applied toggle that changed a mod's enabled state.
 *
 * The run loop polls it to invalidate timeline-derived state: rewind
 * history recorded under the old modset must not replay into the new one,
 * and runahead re-runs its capability gate against the new enabled set.
 * The texture pack, music override and mod texture and material resolvers
 * fold it into their cache keys.
 * @return The epoch. */
unsigned openpete_mods_toggle_epoch(void);
/** @brief Enable or disable a mod's level slots and level-ready hooks.
 *
 * A disabled level slot stops claiming and refuses entry through
 * openpete_mods_level_enter. It stays claimed: the cloned TOC entry and the
 * donor alias make the id loadable at all, and the player may be standing
 * in the level, so callers consult openpete_mods_registry_owner_busy before
 * applying a disable. Enabling clears each hook's once-per-level latch, so
 * the hook fires again for the level in progress on the next live tick.
 * Custom-level rows are not touched: their enablement is read from
 * openpete_mods_is_enabled when used.
 * @param mod_id The mod id; NULL does nothing.
 * @param on nonzero to enable, 0 to disable. */
void openpete_mods_registry_set_owner_enabled(const char* mod_id, int on);

/* Owner teardown and live enable/disable for the three subsystems that
 * key their entries on a mod id. Declared here beside their registry
 * sibling: mod_loader.c calls all of them, and without a prototype in
 * scope C assumes int(), which silently defeats argument checking on
 * calls that pass a pointer. */
/** @brief Remove every override link an owner registered, compacting each chain.
 *
 * The loader calls it when it strips a mod's registrations (hot reload,
 * whose re-entry registers the links again, and a failed load). The
 * owner's enabled state is kept, so re-registered links come back in it.
 * Call only at a tick boundary.
 * @param owner_id the mod id; NULL does nothing. */
void openpete_mods_chain_remove_owner(const char* owner_id);
/** @brief Enable or disable every override link an owner registered, keeping chain order.
 *
 * A disabled link is skipped by dispatch and by base(), and stays in its
 * chain. The owner's state is recorded apart from its links, so links
 * registered later (hot reload) start in it. Call only at a tick boundary.
 * @param owner_id the mod id; NULL does nothing.
 * @param on nonzero to enable, 0 to disable. */
void openpete_mods_chain_set_owner_enabled(const char* owner_id, int on);
/** @brief Report whether a mod owns a link, enabled or not, on the override chain at an address.
 * @param addr The overridden function's vaddr.
 * @param owner_id The mod id; NULL returns 0.
 * @return 1 when it owns one, else 0. */
int  openpete_mods_chain_has_owner(uint32_t addr, const char* owner_id);
/** @brief Remove every present hook an owner registered, compacting the table.
 *
 * @param owner_id the mod id; NULL does nothing. */
void openpete_mods_draw_remove_owner(const char* owner_id);
/** @brief Enable or disable every present hook an owner registered.
 *
 * A disabled hook stays in the table and is skipped by the flush.
 * @param owner_id the mod id; NULL does nothing.
 * @param on nonzero to enable, 0 to disable. */
void openpete_mods_draw_set_owner_enabled(const char* owner_id, int on);
/** @brief Remove a mod's level slots and level-ready hooks, keeping the order of the rest.
 *
 * The loader calls it when it strips a mod's registrations. A claimed slot
 * id is not released, and custom-level rows are not removed.
 * @param mod_id The mod id; NULL does nothing. */
void openpete_mods_registry_remove_owner(const char* mod_id);

/** @brief Add @p ns of self time to @p owner's tick or present accumulator.
 *
 * The override chain runner feeds tick hooks (the override's span minus
 * its base() descent and the overrides nested inside it) and mod_draw.c
 * feeds present hooks (each hook's span). An owner gets a slot on first
 * use.
 * @param owner Mod id; NULL is ignored.
 * @param ns Nanoseconds; a negative value is ignored.
 * @param is_present Non-zero for a present hook, zero for a tick hook. */
void openpete_mods_time_add(const char* owner, int64_t ns, int is_present);
/** @brief Log and reset every owner's accumulated time for the window that just ended.
 *
 * Warns for each mod whose tick-hook time averages more than 4 ms per sim
 * tick; with OPENPETE_MOD_TIMERS set non-zero it logs every other owner's
 * figures too, including owners with no calls.
 * @param ticks_elapsed Sim ticks in the window; 0 counts as 1. */
void openpete_mods_time_report(unsigned ticks_elapsed);

/** @brief The mod whose override is innermost on the calling thread's override stack, for the crash handler.
 *
 * The frame stack is thread-local, so the crash handler calls it on the
 * faulting thread; core/diag/crash.c prints the result as a MOD ATTRIBUTION
 * line. The crash is in that mod's override or in code it called.
 * @param addr_out Receives the overridden guest vaddr; may be NULL.
 * @return The owner mod id, "?" when the link has no owner, or NULL when no
 *         override is running on this thread. */
const char* openpete_mods_crash_attribution(uint32_t* addr_out);

/** @brief Return the set modset tag, the order-free identity of the gameplay-relevant loaded modset.
 *
 * The form is "id@ver+id@ver" with ids sorted, over the mods whose entry
 * has run and that mod_is_mutative classifies as changing gameplay state;
 * the empty string is vanilla. Built on the first call and cached; the
 * first enable of a deferred-entry mod drops the cache, so the tag grows
 * with it and does not shrink on a disable. Memory cards and their
 * `.modsave` snapshots carry this form (core/host/memcard.c,
 * core/mods/mod_save.c) and warn on a mismatch; a savestate compares a
 * file written before the ordered form existed against it.
 * @return The tag, owned by the loader. */
const char* openpete_mods_modset_tag(void);

/** @brief Return the ordered modset tag, which fixes the override order of the gameplay-relevant loaded modset.
 *
 * Covers the same mods as openpete_mods_modset_tag. The form is
 * OPENPETE_MODSET_ORDER_PREFIX, the set tag, '#' and 16 lowercase hex
 * digits of openpete_modset_order_hash over the session's order events
 * of those mods: each entry (boot entries in load order, then each
 * deferred entry and hot-reload re-entry), each strip of a mod's
 * registrations, and each override link registered outside its mod's
 * entry, with its chain. Every chain's link order and the enabled-list
 * walk are functions of that sequence. Vanilla is the empty string.
 * Cached with the set tag; each recorded event drops the cache.
 * core/record/savestate.c writes it into every state.
 * @return The tag, owned by the loader. */
const char* openpete_mods_modset_order_tag(void);

/** The prefix that marks the ordered modset tag form. A mod id cannot
 *  hold ':', so no set-form tag starts with it. */
#define OPENPETE_MODSET_ORDER_PREFIX "v2:"

/** One gameplay-relevant mod, as the modset tag formatters read it. */
typedef struct {
    const char* id;
    const char* version;
} openpete_modset_member_t;

/** Kinds of override-order event. */
typedef enum {
    OPENPETE_MODSET_EV_ENTRY = 1, /* the mod's entry ran (boot, deferred, reload) */
    OPENPETE_MODSET_EV_STRIP = 2, /* every registration of the mod was removed */
    OPENPETE_MODSET_EV_LINK  = 3  /* a link registered outside the mod's entry */
} openpete_modset_event_kind_t;

/** One override-order event, in session order. */
typedef struct {
    openpete_modset_event_kind_t kind;
    const char*                  id;      /* the mod */
    uint32_t                     addr;    /* LINK: the chain's guest vaddr */
    uint32_t                     overlay; /* LINK: the link's overlay binding */
} openpete_modset_event_t;

/** @brief Record an override link registration and drop the cached modset tags.
 *
 * core/mods/mod_chain.c calls it after each link it puts at the front of a
 * chain. A link registered while its owner's entry runs is implied by the
 * entry event; any other link is recorded as a LINK event.
 * @param owner_id The registering mod; NULL or an id that names no loaded
 *        mod records nothing.
 * @param addr The chain's guest vaddr.
 * @param overlay The link's overlay binding. */
void openpete_mods_link_registered(const char* owner_id, uint32_t addr,
                                   uint32_t overlay);

/** @brief Format the set modset tag of @p n members, ids sorted, snprintf-style.
 *
 * @param out Receives the NUL-terminated tag, truncated to @p cap; may be
 *        NULL with @p cap 0 to measure.
 * @param cap Size of @p out.
 * @param m The members.
 * @param n Their count.
 * @return The full tag length, excluding the NUL. */
size_t openpete_modset_format_set(char* out, size_t cap,
                                  const openpete_modset_member_t* m, int n);

/** @brief Format the ordered modset tag of @p n members and an order hash, snprintf-style.
 *
 * No members gives the empty string.
 * @param out Receives the NUL-terminated tag, truncated to @p cap; may be
 *        NULL with @p cap 0 to measure.
 * @param cap Size of @p out.
 * @param m The members.
 * @param n Their count.
 * @param order_hash openpete_modset_order_hash of their order events.
 * @return The full tag length, excluding the NUL. */
size_t openpete_modset_format_order(char* out, size_t cap,
                                    const openpete_modset_member_t* m, int n,
                                    uint64_t order_hash);

/** @brief Hash an order-event sequence with FNV-1a 64 over each event's kind, id length, chain, overlay and id.
 *
 * @param ev The events, in session order.
 * @param n Their count.
 * @return The hash; the FNV-1a offset basis for no events. */
uint64_t openpete_modset_order_hash(const openpete_modset_event_t* ev,
                                    int n);

/** The verdict of a stored modset tag against the running modset. */
typedef enum {
    OPENPETE_MODSET_MATCH = 0,     /* ordered tag, equal */
    OPENPETE_MODSET_SET_MATCH,     /* set-form tag, equal to the running set */
    OPENPETE_MODSET_SET_DIFFERS,   /* the mods or versions differ */
    OPENPETE_MODSET_ORDER_DIFFERS  /* ordered tag: same mods, another order */
} openpete_modset_verdict_t;

/** @brief Judge a stored modset tag against the running modset's two tags.
 *
 * An ordered-form tag (OPENPETE_MODSET_ORDER_PREFIX) matches only an equal
 * running ordered tag; on a mismatch its set part, between the prefix and
 * the hash, decides whether the set or only the order differs. Any other tag, including
 * the empty vanilla tag, is a set-form tag and is compared with the
 * running set tag.
 * @param saved The stored tag; NULL reads as "".
 * @param now_order The running ordered tag; NULL reads as "".
 * @param now_set The running set tag; NULL reads as "".
 * @return The verdict. */
openpete_modset_verdict_t openpete_modset_compare(const char* saved,
                                                  const char* now_order,
                                                  const char* now_set);

/* ── Draw service (core/mods/mod_draw.c) ───────────────────────────── */

#include "openpete_mod_api.h" /* openpete_mod_present_fn / ctx */

/** @brief Commit the tick's mod draw commands, run present hooks, and pass the overlay vertices to the native renderer.
 *
 * Called once per present: by the eager producer for each scene it
 * produces, and otherwise from run.c's canonical and sub-tick present
 * legs. A canonical call with a mod loaded, before openpete_mods_shutdown,
 * first runs openpete_mods_tick_boundary (the live-toggle apply point)
 * and, every 512 canonical calls, openpete_mods_time_report. The rest runs only once a
 * mod has drawn or registered a present hook: a canonical call swaps the
 * staging and committed tick buffers and clears the new staging, with or
 * without a native renderer. With the native renderer enabled it records
 * the present context (tick openpete_frame; alpha 0 on a canonical call,
 * else the interpolation phase at the present instant), runs every enabled
 * present hook with openpete_mods_in_present set (none after
 * openpete_mods_shutdown), lays out the committed
 * tick texts and this present's texts through the game's text emitter,
 * and passes the vertices to openpete_native_set_mod_overlay.
 * Everything after the tick boundary runs inside
 * openpete_subtick_guard_window_begin's RAM guard window, so under
 * OPENPETE_SUBTICK_RAM_GUARD a store into guest RAM from a present hook or
 * the text emitter is logged and counted as a guard hit.
 * @param cpu the game CPU, passed to the tick boundary.
 * @param canonical 1 for a tick's canonical present, 0 for a sub-tick
 *        present. */
void openpete_mods_present_flush(CPUState* cpu, int canonical) OP_REQUIRES(sim);

/* Game-registered text rasterizer: lays out `str` at (x, y) with glyph
 * cap-height cap_h (overlay space) and tint rgb into out_verts (an
 * openpete_hud_vert array, cap entries), returning the vertex count.
 * Spyro 1 registers a text_moby_native wrapper (game_mod_desc_t.text_emit). */
typedef int (*openpete_mods_text_emit_fn)(const char* str, float x, float y,
                                          float cap_h, unsigned rgb,
                                          void* out_verts, int cap);

/* Loader-facing submission entry points (wired into the mod api). */
/** @brief Queue one text for the mod overlay.
 *
 * Inside a present hook the text goes to this present's buffer and draws
 * on this present only. Elsewhere (a tick hook) it goes to the tick
 * staging buffer, which the next canonical present commits for display on
 * every present of that tick, so a mod redraws every tick to keep text on
 * screen. The running hook's owner is recorded for overflow attribution.
 * @param x left edge in overlay space (window-height fractions, origin
 *        top-left, x in [0, aspect]).
 * @param y top edge in overlay space, +y down.
 * @param cap_h glyph cap height in overlay space.
 * @param rgb tint.
 * @param str the text, truncated to 95 characters; NULL or empty is
 *        ignored. */
void openpete_mods_draw_text(float x, float y, float cap_h, uint32_t rgb,
                             const char* str);
/** @brief Add a hook that runs once per present with that present's context.
 *
 * The hook starts disabled when the loader lists the owner and reports it
 * disabled. Sets the flag that keeps the draw flush active.
 * @param fn the hook; NULL fails.
 * @param owner_id the registering mod's id, stored by pointer, or NULL.
 * @return 0, or -1 for a NULL fn or a table allocation failure. */
int  openpete_mods_register_present_hook(openpete_mod_present_fn fn,
                                         const char* owner_id);

/* 1 while a present hook is executing on this thread - the loader's
 * guest/call/override entry points refuse during it (render-purity). */
#ifndef OPENPETE_THREAD_LOCAL
#  ifdef __cplusplus
#    define OPENPETE_THREAD_LOCAL thread_local
#  else
#    define OPENPETE_THREAD_LOCAL _Thread_local
#  endif
#endif
extern OPENPETE_THREAD_LOCAL int openpete_mods_in_present;

/* ── Content registries (core/mods/mod_registry.c) ─────────────────────
 *
 * Namespaced additive content (`modid:entry`): LEVEL SLOTS and the custom
 * levels served over them. Lifts beyond the vanilla table sizes ride
 * openpete-ram-virtualization - acknowledged, not hidden.
 *
 * The registries share one per-tick driver: a chain link on the game's
 * master-update function, added lazily on first registration (nothing
 * registered = no hook = inert). */

/* Game ops the registry delegates to (game addresses stay game-side;
 * Spyro 1 supplies its table as game_mod_desc_t.moby_ops). */
typedef struct {
    uint32_t update_hook_addr; /* master per-tick update fn (chain anchor) */
    /* Current level id, 0 = none/not in gameplay yet. */
    int (*level_id)(CPUState* cpu);
    /* Fill `out` with the moby classes the CURRENT LEVEL has loaded - the
     * classes that can appear at all - ascending, and return the count.
     * -1 while the table cannot be trusted (mid-load). Level-scoped, so the
     * answer is stable as the camera moves; it is a superset of the classes
     * with a live instance right now. */
    int (*classes_loaded)(CPUState* cpu, uint16_t* out, int cap);
} openpete_moby_ops_t;

/* Host-side snapshot of the loaded-class set, refreshed once per tick from
 * the ops above. Mods read it through the api's moby_classes, which must be
 * callable from a UI section - i.e. inside a present, where guest access is
 * refused - so the read never touches guest RAM. Sized to the largest model
 * table of any game (Spyro 1 512 rows, Spyro 3 768); a mod passes its own
 * capacity, so this size is not part of the mod ABI. */
#define OPENPETE_MOBY_CLASSES_MAX 768
/** @brief Refresh the host-side copy of the loaded moby class set from the game ops.
 *
 * Reads guest RAM, so it runs at the tick boundary; the run loop calls it
 * each tick while any mod is loaded. A -1 answer from the ops (mid-load)
 * keeps the previous copy. Does nothing without moby ops or their
 * classes_loaded query. Holds at most OPENPETE_MOBY_CLASSES_MAX classes.
 * @param cpu Live game CPU. */
void openpete_mods_moby_class_census(CPUState* cpu);
/** @brief Copy the last censused loaded moby class set into a caller buffer.
 *
 * Reads the host-side copy only, so it is legal inside a present. The set
 * is ascending; a set larger than @p cap is truncated to @p cap entries.
 * @param out Receives the classes; NULL returns the count without copying.
 * @param cap Capacity of @p out in entries; 0 or less returns the count
 *        without copying.
 * @return The number of entries copied, or the full set size when @p out
 *         is NULL or @p cap is 0 or less. */
int  openpete_mods_moby_classes(uint16_t* out, int cap);

/* Level-slot namespace. A registered slot is a NEW level id
 * whose WAD TOC entry is cloned from a donor level; the game ops own
 * the id->behavior aliasing so the slot loads and plays like the donor
 * while keeping its own per-level state index. v1 capacity: the guest
 * tables have exactly ONE free in-bounds slot (index 35 / id 65 in
 * Spyro 1); more slots need larger guest tables. */
typedef struct {
    /* Claim the free slot for donor_level_id: seed its TOC entry and
     * install the alias hooks. Returns the claimed level id, 0 = not
     * ready yet (boot TOC not loaded) or no free slot. */
    int (*claim_slot)(CPUState* cpu, int donor_level_id);
    /* Re-target an already-claimed slot at a new donor: re-copy the
     * donor's TOC entry into the slot and swap the alias donor. 0 ok. */
    int (*retarget_slot)(CPUState* cpu, int donor_level_id);
    /* Adopt a claim the guest RAM already holds (a state load): set the
     * slot's alias donor without a guest write, installing the alias hooks
     * on first use; donor 0 releases the slot. 0 ok, -1 = id out of range
     * or a hook install failed. */
    int (*adopt_slot)(int donor_level_id);
    /* Declare `len` as a level entry's Data length in the guest WAD TOC
     * (len 0 = restore the stock length). 0 ok, -1 = TOC not live. */
    int (*declare_data_len)(CPUState* cpu, int level_id, uint32_t len);
    /* lower_snake level name (or decimal id string) -> id, -1 unknown. */
    int (*resolve_name)(const char* name);
    /* Trigger a transition into level_id from live gameplay (the
     * level-select cheat's writes). 0 = queued, -1 = not in gameplay. */
    int (*enter_level)(CPUState* cpu, int level_id);
    /* Read a level's raw Data WAD entry host-side from the user's disc.
     * buf=NULL probes length. Returns byte length or -1. */
    int (*read_level_data)(int level_id, void* buf, uint32_t cap);
    /* Show `name` as level_id's in-game title (guest name table:
     * inventory, entry title card, portal name arc). NULL restores the
     * stock name. 0 ok. */
    int (*set_level_name)(CPUState* cpu, int level_id, const char* name);

    /* Reconcile a level's target totals on the three EXE count tables.
     * A non-negative value is written; a negative one restores what the
     * table held at boot, which is how a level index is given back.
     * `eggs` is ignored for an index the egg table has no row for.
     * Writes only differing bytes, so a per-tick call is cheap once the
     * tables agree. Returns bytes written, or -1 for an unknown level. */
    int (*set_level_counts)(CPUState* cpu, int level_id,
                            int dragons, int gems, int eggs);
    /* Per-tick driver: while `on`, the donor level's homeworld portal
     * renders the custom level's own skybox (parsed from
     * <dir>/levels/<stem>.WAD, parked in the guest arena);
     * restores the baked preview when `on` drops or the level changes.
     * Idempotent, cheap when nothing changes. */
    int (*replace_sky_tick)(CPUState* cpu, int donor_id, const char* dir,
                            const char* stem, int on);
} openpete_level_ops_t;


/* ── Declarative custom levels ([[level]] manifest blocks) ───────────
 * A mod ships assets/levels/<stem>.WAD (+ optional .OVL) and a
 * manifest block; the registry owns slot time-sharing, guest TOC
 * declarations and warps - no mod code involved. Rows sort by NAME
 * (the menu's alphabetical Custom World buckets). */
/* A level's target totals, as the manifest declared them. Each field is
 * negative when the block did not declare it, which leaves that table on
 * its stock row; no legal value is negative, so the sentinel cannot
 * collide with a declaration. `eggs` is dropped at registration for a
 * level index the 18-entry egg table has no row for. */
typedef struct {
    int dragons;   /* 0..255,   -1 undeclared */
    int gems;      /* 0..32767, -1 undeclared */
    int eggs;      /* 0..255,   -1 undeclared */
} openpete_level_counts_t;

/** @brief Register a declarative custom-level row for a mod.
 *
 * The donor name is resolved through the level ops and must resolve to a
 * retail level id from 10 to 64. A missing data file does not fail the
 * call: the row registers with files_ok 0 and a warning. The row is
 * inserted in case-insensitive name order, its music duration is probed,
 * and the per-tick driver is armed. Strings are truncated to fit the row:
 * name and stem to 63 bytes, title to 39, the assets directory to 899.
 * @param mod_id Owning mod id.
 * @param name Row name and sort key; must be non-empty.
 * @param stem File stem of `<dir>/levels/<stem>.WAD`; must be non-empty.
 * @param dir The mod's assets directory.
 * @param donor Donor level name or decimal id string; NULL reads as "".
 * @param replace Nonzero serves the level over the donor's own entry, 0
 *        from the shared slot.
 * @param music WAV file name under the mod's music/, or NULL.
 * @param title In-game title override, or NULL.
 * @param counts Declared target totals, or NULL for none.
 * @param log_tag Log tag for the registration lines.
 * @return 0, or -1 on a NULL or empty required argument, an unresolved
 *         or non-retail donor, or a failed table allocation. */
int  openpete_mods_custom_register(const char* mod_id, const char* name,
                                   const char* stem, const char* dir,
                                   const char* donor, int replace,
                                   const char* music, const char* title,
                                   const openpete_level_counts_t* counts,
                                   const char* log_tag);
typedef struct {
    const char* name;   /* human name (sort key) */
    const char* owner;  /* mod id */
    int donor_id;
    int replace_mode;   /* 1 = serve over the donor's own entry */
    int active;         /* currently owns the shared slot */
    int level_id;       /* enterable id right now (slot id or donor), 0 = none */
    int files_ok;       /* data file present at registration */
} openpete_mods_custom_info_t;
/** @brief Return the number of registered custom-level rows.
 *
 * @return The row count. */
int  openpete_mods_custom_count(void);
/** @brief Fill a descriptor for the custom level at alphabetical position @p i.
 *
 * Positions follow case-insensitive name order. The name and owner strings
 * point into registry storage and stay valid until the next registration.
 * level_id is the donor id for a replace row, the shared slot id for the
 * slot row that currently owns it, else 0.
 * @param i Position in name order.
 * @param out Receives the descriptor.
 * @return 1 when filled, 0 when @p i is out of range or @p out is NULL. */
int  openpete_mods_custom_info(int i, openpete_mods_custom_info_t* out);
/** @brief Queue a warp into the custom level at alphabetical position @p i.
 *
 * The registry's per-tick driver consumes the warp; one queued warp is
 * held, and a later call replaces it.
 * @param i Position in the name order openpete_mods_custom_info uses.
 * @return 0, or -1 when @p i is out of range. */
int  openpete_mods_custom_warp(int i);
/** @brief Set the custom level at alphabetical position @p i to slot or replace serving.
 *
 * Queues the toggle; the per-tick driver takes it on its next committed
 * tick, records it for a bug-report bundle, and declares or withdraws the
 * replace over the donor's entry in that tick. One toggle is held; a later
 * call replaces it. An out-of-range @p i is ignored.
 * @param i Position in the name order openpete_mods_custom_info uses.
 * @param replace 0 serves the level from the shared slot, nonzero over its
 *        donor's own entry. */
void openpete_mods_custom_set_mode(int i, int replace);
/** @brief Name the custom-level files that serve a logical level id right now.
 *
 * The WAD overlay calls it. The active slot row serves the claimed slot
 * id, and a replace row whose replace is declared serves its donor's id;
 * either only while its mod is enabled.
 * @param level_id Logical level id.
 * @param dir_out Receives the owning mod's assets directory on a hit.
 * @param stem_out Receives the file stem on a hit.
 * @return 1 when a row serves the level, else 0. */
int  openpete_mods_custom_files_for(int level_id, const char** dir_out,
                                    const char** stem_out);
/** @brief Build the path of a custom level's data or code-overlay file under `<dir>/levels/`.
 *
 * The data file is `<stem>.WAD` (the level's WAD data segment) and the
 * code overlay `<stem>.OVL` (the donor's overlay); level editors emit
 * those extensions in either case, and case matters on Linux, so a miss on
 * the literal lowercase path scans the directory for a case-insensitive
 * match of the whole file name.
 * @param dir The mod's assets directory.
 * @param stem File stem.
 * @param is_ovl Nonzero for the code overlay, 0 for the data file.
 * @param out Receives the path; on a miss, the lowercase path, so callers
 *        can name the missing file.
 * @param cap Capacity of @p out.
 * @return 0 when the file exists, -1 when it is absent or an argument is
 *         NULL or @p cap is 0. */
int  openpete_mods_custom_asset_path(const char* dir, const char* stem,
                                     int is_ovl, char* out, size_t cap);
/** @brief Return the custom song for a logical level id right now.
 *
 * Row selection matches openpete_mods_custom_files_for: the active slot
 * row for the claimed slot id, else the first replace row whose replace is
 * declared for that donor. The song plays only while the row's mod is
 * enabled.
 * @param level_id Logical level id.
 * @param dur_sec_out Receives the song's duration in seconds; may be NULL.
 * @return The WAV path, or NULL when no row serves the level, the row has
 *         no song, the file was missing at registration, or the mod is
 *         disabled. */
const char* openpete_mods_custom_music_for(int level_id,
                                           double* dur_sec_out);

/** @brief Register a level slot for a mod, cloned from a donor level.
 *
 * Adds a row with no slot id and arms the per-tick driver, which calls the
 * level ops' claim_slot with the donor id each tick until it returns a
 * nonzero id. The owner and name are truncated to 63 and 47 bytes. The
 * donor id is not validated here.
 * @param mod_id Owning mod id.
 * @param name Slot name, unique per owner; must be non-empty.
 * @param donor_level_id Level id whose TOC entry the slot clones.
 * @param log_tag Log tag for the registration lines.
 * @return 0, or -1 on a NULL or empty argument, without level ops, on a
 *         duplicate owner and name, when the driver cannot be armed, or
 *         when the table allocation fails. */
int openpete_mods_level_register(const char* mod_id, const char* name,
                                 int donor_level_id, const char* log_tag);
/** @brief Return the level id a registered slot claimed.
 *
 * @param mod_id Owning mod id; must be non-NULL.
 * @param name Slot name; must be non-NULL.
 * @return The id, or -1 when no row matches or the slot is not claimed
 *         yet. */
int openpete_mods_level_slot_id(const char* mod_id, const char* name);
/** @brief Start a transition into a level id from live gameplay through the level ops.
 *
 * An id that belongs to a level slot whose owner is disabled is refused.
 * Retail ids match no slot row and pass through. When the ops accept a
 * slot id, the registry records the entry so
 * openpete_mods_registry_owner_busy reports the owner busy until a live
 * level appears.
 * @param cpu Live game CPU; NULL returns -1.
 * @param level_id Target level id; 0 or less returns -1.
 * @return 0 when queued, -1 when refused, without level ops, or when the
 *         ops report gameplay is not live. */
int openpete_mods_level_enter(CPUState* cpu, int level_id);
/** @brief Read a level's raw Data WAD entry from the disc through the level ops.
 *
 * @param level_id Level id; 0 or less returns -1.
 * @param buf Receives the bytes; NULL probes the length.
 * @param cap Capacity of @p buf in bytes.
 * @return The entry's byte length, or -1 without level ops or their
 *         read_level_data, or when the ops fail. */
int openpete_mods_read_level_data(int level_id, void* buf, uint32_t cap);

/** @brief Register a mod's level-ready hook.
 *
 * The hook fires once per level id, on the first master-update tick with
 * gameplay live (level_id > 0), in tick context, where guest reads, writes
 * and call_addr are legal. A new hook fires on the next live tick,
 * including for a level already in progress. Registration arms the
 * per-tick registry driver. openpete_mods_registry_set_owner_enabled
 * gates the hook and re-arms it on enable;
 * openpete_mods_registry_remove_owner drops it.
 * @param mod_id Owning mod id; NULL returns -1.
 * @param fn The hook; NULL returns -1.
 * @param log_tag Log tag for the registration lines.
 * @return 0, or -1 on a NULL argument, when the driver cannot be armed, or
 *         when the hook table allocation fails. */
int openpete_mods_on_level(const char* mod_id, openpete_mod_level_fn fn,
                           const char* log_tag);
/** @brief Add the per-tick registry driver to the master-update chain if it is absent.
 *
 * The driver is normally added by the first registration in any registry
 * namespace; this arms it without registering a row.
 * @param log_tag Log tag for the error line.
 * @return 0 when the driver is armed, -1 without moby ops or when the
 *         chain add fails. */
int openpete_mods_registry_ensure_driver(const char* log_tag);

/* ── SFX replacement verb (core/mods/mod_sfx.c) ────────────────────
 * sfx_replace registers a symbolic sfx-pack entry from memory; the
 * substitution itself is the pack's (core/host/sfx_pack.h), below
 * everything the guest observes. */
/* Name canonicaliser: the game maps a mod-facing alias ("gem_pickup")
 * to the SoundTable slot name the pack's resolver reports ("gemPickup");
 * the name itself when no alias exists. Never NULL: sfx_replace passes the
 * result on as the name and fails on NULL. Applied to sfx_replace names
 * before they are stored. */
typedef const char* (*openpete_mods_sfx_canon_fn)(const char* name);

/* One overlay of a game, as the mod layer validates override_ovl against
 * it: the overlay table's id and name, and the sorted entry addresses of
 * its generated dispatch (the `case` labels of
 * generated/<overlay>/<overlay>.bin_dispatch.c). */
typedef struct {
    uint32_t        id;
    const char*     name;
    const uint32_t* entries;
    size_t          entry_count;
} game_mod_overlay_t;

/* The game-specific half of the mod layer: one descriptor per game,
 * registered once after the game is identified and before any mod is
 * compiled or loaded. The descriptor and every table it reaches are static
 * storage; the loader keeps the pointer. A NULL member is a surface the
 * game does not serve. */
typedef struct {
    /* The generated SDK symbol table (sdk_symbols_table.c): sorted by
     * strcmp on name, then overlay id. override_name binary-searches it;
     * openpete_mods_symbol_for_addr scans it. NULL makes every lookup
     * fail. */
    const openpete_sdk_symbol_t* symbols;
    size_t                       symbol_count;
    /* Chain anchor, level id and loaded-class query of the moby registry;
     * registrations fail while NULL. */
    const openpete_moby_ops_t*   moby_ops;
    /* Level-slot and custom-level operations; the registry's level work
     * does nothing while NULL. */
    const openpete_level_ops_t*  level_ops;
    /* The mod overlay's text rasterizer; queued text draws nothing while
     * NULL. */
    openpete_mods_text_emit_fn   text_emit;
    /* Alias canonicaliser applied to sfx_replace names; names pass
     * unchanged while NULL. */
    openpete_mods_sfx_canon_fn   sfx_canon;
    /* Names of the game's PSX render and GPU-submission functions, which
     * the native renderer does not rasterize; an override on one warns
     * under pure vk. NULL-terminated; NULL for none. */
    const char* const*           psx_render_fns;
    /* The game's overlays (generated mod_overlay_entries.c) and their
     * shared vaddr window [window_lo, window_hi); NULL, 0 and an empty
     * window for a game with no overlays. */
    const game_mod_overlay_t*    overlays;
    size_t                       overlay_count;
    uint32_t                     window_lo;
    uint32_t                     window_hi;
    /* Nonzero when a mod pinned below the api that adds override_ovl may
     * bind an unscoped link with override_addr at a window address or
     * override_name on a name whose rows lie in several overlays (it fires
     * in every overlay with a function there); 0 refuses both at every
     * pin. */
    int                          legacy_window_links;
    /* Render channels whose draws consult mod materials: bit
     * (1u << OP_CHAN_*) per channel. A declarative [[material]] row naming
     * a channel outside the mask, or channel "any" under an empty mask, is
     * logged and skipped. 0 serves no channel. */
    uint32_t                     channel_mask;
    /* Nonzero when the game's animation frame words carry the 21-bit
     * vertex-stream field that vstream_handle's handles are written into
     * and its frame readers resolve; 0 makes vstream_handle return 0 and
     * log once per mod. */
    int                          vstream_handles;
    /* The guard over guest bytes a mod may not change
     * (openpete_mods_guest_guard_t), installed before any mod's entry
     * runs; NULL for none. */
    const openpete_mods_guest_guard_t* guest_guard;
    /* The game save descriptor of the per-mod save records
     * (openpete_modsave.h); NULL for a game with none, where save_register
     * returns -1. */
    const struct openpete_save_desc*   save_desc;
    /* The anti-piracy scan observer behind antipiracy_scan: fills @p out
     * for a walk key or a scanner moby's data block, returning 0, or -1
     * for an unseen scanner; NULL for a game with none, where the entry
     * returns OPENPETE_MOD_ERR_UNSUPPORTED. */
    int (*antipiracy_scan)(uint32_t key, openpete_mod_scan_t* out);
} game_mod_desc_t;

/* Every render channel, OP_CHAN_MOBY through OP_CHAN_PORTAL, as a
 * game_mod_desc_t.channel_mask. */
#define OPENPETE_MODS_CHANNELS_ALL \
    (((1u << (OP_CHAN_PORTAL + 1)) - 1u) & ~((1u << OP_CHAN_MOBY) - 1u))

/** @brief Register the identified game's mod-layer descriptor.
 *
 * Accepted once, before openpete_mods_init. A second call, or a call
 * after openpete_mods_init ran, logs an error and changes nothing, so the
 * symbol table and ops every mod compiled or loaded against stay fixed for
 * the process.
 * @param desc The descriptor, static storage; NULL is refused.
 * @return 0 when registered, -1 when refused. */
int openpete_mods_set_game_desc(const game_mod_desc_t* desc);

/** @brief Return the registered game descriptor.
 *
 * Never NULL: before registration, and on a refused registration's
 * process, it returns an all-NULL descriptor, so every game surface reads
 * as unsupported.
 * @return The descriptor. */
const game_mod_desc_t* openpete_mods_game_desc(void);

/** @brief Return a mod's stable id string.
 *
 * @param self The mod handle; NULL returns NULL.
 * @return The id, owned by the loader. */
const char* openpete_mods_id_of(openpete_mod_t* self);
/** @brief Return a mod's directory.
 *
 * @param self The mod handle; NULL returns NULL.
 * @return The directory, owned by the loader. */
const char* openpete_mods_dir_of(openpete_mod_t* self);
/** @brief Return the staged SDK directory, <exe>/sdk.
 *
 * @return The directory; empty until openpete_mods_init discovers a mod. */
const char* openpete_mods_sdk_dir(void);

/* ── Mod shaders (mod_shader.c) ───────────────────────────────────────
 * GLSL compile service, postfx passes and material selectors. */
/** @brief Register a mod's GLSL shader, compiling it or loading its cached or prebuilt SPIR-V, and return its handle.
 *
 * The stage comes from the extension (`.vert` or `.frag`). A path holding
 * `..`, of MOD_SHADER_PATH_MAX bytes or more, or registered inside a
 * present hook is refused. With the source present, the SPIR-V is cached
 * under `<mod>/.build/shaders/` keyed on a SHA-256 build key, and a
 * material fragment is compiled a second time for the retained-terrain
 * profile; a failed terrain compile logs and leaves that profile absent.
 * Without the source, `<path>.spv` loads only when `<path>.spv.ihash`
 * holds the current interface hash and the file is a nonempty whole number
 * of 32-bit words. Takes the registry write lock.
 * @param self Registering mod.
 * @param path Shader source path relative to the mod directory.
 * @return The handle, an index that stays valid until the owner is swept,
 *         or -1 on failure. */
int openpete_mods_shader_register(openpete_mod_t* self, const char* path);
/** @brief Return the SPIR-V words of a live shader handle.
 *
 * @param handle Shader handle.
 * @param words Receives the word count; may be NULL.
 * @param stage Receives the glslang_stage_t value (vertex or fragment);
 *        may be NULL.
 * @return The words, or NULL for an out-of-range or swept handle. */
const uint32_t* openpete_mods_shader_spv(int handle, size_t* words,
                                         int* stage);
/** @brief Return the retained-terrain profile of a material fragment shader.
 *
 * The same source compiled with OP_PROFILE_TERRAIN
 * (openpete_psx_material.glsl), whose engine half reads flat per-face
 * varyings instead of a per-draw push block. NULL when the handle is out
 * of range or swept, or the shader has no terrain profile (not a material
 * fragment, or its terrain compile failed); terrain then draws stock.
 * @param handle Shader handle.
 * @param words Receives the word count; may be NULL.
 * @return The words, or NULL. */
const uint32_t* openpete_mods_shader_spv_terrain(int handle, size_t* words);
/** @brief Return the portal profile of a material fragment shader.
 *
 * The same source compiled with OP_PROFILE_PORTAL
 * (openpete_psx_material.glsl), whose walked scene half discards what a
 * portal preview covers. NULL when the handle is out of range or swept, or
 * the shader has no portal profile (not a material fragment, a prebuilt, or
 * its portal compile failed); such terrain then draws through the engine's
 * own portal pipelines without the material.
 * @param handle Shader handle.
 * @param words Receives the word count; may be NULL.
 * @return The words, or NULL. */
const uint32_t* openpete_mods_shader_spv_portal(int handle, size_t* words);
struct openpete_mod_link_member;
/** @brief Return a material fragment shader's prepared composition member for one profile (mod_mat_link.h).
 *
 * Inspected at registration (identity, discard, block layout; verdict
 * pending) and decided by openpete_mods_shader_link_resolve (the verdict
 * and its reason, the preprocessed text a combined module is built from).
 * The caller holds the registry lock; the member lives until the shader is
 * swept.
 * @param handle Shader handle.
 * @param profile 0 stock, 1 terrain, 2 portal (the decal contract has 0 only).
 * @return The member, or NULL when the handle is out of range or swept, the
 *         shader is no material fragment, or it has no such profile. */
const struct openpete_mod_link_member* openpete_mods_shader_link(int handle, int profile);
/** @brief Decide a material fragment shader's pending composition members and copy the verdicts onto its materials.
 *
 * Registration only inspects the members (verdict pending); this runs the
 * source read, the source checks and the one-member compiles, cached under
 * the mod's `.build/shaders/`. File IO: entry or tick context, never a
 * present hook. The caller holds the registry write lock.
 * @param handle Shader handle.
 * @return The profiles decided; 0 when none was pending or the source no
 *         longer reads. */
int openpete_mods_shader_link_resolve(int handle);
/** @brief Free every shader a mod registered and mark its slots dead.
 *
 * Takes the registry write lock. Both SPIR-V profiles are freed and the
 * slot stays allocated with live 0, so a stale handle returns NULL from
 * openpete_mods_shader_spv() and is never reused. Callers remove the
 * owner's postfx passes and materials first, which retires their cached
 * pipelines.
 *
 * @param mod_id Owning mod id. */
void openpete_mods_shader_remove_owner(const char* mod_id);
/** @brief Compute the hot-reload watch key of a mod's shaders/ directory.
 *
 * SHA-256 over the name and content of every `.vert`, `.frag`, `.glsl`,
 * `.spv` and `.ihash` file in `<mod_dir>/shaders/`, in strcmp order, at
 * most 64 files. Shader sources elsewhere in the mod directory are keyed
 * per registration but not watched.
 * @param mod_dir The mod's directory.
 * @param key_out Receives the 64-character hex digest, or "-" when the
 *        directory holds no such file.
 * @return 0. */
int openpete_mods_shader_watch_key(const char* mod_dir, char key_out[65]);
/** @brief Register a fragment shader as a disabled postfx pass and return its handle.
 *
 * Takes the registry write lock. Refused inside a present hook, for an
 * insertion point other than 0 (COMPOSITE) or 1 (SCENE), and for a handle
 * that is not a live fragment-stage shader. The pass runs only after
 * openpete_mods_postfx_enable(); enabled passes at one point run by
 * ascending owner load index, then @p order, then registration index.
 *
 * @param self Registering mod.
 * @param frag_shader Fragment shader handle.
 * @param point Insertion point (OP_POSTFX_*).
 * @param order Position among the same mod's passes; lower runs first.
 * @return The pass handle, or -1. */
int openpete_mods_postfx_register(openpete_mod_t* self, int frag_shader,
                                  int point, int order);
/** @brief Turn a postfx pass on or off.
 *
 * Takes the registry write lock.
 *
 * @param self Owning mod.
 * @param fx Pass handle.
 * @param enabled Nonzero to run the pass.
 * @return 0, or -1 for a NULL mod, an unknown or swept handle, or a pass
 *         another mod owns. */
int openpete_mods_postfx_enable(openpete_mod_t* self, int fx, int enabled);
/** @brief Copy a postfx pass's uniform block.
 *
 * Takes the registry write lock. The block is copied at the call, so the
 * caller keeps its buffer. A block longer than OPENPETE_POSTFX_UNIFORMS_MAX
 * is truncated to that length and the call still succeeds.
 *
 * @param self Owning mod.
 * @param fx Pass handle.
 * @param data Block bytes.
 * @param size Length of @p data.
 * @return 0, or -1 for a NULL mod, a NULL @p data, an unknown or swept
 *         handle, or a pass another mod owns. */
int openpete_mods_postfx_set_uniforms(openpete_mod_t* self, int fx,
                                      const void* data, uint32_t size);

/* Renderer-facing postfx chain (native_render.c executor). Uniform
 * blocks cap at 1 KiB (fits every portable UBO limit with room for the
 * frame ring). pipeline_slot caches the executor's rhi_pipeline* inside
 * the registration; a hot-reload sweep moves orphaned pipelines to the
 * retirement list for the renderer to destroy safely. */
#define OPENPETE_POSTFX_UNIFORMS_MAX 1024u
/* pipeline_slot sentinel: build failed, don't retry or destroy. */
#define OPENPETE_POSTFX_PIPE_FAILED ((void*)(uintptr_t)1)
/* pipeline_slot sentinel: a compile for the slot is in flight on the
 * compile pool. Every reader draws the stock path and does not compile;
 * retirement, sweeps and shutdown leave it out (it is not a pipeline). */
#define OPENPETE_POSTFX_PIPE_PENDING ((void*)(uintptr_t)2)
typedef struct {
    const uint32_t* frag_spv;
    size_t          frag_words;
    const void*     uniforms;
    uint32_t        uniforms_len;
    void**          pipeline_slot;
    const char*     owner;
    unsigned        generation;  /* the slot's generation (openpete_mods_postfx_enum_at) */
    int             index;       /* the pass's registry index (openpete_mods_postfx_enum_at) */
    int             point;       /* its insertion point (OP_POSTFX_*) */
} openpete_postfx_pass_t;
/** @brief Fill @p out with the enabled postfx passes at one insertion point in run order, lowest priority first.
 *
 * A pass counts when it is live, enabled, at @p point, and its owning mod is
 * enabled. The whole set is insertion-sorted ascending by the mod's load
 * index (enabled-list position, later higher), then per-mod order, then
 * registration index, so the highest-priority pass runs last and sees
 * every earlier pass's output. Over @p cap the lowest-priority passes are
 * dropped: a truncated set keeps the highest-priority passes, still in run
 * order, and logs one warning.
 * @param point Postfx insertion point (OP_POSTFX_*).
 * @param out Receives the passes.
 * @param cap Capacity of @p out.
 * @return Passes written. */
int  openpete_mods_postfx_collect(int point, openpete_postfx_pass_t* out,
                                  int cap);
/** @brief Move retired pipeline pointers out of the retirement list for the renderer to destroy.
 *
 * Takes no lock: the renderer calls it inside a compose that holds the
 * registry read lock, and at shutdown. The taken entries are removed from
 * the list; the rest keep their order.
 *
 * @param out Receives the pointers.
 * @param cap Capacity of @p out.
 * @return The number of pointers written. */
int  openpete_mods_postfx_take_retired(void** out, int cap);
/** @brief Take the ids of the mods a live toggle entered (deferred entry) since the last call.
 *
 * The toggle drain sets a per-mod flag on each mod whose entry it ran,
 * dependencies included; this call clears the flags it reads. Any thread;
 * the mod table is fixed after openpete_mods_init.
 * @param out Receives the mod ids, valid while the mods stay loaded.
 * @param cap Capacity of @p out; flags past it stay set for the next call.
 * @return The ids written. */
int  openpete_mods_take_entered(const char** out, int cap);
/** @brief Hold the postfx passes and materials of a set of mods back from the compose in progress; n 0 releases.
 *
 * While held, openpete_mods_postfx_collect leaves the owners' passes out and
 * openpete_mods_material_resolve and openpete_mods_material_terrain_bucket_gpu
 * resolve none of their materials, so their geometry draws through the stock
 * path. The renderer holds a mod entered by a live toggle for the present
 * that queues its compiles, so the mod's passes first draw one present
 * later. Display owner only, inside a compose; the ids must stay valid while
 * held.
 * @param ids Mod ids, or NULL.
 * @param n Count of @p ids; 0 releases the hold. */
void openpete_mods_hold_owners(const char* const* ids, int n);
/** @brief Retire the pipelines of every postfx pass a mod registered and mark the passes dead.
 *
 * Takes the registry write lock. The slots stay allocated with live 0 and
 * are not reused.
 *
 * @param mod_id Owning mod id. */
void openpete_mods_postfx_remove_owner(const char* mod_id);
/** @brief Move the cached pipeline of every live postfx pass, enabled or not, to the retirement list.
 *
 * Takes the registry write lock. Used at shutdown; the passes stay live and
 * rebuild their pipelines on next use. */
void openpete_mods_postfx_retire_all(void);

/** @brief Take the registry read lock for a compose.
 *
 * Held for the whole compose, so the shader, postfx and material tables
 * cannot change under it; their mutators take the write side. Records the
 * calling thread, so a mutator called from inside the compose on that
 * thread proceeds without the write lock instead of deadlocking. */
void openpete_mods_registry_read_lock(void);
/** @brief Release the registry read lock and clear the recorded reader thread. */
void openpete_mods_registry_read_unlock(void);

/* Draw-state variants a material can be built for. Index 5 is the DECAL
 * profile; see openpete_psx_decal.glsl for why that is a separate shading
 * contract rather than another blend class. */
#define OPENPETE_MATERIAL_PIPE_VARIANTS 6
#define OPENPETE_MATERIAL_PIPE_DECAL    5
/* The shading contract a material's fragment stage is compiled against,
 * and the one a draw site's pipeline layout provides. SCENE is
 * openpete_psx_material.glsl (the walked scene, sky and portal sites);
 * DECAL is openpete_psx_decal.glsl (the moby-shadow decal pipe). The two
 * differ in descriptor sets, push block and fragment inputs, so a
 * fragment builds a valid pipeline only at a site of its own contract. */
#define OPENPETE_MATERIAL_CONTRACT_SCENE 0
#define OPENPETE_MATERIAL_CONTRACT_DECAL 1
/* Portal-profile pipelines a material can be built for: [0..4] the cover
 * rule per PSX blend class (index 0 unused), [5] the inner-rim rule, [6]
 * its stencil-MARK twin. */
#define OPENPETE_MATERIAL_PORTAL_PIPES  7
#define OPENPETE_MATERIAL_PORTAL_RIM    5
#define OPENPETE_MATERIAL_PORTAL_RIM_MARK 6
/* Actor-profile pipelines (OP_PROFILE_ACTOR, the opaque ACTOR group's
 * tie-pull depth export): [0] plain, [1] its stencil-MARK twin. */
#define OPENPETE_MATERIAL_ACTOR_PIPES   2

/* The moved constant tables of one fragment profile (mod_spv_cdata.h):
 * the std430 bytes the profile's SPIR-V reads at set 3 binding 2, and the
 * slot where the renderer caches the device copy it binds for them. len 0
 * = the profile reads nothing there. Both live as long as the shader. */
typedef struct openpete_mod_cdata {
    const void* bytes;
    size_t      len;
    void**      gpu;
} openpete_mod_cdata_t;

/* Material selectors (declarative and per-instance) - draw-time resolve
 * for the batcher.
 * NULL spv side = keep the stock stage (executor falls back to the
 * embedded psx_scene shaders). uniform_slot > 0 = a per-instance
 * override: bind material-UBO block (1 + uniform_slot) as the mod
 * block; 0 = declarative whole-class (engine block only). */
typedef struct {
    const uint32_t* vert_spv; size_t vert_words;
    const uint32_t* frag_spv; size_t frag_words;
    /* One cached pipeline per BLEND CLASS (0 opaque, 1 average, 2 additive,
     * 3 subtract): a material reshades within whatever group it lands in and
     * inherits that group's compositing, so one shader needs one pipeline per
     * blend it is drawn under. */
    void** pipeline_slot[OPENPETE_MATERIAL_PIPE_VARIANTS];
        /* [0..3] PSX blend classes, [4] portal sky (opaque, no depth
         * write, stencil TEST against the aperture mask), [5] the decal
         * profile (openpete_psx_decal.glsl). */
    void** pipeline_mark_slot;  /* stencil-MARK twin (hole-fill groups) */
    /* The portal profile of the fragment stage (OP_PROFILE_PORTAL; NULL
     * when the material has none) and its pipelines, indexed as
     * OPENPETE_MATERIAL_PORTAL_PIPES describes. */
    const uint32_t* frag_portal_spv; size_t frag_portal_words;
    void** pipeline_portal_slot[OPENPETE_MATERIAL_PORTAL_PIPES];
    /* The actor profile of the fragment stage (OP_PROFILE_ACTOR; NULL when
     * the material has none: the opaque ACTOR group then draws the stock
     * profile) and its pipelines, indexed as OPENPETE_MATERIAL_ACTOR_PIPES
     * describes. */
    const uint32_t* frag_actor_spv; size_t frag_actor_words;
    void** pipeline_actor_slot[OPENPETE_MATERIAL_ACTOR_PIPES];
    /* The moved tables of the stock, portal and actor fragment profiles. */
    openpete_mod_cdata_t frag_cdata, frag_portal_cdata, frag_actor_cdata;
    const char* owner;
    int   uniform_slot;
    unsigned generation;  /* the material's slot generation: moves when its pipelines are retired */
    int   index;          /* the material's registry index (openpete_mods_material_enum_at) */
    /* A composed draw: the chain entry the tag names (its block pair in the
     * chain uniform set), the combined module the stages and slots above
     * belong to, and the module's vertex variant (the lowest member's
     * vertex stage). chain -1 for a one-material draw. */
    int   chain;
    int   chain_module;
    int   chain_vvar;
} openpete_mod_material_gpu_t;
/** @brief Resolve a draw's provenance tag to the material override or chain that applies.
 *
 * Takes no lock; the compose calls it under the registry read lock. Only
 * materials whose shading contract equals @p contract take part. A moby
 * slot (bits 27:16) selects its material directly: an interned refine
 * outcome, OPENPETE_MATERIAL_SLOT_DECL | index for a declarative landing,
 * which draws with that material's static block, or a stamped chain
 * entry; a held owner there draws stock. A terrain slot (bits 24:16)
 * carries the walked face's resolved class outcome
 * (openpete_mods_material_terrain_walk_slot) the same way. A tag with slot
 * 0, and every tag of the player, flame, particle, sky, shadow, tracer and
 * portal channels, resolves at draw time from the declaratives matching it
 * (mod_mat_resolve_index): owners held for this compose are left out, and
 * composable materials compose into a draw-time chain entry; a decal
 * contract chain draws through the decal module. Rank is the owner's
 * enabled-list position, later higher, and inside one mod the
 * registration order selected by its api pin (later from 13, earlier
 * below). The level a level key tests is the installed view's. An
 * OPENPETE_MATERIAL_SLOT_STOCK slot, a slot past the view, a slot whose
 * material is swept, user-disabled or owner disabled, and a slot whose
 * material has another contract resolve to none.
 *
 * @param tag Provenance tag; 0 resolves to none.
 * @param contract The draw site's OPENPETE_MATERIAL_CONTRACT_*.
 * @param out Receives the shaders, pipeline slots, owner and uniform slot.
 * @return The winning material index, or -1 for none. */
int  openpete_mods_material_resolve(uint32_t tag, int contract,
                                    openpete_mod_material_gpu_t* out);
/** @brief Retire the pipelines of every material a mod registered and mark the materials dead.
 *
 * Takes the registry write lock. Also drops a parked selector retune, bumps
 * the terrain epoch, and re-arms the batcher tag split. The slots stay
 * allocated with live 0 and are not reused, so the table's capacity is
 * consumed by every registration, including re-registrations after a hot
 * reload.
 *
 * @param mod_id Owning mod id. */
void openpete_mods_material_remove_owner(const char* mod_id);
/** @brief Move the cached pipelines of every live material to the retirement list.
 *
 * Takes the registry write lock. Used at shutdown; the materials stay live
 * and rebuild on next use. */
void openpete_mods_material_retire_all(void);

/** @brief Return 1 when any live material selector carries a per-instance callback or is a composable moby material.
 *
 * A cached flag, recomputed whenever a selector registers, toggles, is
 * retuned or is swept. A composable moby material is a scene-contract
 * material on channel -1 or OP_CHAN_MOBY of a mod pinned at api 13 or
 * later without OP_SEL_EXCLUSIVE: a chain is a per-moby outcome only the
 * walk stamps. The moby walk skips the per-instance resolve while it is 0.
 * @return 1 or 0. */
int  openpete_mods_material_has_instance(void);
/* Moby tag slot field [27:16], the extraction walk's outcome: 0 defers to
 * the draw-time resolve (no live refine material at extraction);
 * 1..OPENPETE_MODS_MATERIAL_VIEW_SLOTS-1 is an interned refine outcome;
 * OPENPETE_MATERIAL_SLOT_DECL | index is a declarative landing (the
 * material draws with its static uniform block); OPENPETE_MATERIAL_SLOT_STOCK
 * is an exhausted walk (draw stock). */
#define OPENPETE_MATERIAL_SLOT_STOCK 0xFFF
#define OPENPETE_MATERIAL_SLOT_DECL  0x800
/* OPENPETE_MATERIAL_SLOT_CHAIN + id (0x080-0x17F) names chain entry id of
 * the present: two to OPENPETE_MATERIAL_CHAIN_CAP members composed, the
 * lowest-ranked first. Added, not OR'd: ids 0x80-0xFF would fold onto
 * 0x080-0x0FF under an OR. */
#define OPENPETE_MATERIAL_SLOT_CHAIN 0x080
#define OPENPETE_MATERIAL_CHAINS     256
#define OPENPETE_MATERIAL_CHAIN_CAP  4
/* Terrain chain entries a present holds: the terrain field names entry id
 * as OPENPETE_MATERIAL_TERRAIN_CHAIN | id (0x080-0x0FF), a retained pass as
 * OPENPETE_MATERIAL_TER_PASS_CHAIN + id. */
#define OPENPETE_MATERIAL_TERRAIN_CHAINS 128
/* Chain entry ids across both stamped tables: moby entry id is id, terrain
 * entry id is OPENPETE_MATERIAL_CHAINS + id. */
#define OPENPETE_MATERIAL_CHAIN_ENTRIES (OPENPETE_MATERIAL_CHAINS + OPENPETE_MATERIAL_TERRAIN_CHAINS)
/* Draw-time chain entries one compose holds: the chains the compose walks
 * from the declaratives matching a tag of a channel nothing stamps (every
 * channel but moby and terrain, a moby tag with slot 0, a terrain tag with
 * slot 0). Entry id is OPENPETE_MATERIAL_DT_CHAIN_BASE + id. */
#define OPENPETE_MATERIAL_DT_CHAINS     64
#define OPENPETE_MATERIAL_DT_CHAIN_BASE OPENPETE_MATERIAL_CHAIN_ENTRIES
/* Every chain entry id: the stamped tables, then the draw-time range. */
#define OPENPETE_MATERIAL_CHAIN_IDS     (OPENPETE_MATERIAL_DT_CHAIN_BASE + OPENPETE_MATERIAL_DT_CHAINS)
/* Fragment shaders one installed member set holds, and its profiles
 * (0 stock, 1 portal, 2 terrain of the scene contract; 3 the decal
 * contract's one profile). */
#define OPENPETE_MATERIAL_CHAIN_SET_MAX 64
#define OPENPETE_MATERIAL_CHAIN_PROFILES 4
#define OPENPETE_MATERIAL_CHAIN_TERRAIN  2
#define OPENPETE_MATERIAL_CHAIN_DECAL    3
/* The member set of the installed combined modules: the fragment shaders
 * whose op_material a composed draw can call (the scene-contract members,
 * then the decal-contract members), and each one's case id in the stock,
 * the portal, the terrain and the decal module (-1 when that module lacks
 * it). Built with the modules and immutable once published. */
typedef struct {
    unsigned generation;                  /* nonzero; moves with every published set */
    int      module;                      /* the combined module pair it describes */
    int      n;
    int      shader[OPENPETE_MATERIAL_CHAIN_SET_MAX];
    int      case_id[OPENPETE_MATERIAL_CHAIN_PROFILES][OPENPETE_MATERIAL_CHAIN_SET_MAX];
} openpete_mods_material_chain_set_t;
/** @brief Publish the member set of the combined modules the compose installed; the extraction walk reads it from its next moby.
 *
 * Takes no lock: one atomic pointer store. The record must stay valid
 * while published and while any scene extracted against it can be
 * composed; the publisher never frees a published record.
 * @param set The record, or NULL for none installed. */
void openpete_mods_material_chain_set_publish(const openpete_mods_material_chain_set_t* set);
/** @brief Return the published member set, or NULL when no combined module is installed.
 *
 * @return The record. */
const openpete_mods_material_chain_set_t* openpete_mods_material_chain_set_current(void);
/* Bytes of one chain uniform block: the chain header (member count at 0,
 * the member case ids as a uvec4 at 16) in the first 256 bytes, then one
 * 256-byte member block per chain position, lowest-ranked first. */
#define OPENPETE_MATERIAL_CHAIN_BLOCK_BYTES 1280u
/* Blocks of the chain uniform set: the engine block, two per moby entry
 * (stock, portal), three per terrain entry (stock, portal, terrain) and two
 * per draw-time entry (stock or decal, portal). */
#define OPENPETE_MATERIAL_CHAIN_UBO_BLOCKS \
    (1 + 2 * OPENPETE_MATERIAL_CHAINS + 3 * OPENPETE_MATERIAL_TERRAIN_CHAINS + 2 * OPENPETE_MATERIAL_DT_CHAINS)
/** @brief Return the chain uniform set block of one chain entry and profile, or -1 for a profile the entry's draws never use or an id out of range.
 *
 * A moby entry uses the stock and portal profiles, a terrain entry those
 * and the terrain profile, a draw-time entry the stock and portal profiles
 * or, for a decal-contract chain, the decal profile in the stock one's
 * block.
 * @param entry Chain entry id, 0 to OPENPETE_MATERIAL_CHAIN_IDS - 1.
 * @param profile 0 stock, 1 portal, 2 terrain, 3 decal.
 * @return The block index. */
static inline int openpete_mods_material_chain_ubo_block(int entry, int profile) {
    if (entry < 0 || entry >= OPENPETE_MATERIAL_CHAIN_IDS || profile < 0 ||
        profile >= OPENPETE_MATERIAL_CHAIN_PROFILES)
        return -1;
    if (entry >= OPENPETE_MATERIAL_DT_CHAIN_BASE) {
        if (profile == OPENPETE_MATERIAL_CHAIN_TERRAIN) return -1;
        return 1 + 2 * OPENPETE_MATERIAL_CHAINS + 3 * OPENPETE_MATERIAL_TERRAIN_CHAINS +
               2 * (entry - OPENPETE_MATERIAL_DT_CHAIN_BASE) + (profile == 1);
    }
    if (profile == OPENPETE_MATERIAL_CHAIN_DECAL) return -1;
    if (entry < OPENPETE_MATERIAL_CHAINS)
        return profile < OPENPETE_MATERIAL_CHAIN_TERRAIN ? 1 + 2 * entry + profile : -1;
    return 1 + 2 * OPENPETE_MATERIAL_CHAINS + 3 * (entry - OPENPETE_MATERIAL_CHAINS) + profile;
}
/** @brief Write one chain entry's uniform block for one profile's combined module: the header and each member's block.
 *
 * Reads the installed view (else the live tables) and the published
 * member set. A member's block is its uniform slot's bytes, or the engine
 * block for a member with no params, which its own draw binds in that
 * place. Fails for an entry that draws stock (mod_mat_resolve_chain's
 * cases), for a member the profile's module lacks and for a moby entry's
 * terrain profile.
 * @param entry Chain entry id, 0 to OPENPETE_MATERIAL_CHAIN_ENTRIES - 1.
 * @param profile 0 stock, 1 portal, 2 terrain.
 * @param engine The engine block bytes, or NULL.
 * @param engine_len Their count.
 * @param out Receives OPENPETE_MATERIAL_CHAIN_BLOCK_BYTES bytes.
 * @return 0, or -1 when the entry draws stock in this profile. */
int openpete_mods_material_chain_block(int entry, int profile, const void* engine, uint32_t engine_len,
                                       void* out);
/** @brief Return the draw-time generation: it moves whenever the draw-time chain entries are emptied, so an entry id names one chain while it holds.
 *
 * @return The generation, nonzero. */
unsigned openpete_mods_material_dt_chain_gen(void);
/** @brief Return the chain entries interned this present in the live table.
 *
 * @return The count. */
int  openpete_mods_material_chain_count(void);
/** @brief Return the terrain chain entries interned this present in the live table.
 *
 * @return The count. */
int  openpete_mods_material_terrain_chain_count(void);
/** @brief Generate the combined module texts of the current member set when it changed; the sim thread calls it at the tick boundary and the precompile before it runs.
 *
 * Takes the registry write lock, and returns at once while the material
 * table has not changed since the last call. The member set: the
 * fragment shaders of live scene-contract materials whose stock profile
 * is linkable and not identity, built while a composable scene material
 * (pinned at api 13 or later, not OP_SEL_EXCLUSIVE) is live, and those of
 * live decal-contract materials, built while a composable decal material
 * is live; the first composable material also decides every pending
 * member (openpete_mods_shader_link_resolve). A set equal to one built
 * earlier in this run asks for that one and builds nothing. A new set gets its
 * texts and case ids here and no glslang compile: the compose claims its
 * compile (openpete_mods_material_chain_compile_claim) and runs it
 * (openpete_mods_material_chain_compile), and installs the set
 * (openpete_mods_material_chain_install) once it compiled and its
 * pipelines exist. */
void openpete_mods_material_chain_sync(void);
/** @brief Return the combined module set the newest sync asked for when it is not the installed one, or -1.
 *
 * @return The module id. */
int  openpete_mods_material_chain_pending(void);
/** @brief Claim a module set's compile: true for the one caller that is to run or queue openpete_mods_material_chain_compile.
 *
 * Any thread.
 * @param module The module id.
 * @return 1 when claimed, 0 when already claimed, compiled or unknown. */
int  openpete_mods_material_chain_compile_claim(int module);
/** @brief Compile a claimed module set's texts through the mod shader compile cache, then mark it compiled.
 *
 * Any thread, once per claim: it reads only the set's texts and writes only
 * the set, which no reader reads before it is marked compiled. A failed
 * stock module (or decal module of a set without one) fails the set; a
 * failed portal, terrain, decal or actor module leaves that profile out.
 * @param module The module id. */
void openpete_mods_material_chain_compile(int module);
/** @brief Say whether the compose runs the composition compiles off its thread; the sync then hands pending members to a link job instead of deciding them itself.
 *
 * Display owner, at each live present; set false by default, so the boot
 * sets and every synchronous run decide members at the sync.
 * @param on Nonzero for a live present of a non-synchronous run. */
void openpete_mods_material_chain_async_set(int on);
/** @brief Claim a posted link job: true for the one caller that is to queue openpete_mods_material_chain_links_resolve.
 *
 * @return 1 when claimed, 0 when no job is posted. */
int  openpete_mods_material_chain_links_claim(void);
/** @brief Decide a claimed link job's pending members on copies of their shaders; the next sync applies them.
 *
 * Any thread, once per claim: it reads only the job's copies and the
 * shaders' files, through its own link context. */
void openpete_mods_material_chain_links_resolve(void);
/** @brief Return a module set's compile state.
 *
 * Any thread.
 * @param module The module id.
 * @return 1 compiled, 0 not yet, -1 failed or unknown. */
int  openpete_mods_material_chain_compile_state(int module);
/** @brief Install a built combined module pair: publish its member set to the extraction walk.
 *
 * @param module The module id. */
void openpete_mods_material_chain_install(int module);
/** @brief Describe one combined module pair and vertex variant as a draw's GPU description: the module's stages, pipeline slots and moved tables.
 *
 * @param module The module id.
 * @param vvar Vertex variant, 0 for the stock vertex stage.
 * @param out Receives the description, chain -1.
 * @return 0, or -1 for an unknown module or variant. */
int  openpete_mods_material_chain_gpu(int module, int vvar, openpete_mod_material_gpu_t* out);
/** @brief Describe one module set's decal-contract module and vertex variant as a decal draw's GPU description.
 *
 * @param module The module id.
 * @param vvar Vertex variant, 0 for the stock vertex stage.
 * @param out Receives the description, chain -1.
 * @return 0, or -1 for an unknown module or variant or a set with no decal module. */
int  openpete_mods_material_chain_decal_gpu(int module, int vvar, openpete_mod_material_gpu_t* out);
/* Terrain tag slot field [24:16], 9 bits, the walked face class's resolved
 * outcome: 0 defers to the draw-time resolve (no material was registered
 * at extraction); 1..OPENPETE_MODS_MATERIAL_VIEW_SLOTS-1 is an interned
 * refine outcome; OPENPETE_MATERIAL_TERRAIN_CHAIN | id (0x080-0x0FF) is
 * terrain chain entry id of the present; OPENPETE_MATERIAL_TERRAIN_DECL |
 * index is a declarative landing; OPENPETE_MATERIAL_TERRAIN_STOCK is
 * stock. */
#define OPENPETE_MATERIAL_TERRAIN_STOCK 0x1FF
#define OPENPETE_MATERIAL_TERRAIN_DECL  0x100
#define OPENPETE_MATERIAL_TERRAIN_CHAIN 0x080
/** @brief Walk the materials matching one moby at extraction and return the outcome stamped into its tag.
 *
 * Takes no lock. Returns 0 while openpete_mods_material_has_instance is 0.
 * Otherwise walks the materials one predicate accepts for the moby
 * (channel -1 or moby, class and identity equal to the class or -1, level
 * key in scope, scene contract, enabled; a texture key or an env, tier or
 * untextured flag rejects) from the highest ranked down: a declarative
 * material accepts; a refine material's callback runs, inside a guarded
 * stretch owned by its mod (openpete_mods_guard_enter), and accepts unless
 * it returns -1, which passes the moby to the next material. The first
 * acceptance draws alone when it is terminal (pinned below 13 or
 * OP_SEL_EXCLUSIVE), has a vertex stage, or is not linkable into the
 * installed combined module (openpete_mods_material_chain_set_current);
 * otherwise the walk continues, skipping unlinkable materials unasked,
 * adding each accepting linkable one, and stopping after a terminal one
 * or at OPENPETE_MATERIAL_CHAIN_CAP members; identity members are left
 * out. Two or more members intern a chain entry. An accepted block,
 * capped at OPENPETE_MATERIAL_UNIFORMS_MAX, reuses an interned slot of
 * the same material with identical bytes, else takes a new one; the slot
 * table is reset by openpete_mods_material_frame_reset(), so a slot is
 * valid for one present. The walk's outcome is kept for the tick: a later
 * present of the same tick (a sub-tick re-extract) with an unchanged
 * material table replays it without calling a callback.
 *
 * @param cpu Unused.
 * @param moby_class Moby class id.
 * @param moby_vaddr Guest address of the moby.
 * @return 0 to defer to the draw-time resolve, an interned slot,
 *         OPENPETE_MATERIAL_SLOT_DECL | material index for a declarative
 *         landing, OPENPETE_MATERIAL_SLOT_CHAIN + chain entry id for a
 *         composed moby, or OPENPETE_MATERIAL_SLOT_STOCK when every
 *         material declined or none matched, or the slot or chain table
 *         is full. */
int  openpete_mods_material_moby_slot(CPUState* cpu, int moby_class,
                                      uint32_t moby_vaddr);
/** @brief Resolve one walked terrain face's resolution class and return the outcome stamped into its tag.
 *
 * Takes no lock; sim thread. Returns 0 when no material is registered. The
 * face key is its Tiledef id (-1 when it has none: an imported sector's
 * borrowed face, or an untextured face), its own texturedness, env-rewrite
 * membership and tier. Its class
 * (openpete_mods_material_terrain_class) resolves once per present, down
 * its members from the highest ranked: a refine member is asked through
 * its callback (at most once per present and once per tick per material,
 * inside a guarded stretch owned by the material's mod) and a decline
 * passes the face to the next member; a declarative member accepts. The
 * first acceptance draws alone when it is terminal (pinned below 13 or
 * OP_SEL_EXCLUSIVE), has a vertex stage, or is not linkable into the
 * installed combined module; otherwise the walk continues under the moby
 * walk's rules (openpete_mods_material_moby_slot) and two or more members
 * intern a terrain chain entry.
 * @param cpu Unused.
 * @param texid Tiledef id, or -1.
 * @param untex Nonzero for an untextured face.
 * @param env Nonzero for a face in the env-rewrite set.
 * @param lp Nonzero for an LP-tier face.
 * @return The [24:16] field: an interned slot, OPENPETE_MATERIAL_TERRAIN_DECL
 *         | material index for a declarative landing,
 *         OPENPETE_MATERIAL_TERRAIN_CHAIN | chain entry id for a composed
 *         class, or OPENPETE_MATERIAL_TERRAIN_STOCK when the class has no
 *         member, every member declined, or the slot or chain table is
 *         full. */
int  openpete_mods_material_terrain_walk_slot(CPUState* cpu, int texid, int untex, int env, int lp);
/** @brief Return the epoch the retained terrain cache latches its bake against.
 *
 * A baked face carries its material's bucket, so any change that can move
 * a terrain bucket makes the bake stale. The value is 1 + a counter bumped
 * on a change to a material that could bucket terrain (register, remove,
 * selector retune, enable toggle) and on a level change + the mod toggle
 * epoch; it strictly increases on each such change and is never 0, which
 * the latch keeps as its unset value.
 * @return The epoch. */
unsigned openpete_mods_material_terrain_epoch(void);

/* ── Retained-terrain resolution classes ─────────────────────────────
 * A retained face is baked once and redrawn for many presents without a
 * walk, so it cannot carry a per-present outcome. It carries its
 * RESOLUTION CLASS instead, 0 = no candidate: the candidate list of its
 * face key (every matching material in rank order down to and including
 * the first declarative of a mod pinned below 13). Each present resolves
 * every class to stock, one member or a chain and publishes a
 * class-to-pass table; a pass is a resolved member (its material index +
 * 1) or a terrain chain entry (OPENPETE_MATERIAL_TER_PASS_CHAIN + id),
 * and pass 0 draws stock. The vertex stage keeps only the faces
 * whose class maps to the pass being drawn (a 256-bit class mask in the
 * push constants); the material epoch invalidates the bake. Identity,
 * never an outcome: a refine's outcome may change every tick without a
 * rebake. */
#define OPENPETE_MATERIAL_TER_VARIANTS 5  /* arena vertex stage: plain/anim/near,
                                          * then the anim/near cell records */
/* Colour slot: world, world-MARK (hole-fill coverage), fill (LP backing),
 * then the three PSX blend classes. Depth and key passes shade nothing and
 * need no material pipeline. */
#define OPENPETE_MATERIAL_TER_SLOTS    6
#define OPENPETE_MATERIAL_TER_PIPES \
    (OPENPETE_MATERIAL_TER_VARIANTS * OPENPETE_MATERIAL_TER_SLOTS)
/* Class ids are stamped into 8 spare bits of the baked face material; a
 * pass list holds at most one pass per class. */
#define OPENPETE_MATERIAL_TER_BUCKET_MAX 255
/* The first chain pass id: a pass id below it is a material index + 1. */
#define OPENPETE_MATERIAL_TER_PASS_CHAIN 0x100
/* One past the largest pass id. */
#define OPENPETE_MATERIAL_TER_PASS_IDS (OPENPETE_MATERIAL_TER_PASS_CHAIN + OPENPETE_MATERIAL_TERRAIN_CHAINS)

/** @brief Return the retained-terrain resolution class of a face key, or 0 for none.
 *
 * The candidates of the key are the live, user-enabled, scene-contract
 * materials of enabled mods on channel -1 or OP_CHAN_TERRAIN in scope for
 * the current level: the declaratives whose selector accepts the face
 * (texture key, env, tier, texturedness) and the refine materials whose
 * one key (a Tiledef id, or OP_SEL_UNTEXTURED for the untextured band) is
 * the face's, highest ranked first, down to and including the first
 * declarative of a mod pinned below 13, at most 32 (31 above a trailing
 * pinned-below declarative). OP_SEL_EXCLUSIVE does not end the list.
 * Rank is the owner's enabled-list position, later
 * higher, and inside one mod the registration order selected by its api
 * pin. Equal candidate lists share one class id. Ids are interned for the
 * session; with the table full a class no path used under the current
 * terrain epoch is reused, and with every class current the key gets 0.
 * Marks the class as baked in the current terrain epoch, which puts it in
 * openpete_mods_material_terrain_refine_present's resolution and the pass
 * list.
 * @param texid Tiledef id, or -1 when the face has none.
 * @param untex Nonzero for an untextured face.
 * @param env Nonzero for a face in the env-rewrite set.
 * @param lp Nonzero for an LP-tier face.
 * @return The class id 1..OPENPETE_MATERIAL_TER_BUCKET_MAX, 0 for none. */
int  openpete_mods_material_terrain_class(int texid, int untex, int env, int lp);
/** @brief Write the retained-terrain material pass ids in ascending order.
 *
 * A pass id is a material index + 1, or OPENPETE_MATERIAL_TER_PASS_CHAIN +
 * terrain chain entry id, listed when a class the retained bake stamped in
 * the current terrain epoch resolved to that outcome this present, so the
 * count is at most the distinct outcomes of the baked classes; the stock
 * pass 0 is not listed. The terrain cache draws one pass
 * per listed id after the stock pass. With a material view installed the
 * list is the one the view captured.
 * @param out Receives the ids.
 * @param cap Capacity of @p out; OPENPETE_MATERIAL_TER_BUCKET_MAX always
 *        suffices.
 * @return The count written; 0 for a NULL @p out or @p cap <= 0. */
int  openpete_mods_material_terrain_bucket_list(int* out, int cap);
/** @brief Fill the 256-bit class mask of one retained-terrain pass.
 *
 * Bit c is set when class c draws in @p pass: for pass 0 class 0, every
 * class resolved to stock, and every class not baked in the current
 * terrain epoch or not resolved for the composed present; for a material
 * pass the classes that resolved to it. Reads the installed view's
 * class-to-pass table, else the live one.
 * @param pass 0 for the stock pass, else a listed pass id.
 * @param mask Receives 8 words, bit c in word c / 32.
 * @return The number of classes set. */
int  openpete_mods_material_terrain_pass_mask(int pass, uint32_t mask[8]);

typedef struct {
    const uint32_t* frag_spv;    /* the OP_PROFILE_TERRAIN module          */
    size_t          frag_words;
    void**          pipeline_slot; /* cached rhi_pipeline* for this variant */
    openpete_mod_cdata_t frag_cdata; /* the terrain module's moved tables     */
    const char*     owner;
    int             uniform_slot;  /* material-UBO block (0 = no params)    */
    unsigned        generation;    /* the material's slot generation        */
    int             index;         /* the material's registry index         */
    /* A chain pass: the chain entry id whose terrain block the draw binds in
     * the chain uniform set, and the combined module the stage and slot
     * belong to; both -1 for a one-material pass. */
    int             chain;
    int             chain_module;
} openpete_mod_material_terrain_t;
/** @brief Fill the GPU binding of one retained-terrain material pass for one pipeline variant.
 *
 * The pass's material must be live, user-enabled, owned by an enabled
 * mod, and carry a fragment shader with a terrain profile. A per-instance
 * material binds the uniform slot its refine callback produced for the
 * composed scene: the installed material view's outcome, or with no view
 * the outcome resolved this present. No callback runs here; a declined
 * outcome, or none for that scene, returns -1. A declarative material
 * binds its static slot, read through the installed material view when one
 * is set.
 * A chain pass binds the terrain-profile combined module with the chain
 * entry; it fails when mod_mat_resolve_terrain_chain sends the chain to
 * stock (a member held, unusable, or lacked by the installed terrain
 * module) or no terrain module is built.
 * On -1 the caller draws the pass's faces with the stock pipeline, since
 * the stock pass has already dropped them.
 * @param bucket Pass id: a material index + 1, or
 *        OPENPETE_MATERIAL_TER_PASS_CHAIN + terrain chain entry id.
 * @param pipe_idx Pipeline variant index, 0 to OPENPETE_MATERIAL_TER_PIPES - 1.
 * @param out Receives the terrain module, the cached pipeline slot, the
 *        owner and the uniform slot.
 * @return 0 on success, -1 otherwise. */
int  openpete_mods_material_terrain_bucket_gpu(
        int bucket, int pipe_idx, openpete_mod_material_terrain_t* out);
/** @brief Describe one combined module's retained-terrain pipeline slot: the terrain-profile module, its moved tables and the slot.
 *
 * Takes no lock. The uniform slot and chain entry are not filled.
 * @param module The module id.
 * @param pipe_idx Variant * OPENPETE_MATERIAL_TER_SLOTS + colour slot.
 * @param out Receives the description, chain -1 and chain_module @p module.
 * @return 0, or -1 for an unknown module, an index out of range or a module without a terrain profile. */
int  openpete_mods_material_chain_terrain_gpu(int module, int pipe_idx, openpete_mod_material_terrain_t* out);
/** @brief Recompute the material overlap pairs when a register, enable, retune, owner removal, mod toggle, level change or refine observation moved them.
 *
 * Tick boundary, sim thread. Takes the registry write lock to snapshot the
 * selectors and again to publish; computes between the two without it.
 * Logs each pair whose shadow verdict changed. */
void openpete_mods_material_overlap_tick(void);
/** @brief One overlapping material pair for the dev overlay. */
typedef struct {
    int         hi, lo;          /* material indices; hi ranks above lo */
    const char* hi_owner;
    const char* lo_owner;
    int         hi_refine, lo_refine;
    const char* verdict;         /* "full" or "partial" */
    unsigned    chans;           /* bit OP_CHAN_* per channel with a shared key */
    int         accept_all;      /* full because hi is a moby refine that never declined */
} openpete_mods_material_overlap_info_t;
/** @brief Return the number of listed overlap pairs.
 *
 * Read under the registry read lock (the compose).
 * @return The count. */
int  openpete_mods_material_overlap_count(void);
/** @brief Return the number of overlap pairs found, listed or not.
 *
 * @return The count. */
int  openpete_mods_material_overlap_total(void);
/** @brief Describe one listed overlap pair.
 *
 * Read under the registry read lock (the compose).
 * @param i Index below openpete_mods_material_overlap_count().
 * @param out Receives the pair.
 * @return 0, or -1 out of range. */
int  openpete_mods_material_overlap_at(int i, openpete_mods_material_overlap_info_t* out);
/** @brief Return the size of the material table, live and swept slots alike, for openpete_mods_material_enum_at.
 *
 * Takes no lock; the precompile calls it under the registry read lock.
 * @return The count. */
int  openpete_mods_material_enum_count(void);
/** @brief Describe one material for the pipeline precompile, enabled or not.
 *
 * Takes no lock. Fills the same shaders and pipeline slots the draw-time
 * resolve hands a draw site, with the material's static uniform slot, and
 * reports its shading contract and whether the retained terrain cache can
 * draw it: scene contract, a selector (current or parked) on channel -1 or
 * OP_CHAN_TERRAIN, and a fragment shader with a terrain profile.
 * @param i Material index, below openpete_mods_material_enum_count().
 * @param contract Receives the OPENPETE_MATERIAL_CONTRACT_*.
 * @param terrain Receives 1 when the terrain cache can draw it, else 0.
 * @param out Receives the scene-site shaders and slots.
 * @return 0 for a live material, -1 for a swept slot or a bad index. */
int  openpete_mods_material_enum_at(int i, int* contract, int* terrain,
                                    openpete_mod_material_gpu_t* out);
/** @brief Describe one material's retained-terrain pipeline slot for the precompile, enabled or not.
 *
 * Takes no lock. Valid only for a material openpete_mods_material_enum_at
 * reported terrain-drawable; the uniform slot is not filled.
 * @param i Material index.
 * @param pipe_idx Variant * OPENPETE_MATERIAL_TER_SLOTS + colour slot.
 * @param out Receives the terrain-profile fragment stage, the slot and the owner.
 * @return 0 on success, -1 otherwise. */
int  openpete_mods_material_enum_terrain(int i, int pipe_idx,
                                         openpete_mod_material_terrain_t* out);
/** @brief Return the size of the postfx table, live and swept passes alike, for openpete_mods_postfx_enum_at.
 *
 * Takes no lock.
 * @return The count. */
int  openpete_mods_postfx_enum_count(void);
/** @brief Describe one postfx pass for the pipeline precompile, enabled or not.
 *
 * Takes no lock. Fills the fragment stage, the pipeline slot and the owner
 * as openpete_mods_postfx_collect does; the uniforms are not filled.
 * @param i Pass index, below openpete_mods_postfx_enum_count().
 * @param out Receives the pass.
 * @return 0 for a live pass, -1 for a swept one or a bad index. */
int  openpete_mods_postfx_enum_at(int i, openpete_postfx_pass_t* out);
/** @brief Reset the per-present interned material slot table.
 *
 * native_render calls it at scene begin and at the canonical present's
 * feed reset; the extractor then interns per-instance slots during the
 * walk, and native_render writes each slot's bytes into the material UBO
 * before the scene draw. It advances the present counter that the terrain
 * refine memo keys on, and re-interns the static uniform block of every
 * live material with params whose owner is enabled, so a declarative
 * material has a slot for the whole present; once the table is full the
 * remaining materials get static slot 0 (no params).
 * @param tick_open Nonzero when the game's own render opens the scene of a
 *        new tick: the refine outcomes kept for the previous tick are
 *        dropped. Zero for a re-extract of the same tick, which replays
 *        them. */
void openpete_mods_material_frame_reset(int tick_open);
/** @brief Set the level being rendered, for level-scoped material selector keys.
 *
 * Sector and texture ids are per-level, and the mods layer cannot read the
 * game's level symbol, so the game-side render feeders push the id before
 * they extract (terrain build, moby walk, re-extract, each scene feeder).
 * Until a push the level is -1, which makes level keys inert. A change of
 * level bumps the terrain material epoch, since the retained terrain
 * latches level-dependent verdicts against it. The material view captures
 * the level, and a draw-time resolve reads the installed view's level. One
 * int written on the sim thread; no lock is taken.
 * @param level_id The game's level id. */
void openpete_mods_set_level(int level_id);
/** @brief Resolve this present's member of every terrain class the retained bake stamped in the current terrain epoch.
 *
 * Sim thread, after the bake, the frame reset and the level push, before
 * the walked terrain resolves its own faces: the walk resolves only the
 * classes it reaches, and a class whose faces are all retained has no
 * walked face to resolve it. Each class walks its members as
 * openpete_mods_material_terrain_walk_slot describes; a refine is asked
 * only as a member of a class, above its key's declarative winner, so a
 * refine a higher-ranked declarative outranks on every face is never
 * asked. The material view captures the resolved class-to-pass table and
 * the refine outcomes, so the compose binds them without calling a
 * callback (openpete_mods_material_terrain_bucket_gpu). */
void openpete_mods_material_terrain_refine_present(void);
/** @brief Return one plus the number of slots interned this present in the live table.
 *
 * Slot 0 is reserved, so the valid slots are 1 up to the count minus one.
 * Reads the live table, not the installed view.
 *
 * @return The count. */
int  openpete_mods_material_slot_count(void);
/** @brief Return the uniform bytes of an interned slot in the live table.
 *
 * The pointer addresses the live table and changes at the next
 * openpete_mods_material_frame_reset().
 *
 * @param slot Slot index.
 * @param len Receives the byte count, 0 when the slot is out of range; may
 *        be NULL.
 * @return The bytes, or NULL for slot 0, a negative slot, or a slot at or
 *         past the count. */
const void* openpete_mods_material_slot_uniforms(int slot, uint32_t* len);
/* The material view: the per-present interned-slot table (bytes AND the
 * slot -> material / material -> static-slot indices) as one immutable
 * snapshot. The sim thread captures it at scene publish; the present
 * thread composes a published scene through it, so every index it binds
 * refers to the bytes it uploaded. Reading the live table from the present
 * thread races openpete_mods_material_frame_reset, which zeroes every
 * static slot before re-minting it: a read in that window binds block 0
 * (the engine block) as the mod's uniforms. */
#define OPENPETE_MODS_MATERIAL_VIEW_SLOTS 128
#define OPENPETE_MODS_MATERIAL_VIEW_MATS   255
typedef struct {
    int slot_n;                                   /* 1 + interned slots */
    struct { int mat; uint32_t len; unsigned char bytes[256]; }
        slots[OPENPETE_MODS_MATERIAL_VIEW_SLOTS];
    int static_slot[OPENPETE_MODS_MATERIAL_VIEW_MATS];   /* 0 = no params */
    /* The retained-terrain material pass ids at capture, ascending
     * (openpete_mods_material_terrain_bucket_list). */
    int ter_bucket_n;
    int ter_bucket[OPENPETE_MODS_MATERIAL_VIEW_MATS];
    /* Per terrain class id: the pass it draws in, a material index + 1, a
     * chain pass OPENPETE_MATERIAL_TER_PASS_CHAIN + id, or 0 for the stock
     * pass (openpete_mods_material_terrain_pass_mask). */
    uint16_t ter_class_pass[OPENPETE_MATERIAL_TER_BUCKET_MAX + 1];
    /* Per material: the terrain refine outcome computed for this scene
     * (an interned slot or OPENPETE_MATERIAL_TERRAIN_STOCK), 0 for none. */
    int ter_refine[OPENPETE_MODS_MATERIAL_VIEW_MATS];
    int level;                                    /* level pushed at capture */
    /* The moby chain entries interned this present: per entry its members,
     * lowest-ranked first, and each member's uniform slot (a refine
     * outcome, the material's static slot, or 0 for no params). */
    int chain_n;
    struct { int n; short mat[OPENPETE_MATERIAL_CHAIN_CAP]; short slot[OPENPETE_MATERIAL_CHAIN_CAP]; }
        chains[OPENPETE_MATERIAL_CHAINS];
    /* The terrain chain entries interned this present, the same layout. */
    int ter_chain_n;
    struct { int n; short mat[OPENPETE_MATERIAL_CHAIN_CAP]; short slot[OPENPETE_MATERIAL_CHAIN_CAP]; }
        ter_chains[OPENPETE_MATERIAL_TERRAIN_CHAINS];
} openpete_mods_material_view_t;
/** @brief Copy the interned slots, per-material static slots, terrain refine outcomes, class-to-pass table, pass list and level into a view.
 *
 * Takes no lock; the sim thread calls it at scene publish, at each history
 * push and when the game-render scene is kept. Copies slots 1 up to the
 * live count, the static slot and this present's terrain refine outcome of
 * every material index, the terrain class-to-pass table and pass list
 * from this present's class resolutions, the moby and terrain chain
 * entries interned this present, and the pushed level.
 *
 * @param out Receives the snapshot. */
void openpete_mods_material_view_capture(openpete_mods_material_view_t* out);
/** @brief Install the material view the present thread resolves and binds through.
 *
 * native_render sets a published scene's view before composing it and
 * NULL after; NULL makes the resolve read the live tables.
 * @param v The view, or NULL. */
void openpete_mods_material_view_set(const openpete_mods_material_view_t* v);
/** @brief Return the installed material view, or NULL when the resolve reads the live tables.
 *
 * @return The view. */
const openpete_mods_material_view_t* openpete_mods_material_view_get(void);
/** @brief Return a mod's 0-based position in the loaded mod table, which is its load order.
 *
 * The table holds boot-enabled and deferred-entry mods alike, in
 * dependency order.
 * @param self The mod handle; NULL returns -1.
 * @return The load index, or -1 for NULL. */
int  openpete_mods_load_index_of(openpete_mod_t* self);
/** @brief Return a mod's manifest api pin, the `[mod] api` key.
 *
 * Rules keyed on the pin (the same-mod material tie, the overlay-window
 * override scope) read it through this accessor.
 * @param self The mod handle; NULL returns 0.
 * @return The pin, or 0 when the key is absent or @p self is NULL. */
int  openpete_mods_api_pin_of(openpete_mod_t* self);
/** @brief Return the tick, alpha and canonical flag of the last present flush that reached the present hooks.
 *
 * The postfx engine UBO and the native renderer read it, so shaders see the
 * values the present hooks received. Before the first such flush it reads
 * tick 0, alpha 0, canonical 1.
 * @param tick receives the tick, or NULL.
 * @param alpha receives the sub-tick phase, or NULL.
 * @param canonical receives 1 for a canonical present, or NULL. */
void openpete_mods_present_ctx(uint32_t* tick, float* alpha, int* canonical);
/** @brief Register a material selector with shader overrides, enabled, and return its handle.
 *
 * Takes the registry write lock. Refused inside a present hook, for a NULL
 * mod or selector, for a selector the engine cannot honour exactly, for a
 * shader handle that is not a live shader of the matching stage, when both
 * handles are negative, and when the table holds
 * OPENPETE_MATERIAL_TER_BUCKET_MAX entries, swept ones included. Bumps the
 * terrain epoch when the selector could bucket terrain and re-arms the
 * batcher tag split.
 *
 * @param self Registering mod.
 * @param sel Selector; copied.
 * @param vert_shader Vertex shader handle, or -1 for the stock stage.
 * @param frag_shader Fragment shader handle, or -1 for the stock stage.
 * @param per_instance Refine callback, or NULL for a declarative material.
 * @return The material handle, or -1. */
int openpete_mods_material_register(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int vert_shader, int frag_shader,
                                    openpete_mod_material_fn per_instance);
/** @brief Turn a registered material on or off.
 *
 * Takes the registry write lock. Bumps the terrain epoch when the material
 * could bucket terrain and re-arms the batcher tag split. The caller is not
 * checked against the owner.
 *
 * @param self Unused.
 * @param material Material handle.
 * @param on Nonzero to enable.
 * @return 0, or -1 for an unknown or swept material. */
int openpete_mods_material_enable(openpete_mod_t* self, int material, int on);
/** @brief Set a material's static uniform block.
 *
 * Takes the registry write lock. The block is copied and interned into a
 * slot at each present for a material whose owner is enabled. The caller is
 * not checked against the owner.
 *
 * @param self Unused.
 * @param material Material handle.
 * @param block Block bytes.
 * @param len Length of @p block.
 * @return 0, or -1 for an unknown or swept material, a @p len above
 *         OPENPETE_MATERIAL_UNIFORMS_MAX, or a NULL @p block with nonzero
 *         @p len. */
int openpete_mods_material_set_params(openpete_mod_t* self, int material,
                                      const void* block, uint32_t len);
/** @brief Replace a live material's selector, keeping its handle, shaders, callback and enabled state.
 *
 * Takes the registry write lock. Only the owning mod may retune, and the
 * selector must be one openpete_mods_material_register() accepts. Inside a
 * present the selector is parked until
 * openpete_mods_material_apply_pending(); outside one it applies at once
 * and bumps the terrain epoch when the old or new selector could bucket
 * terrain.
 *
 * @param self Owning mod.
 * @param material Material handle.
 * @param sel New selector; copied.
 * @return 0, or -1 for a NULL argument, an unknown or swept material, a
 *         material another mod owns, or an invalid selector. */
int openpete_mods_material_set_selector(
        openpete_mod_t* self, int material,
        const openpete_mod_material_selector_t* sel);
/** @brief Install material selectors retuned from inside a present.
 *
 * openpete_mods_tick_boundary calls it, where a selector change cannot
 * split one present between two keys. Returns without taking the registry
 * lock when nothing is parked. */
void openpete_mods_material_apply_pending(void);

/** @brief Mod API sfx_replace: register an in-memory SPU-ADPCM body as a symbolic sfx-pack replacement.
 *
 * The name passes through the game's alias canonicaliser, when one is set,
 * then sfx_pack_register_symbolic_adpcm registers the entry under the
 * calling mod's id; the substitution is audible only, and every byte the
 * guest observes stays stock. Logs the outcome.
 * @param self The calling mod.
 * @param name SoundTable slot name or a game alias of one.
 * @param body SPU-ADPCM body.
 * @param len Body length in bytes.
 * @param pitch SPU pitch that pins the playback rate, or 0 to follow the
 *        game's pitch writes.
 * @return 0, or -1 when an argument is NULL or zero or the pack refuses
 *         the entry. */
int openpete_mods_sfx_replace(openpete_mod_t* self, const char* name,
                              const void* body, uint32_t len,
                              uint16_t pitch);

/* Per-mod user configuration (mod_config.c): mods/<id>/config.toml.
 * Keys are dotted paths ("chests.collision"); getters return `def` for an
 * absent file/key/type-mismatch. */
/** @brief Load a mod's config.toml into its key table, replacing what the previous load stored.
 *
 * The loader calls it right before the mod's entry runs, at load and on hot
 * reload, and after a config write; in dev hot-reload mode it watches the
 * file's content hash, so an edit re-enters the mod with fresh values.
 * Nested tables flatten to dotted keys; bool, integer, float and string
 * values are stored, and arrays and other types are logged and skipped. A
 * missing file leaves the table empty; a parse error is logged and leaves
 * it empty, so every getter returns its default and the mod stays enabled.
 * @param mod the mod whose table is filled.
 * @param mod_dir the mod's directory.
 * @param log_tag the mod's log tag. */
void openpete_mods_config_load(openpete_mod_t* mod, const char* mod_dir,
                               const char* log_tag);
/** @brief Read a boolean config value.
 *
 * A bool or integer value reads as nonzero-is-true; a value of another
 * type logs a warning and yields the default.
 * @param self the mod.
 * @param key the dotted key path.
 * @param def the value for an absent key or a type mismatch.
 * @return the value, or @p def. */
int     openpete_mods_config_bool(openpete_mod_t* self, const char* key,
                                  int def);
/** @brief Read an integer config value.
 *
 * A bool reads as 0 or 1 and a float as its truncation; a string logs a
 * warning and yields the default.
 * @param self the mod.
 * @param key the dotted key path.
 * @param def the value for an absent key or a type mismatch.
 * @return the value, or @p def. */
int64_t openpete_mods_config_int(openpete_mod_t* self, const char* key,
                                 int64_t def);
/** @brief Read a float config value.
 *
 * A bool reads as 0.0 or 1.0 and an integer as its double conversion; a
 * string logs a warning and yields the default.
 * @param self the mod.
 * @param key the dotted key path.
 * @param def the value for an absent key or a type mismatch.
 * @return the value, or @p def. */
double  openpete_mods_config_float(openpete_mod_t* self, const char* key,
                                   double def);
/** @brief Read a string config value into a caller buffer.
 *
 * Copies the value, or @p def (empty when NULL) for an absent key or a
 * non-string value, truncated to fit and NUL-terminated. A non-string
 * value logs a warning. Values are stored at most 127 characters long.
 * @param self the mod.
 * @param key the dotted key path.
 * @param def the default text; may be NULL.
 * @param buf the destination; NULL returns 0 without writing.
 * @param cap the destination size in bytes; 0 or less returns 0 without
 *        writing.
 * @return 1 when the key held a string, else 0. */
int     openpete_mods_config_str(openpete_mod_t* self, const char* key,
                                 const char* def, char* buf, int cap);

/** @brief Apply edits to a mod's config.toml as text, keeping every comment (the Mods panel's Apply).
 *
 * Each edit rewrites the value of `leaf = ...` inside the section its
 * dotted key names, keeping indentation and a trailing comment; a missing
 * key is added to its section and a missing section is appended at the end
 * of the file, which is created when absent. All edits are applied in
 * memory, then written to a temporary file beside config.toml and renamed
 * over it, so any failure leaves the file unchanged.
 * @param mod_dir the mod's directory.
 * @param log_tag the mod's log tag.
 * @param n number of edits.
 * @param e the edits.
 * @return 0 when the file was rewritten, -1 otherwise. */
int openpete_mods_config_write(const char* mod_dir, const char* log_tag,
                               int n, const openpete_mods_cfg_edit_t* e);

/** @brief Register or replace a mod-owned RGBA8 texture under a 16-bit id.
 *
 * Model faces select the texture with the PSX-reserved Tiledef tpage mode
 * 3 and the id in the CLUT halfword; the native renderer fetches it with
 * openpete_mods_texture_get() and binds it through the HD pipeline. The
 * texture is host-side only: guest RAM and VRAM are untouched. The pixels
 * are copied. Re-registering an id replaces its image; the old image is
 * retired and never freed, so its address cannot key a new GPU texture
 * while the old one stays cached, and a resolve on another thread that
 * loaded it keeps valid pixels. Legal from any thread: registrations
 * serialize on a lock, and each image is published whole with a release
 * store, so the resolver never sees a half-written entry.
 * @param self Registering mod; its id becomes the owner.
 * @param id Texture id.
 * @param rgba @p w * @p h RGBA8 pixels.
 * @param w Width, 1 to 1024.
 * @param h Height, 1 to 1024.
 * @return 0, or -1 for a NULL @p rgba, an out-of-range size, or an
 *         allocation failure. */
int openpete_mods_texture_register(openpete_mod_t* self, uint16_t id,
                                   const void* rgba, int w, int h);
/** @brief Return the RGBA8 pixels of a registered mod texture.
 *
 * Lock-free linear scan by id, called from the one thread that renders:
 * each slot's image is acquire-loaded, so its pixels, width and height
 * belong to one registration. A texture whose owner is disabled is still
 * served, with one log line per toggle epoch; the first draw of each
 * registered image also logs once.
 *
 * @param id Texture id.
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL.
 * @return The pixels, which stay allocated for the session, or NULL when
 *         the id is not registered. */
const unsigned char* openpete_mods_texture_get(uint16_t id, int* w, int* h);
/** @brief Register every <hexid>.png under an asset root's textures/own/ as a mod-owned texture.
 *
 * Mod-owned texture content without C code: each PNG registers under the
 * 16-bit id its filename stem spells in hex, at most four digits
 * ("53c1.png"); another stem is skipped with a warning, and a PNG that
 * fails to decode or register is skipped with an error. Faceblocks select
 * the texture with the same PSX-reserved mode-3 Tiledef encoding as
 * openpete_mods_texture_register(). Generators derive the pixels from the
 * user's disc into this directory, so no game-derived data ships. Called by
 * openpete_mods_register_asset_root() when a root registers.
 * @param dir Asset root directory (a mod's assets/).
 * @param owner Owning mod id; stored by pointer, so it must live for the
 *              process (the loader's id).
 * @return Textures registered. */
int openpete_mods_texture_scan_root(const char* dir, const char* owner);

/* ── Engine services (core/mods/mod_services.c) ────────────────────── */

/** @brief Append a mod's assets directory to the asset-overlay root table.
 *
 * The loader registers one root per entered mod with an assets/ dir, in
 * enabled order, which is priority order: a later root wins. Consumers
 * (texture pack, music override) layer the roots over their base
 * directories. The table is append-only and never filtered by live-toggle
 * state, so indices stay stable; each consumer skips a root while
 * openpete_mods_is_enabled(owner) is false. Registration also scans the
 * root's textures/own/ with openpete_mods_texture_scan_root.
 * @param dir Root directory, copied.
 * @param owner Owning mod id, stored by pointer; it must live for the
 *        process (the loader's id).
 * @return 0, or -1 when @p dir is NULL or empty or the table allocation
 *         fails. */
int         openpete_mods_register_asset_root(const char* dir,
                                              const char* owner);
/** @brief Return the number of registered asset roots.
 *
 * The count only grows, so a consumer detects a late-registered root by
 * comparing it with the count it last scanned.
 * @return The root count. */
int         openpete_mods_asset_root_count(void);
/** @brief Return the directory of asset root @p i and its owner.
 *
 * Indices run in registration order, lowest priority first. The returned
 * path stays valid until the next root registers, which may move the
 * table, so a caller copies it before then.
 * @param i Root index.
 * @param owner Receives the owning mod id, a string that lives for the
 *        process; may be NULL.
 * @return The directory, or NULL when @p i is out of range. */
const char* openpete_mods_asset_root(int i, const char** owner);

/** @brief Return the compile-on-boot build key of a loaded mod.
 *
 * The key is the 64-character SHA-256 hex digest over the mod API
 * version, the compiler identity, the SDK inputs and the mod's sorted
 * sources. It is empty for a mod without a source build (assets-only, or a
 * prebuilt object loaded without the SDK). core/mods/mod_services.c uses a
 * non-NULL result to tell a loaded mod owner from a non-mod owner.
 * @param id Mod id; NULL returns NULL.
 * @return The key, or NULL when no loaded mod has that id. */
const char* openpete_mods_build_key(const char* id);
/** @brief Test whether every enabled code mod declares a manifest runahead posture.
 *
 * The posture is runahead = "pure" or "host-fx" in the [mod] table. A code
 * mod is one with a loaded object, so a binary-only mod is held to the
 * same declaration as a source one; a mod without an object runs no code
 * and passes. core/runtime/run.c does not arm runahead on 0.
 * @param offender Receives the first undeclared mod's id on 0 and NULL on
 *        1; may be NULL.
 * @return 1 when every enabled code mod declares a posture, else 0. */
int openpete_mods_runahead_capable(const char** offender);
/** @brief Test whether every enabled code mod declares a manifest host-state posture.
 *
 * The posture is state = "none" or "rebuildable" in the [mod] table. It
 * gates timeline jumps (rewind): an undeclared mod's host state can desync
 * when the guest world moves under it. A mod without a loaded object
 * holds no host state and passes.
 * @param offender Receives the first undeclared mod's id on 0 and NULL on
 *        1; may be NULL.
 * @return 1 when every enabled code mod declares a posture, else 0. */
int openpete_mods_state_safe(const char** offender);

/** @brief Size of the arena state section: a header, the ledger rows, and the three used ranges.
 *
 * core/record/savestate.c serializes the section with the engine state.
 * @return Section size in bytes. */
uint32_t openpete_mods_arena_state_size(void);
/** @brief Write the arena state section to a buffer.
 *
 * The layout is a version-4 header (version, row count, used bytes of
 * the upper segment, the lower segment and the general pool), one
 * fixed-width record per ledger row, superseded marks included, then the
 * three used ranges in that order. A row's offset is from 0x80200000 for
 * a DMA-reachable row and from 0x81000000 for an OP_GALLOC_NO_DMA row.
 * Host-only row state (the immutable digest) is not written.
 * @param dst a buffer of at least openpete_mods_arena_state_size() bytes. */
void     openpete_mods_arena_state_save(void* dst);
/** @brief Check a saved arena section against the live ledger before a state load commits.
 *
 * The saved rows must equal the live rows over their common prefix,
 * superseded marks included, or the restored bytes would sit under a
 * different layout; a mismatch is logged naming the mod and entry. A
 * version-3 section, written before the pools, reads as upper-segment
 * rows at their offsets plus 0x500000 and upper-segment bytes, so every
 * byte lands at the vaddr it was saved from.
 * @param src Saved section bytes.
 * @param size Section size in bytes.
 * @return 0 when the load may proceed (also when there is no section), -1
 *         to refuse it. */
int      openpete_mods_arena_state_verdict(const void* src, uint32_t size);
/** @brief Adopt a saved arena section after a state load commits.
 *
 * Saved rows beyond the live ledger are appended, so a mod's later call at
 * that index replays the saved row; each saved range is copied over its
 * segment and that cursor moves to max(live, saved). Version 3 and 4
 * sections are read.
 * @param src Saved section bytes.
 * @param size Section size in bytes. */
void     openpete_mods_arena_state_apply(const void* src, uint32_t size);

/** @brief Size of the registry state section, 0 when no custom level is registered.
 *
 * The section carries the custom-level registry's host state: the shared
 * slot's claim (slot id, owning row), the queued warp, and each row's
 * slot/replace mode and replace declaration. core/record/savestate.c
 * serializes it with the engine state, file and in-memory alike.
 * @return Section size in bytes. */
uint32_t openpete_mods_registry_state_size(void);
/** @brief Write the registry state section to a buffer.
 *
 * The layout is a version-1 header (version, row count, slot id, owning
 * row, queued warp row), then one 4-byte record per row in registration
 * order (mode, declaration, two pad bytes).
 * @param dst A buffer of at least openpete_mods_registry_state_size() bytes. */
void     openpete_mods_registry_state_save(void* dst);
/** @brief Adopt a saved registry state section after a state load commits.
 *
 * A section whose version or row count does not fit the registered rows
 * is logged and ignored; an empty section leaves the registry as it is.
 * A restored slot claim is handed to the level ops' adopt_slot, and a
 * claim the restore drops is released there.
 * @param src Saved section bytes, or NULL.
 * @param size Section size in bytes. */
void     openpete_mods_registry_state_apply(const void* src, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MODS_H */
