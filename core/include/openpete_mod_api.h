/**
 * @file openpete_mod_api.h
 * @brief The mod API.
 *
 * Staged into `<exe>/sdk/` and compiled into every code mod. A mod reaches
 * the engine only through the ::openpete_mod_api_t struct passed to its
 * entry point; it never links against engine symbols.
 *
 * @code
 * int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self);
 * @endcode
 *
 * Return 0 to load; any other value disables the mod and logs the value.
 *
 * @par Stability
 * The struct layout is frozen at api 11: from there on entries are
 * appended, never moved, retyped or removed, and the loader refuses a
 * prebuilt object built against an earlier api. The manifest `api = N`
 * pin is the minimum version the mod needs; check `api_version < N`,
 * never `!=`. The two extension headers - the widget vtable (openpete_mod_ui.h) and
 * the ImGui escape hatch (openpete_imgui.h) - are versioned
 * separately and carry no stability promise. Each entry states the
 * version it appeared in with `@since`.
 *
 * @par Overrides
 * A pre-hook, a post-hook and a full replacement are one mechanism,
 * distinguished by where the override calls `api->base(cpu)`: last, first
 * or never. base() runs the next override in the chain and bottoms out at
 * the recompiled original. There is no event system: detect an event by
 * diffing guest state around base().
 *
 * Override bodies take the raw MIPS register file (::CPUState): arguments
 * in `cpu->a0..a3`, return value in `cpu->v0`. openpete_sdk_wrappers.h
 * provides natural-signature wrappers for every prototyped function.
 */

#ifndef OPENPETE_MOD_API_H
#define OPENPETE_MOD_API_H

#include <stdint.h>

/* A mod's compile line puts the per-game SDK directory (`sdk/games/<id>/`)
 * before `sdk/`; its openpete_game.h defines OPENPETE_GAME_ID and
 * OPENPETE_GAME_SPYRO1/2/3, and #OPENPETE_MOD_ABI_DEFINE records the id in
 * the object. The engine's own build defines OPENPETE_ENGINE_BUILD and
 * never reads it. */
#if !defined(OPENPETE_ENGINE_BUILD) && defined(__has_include)
#if __has_include("openpete_game.h")
#include "openpete_game.h"
#endif
#endif

/* vstream_handle on a game whose frame words carry no vertex-stream field
 * (Spyro 2 and 3): in a mod built for that game a use of the member fails
 * to compile on every compiler, with the reason in the error, and the
 * struct layout stays the same. Where the compiler has the `unavailable`
 * attribute (clang) the member carries it; elsewhere (GCC before 12) the
 * slot is declared as vstream_handle_reserved_slot and the name
 * vstream_handle is defined to an explanatory name the struct lacks, so the
 * error reads "no member named vstream_handle_is_spyro1_only__...". An
 * OPENPETE_MOD_ANY_GAME object keeps the member, and the call returns 0
 * there with one log line per mod. */
#define OPENPETE_VSTREAM_ATTR
#define OPENPETE_VSTREAM_SLOT_RESERVED 0
#if !defined(OPENPETE_ENGINE_BUILD) && defined(OPENPETE_GAME_ID) && \
    (OPENPETE_GAME_SPYRO2 || OPENPETE_GAME_SPYRO3)
#if defined(__has_attribute)
#if __has_attribute(unavailable)
#undef OPENPETE_VSTREAM_ATTR
#define OPENPETE_VSTREAM_ATTR __attribute__((unavailable( \
    "vstream_handle is Spyro 1 only: " OPENPETE_GAME_ID " animation frame " \
    "words carry no vertex-stream field; move a stream through the " \
    "animation header's pointers")))
#endif
#endif
#if !defined(__has_attribute)
#undef OPENPETE_VSTREAM_SLOT_RESERVED
#define OPENPETE_VSTREAM_SLOT_RESERVED 1
#elif !__has_attribute(unavailable)
#undef OPENPETE_VSTREAM_SLOT_RESERVED
#define OPENPETE_VSTREAM_SLOT_RESERVED 1
#endif
#if OPENPETE_VSTREAM_SLOT_RESERVED
#define vstream_handle vstream_handle_is_spyro1_only__move_streams_through_the_animation_header
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct CPUState CPUState;

/** @brief Opaque per-mod handle owned by the loader. */
typedef struct openpete_mod openpete_mod_t;

/**
 * @brief An override body.
 *
 * Read and write guest state through @p cpu; call `api->base(cpu)` to run
 * the next override or the original.
 */
typedef void (*openpete_mod_override_fn)(CPUState* cpu);

/** @name Mouse
 * Button bits of openpete_mod_mouse_t::buttons.
 * @{ */
#define OPENPETE_MOD_MOUSE_LEFT   0x01u
#define OPENPETE_MOD_MOUSE_MIDDLE 0x02u
#define OPENPETE_MOD_MOUSE_RIGHT  0x04u
#define OPENPETE_MOD_MOUSE_X1     0x08u
#define OPENPETE_MOD_MOUSE_X2     0x10u
/** @} */

/**
 * @brief The host mouse as read by openpete_mod_api::mouse_read.
 *
 * Set `struct_size` to `sizeof(openpete_mod_mouse_t)` before the call; the
 * engine refuses any other size. `x, y` are overlay space (units of window
 * height, y in [0,1], origin top-left, the space draw_text uses); `px, py`
 * are window points and are undefined while `captured`. `dx, dy` are mouse
 * counts and `wheel` is in notches, each since this mod's previous read
 * from the same hook kind (tick or present).
 */
typedef struct openpete_mod_mouse {
    uint32_t struct_size;   /**< sizeof(openpete_mod_mouse_t) at the mod's compile time. */
    float    x, y;          /**< Overlay space. */
    float    px, py;        /**< Window points; undefined while captured. */
    float    dx, dy;        /**< Counts since this mod's previous read from this hook kind. */
    float    wheel;         /**< Notches since that read, up positive. */
    uint32_t buttons;       /**< OPENPETE_MOD_MOUSE_* mask. */
    uint8_t  in_window;     /**< The pointer is over the window. */
    uint8_t  captured;      /**< Relative mode is applied (the pointer is hidden and unbounded). */
    uint8_t  owned_by_overlay; /**< The settings menu or the dev overlay owns the mouse; buttons and deltas read 0. */
    uint8_t  reserved_;
} openpete_mod_mouse_t;

/** @brief Pad slots openpete_mod_api::pad_read serves: 0 is player 1. */
#define OPENPETE_MOD_PAD_SLOTS 4

/**
 * @brief One pad slot as read by openpete_mod_api::pad_read.
 *
 * Set `struct_size` to `sizeof(openpete_mod_pad_t)` before the call; the
 * engine refuses any other size. The bytes come from the input-log record
 * staged for the tick that is running, the record the log stores for that
 * tick, so the same read returns the same bytes under live play, bk2
 * replay, the rewind burst and runahead speculation.
 *
 * `vbl` is the VBL index that record was staged for. A speculative tick
 * re-executes on the last committed tick's record - held input, the same
 * prediction player 1 gets - and carries that tick's `vbl`. `buttons` is
 * the PSX pad bitfield, 1 = released. `axes` is LX, LY, RX, RY with 128
 * centred. `libpad` is the eight bytes libpad reports in a PADRAW buffer:
 * `00, 73`, low button byte, high button byte, RX, RY, LX, LY.
 *
 * An absent slot (`present` 0) is the canonical empty port rather than a
 * derivation: `buttons` 0xFFFF, axes centred, `libpad` `FF 00 00 00 00 00
 * 00 00`, what libpad reports for a port with no controller in it, so a
 * mod that copies `libpad` into a pad buffer of its own behaves as
 * hardware with nothing plugged in.
 */
typedef struct openpete_mod_pad {
    uint32_t struct_size; /**< sizeof(openpete_mod_pad_t) at the mod's compile time. */
    uint32_t slot;        /**< The slot read, 0..#OPENPETE_MOD_PAD_SLOTS-1. */
    uint32_t vbl;         /**< VBL index the record was staged for. */
    uint32_t present;     /**< 1 = a device drove this slot at that VBL. */
    uint32_t buttons;     /**< PSX pad bitfield; 1 = released. */
    uint8_t  axes[4];     /**< LX, LY, RX, RY; 128 = centre. */
    uint8_t  libpad[8];   /**< The slot as a libpad PADRAW buffer. */
} openpete_mod_pad_t;

/**
 * @brief The player's pose as read by openpete_mod_api::player_pose.
 *
 * Set `struct_size` to `sizeof(openpete_mod_pose_t)` before the call; the
 * engine refuses any other size. Positions are the game's world units, in
 * the same space on all three games.
 */
typedef struct openpete_mod_pose {
    uint32_t struct_size; /**< sizeof(openpete_mod_pose_t) at the mod's compile time. */
    int32_t  x;           /**< World x. */
    int32_t  y;           /**< World y. */
    int32_t  z;           /**< World z (up). */
    int32_t  yaw;         /**< Facing, 4096 per turn; -1 on a game whose facing field is not located. */
    int32_t  level;       /**< The level id (openpete_mod_api::level_id). */
    uint32_t placeable;   /**< 1 while the player is under pad control in the ordinary follow camera; 0 in the attract demo, a fly-in or an exit sequence. */
    uint32_t tick;        /**< The canonical tick the record was sampled at; 0 for a live read from tick context. */
} openpete_mod_pose_t;

/** @name Anti-piracy scan states (openpete_mod_scan_t::state)
 * @{ */
#define OP_SCAN_IDLE    0 /**< Not started: no step seen, or a walk not yet entered. */
#define OP_SCAN_RUNNING 1 /**< Started and not yet compared. */
#define OP_SCAN_PASS    2 /**< Compared, and the game's own test passed. */
#define OP_SCAN_FAIL    3 /**< Compared, and the game's own test failed. */
/** @} */

/** @name Anti-piracy walk keys (openpete_mod_api::antipiracy_scan)
 * A key with bit 31 clear names a walk; every other key is a scanner moby's
 * data block (the word at the moby record's offset 0).
 * @{ */
#define OP_SCAN_KEY_BOOT    1u /**< The boot walk: four CRC-16 checks over EXE and title text at power-on. */
#define OP_SCAN_KEY_TITLE   2u /**< The title walk: two CRC-16 checks over EXE and title text when the title sequence starts. */
#define OP_SCAN_KEY_LOADING 3u /**< The loading walk: CRC over EXE and loading-screen text during each level load. */
/** @} */

/**
 * @brief One anti-piracy scan as read by openpete_mod_api::antipiracy_scan.
 *
 * Set `struct_size` to `sizeof(openpete_mod_scan_t)` before the call; the
 * engine refuses any other size. A scan that restarts (the boss levels'
 * one-function scans repeat) reads RUNNING again, with @c completed
 * counting the comparisons made.
 */
typedef struct openpete_mod_scan {
    uint32_t struct_size; /**< sizeof(openpete_mod_scan_t) at the mod's compile time. */
    int32_t  state;       /**< OP_SCAN_IDLE, OP_SCAN_RUNNING, OP_SCAN_PASS or OP_SCAN_FAIL. */
    uint32_t func;        /**< The scanning function's guest address. */
    uint32_t cursor;      /**< The scan position at the last observation, a guest address. */
    uint32_t end;         /**< The position at which the scan compares. */
    uint32_t crc;         /**< The value the last comparison tested; 0 before one. */
    uint32_t crc_addr;    /**< Guest address of the running CRC word; 0 for a walk that keeps it in registers. */
    uint32_t steps;       /**< Advances observed (scanner calls that moved the cursor; checks made for the boot and title walks). */
    uint32_t completed;   /**< Comparisons made. */
} openpete_mod_scan_t;

/** @brief Context handed to a present hook (openpete_mod_api::register_present_hook). */
typedef struct openpete_present_ctx {
    uint32_t tick;   /**< Sim tick counter. */
    float    alpha;  /**< Sub-tick phase [0,1) between tick and tick+1; 0 on a canonical present. */
    float    dt;     /**< Seconds between this scene's animation moment and the previous scene's. */
    float    aspect; /**< Overlay-space width: x spans [0, aspect]. */
} openpete_present_ctx_t;

/** @brief A present hook body. */
typedef void (*openpete_mod_present_fn)(const openpete_present_ctx_t* ctx);

/**
 * @brief Live enable/disable callback (openpete_mod_api::register_toggle_hook).
 *
 * Runs at the tick boundary after the mod's registrations have flipped;
 * guest reads and writes are legal. @p on = 0 is where a mod restores
 * guest bytes its entry or hooks changed.
 */
typedef void (*openpete_mod_toggle_fn)(CPUState* cpu, int on);

/**
 * @brief Exit callback (openpete_mod_api::register_shutdown_hook).
 *
 * Runs once, on the sim thread, after the last tick of a session and
 * before the engine tears down anything an api entry reaches. The body
 * signals every thread the mod started and joins it.
 * @since api 13
 */
typedef void (*openpete_mod_shutdown_fn)(void);

/** @brief The api version that added openpete_mod_api::save_register; guard its use with `#ifdef`. */
#define OPENPETE_MOD_SAVE_API 13u

/**
 * @brief The game save a save-record callback concerns.
 * @since api 13
 */
typedef struct {
    uint32_t struct_size; /**< sizeof at the engine's compile time. */
    uint32_t save_index;  /**< Index of the save in the game's card file. */
} openpete_mod_save_id_t;

/**
 * @brief What a save carries for a mod (openpete_mod_save_read_t::status).
 *
 * A mod handles a value it does not know as #OP_MOD_SAVE_ABSENT.
 * @since api 13
 */
typedef enum {
    OP_MOD_SAVE_OK = 0,     /**< Record present, schema <= the mod's, intact. */
    OP_MOD_SAVE_ABSENT,     /**< The save carries no record from this mod; a fresh playthrough when no data. */
    OP_MOD_SAVE_GAP,        /**< Record intact; the game saved at least once since while this mod was not loaded or was toggled off. */
    OP_MOD_SAVE_STALE,      /**< The save index has records from this playthrough, but the save on the card matches none of them (changed outside openpete); data is the playthrough's newest. */
    OP_MOD_SAVE_NEWER,      /**< The record was written by a newer schema; no data. The engine keeps it and does not call the mod's write for that playthrough. */
    OP_MOD_SAVE_LEGACY_PAD, /**< No record; data is the game's spare span of the loaded save (Spyro 1: the 112 pad bytes at SaveFile+0x590), schema 0. Import it only when it carries the mod's own magic; anything else is another mod's payload, not damage. */
} openpete_mod_save_status_t;

/**
 * @brief One save-record delivery (openpete_mod_save_read_fn).
 * @since api 13
 */
typedef struct {
    uint32_t struct_size;  /**< sizeof at the engine's compile time; fields past a mod's own sizeof are not read by it. */
    uint32_t save_index;   /**< Index of the save in the game's card file; 0 with no save (ABSENT after a new playthrough). */
    openpete_mod_save_status_t status;
    uint32_t schema;       /**< Schema of the delivered record; 0 with no data. */
    const void* data;      /**< The record, valid for the call only; NULL with no data. */
    uint32_t size;         /**< Bytes at data. */
} openpete_mod_save_read_t;

/**
 * @brief Save-record write callback: serialize the mod's save-derived state.
 *
 * Called on the sim thread in tick context when the game writes a
 * non-empty save, once per save. Set @p data to the mod's own buffer and
 * @p size to its length (any size); the buffer stays valid until the
 * engine next calls into the mod, and the engine copies it on return.
 * Guest RAM is readable and the arena writable; api->call is not.
 * @return 0 to store the record, -1 to keep the mod's previous record of
 *         this playthrough (none under a new playthrough).
 * @since api 13
 */
typedef int (*openpete_mod_save_write_fn)(openpete_mod_t* self,
        const openpete_mod_save_id_t* id, const void** data, uint32_t* size);

/**
 * @brief Save-record read callback: replace all save-derived state from @p r.
 *
 * Called on the sim thread in tick context, at the first entry of the
 * game's update function, when the save or playthrough the mod last saw
 * differs from the game's current one (a load, a new playthrough, a mod
 * enabled after either). Never called on a savestate load, rewind,
 * runahead rollback, process handoff or hot reload, which restore the
 * arena with the rest of the timeline. Guest RAM and the arena may be
 * written; api->call is not available.
 * @since api 13
 */
typedef void (*openpete_mod_save_read_fn)(openpete_mod_t* self,
        const openpete_mod_save_read_t* r);

/** @brief Log levels for openpete_mod_api::log. */
enum {
    OP_MOD_LOG_DEBUG = 1,
    OP_MOD_LOG_INFO  = 2,
    OP_MOD_LOG_WARN  = 3,
    OP_MOD_LOG_ERROR = 4,
};

/** @brief Flags for openpete_mod_api::guest_alloc. */
enum {
    /**
     * The mod promises that after the allocating tick neither it nor the
     * game writes these bytes. The engine may leave them out of savestates
     * and rewind history and may abort on a detected write; today it
     * snapshots them like other blocks, checksums them at each rewind
     * keyframe and file savestate, and logs a changed block once. Use for
     * parked model data, never for scratch.
     */
    OP_GALLOC_IMMUTABLE = 1u << 0,
    /**
     * The mod states that nothing truncates an address inside the block to
     * 24 bits: no ordering-table link, no block passed to a libgpu call
     * that links or walks it, and no pointer to it stored in a 24-bit
     * field. The engine may then place the block anywhere in 32-bit guest
     * space (today the general pool, physical 0x01000000-0x1EFFFFFF,
     * 480 MiB). Without the flag a block lands below physical 16 MiB,
     * where a GPU packet must lie. Combines with #OP_GALLOC_IMMUTABLE.
     * @since api 13
     */
    OP_GALLOC_NO_DMA = 1u << 1,
};

/** @brief Post-process injection points for openpete_mod_api::postfx_register. */
enum {
    OP_POSTFX_COMPOSITE = 0, /**< After scene and HUD compose, before the present blit; the default point. */
    OP_POSTFX_SCENE     = 1, /**< After the 3D scene, before the HUD, so the HUD stays unaffected.
                              *   Scene depth is available at `op_scene_depth` (terrain only, not mobys). */
    OP_POSTFX_PRESENT   = 2, /**< Reserved; registration is refused. */
};

/**
 * @brief Sim tick rate in Hz: 1125000/37609 (29.913...).
 *
 * The PSX outputs 263-line non-interlaced video at 2250000/37609 Hz
 * (59.826...) and the game runs one tick per two VBLs. Not 29.97: that is
 * the interlaced field rate, which this game never uses, and a mod
 * converting ticks to seconds with it drifts 0.19 % against game time.
 * `--tick-scale` stretches wall time only; a tick stays one tick.
 */
#define OPENPETE_TICK_HZ  (1125000.0 / 37609.0)
/** @brief VBL rate in Hz: 2250000/37609 (59.826...). */
#define OPENPETE_VBL_HZ   (2250000.0 / 37609.0)

/**
 * @brief Render channels: the renderer's feeder paths.
 *
 * A material selector names one of these. Each channel can be narrowed on
 * the keys listed in openpete_mod_material_selector.
 */
enum {
    OP_CHAN_MOBY     = 1,
    OP_CHAN_TERRAIN  = 2,
    OP_CHAN_PLAYER   = 3,
    OP_CHAN_FLAME    = 4,
    OP_CHAN_PARTICLE = 5,
    OP_CHAN_SKY      = 6,
    OP_CHAN_SHADOW   = 7,
    OP_CHAN_TRACER   = 8,
    OP_CHAN_PORTAL   = 9,   /**< Destination-sky preview inside a portal. */
};
/**
 * @brief Portal preview kinds: the `ident` key for #OP_CHAN_PORTAL.
 *
 * Spyro 1 draws a portal three ways across the approach, Spyro 2 and 3
 * two ways; a material keyed on one kind pops as the player walks up, so
 * -1 (every kind) is the usual choice.
 */
enum {
    OP_PORTAL_FLAT    = 0,  /**< Far colour chip / close backstop; on Spyro 2 and 3 the background quad. */
    OP_PORTAL_DOME    = 1,  /**< Full gouraud dome, close up; on Spyro 2 and 3 the destination picture. */
    OP_PORTAL_BLENDED = 2,  /**< Fogged dome, medium cross-dissolve; Spyro 1 only. */
};
/**
 * @brief Particle kinds: the high byte of the `ident` key for `OP_CHAN_PARTICLE`.
 *
 * The particle feeder draws six effects. The ident of a draw is
 * `(kind << 8) | sel`, where `sel` is the game's own particle class for
 * the two kinds that carry one (quads and lines) and 0 otherwise.
 * #OP_PART_IDENT builds the key. The ident match is exact, so a kind
 * that carries a class is claimed one class per row. Glow rings and
 * sparkles are separate game renderers that share this channel; a
 * glow-only or sparkle-only material keys on the kind with `sel` 0.
 *
 * On Spyro 2 and 3 `sel` is reserved: a selector whose ident carries a
 * nonzero `sel` matches no draw, and the engine logs one warning per such
 * material at registration. A `sel` 0 key matches none of their quads
 * or lines either; ident -1 on this channel reaches them.
 */
enum {
    OP_PART_QUAD  = 0,  /**< Billboard quads; `sel` = particle class on Spyro 1, reserved on Spyro 2 and 3. */
    OP_PART_LINE  = 1,  /**< Streak lines; `sel` = particle class on Spyro 1, reserved on Spyro 2 and 3. */
    OP_PART_GLOW  = 2,  /**< Additive glow rings around collectables. */
    OP_PART_FLARE = 3,  /**< Sparkle flares (the twinkle quads). */
    OP_PART_SPARK = 4,  /**< Sparkle lines. */
    OP_PART_STAR  = 5,  /**< Dragon-rescue star fan. */
};
/** @brief The `ident` key for one particle kind and class; `sel` is 0 for kinds without a class. */
#define OP_PART_IDENT(kind, sel) \
    ((int)(((unsigned)(kind) << 8) | ((unsigned)(sel) & 0xFFu)))
/**
 * @brief Player draw kinds: the `ident` key for #OP_CHAN_PLAYER.
 *
 * The player channel carries Spyro only; Spyro 3's other playable
 * characters draw on the moby channel. Spyro 1 draws only the avatar.
 */
enum {
    OP_PLAYER_AVATAR    = 0,  /**< Spyro as the player draw renders him. */
    OP_PLAYER_MIRROR    = 1,  /**< The mirrored reflection copy; Spyro 2 and 3. */
    OP_PLAYER_SUBMERGED = 2,  /**< The depth-cued half below the water plane; Spyro 2 and 3. */
};
/** @brief Tracer producers: the `ident` key for #OP_CHAN_TRACER. Spyro 1's beam is a ribbon. */
enum {
    OP_TRACER_RIBBON = 0,  /**< Point-ring ribbons: Spyro 1's beam, the Spyro 2 and 3 ribbon routine. */
    OP_TRACER_MESH   = 1,  /**< Indexed tracer meshes; Spyro 2 and 3. */
    OP_TRACER_ZAP    = 2,  /**< Zoe's zap; Spyro 2 and 3. */
};
/** @brief Legacy 0-based path values; prefer `OP_CHAN_*`. */
enum { OP_RP_MOBY = 0, OP_RP_TERRAIN = 1 };
/**
 * @brief Selector refinements.
 *
 * Each pair narrows: setting neither member of a pair selects both.
 */
enum {
    OP_SEL_ENV_ANIM   = 1u << 0,  /**< Terrain faces the game animates only; neither env bit = both, both = refused. */
    OP_SEL_HP_TIER    = 1u << 1,  /**< High-poly tier only; neither tier bit = both. */
    OP_SEL_LP_TIER    = 1u << 2,  /**< Low-poly tier only. */
    OP_SEL_UNTEXTURED = 1u << 3,  /**< Gouraud faces only; neither texture bit = both. */
    OP_SEL_TEXTURED   = 1u << 4,  /**< Textured faces only; on the moby channel every moby triangle. */
    /** Not a narrowing bit: it selects nothing and narrows nothing. It marks
     *  the material terminal, so no lower material composes beneath it, on a
     *  mod pinned at api 13 or later; a mod pinned below 13 is refused it. */
    OP_SEL_EXCLUSIVE  = 1u << 5,
    /** Terrain faces the game does not animate only (every terrain face
     *  outside the #OP_SEL_ENV_ANIM set); a moby or any other channel's
     *  draw never matches it. Added at api 13. */
    OP_SEL_ENV_STATIC = 1u << 6,
};
/**
 * @brief Every flag the engine understands.
 *
 * A selector carrying any other bit is refused: `struct_size` cannot
 * detect a new flag bit, since adding one does not change `sizeof`.
 */
#define OP_SEL_FLAGS_KNOWN (OP_SEL_ENV_ANIM | OP_SEL_HP_TIER | \
                            OP_SEL_LP_TIER  | OP_SEL_UNTEXTURED | \
                            OP_SEL_TEXTURED | OP_SEL_EXCLUSIVE | \
                            OP_SEL_ENV_STATIC)

/** @brief Chooses the geometry a material may claim. */
typedef struct openpete_mod_material_selector {
    uint32_t struct_size; /**< `sizeof(*this)` at the mod's compile time; the engine refuses a size it does not know. */
    int      channel;     /**< `OP_CHAN_*`; -1 = any channel. */
    int      ident;       /**< Channel-local identity, meaning per channel: flame ribbon or cap,
                           *   particle `OP_PART_*` kind `(kind << 8 | sel)`, shadow caster kind,
                           *   portal preview kind, the moby class on the moby channel. -1 = any.
                           *   player `OP_PLAYER_*`, tracer `OP_TRACER_*`; sky emits only 0. */
    int      moby_class;  /**< Moby only; -1 = any. */
    int      texture;     /**< Terrain texture-record id 0..127; -1 = any. */
    int      level;       /**< `g_LevelId`; -1 = any. Sector and texture ids are per level, so a
                           *   key without this matches the same id in every level. */
    unsigned flags;       /**< `OP_SEL_*` refinements. */
} openpete_mod_material_selector_t;
/**
 * @brief A selector that matches everything; narrow only the fields you mean.
 *
 * A zero-initialised selector would mean channel 0 and class 0.
 */
static inline openpete_mod_material_selector_t openpete_mod_material_sel_any(void) {
    openpete_mod_material_selector_t s;
    s.struct_size = (uint32_t)sizeof s;
    s.channel = -1; s.ident = -1; s.moby_class = -1;
    s.texture = -1; s.level = -1; s.flags = 0u;
    return s;
}

/**
 * @brief Key version per channel, stamped into openpete_mod_material_key::version.
 *
 * Bumped when a channel's key members change meaning.
 */
#define OPENPETE_MATERIAL_KEY_VER_MOBY    1u
/** @copydoc OPENPETE_MATERIAL_KEY_VER_MOBY */
#define OPENPETE_MATERIAL_KEY_VER_TERRAIN 1u

/**
 * @brief One instance at refine time.
 *
 * Every field is filled by the engine; @c channel selects the live union
 * member.
 */
typedef struct openpete_mod_material_key {
    uint32_t struct_size;   /**< `sizeof` at engine compile time. */
    uint32_t version;       /**< `OPENPETE_MATERIAL_KEY_VER_*` for @c channel. */
    int      channel;       /**< `OP_CHAN_*`. */
    union {
        struct {
            uint32_t vaddr;      /**< Guest address of the moby record. */
            int      moby_class; /**< `m_Class`. */
        } moby;
        struct {
            int      texture;    /**< Tiledef id, or -1 for the untextured band. */
            unsigned flags;      /**< The `OP_SEL_*` bits describing this face. */
        } terrain;
    } u;
} openpete_mod_material_key_t;

/** @brief Size cap of a material's per-instance uniform block. */
#define OPENPETE_MATERIAL_UNIFORMS_MAX 256u

/**
 * @brief Refine callback (openpete_mod_api::material_register_refine).
 *
 * The selector decides which geometry a material may claim; the refine
 * callback decides per instance whether it does, and with what uniform
 * block. Example: the selector prunes to class 42, the callback claims
 * only instances that are burning.
 *
 * The callback runs during extraction in tick context: guest RAM is
 * readable through `api->guest()` and `op_read_*`; host render state is
 * off limits. It is asked once per tick per key, and the outcome is
 * replayed on the presents the tick's sub-tick re-extracts build, so the
 * callback must be a pure function of the key and tick-stable guest
 * state; the call count is not part of the contract.
 *
 * @param key  The instance.
 * @param out  Uniform block to fill, at most @p cap bytes.
 * @param cap  #OPENPETE_MATERIAL_UNIFORMS_MAX.
 * @return Bytes written to @p out (0 = claim with no parameters), or -1:
 *         not mine. On the moby channel the instance passes to the next
 *         material in priority order that matches it (a declarative one
 *         takes it, a refine one is asked), and keeps stock shading when
 *         none takes it. On terrain the faces of the key pass the same
 *         way to the next refine keyed to them, then to the
 *         highest-priority declarative material that matches each face.
 */
typedef int (*openpete_mod_refine_fn)(const openpete_mod_material_key_t* key,
                                      void* out, uint32_t cap);

/** @name Buses
 * Named synchronous message buses between mods (openpete_mod_api::bus_attach).
 * @{ */
/**
 * @brief A mod as a bus sees it: the sender of a message, a member, or a key's owner.
 *
 * `id` points to a string interned for the life of the process; it is host
 * memory and never belongs in guest memory or the arena. `key` is the mod
 * key (openpete_mod_key_of), which is the same in every process and build
 * and is safe to store in the arena. The caller sets `struct_size` to the
 * `sizeof` it was compiled with; the engine refuses a smaller size than
 * this struct's api-13 size and writes at most the caller's size.
 * @since api 13
 */
typedef struct openpete_mod_bus_sender {
    uint32_t    struct_size; /**< `sizeof` at the caller's compile time. */
    const char* id;          /**< Manifest id, interned for the process; host memory. */
    uint32_t    key;         /**< Mod key; 0 is never a mod's key. */
} openpete_mod_bus_sender_t;

/**
 * @brief A bus handler (openpete_mod_api::bus_attach).
 *
 * Runs on the sim thread inside the sender's `bus_call` or `bus_call_to`.
 * The engine saves the sender's caller-saved integer registers, `hi`,
 * `lo`, `sp` and both GTE banks around the call and lowers `sp` by a
 * 64-byte reserve first, so @p cpu may be passed to `api->call` (a fifth
 * argument goes at `sp + 16`) without changing what the sender's code
 * relies on. `api->base` is refused inside a handler.
 * @param self The handler owner's own handle.
 * @param cpu The sender's CPU state.
 * @param from The sender, filled by the engine.
 * @param msg_addr The message's guest address, as the sender passed it.
 * @param size The message's length in bytes.
 * @param user The pointer given to `bus_attach`.
 * @return #OP_BUS_PASS to let the walk continue, a positive value to end
 *         it; a negative value is treated as #OP_BUS_PASS and logged once.
 * @since api 13
 */
typedef int (*openpete_mod_bus_fn)(openpete_mod_t* self, CPUState* cpu,
                                   const openpete_mod_bus_sender_t* from,
                                   uint32_t msg_addr, uint32_t size,
                                   void* user);

/** @brief Handler result that continues the walk; `bus_call` result when every handler passed. @since api 13 */
#define OP_BUS_PASS    0
/** @brief `bus_call` and `bus_call_to` result: no enabled handler is attached, or the directed key is not one. @since api 13 */
#define OP_BUS_NONE  (-3)
/** @brief `bus_call` and `bus_call_to` result: the nesting limit (8) or the frame stack is full; no handler ran. @since api 13 */
#define OP_BUS_DEPTH (-4)

/**
 * @brief Return the mod key of a manifest id: the 32-bit FNV-1a hash of its bytes without the terminating NUL.
 *
 * Offset basis 2166136261, prime 16777619. `api->mod_key(self)` returns
 * this value of the mod's own id, so a mod computes a peer's key from the
 * peer's id. Key 0 is never a mod's key.
 * @param id The manifest id.
 * @return The key.
 * @since api 13
 */
static inline uint32_t openpete_mod_key_of(const char* id) {
    uint32_t h = 2166136261u;
    for (; *id; ++id) {
        h ^= (uint8_t)*id;
        h *= 16777619u;
    }
    return h;
}
/** @} */

/** @brief The api version this header describes. */
#define OPENPETE_MOD_API_VERSION 13u

/**
 * @brief Return value of an api entry whose surface the running game does not serve.
 *
 * `read_level_data`, `moby_classes` and `antipiracy_scan` read a
 * game-specific layer (level data, the moby class census, Spyro 3's
 * anti-piracy scans) that a game may leave out; on such a game
 * they return this code and log one line per mod naming the game, the
 * first time the mod calls each of them. Every other failure keeps its
 * documented value (-1 throughout the api), so `rc < 0` still tests for
 * failure and `rc == OPENPETE_MOD_ERR_UNSUPPORTED` tells an absent
 * surface from a refused call. Query `game_id` to branch per game.
 */
#define OPENPETE_MOD_ERR_UNSUPPORTED (-2)

/** @name Game ids
 * The id strings the disc calls and the manifest's `game`,
 * `requires_games` and `optional_games` keys take. A raw string is
 * accepted in their place, and `api->game_id(self)` returns the running
 * game's.
 * @{ */
/** @brief Spyro the Dragon (SCUS-94228). @since api 13 */
#define OPENPETE_MOD_GAME_SPYRO1 "spyro-1"
/** @brief Spyro 2: Ripto's Rage! (SCUS-94425). @since api 13 */
#define OPENPETE_MOD_GAME_SPYRO2 "spyro-2"
/** @brief Spyro: Year of the Dragon (SCUS-94467). @since api 13 */
#define OPENPETE_MOD_GAME_SPYRO3 "spyro-3"
/** @} */

/** @brief The engine's function table, passed to the mod entry point. */
typedef struct openpete_mod_api {
    /** @brief The engine's #OPENPETE_MOD_API_VERSION; compare with `<`, never `!=`.
     * @since api 1 */
    uint32_t api_version;

    /** @name Overrides
     * Attach an override to a guest function by symbol name (resolved
     * through openpete_sdk_symbols.h, e.g. "CameraUpdate") or by PSX vaddr
     * (pinned to this binary; the loader warns once per mod on the first
     * raw registration). Priority is the enabled-list order: overrides
     * from later-listed mods run first, and base() walks toward earlier
     * mods and then the recompiled original.
     * @{ */
    /**
     * @brief Attach an override by PSX vaddr.
     *
     * An address inside the overlay window names one function per
     * overlay: with `[mod] api` at 13 or later it is refused (bind it with
     * `override_ovl`); below 13 or with no pin it binds every overlay
     * with a function there, on Spyro 1 only, with a deprecation warning.
     * @return 0 on success, non-zero for invalid arguments or a refusal.
     * @since api 1
     */
    int (*override_addr)(openpete_mod_t* self, uint32_t addr,
                         openpete_mod_override_fn fn);
    /**
     * @brief Attach an override by symbol name.
     *
     * The name binds its row's address and overlay, so an overlay
     * function fires only in its own overlay. A raw `func_XXXXXXXX` name
     * whose rows lie in several overlays is refused with `[mod] api` at 13
     * or later; below 13 or with no pin it binds every one of them, on
     * Spyro 1 only, with a deprecation warning.
     * @return 0 on success, non-zero for an unknown or refused name.
     * @since api 1
     */
    int (*override_name)(openpete_mod_t* self, const char* name,
                         openpete_mod_override_fn fn);

    /**
     * @brief Run the next override in the chain, or the original.
     *
     * Valid only while an override registered by this mod is on the call
     * stack. May be called any number of times; each call runs the
     * continuation once.
     * @since api 1
     */
    void (*base)(CPUState* cpu);

    /**
     * @brief Call a guest function by vaddr through the dispatch layer, so
     *        the callee's own override chain runs.
     *
     * Write `cpu->a0..a3` before, read `cpu->v0` after. Valid only inside
     * an override. An address inside the overlay window runs the resident
     * overlay's function there: while another overlay is loaded, that
     * overlay's function at the address runs.
     * @since api 1
     */
    void (*call)(CPUState* cpu, uint32_t addr);
    /** @} */

    /** @name Guest memory
     * @{ */
    /**
     * @brief Translate a guest vaddr (kuseg, kseg0 or kseg1) to a host pointer.
     *
     * The 2 MB RAM, the scratchpad and guest_alloc's arena all resolve.
     * Guest RAM and every supported host are little-endian, so typed reads
     * and writes through the pointer see what the game sees. Pointer
     * fields inside guest structs hold guest vaddrs; translate them again
     * before dereferencing. Global addresses are `OP_GADDR_*` in
     * openpete_sdk_symbols.h. Sim thread only.
     * @return The host pointer, or NULL for an address that names nothing,
     *         including guest address 0.
     * @since api 1
     */
    void* (*guest)(uint32_t vaddr);

    /**
     * @brief Inverse of guest().
     *
     * Interior pointers map to the corresponding interior vaddr.
     * @return The guest vaddr of a host pointer into RAM, the scratchpad or
     *         an arena allocation; 0 if the pointer names no guest address.
     * @since api 1
     */
    uint32_t (*guest_addr)(const void* host);

    /**
     * @brief Allocate guest-addressable memory.
     *
     * The arena is a bump allocator with no free: allocate at entry and
     * reuse. Tick context only: call it, and write through @p host_view,
     * only from entry, tick hooks, toggle hooks and the save-record
     * callbacks.
     *
     * The flags pick one of two pools by intent. Without
     * #OP_GALLOC_NO_DMA the block comes from the DMA-reachable pool
     * (14 MiB below physical 16 MiB), where anything the GPU or a libgpu
     * call links must lie, since packet and ordering-table links hold 24
     * bits of address. With it the block comes from the general pool
     * (480 MiB above physical 16 MiB); pass it for every block nothing
     * truncates to 24 bits. A full pool fails the call and never spills
     * into the other. The largest block is 9 MiB in the DMA-reachable
     * pool and 480 MiB in the general pool.
     *
     * Contents are zero on first allocation. On reload the allocation
     * sequence is replayed: the i-th call with the same size, alignment
     * and flags returns the same vaddr and view, holding the bytes as
     * last written. Initialise what needs initialising.
     *
     * The vaddr and host view are stable for the process lifetime, and
     * the bytes are reachable through every engine guest access path
     * (`cpu->read_*`, `cpu->write_*`, guest()).
     *
     * Unless flagged #OP_GALLOC_IMMUTABLE the bytes are timeline state:
     * savestates, rewind, runahead and process handoff carry and restore
     * them like guest RAM. An allocation made after a snapshot keeps its
     * contents when that snapshot is restored; the cursor never rewinds.
     * Keep mutable per-session state here rather than in host statics,
     * and store guest vaddrs rather than host pointers, since carried
     * bytes cross process boundaries at handoff.
     *
     * Snapshot cost is per used byte, the same in both pools and for
     * #OP_GALLOC_IMMUTABLE blocks: a pool's used range runs from its base
     * to the end of its last block. A file savestate writes the used
     * ranges; runahead copies them out and back each tick; a rewind
     * keyframe copies them once and then keeps only the 4 KiB pages that
     * differ from the previous keyframe, so bytes rewritten every tick
     * cost rewind their pages per second of window.
     *
     * Not promised: the arena's base or layout (never hardcode a vaddr; a
     * savestate whose allocation sequence does not prefix-match the live
     * session is refused, naming the mod and entry), survival of
     * @p host_view across handoff, or any particular snapshot mechanism.
     *
     * Model vertex streams referenced from animation frame words are
     * parked here too; the frame word carries a handle from
     * vstream_handle(), not an address.
     *
     * @param size       Bytes to allocate; 0 or above the pool's largest
     *                   block fails.
     * @param align      Power of two; 0 = 8.
     * @param flags      `OP_GALLOC_*`. An unknown flag fails the call and
     *                   logs the flag.
     * @param host_view  Receives a host alias of the same bytes; NULL on
     *                   failure.
     * @return The guest vaddr; 0 = exhausted, unavailable or invalid. An
     *         exhausted pool logs the request and every mod's use of the
     *         pool.
     * @since api 1
     */
    uint32_t (*guest_alloc)(openpete_mod_t* self, uint32_t size,
                            uint32_t align, uint32_t flags,
                            void** host_view);
    /** @} */

    /** @name Lifecycle
     * @{ */
    /**
     * @brief printf-style logging, tagged `mod:<id>`.
     * @param level `OP_MOD_LOG_*`.
     * @since api 1
     */
    void (*log)(openpete_mod_t* self, int level, const char* fmt, ...);

    /**
     * @brief Per-mod persistent data directory (`mods/<id>/data`, created on first call).
     * @return An engine-owned absolute path; use plain stdio inside it.
     * @since api 1
     */
    const char* (*data_dir)(openpete_mod_t* self);

    /**
     * @brief Register the live-toggle callback.
     *
     * Runs at the tick boundary after each flip of this mod's enable
     * state. The engine freezes a disabled mod's registrations but never
     * rewinds guest state, so a mod that wrote guest state restores the
     * stock bytes on `on = 0`. One registration per mod; re-registering
     * replaces.
     * @return 0.
     * @since api 1
     */
    int (*register_toggle_hook)(openpete_mod_t* self,
                                openpete_mod_toggle_fn fn);
    /** @} */

    /** @name Settings
     * Read a value from `mods/<id>/config.toml`. Keys are dotted paths:
     * "chests.collision" is key `collision` in table `[chests]`. @p def is
     * returned when the file, the key or a type-compatible value is
     * absent. The file is parsed once, before entry runs; when the user
     * commits a change in the Mods panel (rendered from the manifest's
     * `[[config]]` rows) the engine rewrites config.toml and reloads the
     * mod, so read config at entry. Coercion: bools read as 0/1 through
     * config_int and config_float; ints read through config_float; floats
     * truncate through config_int; strings never coerce.
     * @{ */
    /** @brief Read a bool. @since api 1 */
    int     (*config_bool)(openpete_mod_t* self, const char* key, int def);
    /** @brief Read an integer. @since api 1 */
    int64_t (*config_int)(openpete_mod_t* self, const char* key,
                          int64_t def);
    /** @brief Read a float. @since api 1 */
    double  (*config_float)(openpete_mod_t* self, const char* key,
                            double def);
    /**
     * @brief Read a string.
     *
     * Copies at most `cap-1` chars plus NUL into @p buf.
     * @return 1 if the key was present, 0 if the default was copied.
     * @since api 1
     */
    int     (*config_str)(openpete_mod_t* self, const char* key,
                          const char* def, char* buf, int cap);

    /**
     * @brief Print a status line into this mod's section of the Mods panel.
     *
     * Legal from any context; host state only. Lines accumulate within a
     * tick and persist until the next tick in which the mod calls
     * ui_status again, so a line printed once stays visible and a per-tick
     * printer repaints its block each tick. The block truncates past about
     * 24 lines. Custom panels use openpete_mod_ui.h, or openpete_imgui.h
     * for the whole ImGui C API.
     * @since api 1
     */
    void (*ui_status)(openpete_mod_t* self, const char* fmt, ...);
    /** @} */

    /** @name Present hooks and screen-space draws
     * @{ */
    /**
     * @brief Register a per-present hook.
     *
     * Runs once per scene the engine extracts (the canonical one each tick
     * plus every sub-tick scene at a higher `--render-fps`). The hook
     * fires when its scene is extracted on the sim thread, ahead of
     * display, and the engine may produce fewer scenes than display
     * slots; call count and timing are not part of the contract.
     * `ctx->alpha` is the animation moment the scene shows and `ctx->dt`
     * the spacing to the previous scene, so a curve evaluated at
     * `ctx->alpha` is correct under any cadence. Publish state from tick
     * hooks into your own variables and evaluate it here; the engine never
     * interpolates mod visuals.
     *
     * Render-pure, enforced: guest access, game calls, base() and override
     * registration are refused inside a present hook, since presents are
     * non-deterministic in count and timing.
     *
     * Runs only when something is presented: a plain headless run never
     * calls it, a headless run that takes a screenshot does.
     * @since api 1
     */
    int (*register_present_hook)(openpete_mod_t* self,
                                 openpete_mod_present_fn fn);

    /**
     * @brief Draw text in overlay space on the native renderer.
     *
     * Overlay space: units of window height, origin top-left, +y down, x
     * in [0, aspect], y in [0, 1]. Host state only. Immediate mode: a call
     * from tick context shows for all of that tick's presents (redraw
     * every tick); a call from a present hook shows for that present.
     * Glyph set: digits, A-Z (lowercase folds up), space, and
     * `% / ? + ^ . ' :`, drawn with the game's own gold text glyphs; text
     * appears once the game has loaded the glyph font. Headless and
     * PsyCross-only runs consume nothing.
     *
     * The command stores the string in a fixed 96-byte field, so a
     * longer one is truncated to 95 characters.
     * @param cap_h  Glyph cap height in overlay units.
     * @param rgb    Glyph tint (0x605010 = HUD gold).
     * @since api 1
     */
    void (*draw_text)(float x, float y, float cap_h, uint32_t rgb,
                      const char* str);
    /** @} */

    /** @name Post-process shaders
     * @{ */
    /**
     * @brief Register a GLSL shader by path relative to the mod directory.
     *
     * The extension selects the stage (e.g. "shaders/crt.frag"). The
     * engine compiles it to SPIR-V on load (in-process glslang, Vulkan
     * 1.0 dialect), cached by content hash under `.build/shaders/`. A
     * prebuilt `<path>.spv` next to a missing source loads instead if a
     * `<path>.spv.ihash` sidecar matches the current shader-interface
     * hash. `#include "openpete_shader_api.glsl"` resolves to the SDK
     * interface header. Entry or tick context. Sources join the
     * hot-reload watch.
     * @return A shader handle >= 0, or -1 for a missing file or compile
     *         error (see `.build/shaders/<name>.log`; the mod still loads).
     * @since api 1
     */
    int (*shader_register)(openpete_mod_t* self, const char* path);

    /**
     * @brief Register a fullscreen fragment pass at an injection point.
     *
     * Pass order across mods is enabled-list priority; @p order breaks
     * ties within a mod (lower runs first). The pass samples the previous
     * pass (set 0) and receives the engine UBO (time, tick, alpha,
     * resolution; see `shaders/openpete_shader_api.glsl`) plus the mod's
     * uniform block. Entry or tick context.
     * @param frag_shader  Handle from shader_register().
     * @param point        `OP_POSTFX_*`.
     * @return An fx handle >= 0, registered disabled, or -1.
     * @since api 1
     */
    int (*postfx_register)(openpete_mod_t* self, int frag_shader,
                           int point, int order);
    /** @brief Enable or disable a pass. Any context. @since api 1 */
    int (*postfx_enable)(openpete_mod_t* self, int fx, int enabled);
    /**
     * @brief Set a pass's uniform block.
     *
     * Copies @p data on call. Any context; push from tick or present hooks.
     * @param size At most 1024 bytes.
     * @since api 1
     */
    int (*postfx_set_uniforms)(openpete_mod_t* self, int fx,
                               const void* data, uint32_t size);
    /** @} */

    /** @name Audio
     * @{ */
    /**
     * @brief Substitute the sample behind a named game sound.
     *
     * The code form of dropping `<name>.wav` into this mod's `assets/sfx`.
     * The swap happens at the voice's PCM upload, below the SPU RAM image
     * and voice mirror, so guest RAM, the gameplay rand() stream and the
     * tick on which the game sees the sound end (it polls the stock
     * sample's duration) stay stock; a longer replacement rings out past
     * that tick. Sounds without a SoundTable name (a moby's own m_Sounds
     * def) are unreachable here; a hash-named file in `assets/sfx/`
     * covers every sample and needs no code.
     * @param name        A game sound name.
     * @param adpcm_body  Raw SPU-ADPCM frames (no VAG header, final block
     *                    end-flag 0x01); decoded and copied by the engine.
     * @param pitch       SPU units (0x1000 = 44.1 kHz), overriding the
     *                    game's pitch writes; 0 keeps the game's own pitch.
     * @return 0, or -1 when the name is empty or 64 bytes or longer, len is
     *         under 16 or over 1 MiB, the body does not decode, or
     *         allocation fails. A name no game sound uses is accepted and
     *         never plays.
     * @since api 1
     */
    int (*sfx_replace)(openpete_mod_t* self, const char* name,
                       const void* adpcm_body, uint32_t len,
                       uint16_t pitch);
    /**
     * @brief Play `assets/sfx/own/<name>.wav` from this mod's directory as
     *        a host-side one-shot.
     *
     * Never routed through the SPU, so it is legal from tick and present
     * hooks. A WAV is decoded on its first play and the buffer is then
     * retained for the rest of the session; the cache has no eviction,
     * so a mod that plays many distinct files holds all of them.
     * @param gain 0..4; 1.0 = as authored.
     * @return 0, or -1 for a missing file or no audio device (logged).
     * @since api 1
     */
    int (*sfx_play)(openpete_mod_t* self, const char* name, float gain);
    /** @} */

    /** @name Input bindings
     * @{ */
    /**
     * @brief Read a `[[binding]]` declared in this mod's mod.toml.
     *
     * Declared bindings (`{name, key, purpose}`) are conflict-checked
     * across the enabled set at load, and the user rebinds through
     * openpete.toml `[keys.mod.<id>]`. The `key` spelling is the keymap's:
     * a keyboard name, a gamepad source ("pad:north", "pad:-lefty"), a
     * mouse button ("mouse:right"), a comma-separated list, or "none".
     * Wheel notches and mouse motion are not bindable here (this is a
     * level read); a key that fails to parse is logged at load and reads
     * 0. The read goes through the keymap, so it is inert while the dev
     * overlay owns the keyboard and while a press has been consumed by the
     * overlay's Escape dismiss, like every host action. Reading host input
     * in a tick hook diverges gameplay from a replay by design;
     * display-only reads belong in present hooks.
     * @return 1 while any source of the binding is held; an undeclared
     *         name logs an error and returns 0.
     * @since api 1
     */
    int (*binding_down)(openpete_mod_t* self, const char* name);
    /** @} */

    /** @name Game data and texture packs
     * @{ */
    /**
     * @brief Read one level's raw Data blob from the mounted disc.
     *
     * Lets a mod use another level's sky, palette or geometry without
     * shipping derived data.
     * @param buf  Destination of @p cap bytes; NULL queries the length.
     * @return The blob length; #OPENPETE_MOD_ERR_UNSUPPORTED on a game
     *         that serves no level data; -1 for an unknown id or a failed
     *         read.
     * @since api 2
     */
    int (*read_level_data)(openpete_mod_t* self, int level_id,
                           void* buf, uint32_t cap);

    /**
     * @brief Retarget the texture-pack fog ambience.
     *
     * Fogged replacement texels blend toward `scale*stock+offset`
     * (offsets in 0..255 units) instead of the stock ladder colours, for
     * packs whose art re-grades the level. NULL/NULL restores stock.
     * Per-texture grade fits take precedence where they have signal.
     * Display only.
     * @since api 2
     */
    int (*texpack_fog_shift)(openpete_mod_t* self, const float scale_rgb[3],
                             const float offset_rgb[3]);
    /** @} */

    /** @name Materials
     * @{ */
    /**
     * @brief Attach a fragment shader to a set of geometry.
     *
     * The set is chosen by a render channel and that channel's keys (moby
     * class, terrain texture or tier, channel-local identity, level).
     * Build the selector with openpete_mod_material_sel_any() and narrow
     * only what you mean; the engine refuses a selector it cannot fully
     * honour rather than matching wider than asked.
     *
     * The shader reshades within the draw group the geometry already
     * belongs to: it cannot change blending, depth policy or draw order.
     * Unmatched or failed builds keep stock shading; a material cannot
     * make geometry invisible.
     *
     * A channel the running game does not serve (see the game's channel
     * list in the modding docs) is refused with one log line, as a
     * `[[material]]` row on that channel is skipped.
     * @param shader A frag handle from shader_register().
     * @return A material handle, registered enabled, or -1.
     * @since api 3
     */
    int (*material_register)(openpete_mod_t* self,
                             const openpete_mod_material_selector_t* sel,
                             int shader);

    /**
     * @brief Turn one registered material on or off.
     *
     * Callable from any context; the natural target of a `[[config]]`
     * toggle.
     * @since api 3
     */
    int (*material_enable)(openpete_mod_t* self, int material, int on);

    /**
     * @brief Set the uniform block the material's shader reads.
     *
     * May be called per tick; for time-varying effects prefer the engine
     * UBO's time in the shader.
     * @param block std140, at most #OPENPETE_MATERIAL_UNIFORMS_MAX bytes.
     * @since api 3
     */
    int (*material_set_params)(openpete_mod_t* self, int material,
                               const void* block, uint32_t len);

    /**
     * @brief material_register() with a per-instance refine callback.
     *
     * The engine calls @p refine once per candidate instance during
     * extraction and it returns that instance's uniform block, or -1 for
     * "not mine" (openpete_mod_refine_fn). Instances with byte-identical blocks share one slot
     * and one batch, so a callback with few distinct outcomes is cheap and
     * one with a distinct block per instance costs a draw each. Scope the
     * selector tightly and let the callback decide the rare cases.
     *
     * Available on the moby channel (keyed per instance) and on terrain
     * (keyed per texture id or the untextured band, asked once per tick);
     * on other channels register declaratively and drive the material
     * with material_set_params(). A refine that cannot take effect is
     * refused with one log line and -1 (an engine below api 13 registered
     * it and never asked it): a channel other than moby, terrain or -1, a
     * decal-contract shader, and a terrain selector with neither a texture
     * key nor #OP_SEL_UNTEXTURED. A channel -1
     * refine takes effect on mobys and on keyed terrain. A channel the
     * running game does not serve is refused as material_register()
     * refuses it.
     * @since api 4
     */
    int (*material_register_refine)(openpete_mod_t* self,
                                    const openpete_mod_material_selector_t* sel,
                                    int shader, openpete_mod_refine_fn refine);

    /**
     * @brief Point a live material at a different selector.
     *
     * Keeps its handle, shader, refine callback and enabled state.
     * Selection is constant across a present, so a call from a present
     * hook or UI section takes effect at the next tick; from entry or a
     * tick hook it applies at once. The material keeps shading under its
     * old key until the new one applies. The selector is validated as
     * material_register() validates it, and a refine material's selector
     * as material_register_refine() refuses one; a refused selector, or a
     * channel the running game does not serve, returns -1 and the old
     * selector stays in force.
     * @since api 6
     */
    int (*material_set_selector)(openpete_mod_t* self, int material,
                                 const openpete_mod_material_selector_t* sel);

    /**
     * @brief The moby classes the current level's load placed in the model table, ascending.
     *
     * Level-scoped, not visibility-scoped: a superset of the classes with
     * a live instance at this instant, and the set worth offering a user
     * to choose from. On Spyro 3, whose model table keeps the rows of
     * earlier levels, the set is the classes of the load's own model
     * lists; on Spyro 2, whose table also keeps them and whose loads keep
     * no list, it is the classes of the level's moby array that have a
     * model row, spawned ones included. Class ids run to 511 on Spyro 1
     * and 767 on Spyro 2 and 3; at most
     * @p cap entries are copied. Reads an engine-side snapshot refreshed
     * once per tick, so it is safe from a UI section. Empty before the
     * first level finishes loading.
     * @param out  Destination of @p cap entries; `out = NULL, cap = 0`
     *             sizes the buffer.
     * @return The count; #OPENPETE_MOD_ERR_UNSUPPORTED on a game that
     *         serves no moby class census.
     * @since api 6
     */
    int (*moby_classes)(openpete_mod_t* self, uint16_t* out, int cap);
    /** @} */

    /** @name Vertex streams
     * @{ */
    /**
     * @brief A stream handle for a vertex stream parked in the arena.
     *
     * The handle is the value to write into an animation frame word's
     * 21-bit vertex-stream field (bits 0-20) in place of an address that
     * does not fit. The game's readers resolve it to the full vaddr, so a
     * stream may live anywhere guest_alloc() puts it. The same vaddr
     * always yields the same handle; handles are below 0x8000 and never
     * 0. The game stores frame-word addresses halved; a handle is written
     * as-is in both encodings.
     *
     * Ask at entry, in a fixed order, once per stream: every call is
     * ledgered like an allocation, replayed on reload, and verified when
     * a savestate loads, so a sequence that depends on the order levels
     * were visited makes a savestate from another route refuse to load.
     * Tick context only.
     *
     * Spyro 1 only. Spyro 2 and 3 frame words carry a halfword offset from
     * the animation header instead of a stream field, so a mod moves a
     * stream there through the header's pointers. A mod built for Spyro 2
     * or 3 does not compile a use of this member, on any compiler; an
     * any-game object gets 0 and one log line.
     * @return The handle, or 0 for vaddr 0, a kernel-area vaddr, an
     *         exhausted table, or a game without the stream field.
     * @since api 9
     */
#if OPENPETE_VSTREAM_SLOT_RESERVED
    uint32_t (*vstream_handle_reserved_slot)(openpete_mod_t* self,
                                             uint32_t stream_vaddr);
#else
    uint32_t (*vstream_handle)(openpete_mod_t* self, uint32_t stream_vaddr)
        OPENPETE_VSTREAM_ATTR;
#endif
    /** @} */

    /** @name Notifications
     * @{ */
    /**
     * @brief Post a transient on-screen notification.
     *
     * Drawn in the game's gold-bordered chrome at the anchor the player
     * chose. ui_status() reports state; notify reports an event.
     *
     * Legal from entry, tick hooks and present hooks; host state only.
     * Rate-limited per mod (a small burst, then a slow refill). A no-op
     * on runahead and rewind-replay ticks, whose effects are rolled back
     * or re-executed; a mod therefore sees fewer toasts than calls.
     *
     * Glyph set: 0-9, A-Z (lowercase folds up), space, and
     * `% / ? + ^ . ' :`. Any other character renders as a space.
     * A long text wraps at spaces onto at most 10 lines; when text
     * remains, or the text exceeds 255 bytes, the last line ends in "...".
     *
     * @param key         May be NULL. Coalesces: a live toast with the
     *                    same key is repainted with the new text and a
     *                    restarted timer instead of a second toast
     *                    stacking. The engine prefixes the mod id, so keys
     *                    never collide across mods.
     * @param timeout_ms  0 takes the player's `--osd-timeout` default;
     *                    values above 30000 clamp to it. A repaint takes
     *                    the repainting call's timeout.
     * @since api 8
     */
    void (*notify)(openpete_mod_t* self, const char* key,
                   uint32_t timeout_ms, const char* fmt, ...);
    /** @} */

    /** @name Mouse
     * @{ */
    /**
     * @brief Read the host mouse.
     *
     * Position, buttons and the deltas since this mod's previous read from
     * the same hook kind: tick hooks and present hooks keep separate
     * markers, so one cadence never steals the other's motion. While an
     * overlay owns the mouse (`owned_by_overlay`) buttons and deltas read 0
     * and the markers still advance. From a tick hook during runahead
     * speculation or rewind replay the deltas are returned without
     * advancing the marker, so the committed tick sees the whole motion.
     * Reading host input in a tick hook diverges gameplay from a replay by
     * design, as binding_down does.
     * @param out  `out->struct_size` must be `sizeof(openpete_mod_mouse_t)`.
     * @return 0, or -1 for a wrong struct_size or a channel that is not
     *         initialised.
     * @since api 12
     */
    int (*mouse_read)(openpete_mod_t* self, openpete_mod_mouse_t* out);
    /**
     * @brief Request relative mouse mode (hidden, unbounded pointer).
     *
     * This mod's request bit; the engine applies relative mode while any
     * owner (a mod, the free camera) requests it and no overlay owns the
     * mouse, releasing it under the settings menu and the dev overlay
     * without dropping the request. Cleared when the mod is disabled or
     * unloaded. Host state only; legal from any context.
     * @return 0.
     * @since api 12
     */
    int (*mouse_capture)(openpete_mod_t* self, int on);
    /** @} */

    /** @name Pads
     * @{ */
    /**
     * @brief Read one pad slot's bytes for the tick being executed.
     *
     * Slot 0 is player 1, the slot the retail game reads. Slots 1-3 are
     * host-only: nothing writes them into guest RAM, so a mod that wants
     * extra players decodes `libpad` into a pad buffer it allocates
     * itself. The source is the input-log record staged for this tick, so
     * the read reproduces under bk2 replay and under the rewind burst,
     * which restages every slot from the log. A speculative runahead tick
     * reads the last committed tick's record and reports that tick's
     * `vbl`.
     *
     * A slot with no device assigned reads as the canonical empty port and
     * is reported once per mod in the log. A bk2 movie carries two
     * players, so slots 2 and 3 read absent on any replay.
     *
     * Tick hooks only: a present hook is refused (reported once per mod),
     * because the staged record belongs to the sim thread. Read in a tick
     * hook and display from your own copy.
     * @param slot 0..#OPENPETE_MOD_PAD_SLOTS-1.
     * @param out  `out->struct_size` must be `sizeof(openpete_mod_pad_t)`.
     * @return 0, or -1 for a wrong struct_size, a slot out of range, or a
     *         call from a present hook.
     * @since api 12
     */
    int (*pad_read)(openpete_mod_t* self, uint32_t slot,
                    openpete_mod_pad_t* out);

    /**
     * @brief Drive player @p slot's rumble motors.
     *
     * The two arguments are libpad's actuator bytes, the same pair the
     * game writes into its own actuator buffer: @p small_byte is the
     * small motor's on/off byte (bit 0) and @p large_byte the large
     * motor's 0..255 byte. One byte-to-motor conversion serves every
     * slot, carrying the user's vibration strength setting, so a mod
     * neither needs a second mapping nor can bypass the setting.
     *
     * Slots 1..#OPENPETE_MOD_PAD_SLOTS-1. Slot 0 is refused (reported
     * once per mod): it is the game's own actuator table, and a second
     * writer would fight it.
     *
     * Level-triggered. The pair persists, re-issued to the device with a
     * dead-man refresh, until the next call for that slot or until the
     * mod is disabled or unloaded, either of which silences the slots the
     * mod drove. Publish `{0, 0}` to stop.
     *
     * A speculative runahead tick and a rewind-burst replay accept the
     * call and do nothing: those timelines were never lived, and buzzing
     * for them is a side effect escaping the re-execution. A tick hook
     * therefore needs no speculation test of its own. The host's own
     * refusals still apply at drive time - headless, an unfocused window,
     * pause, a strength of 0.
     *
     * To carry a swapped-in player's rumble, decode that player's timers
     * the way the game decodes player 1's (normal `{1,120}`, electric
     * `{1,0}`, custom `{0,amount}`, else `{0,0}`, timers decremented once
     * per VBL) in a `PadVSync` post-hook and pass the result here. The
     * game's own decode is not re-entrant per player, so it cannot be
     * called for them.
     *
     * @param slot        1..#OPENPETE_MOD_PAD_SLOTS-1.
     * @param small_byte  small motor, on/off (bit 0).
     * @param large_byte  large motor, 0..255.
     * @return 0, or -1 for slot 0, a slot out of range, or a call from a
     *         present hook.
     * @since api 12
     */
    int (*pad_rumble)(openpete_mod_t* self, uint32_t slot,
                      uint32_t small_byte, uint32_t large_byte);
    /** @} */

    /** @name Disc
     * @{ */
    /**
     * @brief The disc image this session booted, or NULL when none is open.
     *
     * The path the engine itself opened, so it is correct in every layout:
     * an explicit `--disc`, a library entry picked in the splash, an
     * ingested image, portable or user data directory. A mod that reads
     * sectors wants @p want_bin: the cue names no sectors.
     *
     * The string is owned by the engine and stays valid for the session.
     * Legal from any context.
     * @param want_bin Non-zero for the sector image (`.bin`), zero for the
     *                 path as given (the `.cue` where one was named).
     * @return The path, or NULL when no disc is open or the sector image
     *         behind the cue cannot be resolved.
     * @since api 12
     */
    const char* (*disc_path)(openpete_mod_t* self, int want_bin);
    /**
     * @brief Read one raw 2352-byte sector of a game's disc by LBA.
     *
     * @p game is the running game's id (`api->game_id(self)`), or a game
     * the manifest names in `requires_games` or `optional_games`
     * (#OPENPETE_MOD_GAME_SPYRO1 and its siblings, or the raw string).
     * The running game's disc is the booted one, read through the engine's
     * own reader: served from the disc cache when the sector is resident,
     * from the image file otherwise, never reordering the cache's loading.
     * Another game's disc is its library entry, any release of that game,
     * read from the image file alone: the read never touches the booted
     * disc's cache or its loading order. Sector 0 is the image's first
     * sector; a Mode 2 Form 1 sector's 2048 user-data bytes start at
     * byte 24. Legal from any context.
     *
     * A game named in neither list is refused, with one log line per mod
     * naming the game; a listed game with no disc in the library returns
     * -1 without a line (query `game_installed`).
     * @param game The game id.
     * @param lba The sector.
     * @param buf Receives 2352 bytes.
     * @return 0, or -1 when @p game is NULL, refused or has no disc, @p buf
     *         is NULL or the sector lies past the end of the image.
     * @since api 13
     */
    int (*read_sector)(openpete_mod_t* self, const char* game, uint32_t lba,
                       uint8_t* buf);
    /** @} */

    /** @name Overlay-scoped overrides
     * @{ */
    /**
     * @brief Attach an override to the function at a PSX vaddr in one overlay.
     *
     * Every overlay of a game loads at one vaddr window, so an address
     * inside it names one function per overlay. The override runs only
     * while @p overlay_id is the resident overlay; while another overlay
     * is resident the call runs that overlay's function untouched. Id 0
     * is the EXE. The generated `OP_OVLID_<name>` of an overlay function
     * is its id, and its `OP_REGISTER_OVERRIDE_<name>` wrapper binds
     * through this entry.
     *
     * Refused, binding nothing and logging the mod, id, address and game:
     * an id that is neither 0 nor in the game's overlay table; id 0 with
     * an address inside the overlay window; a nonzero id with an address
     * outside it; an address that is not a function entry of that
     * overlay.
     *
     * A mod pinning `[mod] api` at 13 or later binds every overlay-window
     * address through this entry or through `override_name` on a name of
     * one overlay: `override_addr` at a window address, and
     * `override_name` on a raw `func_XXXXXXXX` name whose rows lie in
     * several overlays, are refused.
     * @param overlay_id The overlay id, or 0 for the EXE.
     * @param addr The function's vaddr.
     * @return 0 on success, -1 when refused.
     * @since api 13
     */
    int (*override_ovl)(openpete_mod_t* self, uint32_t overlay_id,
                        uint32_t addr, openpete_mod_override_fn fn);
    /** @} */

    /** @name Game-neutral services
     * The running game's identity, its gameplay state, the player's pose
     * and the level, through each game's own reader, so one source reads
     * them on every game without a per-game header.
     *
     * A call with a non-NULL `cpu` from tick context (entry, an override,
     * a toggle hook) reads guest RAM through it at the call. A call with
     * `cpu` NULL, and every call from a present hook or a UI section,
     * returns the record the engine sampled at the last canonical tick
     * boundary: the state after that tick, the state every present of the
     * tick shows. Before the first boundary that record reads as no level
     * live.
     * @{ */
    /**
     * @brief The running game's id: "spyro-1", "spyro-2" or "spyro-3".
     *
     * The id a manifest's `game` key names. Legal from any context.
     * @return A string owned by the engine, valid for the session.
     * @since api 13
     */
    const char* (*game_id)(openpete_mod_t* self);
    /**
     * @brief Whether gameplay is live: in a level, not booting, loading or on the title screen.
     *
     * True in the attract demo's levels.
     * @param cpu The tick hook's CPU, or NULL for the sampled record.
     * @return 1 while live, 0 otherwise.
     * @since api 13
     */
    int (*gameplay_live)(openpete_mod_t* self, CPUState* cpu);
    /**
     * @brief Read the player's position, facing and level while a level is live.
     *
     * The game's player-object position and the level id word, read
     * during the attract demo as during play; `placeable` tells the two
     * apart.
     * @param cpu The tick hook's CPU, or NULL for the sampled record.
     * @param out `struct_size` set by the caller; filled on success.
     * @return 0 on success; -1, writing nothing, for a wrong
     *         `struct_size` or while no level is live.
     * @since api 13
     */
    int (*player_pose)(openpete_mod_t* self, CPUState* cpu,
                       openpete_mod_pose_t* out);
    /**
     * @brief The id of the level being played.
     *
     * The game's own level number: Spyro 1 `g_LevelId` (10 = Artisans),
     * Spyro 2 the level the loader loaded (10..66), Spyro 3
     * `currentLevel`. Each game numbers its levels differently.
     * @param cpu The tick hook's CPU, or NULL for the sampled record.
     * @return The id, or -1 while no level is live.
     * @since api 13
     */
    int (*level_id)(openpete_mod_t* self, CPUState* cpu);
    /** @} */

    /** @name Other games' discs
     * A mod reads another game's disc when its manifest names that game
     * in `requires_games` (the mod is refused at load while the game has
     * no disc in the library) or `optional_games` (the mod loads; the
     * calls for the game fail until a disc is added). `read_sector` reads
     * sectors of either.
     * @{ */
    /**
     * @brief The disc image of a game: the booted one for the running game, the library's for another.
     *
     * The `disc_path` of @p game: for the running game the same string
     * `disc_path` returns. Another game resolves to its library entry,
     * the most recently played one when the library holds several, any
     * release of that game. Refused like `read_sector` for a game the
     * manifest names in neither list. The string is owned by the engine
     * and stays valid for the session. Legal from any context.
     * @param game The game id.
     * @param want_bin Non-zero for the sector image (`.bin`), zero for the
     *                 path as given (the `.cue` where one was named).
     * @return The path, or NULL when @p game is NULL, refused or has no
     *         disc, or the sector image cannot be resolved.
     * @since api 13
     */
    const char* (*game_disc_path)(openpete_mod_t* self, const char* game,
                                  int want_bin);
    /**
     * @brief Whether a game is installed: the running game, or a game with a disc in the library.
     *
     * Answers for any game id, listed in the manifest or not, from the
     * library as it stood when mods loaded. Legal from any context.
     * @param game The game id.
     * @return 1 when installed, 0 otherwise or for NULL.
     * @since api 13
     */
    int (*game_installed)(openpete_mod_t* self, const char* game);
    /** @} */

    /** @name Mod-owned textures
     * @{ */
    /**
     * @brief Register or replace a mod-owned RGBA8 texture under a 16-bit id.
     *
     * The code form of `assets/textures/own/<hexid>.png`, for pixels the
     * mod builds at run time. A model face selects the texture with the
     * PSX-reserved Tiledef encoding: tpage mode bits (attr >> 7 & 3) = 3
     * and the id in the CLUT halfword; its UVs are texel coordinates into
     * the registered image. The native renderer draws the face through
     * the replacement pipeline; the PSX path has no mode 3 and samples
     * VRAM noise there. Nothing reaches guest RAM or VRAM.
     *
     * The pixels are copied, so @p rgba may be freed on return.
     * Re-registering an id replaces its image for every face that selects
     * it, whichever mod registered it before; the superseded image stays
     * allocated for the session, so register at entry or once per asset
     * rather than per tick. A face selecting an id with no registration
     * draws nothing. The registration outlives the mod's disable, since
     * the faces selecting it are the mod's guest writes and revert with
     * them. Legal from any context, including a thread the mod starts:
     * the renderer sees either the previous image or the whole new one.
     * @param id The texture id the faces carry in the CLUT halfword.
     * @param rgba @p w * @p h pixels, 4 bytes each, rows top to bottom.
     * @param w Width, 1 to 1024.
     * @param h Height, 1 to 1024.
     * @return 0, or -1 for a NULL @p rgba, a size out of range, or an
     *         allocation failure.
     * @since api 13
     */
    int (*texture_register)(openpete_mod_t* self, uint16_t id,
                            const void* rgba, int w, int h);
    /** @} */

    /** @name Shutdown
     * @{ */
    /**
     * @brief Register the exit callback that stops the mod's own threads.
     *
     * The engine runs the callback once per mod whose entry ran, enabled
     * or disabled, on each exit it takes deliberately: a quit from the
     * menu or a binding, a closed window, SIGINT or SIGTERM, the end of
     * `--frame-limit`, `--max-runtime-seconds` or `--record-frames`, a
     * finished `--psxtrace`, the game returning, and a setup error after
     * mods loaded. It runs on the sim thread after the last tick, with
     * the present thread stopped, in reverse load order (a mod's hook
     * before the hooks of the mods it depends on), and before the engine
     * tears down the renderer, the texture registry, the mod registries
     * or audio. A hot reload runs the outgoing object's callback before
     * the new object's entry, so a reloaded mod's threads stop as well.
     *
     * Inside the callback every entry that tick context allows may be
     * called, and texture_register and log are the expected ones.
     * Registrations made there never run: no tick, present or UI body
     * follows. The callback sets the flag its threads poll, joins each
     * thread, and returns.
     *
     * When the callback returns, every api entry the mod calls from any
     * thread is refused: it does nothing and returns the entry's failure
     * value (-1, 0 or NULL; a config read returns its default), and the
     * first refusal logs one warning naming the mod and the entry. Once
     * every callback has returned the engine waits up to 1000 ms for api
     * calls already in progress on other threads to return, then tears
     * down.
     *
     * Budget: 500 ms per callback. A callback that runs longer logs a
     * warning with its time, and the engine keeps waiting for it. The
     * budget keeps a quit prompt for the player while leaving a worker
     * that checks its stop flag between units of work (one file decode,
     * one texture build) time to finish the unit in progress. The engine
     * cannot interrupt a callback: after a quit request from the menu, a
     * binding, the window or a signal, the default threaded host ends the
     * process when one callback runs 20 s without returning, and a
     * scripted end (`--frame-limit` and the others) waits for it.
     *
     * A crash, a hang the watchdog ends, a second SIGINT and any other
     * forced exit run no callback.
     * @param fn The callback, or NULL to clear it. One registration per
     *           mod; re-registering replaces.
     * @return 0, or -1 for a NULL self or a call from a present hook.
     * @since api 13
     */
    int (*register_shutdown_hook)(openpete_mod_t* self,
                                  openpete_mod_shutdown_fn fn);
    /** @} */

    /** @name Save records
     * @{ */
    /**
     * @brief Store per-save data beside each game save, in place of the save's spare bytes.
     *
     * The engine keeps one record per mod per save in `<card>.modsave`
     * beside the card image, which it never modifies. At each non-empty
     * save the game writes, @p write serializes the mod's state; the
     * record of a mod that is not loaded or is toggled off is carried
     * forward unchanged. When the game loads a save or starts a new
     * playthrough, @p read delivers that save's record with its status
     * (openpete_mod_save_status_t). Keep save-derived state in the arena:
     * savestates, rewind, runahead and hot reload restore it and deliver
     * no read. A mod's state at entry is the fresh-playthrough state.
     * Call at entry, once; a second call replaces the first.
     * @param schema The record format version, 1 or more; a record of a
     *        higher schema reads #OP_MOD_SAVE_NEWER and is never overwritten.
     * @param write Serializer, non-NULL.
     * @param read Deliverer, non-NULL.
     * @return 0, -1 for a NULL callback, a schema of 0, or a game with no
     *         save descriptor (Spyro 1 has one).
     * @since api 13
     */
    int (*save_register)(openpete_mod_t* self, uint32_t schema,
                         openpete_mod_save_write_fn write,
                         openpete_mod_save_read_fn read);
    /** @} */

    /** @name Anti-piracy scans
     * @{ */
    /**
     * @brief Read the state of one of Spyro 3's anti-piracy scans, as the engine's observer of them sees it.
     *
     * Spyro 3 hashes its own code with CRC checks: a boot walk, a title
     * walk, a walk during each level load, and per-level scanner mobys
     * that hash a few dozen bytes per update. The engine observes each
     * one without changing it and applies the game's own pass test at
     * the game's own comparison; this entry reports that verdict. Read
     * only. A scanner moby's key is its data block (the word at the
     * moby record's offset 0); the walks take the OP_SCAN_KEY_* keys. A
     * scanner's state is kept until the level's overlay is replaced.
     * Legal from tick context (overrides, refine callbacks).
     * @param key OP_SCAN_KEY_BOOT, OP_SCAN_KEY_TITLE, OP_SCAN_KEY_LOADING,
     *        or a scanner moby's data block.
     * @param out Receives the state; its `struct_size` must equal
     *        `sizeof(openpete_mod_scan_t)`.
     * @return 0 when @p out was filled; -1 for a NULL @p out, a wrong
     *         `struct_size`, or a data block whose scanner has not run
     *         this level (no step observed); #OPENPETE_MOD_ERR_UNSUPPORTED
     *         on Spyro 1 and 2.
     * @since api 13
     */
    int (*antipiracy_scan)(openpete_mod_t* self, uint32_t key,
                           openpete_mod_scan_t* out);
    /** @} */

    /** @name Buses
     * A bus is a name of the form `<name>/<major>` (a non-empty name, a
     * '/', and a decimal major of 1 or more), created by its first
     * `bus_attach`. Handlers walk in the mod entry sequence, newest entry
     * first: a mod's handlers go to the front at each of its entries
     * (boot, first enable, hot reload). A mod whose manifest declares
     * `sim_mutative = false` is refused `bus_attach`, `bus_call` and
     * `bus_call_to`; a keyless mod (its key is 0, collides with another
     * discovered mod's, or its id is 64 bytes or longer) is refused every
     * bus entry and `mod_key`, and refused at load with an `api` pin of 13
     * or more.
     * @{ */
    /**
     * @brief Attach the calling mod's handler to a bus.
     *
     * One handler per mod per bus: a second attach replaces the handler and
     * @p user in place and keeps the walk position. Another major of the
     * same name is another bus. Legal only in the entry point (boot or hot
     * reload); a hot reload strips the handlers and the new entry attaches
     * them again.
     * @param bus The bus name.
     * @param fn The handler; NULL returns -1.
     * @param user Passed to every call of @p fn.
     * @return 0, or -1 for a malformed name, a NULL argument, a call outside
     *         the entry point, or a refused mod (one log line per mod).
     * @since api 13
     */
    int (*bus_attach)(openpete_mod_t* self, const char* bus,
                      openpete_mod_bus_fn fn, void* user);
    /**
     * @brief Run the enabled handlers of a bus in walk order, the caller's own included.
     *
     * Legal on the sim thread from an override (or a bus handler reached
     * from one) and from a toggle hook; refused from the entry point,
     * present hooks, UI sections, refine callbacks, shutdown hooks,
     * save-record callbacks and threads a mod starts, whatever `CPUState`
     * the caller holds. The engine never writes the message.
     * @param cpu The caller's CPU state.
     * @param bus The bus name.
     * @param msg_addr Guest address of the message; `[msg_addr, msg_addr +
     *        size)` must be one contiguous host view (an arena block is).
     * @param size Message length; 0 is allowed.
     * @return The first positive handler result, which ends the walk;
     *         #OP_BUS_PASS when every handler passed; #OP_BUS_NONE with no
     *         enabled handler attached (a misspelt name included);
     *         #OP_BUS_DEPTH at 8 nested bus calls or a full frame stack;
     *         -1 when refused or for an invalid range.
     * @since api 13
     */
    int (*bus_call)(openpete_mod_t* self, CPUState* cpu, const char* bus,
                    uint32_t msg_addr, uint32_t size);
    /**
     * @brief Run one mod's handler on a bus and return its result.
     *
     * Contexts, range check and refusals as `bus_call`. The caller's own
     * key runs the caller's own handler.
     * @param mod_key The target's key.
     * @return The handler's result (a negative one as #OP_BUS_PASS);
     *         #OP_BUS_NONE when @p mod_key is not attached and enabled on
     *         @p bus (unloaded, disabled, another major, never attached);
     *         #OP_BUS_DEPTH and -1 as `bus_call`.
     * @since api 13
     */
    int (*bus_call_to)(openpete_mod_t* self, CPUState* cpu, const char* bus,
                       uint32_t mod_key, uint32_t msg_addr, uint32_t size);
    /**
     * @brief List the attached, enabled mods of a bus in walk order.
     *
     * Legal from any thread. Reads an immutable membership snapshot the
     * engine republishes at entries, toggles and reloads. Membership is not
     * participation: a protocol elects a leader from its own gather walk.
     * @param bus The bus name.
     * @param out Records to fill; `out[0].struct_size` sets the record size
     *        and the stride. NULL with @p cap 0 counts.
     * @param cap Records @p out holds.
     * @return The member count (records written: at most @p cap), or -1 for
     *         a malformed name, a `struct_size` below this api's record, or
     *         a refused mod.
     * @since api 13
     */
    int (*bus_members)(openpete_mod_t* self, const char* bus,
                       openpete_mod_bus_sender_t* out, int cap);
    /**
     * @brief Return the calling mod's own key, openpete_mod_key_of of its id.
     *
     * Legal from any context and thread.
     * @return The key, or 0 for a keyless mod.
     * @since api 13
     */
    uint32_t (*mod_key)(openpete_mod_t* self);
    /**
     * @brief Fill the sender record of the mod a key names.
     *
     * Covers every mod the loader discovered, enabled or not and on a bus
     * or not. Legal from any thread.
     * @param key The key.
     * @param out Record to fill; the caller sets `struct_size`.
     * @return 0, or -1 for an unknown key or a `struct_size` below this
     *         api's record.
     * @since api 13
     */
    int (*mod_info)(openpete_mod_t* self, uint32_t key,
                    openpete_mod_bus_sender_t* out);
    /** @} */

    /** @name Service tables
     * A service table is a struct of function pointers (and data) one mod
     * exports under a name and a major and other mods call directly, from
     * any thread the provider's own header permits. The engine copies the
     * table at export, never frees a copy and never unloads a mod's code,
     * so a pointer `svc_get` returned stays readable for the process; the
     * provider's state ends at its shutdown hook, so a provider gates every
     * function with an in-flight count and a live flag that its shutdown
     * hook clears and drains. A hot reload runs the old copy's shutdown
     * hook, removes its exports, then runs the new entry, which exports
     * again. A keyless mod is refused every service entry.
     * @{ */
    /**
     * @brief Export a copy of a table as the calling mod's provider of a service.
     *
     * Legal only in the entry point (boot or hot reload). The engine copies
     * @p size bytes; the caller may reuse its buffer afterwards. A second
     * export of one name and major by one mod replaces its copy in place
     * (its position in the provider order does not change). Providers of a
     * service are listed newest entry first, as bus handlers walk.
     * @param name The service name, 1 to 95 bytes.
     * @param major Its major, 1 or more; another major is another service.
     * @param table The bytes to copy; NULL returns -1.
     * @param size Their count; 0 returns -1.
     * @return 0, or -1 for a malformed argument, a call outside the entry
     *         point, a keyless mod or an allocation failure (one log line
     *         per mod and cause).
     * @since api 13
     */
    int (*svc_export)(openpete_mod_t* self, const char* name, uint32_t major,
                      const void* table, uint32_t size);
    /**
     * @brief Return the engine's copy of one provider's table for a service.
     *
     * Legal from any thread; one hash probe of an immutable snapshot the
     * engine republishes at entries, toggles and reloads, so a consumer may
     * look up on every use instead of caching. At entry it returns a table
     * only from a provider that has already entered, which a `deps` entry
     * on the provider guarantees.
     * @param provider_key The provider's key (openpete_mod_key_of of its id).
     * @param name The service name.
     * @param major The service major.
     * @param min_size The fewest table bytes the caller reads.
     * @return The table, or NULL when the provider is not loaded, is
     *         disabled, exports no such name or major, or exported fewer
     *         than @p min_size bytes; NULL for @p provider_key 0, for a
     *         keyless caller and for a malformed name.
     * @since api 13
     */
    const void* (*svc_get)(openpete_mod_t* self, uint32_t provider_key,
                           const char* name, uint32_t major, uint32_t min_size);
    /**
     * @brief List the keys of a service's enabled providers, newest entry first.
     *
     * Legal from any thread. A consumer that uses every provider lists once
     * and calls `svc_get` per key.
     * @param name The service name.
     * @param major The service major.
     * @param keys_out Keys to fill; NULL with @p cap 0 counts.
     * @param cap Keys @p keys_out holds.
     * @return The provider count (keys written: at most @p cap); 0 for a
     *         keyless caller; -1 for a malformed name or a negative @p cap.
     * @since api 13
     */
    int (*svc_list)(openpete_mod_t* self, const char* name, uint32_t major,
                    uint32_t* keys_out, int cap);
    /** @} */
} openpete_mod_api_t;

/**
 * @brief ABI descriptor every code object exports as `openpete_mod_abi`.
 *
 * Records the api version and api-struct size the object was compiled
 * against. The loader reads it before the entry point runs and refuses an
 * object built for a newer api or a larger struct than the engine has,
 * and an object with no descriptor. An older object loads, since the
 * struct is append-only from api 10.
 *
 * The engine adds the SDK's openpete_mod_abi.c to every compile line, so
 * a mod's own sources need not define the descriptor. A hand-built object
 * compiles that file in, or puts #OPENPETE_MOD_ABI_DEFINE in exactly one
 * of its sources.
 */
typedef struct openpete_mod_abi {
    uint32_t magic;       /**< #OPENPETE_MOD_ABI_MAGIC. */
    uint32_t api_version; /**< #OPENPETE_MOD_API_VERSION at the mod's compile time. */
    uint32_t api_size;    /**< `sizeof(openpete_mod_api_t)` at the mod's compile time. */
} openpete_mod_abi_t;
/** @brief 'OPAB'. */
#define OPENPETE_MOD_ABI_MAGIC 0x4F504142u
/* The game identity an object records as `openpete_mod_game`: "*" under
 * OPENPETE_MOD_ANY_GAME (every per-game header then refuses to compile),
 * the per-game SDK's OPENPETE_GAME_ID when openpete_game.h was found, and
 * no symbol when neither holds (a hand-built object compiled against
 * `sdk/` alone). The loader matches it against the running game. */
#if defined(OPENPETE_MOD_ANY_GAME)
#define OPENPETE_MOD_GAME_DEFINE_                                          \
    extern const char openpete_mod_game[];                                 \
    const char openpete_mod_game[] = "*";
#elif defined(OPENPETE_GAME_ID)
#define OPENPETE_MOD_GAME_DEFINE_                                          \
    extern const char openpete_mod_game[];                                 \
    const char openpete_mod_game[] = OPENPETE_GAME_ID;
#else
#define OPENPETE_MOD_GAME_DEFINE_
#endif
/** @brief Defines the ABI descriptor and the game identity; use in exactly one source of a hand-built object.
 *
 * Both are ordinary globals. A Windows object exports them through the
 * MinGW linker's export of every global, which one `dllexport` in the DLL
 * turns off; the engine's compile line passes `-Wl,--export-all-symbols`,
 * which keeps every global exported either way. */
#define OPENPETE_MOD_ABI_DEFINE()                                          \
    OPENPETE_MOD_GAME_DEFINE_                                              \
    const openpete_mod_abi_t openpete_mod_abi = {                          \
        OPENPETE_MOD_ABI_MAGIC, OPENPETE_MOD_API_VERSION,                  \
        (uint32_t)sizeof(openpete_mod_api_t) }

/** @name Typed guest accessors
 * One read and one write per width and signedness: `op_read_{u,s}{8,16,32}`
 * and `op_write_{u,s}{8,16,32}`. A read of an unmapped address returns 0;
 * a write to one is dropped. Alignment is the guest's: a 16-bit access on
 * an odd address or a 32-bit access off a 4-byte boundary is a bus error
 * on the PSX and is not something the game's data does.
 * @{ */

/** @brief Read an unsigned 32-bit value from guest address va.
 *
 * Returns 0 when api->guest(va) is NULL. Only the first byte of va is
 * validated by guest() (a span of 1), so a 32-bit access starting in the last 3
 * bytes of a region extends past its end.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @return the value, or 0 for an unmapped address. */
static inline uint32_t op_read_u32(const openpete_mod_api_t* api, uint32_t va) {
    void* p = api->guest(va);
    return p ? *(uint32_t*)p : 0u;
}
/** @brief Write an unsigned 32-bit value to guest address va.
 *
 * Drops the write when api->guest(va) is NULL. Only the first byte of va is
 * validated by guest() (a span of 1), so a 32-bit access starting in the last 3
 * bytes of a region extends past its end.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @param v the value. */
static inline void op_write_u32(const openpete_mod_api_t* api, uint32_t va,
                                uint32_t v) {
    void* p = api->guest(va);
    if (p) *(uint32_t*)p = v;
}
/** @brief Read a signed 32-bit value from guest address va.
 *
 * The unsigned read of the same width reinterpreted as signed.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @return the value, or 0 for an unmapped address. */
static inline int32_t op_read_s32(const openpete_mod_api_t* api, uint32_t va) {
    return (int32_t)op_read_u32(api, va);
}
/** @brief Write a signed 32-bit value to guest address va.
 *
 * The value is reinterpreted as unsigned and written by the unsigned write
 * of the same width.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @param v the value. */
static inline void op_write_s32(const openpete_mod_api_t* api, uint32_t va,
                                int32_t v) {
    op_write_u32(api, va, (uint32_t)v);
}
/** @brief Read an unsigned 16-bit value from guest address va.
 *
 * Returns 0 when api->guest(va) is NULL. Only the first byte of va is
 * validated by guest() (a span of 1), so a 16-bit access starting at the last
 * byte of a region extends past its end.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @return the value, or 0 for an unmapped address. */
static inline uint16_t op_read_u16(const openpete_mod_api_t* api, uint32_t va) {
    void* p = api->guest(va);
    return p ? *(uint16_t*)p : 0u;
}
/** @brief Write an unsigned 16-bit value to guest address va.
 *
 * Drops the write when api->guest(va) is NULL. Only the first byte of va is
 * validated by guest() (a span of 1), so a 16-bit access starting at the last
 * byte of a region extends past its end.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @param v the value. */
static inline void op_write_u16(const openpete_mod_api_t* api, uint32_t va,
                                uint16_t v) {
    void* p = api->guest(va);
    if (p) *(uint16_t*)p = v;
}
/** @brief Read a signed 16-bit value from guest address va.
 *
 * The unsigned read of the same width reinterpreted as signed.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @return the value, or 0 for an unmapped address. */
static inline int16_t op_read_s16(const openpete_mod_api_t* api, uint32_t va) {
    return (int16_t)op_read_u16(api, va);
}
/** @brief Write a signed 16-bit value to guest address va.
 *
 * The value is reinterpreted as unsigned and written by the unsigned write
 * of the same width.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @param v the value. */
static inline void op_write_s16(const openpete_mod_api_t* api, uint32_t va,
                                int16_t v) {
    op_write_u16(api, va, (uint16_t)v);
}
/** @brief Read an unsigned 8-bit value from guest address va.
 *
 * Returns 0 when api->guest(va) is NULL.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @return the value, or 0 for an unmapped address. */
static inline uint8_t op_read_u8(const openpete_mod_api_t* api, uint32_t va) {
    void* p = api->guest(va);
    return p ? *(uint8_t*)p : 0u;
}
/** @brief Write an unsigned 8-bit value to guest address va.
 *
 * Drops the write when api->guest(va) is NULL.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @param v the value. */
static inline void op_write_u8(const openpete_mod_api_t* api, uint32_t va,
                               uint8_t v) {
    void* p = api->guest(va);
    if (p) *(uint8_t*)p = v;
}
/** @brief Read a signed 8-bit value from guest address va.
 *
 * The unsigned read of the same width reinterpreted as signed.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @return the value, or 0 for an unmapped address. */
static inline int8_t op_read_s8(const openpete_mod_api_t* api, uint32_t va) {
    return (int8_t)op_read_u8(api, va);
}
/** @brief Write a signed 8-bit value to guest address va.
 *
 * The value is reinterpreted as unsigned and written by the unsigned write
 * of the same width.
 * @param api the mod API table.
 * @param va guest virtual address.
 * @param v the value. */
static inline void op_write_s8(const openpete_mod_api_t* api, uint32_t va,
                               int8_t v) {
    op_write_u8(api, va, (uint8_t)v);
}
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOD_API_H */
