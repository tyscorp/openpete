// core/mods/mod_registry.c - namespaced content registries.
//
// Namespaces: LEVEL SLOTS (a new level id cloned from a donor level) and
// CUSTOM LEVELS (a mod-supplied WAD/OVL served over a slot), plus the
// per-level event hooks both ride.
//
// Inertness: the per-tick chain link is added lazily on the FIRST
// registration - nothing registered means no hook, no guest reads, no
// behavior change. All registry guest writes are gameplay-divergent by
// design (content mods), gated behind mods being loaded at all.

#include "openpete_mods.h"
#include "game_extras.h" /* game_get_id */
#include "openpete_bugrec.h" /* openpete_bugrec_warp, openpete_bugrec_hold */
#include "openpete_bugreport_replay.h" /* openpete_bugreport_replay_warp_take */
#include "openpete_fiber.h" /* openpete_speculative */
#include "openpete_knobs.h" /* openpete_knob_bind_vocab: the custom_level row */
#include "openpete_log.h"
#include "openpete_rewind.h" /* openpete_rewind_note_intervention */
#include "mod_vec.h"
#include "openpete_str.h"
#include "../host/wav_loader.h" /* op_wav_probe_seconds (custom-level music) */

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Chain plumbing (defined in mod_chain.c; deliberately not in the public
 * header - the registry is the one engine-internal chain client). */
int  openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                             const char* owner_id);
void openpete_mods_base(CPUState* cpu);

/* Bug-report warps.txt row ids: the custom-level warp (target: the row's
 * position in name order) and openpete_mods_level_enter (target: the
 * level id). */
#define CUSTOM_WARP_ID "custom_warp"
#define LEVEL_ENTER_ID "mod_level_enter"
/* warps.txt row id of the Mods menu's slot/replace toggle; its target is
 * the row's position in name order times 2, plus 1 for replace. */
#define CUSTOM_MODE_ID "custom_mode"

#define MOBY_NAME_MAX   48
#define MOBY_OWNER_MAX  64 /* mirrors the loader's MOD_ID_MAX */

/** @brief The game descriptor's moby ops, or NULL when the game serves none. */
static const openpete_moby_ops_t* moby_ops(void) {
    return openpete_mods_game_desc()->moby_ops;
}
static int s_hooked = 0;
static int s_level  = -1;

/* Level id of an in-flight transition INTO a registered slot (0 = none).
 * The transition window is invisible to s_level - the game ops report
 * level 0 for every non-GS_Playing tick - yet it is exactly when the
 * id-keyed loader functions run, so the busy query below needs it. */
static int s_slot_entering = 0;

/* Last level that was LIVE, regardless of the current gamestate. The
 * ops report level 0 for every non-GS_Playing tick - pause, demo
 * rollover, level teardown - which are exactly the windows where the
 * id-keyed loaders may still run against the old id, so residency only
 * moves when a DIFFERENT level goes live. The busy query keys on this,
 * never on the instantaneous playing-state view. */
static int s_resident_level = 0;

/* ── Level-ready event (api->on_level) ───────────────────────────────
 * Fires each hook once per level, on the first master-update tick with
 * gameplay live (level_id > 0): the model table is installed, the CD
 * stream is idle, and - because the anchor is the master update, not a
 * camera function - the event is immune to freecam freezes (the trap
 * that killed two generations of spyro3's per-level SFX re-staging).
 * Tick context: guest reads/writes and call_addr are legal inside. */
/* Hooks the table allocates up front; it grows past this. */
#define LEVEL_HOOKS_INIT 32

typedef struct {
    char mod_id[MOBY_OWNER_MAX];
    openpete_mod_level_fn fn;
    int  fired_level; /* last level this hook fired for (0 = never) */
    int  disabled;
} level_hook_t;

static mod_fvec s_level_hooks;   /* level_hook_t */

/** @brief Return the number of registered level-ready hooks. */
static int level_hook_count(void) {
    return (int)mod_fvec_count(&s_level_hooks);
}
/** @brief Return level-ready hook @p i, or NULL when @p i is out of range. */
static level_hook_t* level_hook_at(int i) {
    return (level_hook_t*)mod_fvec_at(&s_level_hooks, (size_t)i);
}

/* ── Loaded moby classes ────────────────────────────────────────────────
 * Which classes a level loaded is a fact about guest RAM, but its consumer
 * is a mod's UI section, which runs inside a present where guest access is
 * refused. So the engine walks it at the tick boundary and mods read this
 * copy - the same shape as openpete_mods_set_level. */
static uint16_t s_moby_classes[OPENPETE_MOBY_CLASSES_MAX];
static int      s_moby_class_count;

/* Documented at the declaration in openpete_mods.h. */
void openpete_mods_moby_class_census(CPUState* cpu) {
    if (!moby_ops() || !moby_ops()->classes_loaded) return;
    int n = moby_ops()->classes_loaded(cpu, s_moby_classes,
                                  OPENPETE_MOBY_CLASSES_MAX);
    /* -1 means mid-load: keep the last good answer rather than blanking the
     * set every time the player walks through a portal. */
    if (n >= 0) s_moby_class_count = n;
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_moby_classes(uint16_t* out, int cap) {
    if (!out || cap <= 0) return s_moby_class_count;
    int n = s_moby_class_count < cap ? s_moby_class_count : cap;
    for (int i = 0; i < n; i++) out[i] = s_moby_classes[i];
    return n;
}

static void registry_tick(CPUState* cpu);
static void level_slots_tick(CPUState* cpu);
static void custom_tick(CPUState* cpu);
static int  custom_warp_queued(void);
static void counts_tick(CPUState* cpu);
/** @brief Add the shared per-tick driver to the master-update chain once.
 *
 * Called by the first registration in any namespace; the anchor is the
 * moby ops' update_hook_addr (the game registers both ops structs at
 * boot). Later calls return at once.
 * @param log_tag Log tag for the error line.
 * @return 0 when the driver is hooked, -1 without moby ops or when the
 *         chain add fails. */
static int ensure_hooked(const char* log_tag) {
    if (s_hooked) return 0;
    if (!moby_ops()) {
        OPL_ERROR_F(log_tag, "content registry: %s serves no moby ops",
                    game_get_id());
        return -1;
    }
    if (openpete_mods_chain_add(moby_ops()->update_hook_addr, registry_tick,
                                "content-registry") != 0)
        return -1;
    s_hooked = 1;
    return 0;
}

/** @brief Per-tick registry driver, a link on the game's master-update chain.
 *
 * ensure_hooked adds it at the moby ops' update_hook_addr on the first
 * registration in any namespace; links added after it wrap it. It calls
 * openpete_mods_base first, so its work sees the state after the original
 * update and every deeper link. It then claims pending level slots,
 * reconciles custom-level declarations and warps, and reconciles target
 * totals. A live level (level_id > 0) clears s_slot_entering and moves
 * s_resident_level (on a committed tick it sets the bug-report anchor
 * hold of a queued custom warp and releases the level-entry hold), and each enabled level-ready hook whose fired_level differs from
 * it fires once with that level id.
 * @param cpu Live game CPU. */
static void registry_tick(CPUState* cpu) {
    openpete_mods_base(cpu);
    if (!moby_ops()) return;

    level_slots_tick(cpu);
    custom_tick(cpu);
    /* After custom_tick, not inside it: the warp path returns early on
     * several branches, and a claim made this tick must reach the tables
     * this tick - the all-gems test reads them before the next one. */
    counts_tick(cpu);

    int lvl = moby_ops()->level_id(cpu);
    /* Any live level ends an in-flight slot transition: either we arrived
     * (lvl == the slot id) or the load went somewhere else. */
    if (lvl > 0) { s_slot_entering = 0; s_resident_level = lvl; }
    if (!openpete_speculative) {
        /* A queued custom warp, and an accepted entry until a level is
         * live (the title route arms the game's level skip), are host
         * state outside every savestate: no bug-report anchor is taken
         * while either is held. */
        openpete_bugrec_hold(OPENPETE_BUGREC_HOLD_CUSTOM_WARP, custom_warp_queued());
        if (lvl > 0) openpete_bugrec_hold(OPENPETE_BUGREC_HOLD_LEVEL_ENTRY, 0);
    }
    if (lvl != s_level) s_level = lvl;
    if (lvl <= 0) return;

    for (int i = 0; i < level_hook_count(); i++) {
        level_hook_t* h = level_hook_at(i);
        openpete_mod_level_fn fn;
        if (h->disabled || h->fired_level == lvl) continue;
        h->fired_level = lvl;
        /* Read out before the call: a hook body may register another
         * level hook, which moves the table. The owner is copied for the
         * guest-byte guard, which names it after the call. */
        fn = h->fn;
        char owner[MOBY_OWNER_MAX];
        memcpy(owner, h->mod_id, sizeof owner);
        openpete_mods_guard_enter(owner);
        fn(cpu, lvl);
        openpete_mods_guard_leave();
    }
}

/* ── Level-slot namespace ──────────────────────────────────────────── */

/* Slots the registry starts with. The game ops expose one free slot
 * today, so a stock set never grows this. */
#define LEVEL_SLOTS_INIT 4

typedef struct {
    char mod_id[MOBY_OWNER_MAX];
    char name[MOBY_NAME_MAX];
    int  donor_level_id;
    int  slot_id;  /* 0 = not claimed yet */
    int  disabled; /* live toggle */
} level_slot_t;

static mod_fvec s_levels;   /* level_slot_t */

/** @brief Return the number of registered level-slot rows. */
static int level_slot_count(void) {
    return (int)mod_fvec_count(&s_levels);
}
/** @brief Return level-slot row @p i, or NULL when @p i is out of range. */
static level_slot_t* level_slot_at(int i) {
    return (level_slot_t*)mod_fvec_at(&s_levels, (size_t)i);
}
/** @brief The game descriptor's level ops, or NULL when the game serves none. */
static const openpete_level_ops_t* level_ops(void) {
    return openpete_mods_game_desc()->level_ops;
}

/** @brief Claim a level slot for each enabled level-slot row that has none.
 *
 * Calls the level ops' claim_slot with the row's donor level every tick
 * until it returns a nonzero slot id, then logs the slot live. Does
 * nothing while no level ops are registered.
 * @param cpu Live game CPU. */
static void level_slots_tick(CPUState* cpu) {
    if (!level_ops()) return;
    for (int i = 0; i < level_slot_count(); i++) {
        level_slot_t* s = level_slot_at(i);
        if (s->disabled || s->slot_id) continue;
        s->slot_id = level_ops()->claim_slot(cpu, s->donor_level_id);
        if (s->slot_id)
            OPL_INFO_F("mods", "level slot %s:%s live (id=%d, donor %d)",
                       s->mod_id, s->name, s->slot_id, s->donor_level_id);
    }
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_registry_ensure_driver(const char* log_tag) {
    return ensure_hooked(log_tag);
}

int openpete_mods_on_level(const char* mod_id, openpete_mod_level_fn fn,
                           const char* log_tag) {
    if (!mod_id || !fn) return -1;
    if (ensure_hooked(log_tag) != 0) return -1;
    if (!s_level_hooks.elem_size)
        mod_fvec_init(&s_level_hooks, sizeof(level_hook_t),
                      LEVEL_HOOKS_INIT, "level_hooks", LEVEL_HOOKS_INIT);
    level_hook_t* h = (level_hook_t*)mod_fvec_push(&s_level_hooks);
    if (!h) {
        OPL_ERROR_F(log_tag, "on_level: hook table allocation failed");
        return -1;
    }
    snprintf(h->mod_id, sizeof h->mod_id, "%s", mod_id);
    h->fn = fn;
    h->fired_level = 0; /* fires on the next live tick - including for a
                         * level already in progress (hot reload) */
    OPL_INFO_F(log_tag, "level-ready hook registered");
    return 0;
}

/* Documented at the declaration in openpete_mods.h. */
void openpete_mods_registry_remove_owner(const char* mod_id) {
    if (!mod_id) return;
    int w = 0;
    for (int i = 0; i < level_slot_count(); i++) {
        if (strcmp(level_slot_at(i)->mod_id, mod_id) == 0) continue;
        *level_slot_at(w++) = *level_slot_at(i);
    }
    mod_fvec_truncate(&s_levels, (size_t)w);
    w = 0;
    for (int i = 0; i < level_hook_count(); i++) {
        level_hook_t* h = level_hook_at(i);
        if (strcmp(h->mod_id, mod_id) == 0) continue;
        *level_hook_at(w++) = *h;
    }
    mod_fvec_truncate(&s_level_hooks, (size_t)w);
}

/* Documented at the declaration in openpete_mods.h. */
void openpete_mods_registry_set_owner_enabled(const char* mod_id, int on) {
    if (!mod_id) return;
    for (int i = 0; i < level_slot_count(); i++)
        if (strcmp(level_slot_at(i)->mod_id, mod_id) == 0)
            level_slot_at(i)->disabled = !on;
    for (int i = 0; i < level_hook_count(); i++)
        if (strcmp(level_hook_at(i)->mod_id, mod_id) == 0) {
            level_hook_at(i)->disabled = !on;
            /* Enable = (re)apply: clear the once-per-level latch so the
             * hook fires for the level IN PROGRESS on the next live tick.
             * Level hooks are once-per-EVERY-level by contract, so they
             * are already written idempotent; a re-enabled mod re-runs
             * its per-level work instead of staying silently inert until
             * the next level change. */
            if (on) level_hook_at(i)->fired_level = 0;
        }
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_level_register(const char* mod_id, const char* name,
                                 int donor_level_id, const char* log_tag) {
    if (!mod_id || !name || !name[0]) return -1;
    if (!level_ops()) {
        OPL_ERROR_F(log_tag, "level registry: no game ops registered");
        return -1;
    }
    for (int i = 0; i < level_slot_count(); i++) {
        if (strcmp(level_slot_at(i)->mod_id, mod_id) == 0 &&
            strcmp(level_slot_at(i)->name, name) == 0) {
            OPL_ERROR_F(log_tag, "level slot \"%s\" already registered",
                        name);
            return -1;
        }
    }
    if (ensure_hooked(log_tag) != 0) return -1;
    if (!s_levels.elem_size)
        mod_fvec_init(&s_levels, sizeof(level_slot_t), LEVEL_SLOTS_INIT,
                      "level_slots", LEVEL_SLOTS_INIT);
    level_slot_t* s = (level_slot_t*)mod_fvec_push(&s_levels);
    if (!s) {
        OPL_ERROR_F(log_tag, "level slot allocation failed - \"%s\" "
                    "skipped", name);
        return -1;
    }
    snprintf(s->mod_id, sizeof s->mod_id, "%s", mod_id);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->donor_level_id = donor_level_id;
    s->slot_id = 0;
    OPL_INFO_F(log_tag, "level slot \"%s\" registered (donor level %d)",
               name, donor_level_id);
    return 0;
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_level_slot_id(const char* mod_id, const char* name) {
    for (int i = 0; i < level_slot_count(); i++)
        if (strcmp(level_slot_at(i)->mod_id, mod_id) == 0 &&
            strcmp(level_slot_at(i)->name, name) == 0)
            return level_slot_at(i)->slot_id ? level_slot_at(i)->slot_id : -1;
    return -1;
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_level_enter(CPUState* cpu, int level_id) {
    if (!level_ops() || !cpu || level_id <= 0) return -1;
    /* Entering a slot commits the session to a level its owner defines.
     * With the owner disabled the id still loads - the donor alias that
     * services the id-keyed load functions is engine-owned, not mod-owned
     * - but it arrives stripped of the mod's content and of any load-path
     * hooks the mod installs, i.e. as a bare clone of the donor. Refuse
     * the warp instead of shipping the player a half-level. Retail ids
     * match no slot row and pass straight through. */
    for (int i = 0; i < level_slot_count(); i++) {
        if (level_slot_at(i)->slot_id != level_id) continue;
        if (level_slot_at(i)->disabled) {
            OPL_ERROR_F("mods", "level %d belongs to disabled mod \"%s\" - "
                        "entry refused", level_id, level_slot_at(i)->mod_id);
            return -1;
        }
        break;
    }
    int rc = level_ops()->enter_level(cpu, level_id);
    if (rc == 0 && !openpete_speculative) {
        /* The caller is mod code on the tick, which a replay runs again:
         * the bundle records the entry and the replay matches its own
         * call against it, so an entry the replay does not repeat on the
         * recorded tick is reported. */
        int recorded = 0;
        if (openpete_bugreport_replay_warp_take(LEVEL_ENTER_ID, &recorded) && recorded != level_id)
            OPL_WARN_F("mods", "level entry to %d where the bundle recorded %d", level_id, recorded);
        openpete_bugrec_warp(LEVEL_ENTER_ID, level_id);
        openpete_bugrec_hold(OPENPETE_BUGREC_HOLD_LEVEL_ENTRY, 1);
    }
    if (rc == 0) {
        for (int i = 0; i < level_slot_count(); i++)
            if (level_slot_at(i)->slot_id == level_id) {
                s_slot_entering = level_id;
                break;
            }
    }
    return rc;
}


/* ── Declarative custom levels ([[level]] manifest blocks) ───────────
 * Hand-authored level mods with no C: the loader registers each block
 * here, and this driver owns everything the guest needs - the shared
 * slot (claim/retarget to the row's donor), the guest TOC length
 * declaration for the row's data file, replace-mode declarations on a
 * donor's own entry, and warps. The WAD overlay asks
 * openpete_mods_custom_files_for() which files serve a level id.
 * v1 capacity: rows TIME-SHARE the one free slot (id 65); per-level
 * progress on the shared index is common to all of them, since the guest
 * tables have no second free slot. */
/* Rows the custom-level table allocates up front; it grows past this. */
#define CUSTOM_INIT 64

typedef struct {
    char mod_id[MOBY_OWNER_MAX];
    char name[64];
    char stem[64];
    char dir[900];         /* owning mod's assets dir */
    int  donor_id;
    int  replace_mode;     /* 1 = serve over the donor's own entry */
    uint32_t data_size;    /* stat at registration; 0 = files missing */
    int  declared_replace; /* donor entry currently carries our length */
    char music[1088];      /* custom song WAV, "" = none */
    double music_sec;      /* its duration; 0 = file missing/bad */
    char title[40];        /* in-game title override; "" = use name */
    openpete_level_counts_t counts; /* declared target totals; a field is
                                     * -1 when the manifest declared none,
                                     * leaving that table on its stock row */
    uint8_t counts_clash_logged;    /* lost a donor to a lower-ordinal row;
                                     * latched so the clash logs once */
} custom_level_t;

/* The target-egg table covers the first three worlds only, so a level
 * index at or above this has no egg row. */
#define EGG_TABLE_ROWS 18

static mod_fvec s_customs;   /* custom_level_t */

/** @brief Return the number of registered custom-level rows. */
static int custom_count(void) { return (int)mod_fvec_count(&s_customs); }
/** @brief Return custom-level row @p i by registration index, or NULL when out of range. */
static custom_level_t* custom_at(int i) {
    return (custom_level_t*)mod_fvec_at(&s_customs, (size_t)i);
}
/* Row indices, alphabetical by name. Parallel to s_customs, so it
 * converts with it. */
static mod_fvec s_custom_order;   /* int */

/** @brief Return element @p i of a vector of int, or 0 when @p i is out of range. */
static int id_at(const mod_fvec* v, int i) {
    const int* p = (const int*)mod_fvec_at(v, (size_t)i);
    return p ? *p : 0;
}

/** @brief Return the registration index of the row at alphabetical position @p i.
 *
 * Reads s_custom_order. An out-of-range @p i returns 0, which is also a
 * valid row index. */
static int order_row(int i) {
    const int* p = (const int*)mod_fvec_at(&s_custom_order, (size_t)i);
    return p ? *p : 0;
}
static int s_custom_active = -1;       /* row owning the shared slot */
static int s_custom_slot_id = 0;       /* claimed slot id (65), 0 = none */
static int s_custom_warp = -1;         /* queued warp (real row index) */
/* A warp request from the host (the Mods menu's Warp button,
 * OPENPETE_CUSTOM_WARP): the row's position in name order, -1 = none.
 * custom_tick takes it into s_custom_warp on a committed tick. */
static _Atomic int s_custom_req = -1;
/* A slot/replace toggle from the host (the Mods menu, OPENPETE_CUSTOM_MODE):
 * position * 2 + replace, -1 = none. custom_tick takes it on a committed
 * tick, so a toggle is a timeline event: recorded, replayed, rewound. */
static _Atomic int s_custom_mode_req = -1;

/* Literal lowercase path first, then a case-insensitive directory scan. */
int openpete_mods_custom_asset_path(const char* dir, const char* stem,
                                    int is_ovl, char* out, size_t cap) {
    const char* ext = is_ovl ? "ovl" : "wad";
    if (!dir || !stem || !out || cap == 0) return -1;
    snprintf(out, cap, "%s/levels/%s.%s", dir, stem, ext);
    struct stat st;
    if (stat(out, &st) == 0) return 0;

    char want[160];
    snprintf(want, sizeof want, "%s.%s", stem, ext);
    char ldir[1024];
    snprintf(ldir, sizeof ldir, "%s/levels", dir);
    DIR* d = opendir(ldir);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (op_strcasecmp(e->d_name, want) != 0) continue;
            snprintf(out, cap, "%s/%s", ldir, e->d_name);
            closedir(d);
            return 0;
        }
        closedir(d);
    }
    /* Canonical (missing) path stays in `out` for the caller's log. */
    snprintf(out, cap, "%s/levels/%s.%s", dir, stem, ext);
    return -1;
}

/** @brief Copy a custom-level row's declared target totals, dropping an egg total with no egg-table row.
 *
 * Every total starts at -1 (undeclared); a NULL `counts` leaves them so.
 * An egg total needs a row in the 18-entry egg table: a replace row takes
 * its donor's level index, (donor_id / 10 - 1) * 6 + donor_id % 10, and a
 * slot row lands on the shared free slot, past the table. An egg total
 * with no row is reset to -1 with a warning; the row still registers, and
 * its gem and dragon totals still apply.
 * @param r The row being registered.
 * @param replace 1 for a replace-mode row.
 * @param counts Declared totals, or NULL.
 * @param name Row name for log lines.
 * @param log_tag Log tag. */
static void custom_apply_counts(custom_level_t* r, int replace,
                                const openpete_level_counts_t* counts,
                                const char* name, const char* log_tag) {
    r->counts.dragons = r->counts.gems = r->counts.eggs = -1;
    if (!counts) return;
    r->counts = *counts;
    if (r->counts.eggs < 0) return;
    int idx = replace ? (r->donor_id / 10 - 1) * 6 + r->donor_id % 10 : -1;
    if (idx >= 0 && idx < EGG_TABLE_ROWS) return;
    if (idx < 0)
        OPL_WARN_F(log_tag, "level \"%s\": eggs = %d dropped - a "
                   "slot-mode level has no row in the %d-entry egg table",
                   name, r->counts.eggs, EGG_TABLE_ROWS);
    else
        OPL_WARN_F(log_tag, "level \"%s\": eggs = %d dropped - donor "
                   "index %d is outside the %d-entry egg table",
                   name, r->counts.eggs, idx, EGG_TABLE_ROWS);
    r->counts.eggs = -1;
}

/** @brief Fill a custom-level row's level data size, display title and music track.
 *
 * data_size is the size of `<dir>/levels/<stem>.WAD` found by
 * openpete_mods_custom_asset_path, 0 with a warning when it is missing.
 * title is copied from `title`, empty when NULL or empty. A non-empty
 * `music` names `<dir>/music/<music>`; its duration comes from
 * op_wav_probe_seconds, and a file that probes to 0 seconds or less
 * clears the track with a warning, which leaves the donor's track playing.
 * @param r The row being registered; dir, stem and name are already set.
 * @param music WAV file name under the mod's music/, or NULL.
 * @param title In-game title override, or NULL.
 * @param log_tag Log tag. */
static void custom_probe_assets(custom_level_t* r, const char* music,
                                const char* title, const char* log_tag) {
    char p[1088];
    struct stat st;
    r->data_size = openpete_mods_custom_asset_path(r->dir, r->stem, 0, p,
                                                   sizeof p) == 0 &&
                           stat(p, &st) == 0
                       ? (uint32_t)st.st_size
                       : 0;
    if (!r->data_size)
        OPL_WARN_F(log_tag, "level \"%s\": %s missing - listed but not "
                   "playable until derived", r->name, p);
    snprintf(r->title, sizeof r->title, "%s",
             title && title[0] ? title : "");
    r->music[0] = '\0';
    r->music_sec = 0.0;
    if (!music || !music[0]) return;
    snprintf(r->music, sizeof r->music, "%s/music/%s", r->dir, music);
    r->music_sec = op_wav_probe_seconds(r->music);
    if (r->music_sec > 0.0) return;
    OPL_WARN_F(log_tag, "level \"%s\": music %s missing/unreadable - "
               "donor's track plays until derived", r->name, r->music);
    r->music[0] = '\0';
}

/** @brief Insert a row index into s_custom_order at its case-insensitive alphabetical place by name.
 *
 * The menu's Custom World buckets read the rows in this order. The order
 * vec already holds `row + 1` entries; the new row goes before the first
 * existing row whose name compares greater, else last.
 * @param row Index of the new row in s_customs.
 * @param name The new row's name. */
static void custom_order_insert(int row, const char* name) {
    int at = row;
    for (int i = 0; i < row; i++)
        if (op_strcasecmp(custom_at(order_row(i))->name, name) > 0) {
            at = i;
            break;
        }
    for (int i = row; i > at; i--)
        *(int*)mod_fvec_at(&s_custom_order, (size_t)i) = order_row(i - 1);
    *(int*)mod_fvec_at(&s_custom_order, (size_t)at) = row;
}

/* The custom_level row's values, `<mod id>/<level name>` in name order,
 * and their count. Rebuilt at each registration; registration runs at boot,
 * before the dev overlay or the bug-report sampler reads the row, and the
 * atomics publish the array to those threads. */
static _Atomic(char**) s_custom_tokens;
static atomic_int      s_custom_token_n;

/** @brief Rebuild the custom_level row's values from the name order and bind them to the row.
 *
 * On an allocation failure the row keeps its previous values. */
static void custom_tokens_bind(void) {
    const int n = custom_count();
    char** t = (char**)calloc((size_t)n, sizeof *t);
    if (!t) return;
    for (int i = 0; i < n; i++) {
        const custom_level_t* r = custom_at(order_row(i));
        const size_t len = strlen(r->mod_id) + 1 + strlen(r->name) + 1;
        t[i] = (char*)malloc(len);
        if (!t[i]) {
            for (int j = 0; j < i; j++) free(t[j]);
            free(t);
            return;
        }
        snprintf(t[i], len, "%s/%s", r->mod_id, r->name);
    }
    (void)openpete_knob_bind_vocab("custom_level", (const char* const*)t, n);
    char** old = atomic_exchange(&s_custom_tokens, t);
    const int old_n = atomic_exchange(&s_custom_token_n, n);
    for (int i = 0; old && i < old_n; i++) free(old[i]);
    free(old);
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_custom_register(const char* mod_id, const char* name,
                                  const char* stem, const char* dir,
                                  const char* donor, int replace,
                                  const char* music, const char* title,
                                  const openpete_level_counts_t* counts,
                                  const char* log_tag) {
    if (!mod_id || !name || !name[0] || !stem || !stem[0] || !dir)
        return -1;
    int donor_id = 0;
    if (level_ops() && level_ops()->resolve_name)
        donor_id = level_ops()->resolve_name(donor ? donor : "");
    if (donor_id < 10 || donor_id > 64) {
        OPL_ERROR_F(log_tag, "level \"%s\": donor \"%s\" is not a retail "
                    "level (lower_snake name or id) - skipped", name,
                    donor ? donor : "");
        return -1;
    }
    if (!s_customs.elem_size) {
        mod_fvec_init(&s_customs, sizeof(custom_level_t), CUSTOM_INIT,
                      "custom_levels", CUSTOM_INIT);
        mod_fvec_init(&s_custom_order, sizeof(int), CUSTOM_INIT, NULL, 0);
    }
    custom_level_t* r = (custom_level_t*)mod_fvec_push(&s_customs);
    if (!r) {
        OPL_ERROR_F(log_tag, "custom-level table allocation failed - "
                    "\"%s\" skipped", name);
        return -1;
    }
    int row = custom_count() - 1;
    snprintf(r->mod_id, sizeof r->mod_id, "%s", mod_id);
    snprintf(r->name, sizeof r->name, "%s", name);
    snprintf(r->stem, sizeof r->stem, "%s", stem);
    snprintf(r->dir, sizeof r->dir, "%s", dir);
    r->donor_id = donor_id;
    r->replace_mode = replace ? 1 : 0;
    r->declared_replace = 0;
    custom_apply_counts(r, replace, counts, name, log_tag);
    custom_probe_assets(r, music, title, log_tag);
    /* The per-tick driver does all the work - make sure it's armed. */
    (void)ensure_hooked(log_tag);
    if (!mod_fvec_push(&s_custom_order)) {
        OPL_ERROR_F(log_tag, "custom-level order allocation failed - "
                    "\"%s\" skipped", name);
        mod_fvec_truncate(&s_customs, (size_t)row);
        return -1;
    }
    custom_order_insert(row, name);
    custom_tokens_bind();
    OPL_INFO_F(log_tag, "custom level \"%s\" registered (donor %d%s)",
               name, donor_id, replace ? ", replace" : "");
    return 0;
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_custom_count(void) { return custom_count(); }

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_custom_info(int i, openpete_mods_custom_info_t* out) {
    if (i < 0 || i >= custom_count() || !out) return 0;
    const custom_level_t* r = custom_at(order_row(i));
    out->name = r->name;
    out->owner = r->mod_id;
    out->donor_id = r->donor_id;
    const int pending = atomic_load(&s_custom_mode_req);
    out->replace_mode = pending >= 0 && pending / 2 == i ? pending & 1 : r->replace_mode;
    out->active = s_custom_active == order_row(i);
    out->level_id = r->replace_mode ? r->donor_id
                  : (out->active ? s_custom_slot_id : 0);
    out->files_ok = r->data_size != 0;
    return 1;
}

int openpete_mods_custom_warp(int i) {
    if (i < 0 || i >= custom_count()) return -1;
    atomic_store(&s_custom_req, i);
    return 0;
}

void openpete_mods_custom_set_mode(int i, int replace) {
    if (i < 0 || i >= custom_count()) return;
    atomic_store(&s_custom_mode_req, i * 2 + (replace ? 1 : 0));
}

int openpete_mods_custom_files_for(int level_id, const char** dir_out,
                                   const char** stem_out) {
    if (s_custom_slot_id && level_id == s_custom_slot_id &&
        s_custom_active >= 0) {
        const custom_level_t* r = custom_at(s_custom_active);
        if (openpete_mods_is_enabled(r->mod_id)) {
            *dir_out = r->dir;
            *stem_out = r->stem;
            return 1;
        }
    }
    for (int i = 0; i < custom_count(); i++) {
        const custom_level_t* r = custom_at(i);
        if (r->replace_mode && r->declared_replace &&
            r->donor_id == level_id &&
            openpete_mods_is_enabled(r->mod_id)) {
            *dir_out = r->dir;
            *stem_out = r->stem;
            return 1;
        }
    }
    return 0;
}

const char* openpete_mods_custom_music_for(int level_id,
                                           double* dur_sec_out) {
    const custom_level_t* r = NULL;
    if (s_custom_slot_id && level_id == s_custom_slot_id &&
        s_custom_active >= 0)
        r = custom_at(s_custom_active);
    else
        for (int i = 0; i < custom_count() && !r; i++)
            if (custom_at(i)->replace_mode && custom_at(i)->declared_replace &&
                custom_at(i)->donor_id == level_id)
                r = custom_at(i);
    if (!r || !r->music[0] || !openpete_mods_is_enabled(r->mod_id))
        return NULL;
    if (dur_sec_out) *dur_sec_out = r->music_sec;
    return r->music;
}

/* Ticks one harness variable can schedule. */
#define CUSTOM_ENV_TICKS 4

/* A harness request read from an environment variable
 * `<tick>[,<tick>...]:<rest>`. */
typedef struct {
    int      parsed;
    int      n;
    unsigned at[CUSTOM_ENV_TICKS];
    char     rest[80];
} custom_env_req_t;

/** @brief Parse a `<tick>[,<tick>...]:<rest>` harness variable once and report whether it is due at this tick.
 *
 * A value without a colon schedules nothing; ticks past CUSTOM_ENV_TICKS
 * are ignored.
 * @param q The request's parse cache.
 * @param var The variable's name.
 * @param tick_no 1-based count of custom_tick calls.
 * @return The text after the colon when due, else NULL. */
static const char* custom_env_due(custom_env_req_t* q, const char* var, unsigned tick_no) {
    if (!q->parsed) {
        q->parsed = 1;
        const char* e = getenv(var);
        const char* c = e ? strchr(e, ':') : NULL;
        const char* t = e;
        while (c && q->n < CUSTOM_ENV_TICKS) {
            q->at[q->n++] = (unsigned)strtoul(t, NULL, 10);
            const char* comma = (const char*)memchr(t, ',', (size_t)(c - t));
            if (!comma) break;
            t = comma + 1;
        }
        if (c) snprintf(q->rest, sizeof q->rest, "%s", c + 1);
    }
    for (int i = 0; q->rest[0] && i < q->n; i++)
        if (q->at[i] == tick_no) return q->rest;
    return NULL;
}

/** @brief Return the name-order position of the last row whose name matches exactly, or -1.
 * @param name The row name. */
static int custom_pos_named(const char* name) {
    int pos = -1;
    for (int i = 0; i < custom_count(); i++)
        if (strcmp(custom_at(order_row(i))->name, name) == 0) pos = i;
    return pos;
}

/** @brief Queue the custom-level warp and slot/replace toggle the harness variables schedule.
 *
 * OPENPETE_CUSTOM_WARP reads `<ticks>:<Level Name>` and queues the request
 * the Mods menu's Warp button queues; OPENPETE_CUSTOM_MODE reads
 * `<ticks>:slot:<Level Name>` or `<ticks>:replace:<Level Name>` and queues
 * the toggle its mode checkbox queues. `<ticks>` is one tick or up to
 * CUSTOM_ENV_TICKS separated by commas. Each is parsed once and fires when
 * `tick_no` equals one of its ticks, on the last row whose name matches
 * exactly.
 * @param tick_no 1-based count of custom_tick calls. */
static void custom_env_schedule(unsigned tick_no) {
    static custom_env_req_t s_warp, s_mode;
    const char* w = custom_env_due(&s_warp, "OPENPETE_CUSTOM_WARP", tick_no);
    const int wpos = w ? custom_pos_named(w) : -1;
    if (wpos >= 0) {
        atomic_store(&s_custom_req, wpos);
        OPL_INFO_F("mods", "custom warp scheduled: \"%s\"", w);
    }
    const char* m = custom_env_due(&s_mode, "OPENPETE_CUSTOM_MODE", tick_no);
    const char* c = m ? strchr(m, ':') : NULL;
    const int mpos = c ? custom_pos_named(c + 1) : -1;
    if (mpos >= 0) {
        const int replace = strncmp(m, "replace:", 8) == 0;
        openpete_mods_custom_set_mode(mpos, replace);
        OPL_INFO_F("mods", "custom mode scheduled: \"%s\" %s", c + 1, replace ? "replace" : "slot");
    }
}

/** @brief Report whether a custom-level warp is queued and not yet accepted by the level ops.
 * @return Nonzero while one is queued. */
static int custom_warp_queued(void) { return s_custom_warp >= 0; }

/** @brief Take the custom-level warp request of this tick into s_custom_warp and record it for the bug-report bundle.
 *
 * Under --bug-report-replay the bundle's request recorded at this tick is
 * taken first, else the host's (openpete_mods_custom_warp). The request is
 * the warp's only host input: the drive that follows reads guest state and
 * registry rows, so a replay that takes the same row on the same tick
 * reproduces the warp. Runs on committed ticks only. */
static void custom_take_request(void) {
    int pos = -1;
    if (!openpete_bugreport_replay_warp_take(CUSTOM_WARP_ID, &pos))
        pos = atomic_exchange(&s_custom_req, -1);
    if (pos < 0 || pos >= custom_count()) return;
    s_custom_warp = order_row(pos);
    openpete_bugrec_warp(CUSTOM_WARP_ID, pos);
}

/** @brief Take this tick's slot/replace toggle, apply it to its row and record it for the bug-report bundle.
 *
 * Under --bug-report-replay the bundle's toggle recorded at this tick is
 * taken first, else the host's (openpete_mods_custom_set_mode). The
 * declare loop in custom_tick acts on the new mode in the same tick.
 * Does nothing on a speculative tick, so a toggle is taken on a committed
 * one only. */
static void custom_take_mode(void) {
    if (openpete_speculative) return;
    int v = -1;
    if (!openpete_bugreport_replay_warp_take(CUSTOM_MODE_ID, &v))
        v = atomic_exchange(&s_custom_mode_req, -1);
    if (v < 0 || v / 2 >= custom_count()) return;
    custom_at(order_row(v / 2))->replace_mode = v & 1;
    openpete_bugrec_warp(CUSTOM_MODE_ID, v);
}

/** @brief Test whether a custom-level row's data file was found and its mod is enabled.
 *
 * Mode-independent, unlike custom_tick's `want`, which also requires a
 * replace row.
 * @return Nonzero when live. */
static int custom_row_live(const custom_level_t* r) {
    return r->data_size && openpete_mods_is_enabled(r->mod_id);
}

/* Bytes the count reconcile has written this session - the engagement
 * counter a gate needs to tell a passing run from one that never ran. */
static unsigned long s_counts_writes = 0;

/** @brief Return the custom-level row that owns a level's target totals this tick, or -1.
 *
 * For the shared slot id the owner is the active row, when it is a live
 * slot-mode row. Every slot-mode row resolves to the SAME shared-slot index, so the
 * owner there is the row that actually holds the slot, not every row
 * that could hold it; without that distinction two enabled slot rows
 * would both claim the index and loop order would decide what the
 * player sees.
 *
 * Two live replace rows on one donor is a conflict between manifests.
 * The lowest-ordinal row wins, so the choice is stable across ticks
 * rather than dependent on iteration order, and the clash is logged once
 * per losing row instead of every tick. */
static int counts_declarer_for(int level_id) {
    if (s_custom_slot_id && level_id == s_custom_slot_id) {
        if (s_custom_active < 0) return -1;
        const custom_level_t* a = custom_at(s_custom_active);
        if (a->replace_mode || !custom_row_live(a)) return -1;
        return s_custom_active;
    }
    int found = -1;
    for (int i = 0; i < custom_count(); i++) {
        custom_level_t* r = custom_at(i);
        if (!r->replace_mode || r->donor_id != level_id) continue;
        if (!custom_row_live(r)) continue;
        if (found < 0) { found = i; continue; }
        if (!r->counts_clash_logged) {
            r->counts_clash_logged = 1;
            OPL_WARN_F("mods", "custom level \"%s\" and \"%s\" both "
                       "replace level %d - \"%s\" declares its target "
                       "totals", custom_at(found)->name, r->name,
                       level_id, custom_at(found)->name);
        }
    }
    return found;
}

/** @brief Collect the level ids whose target totals counts_tick reconciles.
 *
 * Clears `ids`, then pushes the claimed shared slot id (when one is
 * claimed) and each distinct donor level id of the registered rows.
 * @param ids Vector of int, reused across ticks. */
static void counts_collect_ids(mod_fvec* ids) {
    mod_fvec_clear(ids);
    if (s_custom_slot_id) {
        int* slot = (int*)mod_fvec_push(ids);
        if (slot) *slot = s_custom_slot_id;
    }
    for (int i = 0; i < custom_count(); i++) {
        int id = custom_at(i)->donor_id;
        int dup = 0;
        for (int j = 0; j < (int)mod_fvec_count(ids); j++)
            if (id_at(ids, j) == id) { dup = 1; break; }
        if (dup) continue;
        int* slot = (int*)mod_fvec_push(ids);
        if (slot) *slot = id;
    }
}

/** @brief Reconcile one level's target totals with the row that declares them.
 *
 * counts_declarer_for picks the row. Its declared totals go to the level
 * ops' set_level_counts; with no declarer, or for a total the row left
 * undeclared, -1 is passed, which restores the table's boot value. Written
 * bytes are added to s_counts_writes and logged; the op writes nothing
 * once the tables agree, so a line appears only when ownership changes.
 * @param cpu Live game CPU.
 * @param id Level id. */
static void counts_apply_one(CPUState* cpu, int id) {
    int d = counts_declarer_for(id);
    const openpete_level_counts_t* c = d >= 0 ? &custom_at(d)->counts : NULL;
    int wrote = level_ops()->set_level_counts(
        cpu, id, c ? c->dragons : -1, c ? c->gems : -1, c ? c->eggs : -1);
    if (wrote <= 0) return;
    s_counts_writes += (unsigned long)wrote;
    /* Only an ownership change reaches this: the op writes nothing once
     * the tables hold what the rule asks for. A line per tick would mean
     * that idempotence had broken. */
    OPL_INFO_F("mods", "level %d target totals: %s (dragons=%d gems=%d "
               "eggs=%d, %d B, %lu total)",
               id, c ? custom_at(d)->name : "stock",
               c ? c->dragons : -1, c ? c->gems : -1, c ? c->eggs : -1,
               wrote, s_counts_writes);
}

/** @brief Reconcile the target totals of every level id a registered row could own.
 *
 * Idempotent: set_level_counts writes only bytes that differ, so this
 * costs three guest reads per level once the tables agree. Reconciling
 * every tick, rather than writing on the edges where rows go live and
 * stop being live, covers the cases with no such edge: the shared slot
 * has no undeclare edge, a mode flip from the Mods panel is unguarded,
 * and a savestate load replaces the tables with no edge firing. Does
 * nothing without the level ops' set_level_counts.
 * @param cpu Live game CPU. */
static void counts_tick(CPUState* cpu) {
    if (!level_ops() || !level_ops()->set_level_counts) return;

    /* Static, cleared per call: this runs every tick and the storage is
     * worth keeping. */
    static mod_fvec s_ids;   /* int */
    if (!s_ids.elem_size)
        mod_fvec_init(&s_ids, sizeof(int), CUSTOM_INIT + 1, NULL, 0);
    counts_collect_ids(&s_ids);
    for (int k = 0; k < (int)mod_fvec_count(&s_ids); k++)
        counts_apply_one(cpu, id_at(&s_ids, k));
}

unsigned long openpete_mods_registry_counts_writes(void);
/** @brief Return the bytes the target-total reconcile has written this session.
 *
 * The engagement counter for a run that has to show the reconcile
 * executed. Not declared in a project header.
 * @return The running byte total. */
unsigned long openpete_mods_registry_counts_writes(void) {
    return s_counts_writes;
}

/** @brief Take this tick's custom-level warp request, then drive the queued warp one step.
 *
 * A queued warp into a row whose files are missing or whose mod is
 * disabled is dropped. A replace-mode warp enters the donor and retries
 * each tick until enter_level succeeds. A slot-mode warp claims the shared
 * slot (retrying each tick until the claim succeeds), or retargets it to
 * the row's donor when another row holds it (dropping the warp on
 * failure), declares the row's data length and title on the slot, and
 * enters it, retrying the entry each tick. An accepted entry sets the
 * bug-report level-entry hold. Runs on committed ticks only.
 * @param cpu Live game CPU. */
static void custom_warp_step(CPUState* cpu) {
    custom_take_request();
    if (s_custom_warp < 0) return;
    custom_level_t* r = custom_at(s_custom_warp);
    if (!r->data_size || !openpete_mods_is_enabled(r->mod_id)) {
        s_custom_warp = -1;
        return;
    }
    if (r->replace_mode) {
        if (level_ops()->enter_level(cpu, r->donor_id) == 0) {
            s_custom_warp = -1;
            openpete_bugrec_hold(OPENPETE_BUGREC_HOLD_LEVEL_ENTRY, 1);
            openpete_rewind_note_intervention("custom-level warp");
        }
        return; /* refused = not in gameplay yet; retry next tick */
    }
    if (!s_custom_slot_id) {
        int got = level_ops()->claim_slot(cpu, r->donor_id);
        if (!got) return; /* TOC not read yet, or the slot is another
                           * mod's - keep retrying quietly */
        s_custom_slot_id = got;
    } else if (s_custom_active != s_custom_warp) {
        if (!level_ops()->retarget_slot ||
            level_ops()->retarget_slot(cpu, r->donor_id) != 0) {
            s_custom_warp = -1;
            return;
        }
    }
    s_custom_active = s_custom_warp;
    if (level_ops()->declare_data_len)
        (void)level_ops()->declare_data_len(cpu, s_custom_slot_id,
                                            r->data_size);
    if (level_ops()->set_level_name)
        (void)level_ops()->set_level_name(cpu, s_custom_slot_id,
                                          r->title[0] ? r->title
                                                      : r->name);
    if (level_ops()->enter_level(cpu, s_custom_slot_id) == 0) {
        s_slot_entering = s_custom_slot_id;
        s_custom_warp = -1;
        openpete_bugrec_hold(OPENPETE_BUGREC_HOLD_LEVEL_ENTRY, 1);
        openpete_rewind_note_intervention("custom-level warp");
        OPL_INFO_F("mods", "custom level \"%s\" entering (id %d, donor "
                   "%d)", r->name, s_custom_slot_id, r->donor_id);
    }
}

/** @brief Keep each custom-level row's replace declaration in sync, then run a queued warp.
 *
 * On a committed tick a queued slot/replace toggle is taken first
 * (custom_take_mode). A row wants its replace declared when its data file
 * exists, it is in replace mode and its mod is enabled. Declaring writes the data length
 * over the donor's entry and sets the donor's title to the row's title or
 * name; undeclaring writes length 0 and restores the stock name. Each
 * replace row also drives the donor portal's sky preview every tick. The
 * warp runs through custom_warp_step on committed ticks.
 * @param cpu Live game CPU. */
static void custom_tick(CPUState* cpu) {
    static unsigned s_tickno = 0;
    custom_env_schedule(++s_tickno);
    if (!level_ops() || !custom_count()) return;
    custom_take_mode();

    for (int i = 0; i < custom_count(); i++) {
        custom_level_t* r = custom_at(i);
        int want = r->data_size && r->replace_mode &&
                   openpete_mods_is_enabled(r->mod_id);
        if (want && !r->declared_replace) {
            if (level_ops()->declare_data_len &&
                level_ops()->declare_data_len(cpu, r->donor_id,
                                              r->data_size) == 0) {
                r->declared_replace = 1;
                if (level_ops()->set_level_name)
                    (void)level_ops()->set_level_name(
                        cpu, r->donor_id,
                        r->title[0] ? r->title : r->name);
                OPL_INFO_F("mods", "custom level \"%s\": replacing level "
                           "%d (data %u)", r->name, r->donor_id,
                           r->data_size);
            }
        } else if (!want && r->declared_replace) {
            if (level_ops()->declare_data_len &&
                level_ops()->declare_data_len(cpu, r->donor_id, 0) == 0) {
                r->declared_replace = 0;
                if (level_ops()->set_level_name)
                    (void)level_ops()->set_level_name(cpu, r->donor_id,
                                                      NULL);
                OPL_INFO_F("mods", "custom level \"%s\": level %d back "
                           "to stock", r->name, r->donor_id);
            }
        }
        /* Portal preview: while the replace is declared, the donor's
         * homeworld portal shows the custom level's own sky. */
        if (r->replace_mode && level_ops()->replace_sky_tick)
            (void)level_ops()->replace_sky_tick(cpu, r->donor_id, r->dir,
                                                r->stem,
                                                r->declared_replace);
    }

    /* A speculative tick's guest writes are rolled back: the warp is taken
     * and driven on committed ticks only, so it is not spent on one. */
    if (!openpete_speculative) custom_warp_step(cpu);
}

/* The registry section of a state: the custom-level registry's host state,
 * which mirrors the guest writes it made (the shared slot's claim, the
 * replace declarations) or decides the next ones (each row's mode, a queued
 * warp), so it travels with the guest RAM it describes. */
#define REGISTRY_STATE_VERSION 1u
typedef struct {
    uint32_t version;
    uint32_t rows;
    int32_t  slot_id;   /* s_custom_slot_id */
    int32_t  active;    /* s_custom_active */
    int32_t  warp;      /* s_custom_warp */
} registry_state_hdr_t;
typedef struct {
    uint8_t replace_mode;
    uint8_t declared_replace;
    uint8_t pad[2];
} registry_state_row_t;

/* Documented at the declaration in openpete_mods.h. */
uint32_t openpete_mods_registry_state_size(void) {
    if (!custom_count()) return 0;
    return (uint32_t)(sizeof(registry_state_hdr_t) +
                      (size_t)custom_count() * sizeof(registry_state_row_t));
}

/* Documented at the declaration in openpete_mods.h. */
void openpete_mods_registry_state_save(void* dst) {
    const registry_state_hdr_t h = {REGISTRY_STATE_VERSION, (uint32_t)custom_count(),
                                    s_custom_slot_id, s_custom_active, s_custom_warp};
    memcpy(dst, &h, sizeof h);
    uint8_t* p = (uint8_t*)dst + sizeof h;
    for (int i = 0; i < custom_count(); i++, p += sizeof(registry_state_row_t)) {
        const registry_state_row_t r = {(uint8_t)custom_at(i)->replace_mode,
                                        (uint8_t)custom_at(i)->declared_replace, {0, 0}};
        memcpy(p, &r, sizeof r);
    }
}

/* Documented at the declaration in openpete_mods.h. */
void openpete_mods_registry_state_apply(const void* src, uint32_t size) {
    if (!src || !size) return;
    registry_state_hdr_t h;
    if (size < sizeof h) return;
    memcpy(&h, src, sizeof h);
    const int n = custom_count();
    if (h.version != REGISTRY_STATE_VERSION || h.rows != (uint32_t)n ||
        size != sizeof h + (size_t)n * sizeof(registry_state_row_t) ||
        h.active >= n || h.warp >= n) {
        OPL_WARN_F("mods", "state registry section (v%u, %u rows, %u B) does not fit the %d "
                   "registered custom level(s); the live registry is kept", h.version, h.rows,
                   size, n);
        return;
    }
    const int had_slot = s_custom_slot_id != 0;
    s_custom_slot_id = h.slot_id;
    s_custom_active = h.active < 0 ? -1 : h.active;
    s_custom_warp = h.warp < 0 ? -1 : h.warp;
    const uint8_t* p = (const uint8_t*)src + sizeof h;
    for (int i = 0; i < n; i++, p += sizeof(registry_state_row_t)) {
        registry_state_row_t r;
        memcpy(&r, p, sizeof r);
        custom_at(i)->replace_mode = r.replace_mode ? 1 : 0;
        custom_at(i)->declared_replace = r.declared_replace ? 1 : 0;
    }
    /* The level ops hold the slot's alias donor host-side: hand them the
     * restored claim, or release the one this registry made. */
    if (!level_ops() || !level_ops()->adopt_slot) return;
    if (s_custom_slot_id && s_custom_active >= 0) {
        if (level_ops()->adopt_slot(custom_at(s_custom_active)->donor_id) != 0)
            OPL_WARN_F("mods", "state load: the level ops refused the restored slot claim "
                       "(id %d, donor %d)", s_custom_slot_id,
                       custom_at(s_custom_active)->donor_id);
    } else if (had_slot && !s_custom_slot_id) {
        (void)level_ops()->adopt_slot(0);
    }
}

/* Reads host-side statics only: the last level the per-tick driver saw. */
int openpete_mods_registry_owner_busy(const char* mod_id);
int openpete_mods_registry_owner_busy(const char* mod_id) {
    if (!mod_id) return 0;
    for (int i = 0; i < level_slot_count(); i++) {
        const level_slot_t* s = level_slot_at(i);
        if (!s->slot_id || strcmp(s->mod_id, mod_id) != 0) continue;
        if (s_resident_level == s->slot_id ||
            s_slot_entering == s->slot_id)
            return 1;
    }
    if (s_custom_active >= 0 && s_custom_slot_id &&
        strcmp(custom_at(s_custom_active)->mod_id, mod_id) == 0 &&
        (s_resident_level == s_custom_slot_id ||
         s_slot_entering == s_custom_slot_id))
        return 1;
    return 0;
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_read_level_data(int level_id, void* buf, uint32_t cap) {
    if (!level_ops() || !level_ops()->read_level_data || level_id <= 0) return -1;
    return level_ops()->read_level_data(level_id, buf, cap);
}
