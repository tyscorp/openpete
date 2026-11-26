/* core/renderer/native_terrain_cache.c - terrain sector-mesh GPU cache,
 * core side. See openpete_terrain_cache.h for the contract. */

#include "rhi.h"
#include "openpete_terrain_cache.h"
#include "openpete_mods.h"   /* terrain material buckets + their pipelines */
#include "openpete_native_render.h"   /* scene ring: is a build stamp still referenced */
#include "openpete_config.h"          /* OPENPETE_TERRAIN_SECDELTA, a device-creation knob */
#include "openpete_time.h"            /* op_now: the growth event timings */
#include "openpete_tile_census.h"     /* tc_tri_in_view: the near-cut census's view test */
#include "openpete_profile.h"         /* OP_PROF_SCOPE: the device service's parts */

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TC_DEFER_MAX  1024   /* deferred releases across arenas */
/* Freed ranges stay unreused for this many BUILDS: covers both the RHI
 * frames-in-flight window and the one-build presentation lag (the LIVE
 * state may still generation-reference a chunk the newest build replaced). */
#define TC_DEFER_PRESENTS 4

/* Buffers an arena may hold through growth (OPENPETE_TC_BUF_MAX lowers it),
 * and the buffer slots: one more, for the consolidation buffer that can be
 * installed beside N sources. */
#define TC_BUF_MAX   8
#define TC_BUF_SLOTS (TC_BUF_MAX + 1)

/* Previous-tick store capacity, in units (OPTC_PREV_REC_WORDS words each).
 * Poly-anim sectors drawn with near-variant chunks take one unit per face
 * whose previous corners differ from its current ones; env-anim colour
 * sectors under OPENPETE_TERRAIN_CANIMNEAR=1 take a colour-table ring of
 * OPTC_COLTAB_RING entries, each (1 + 2 * (LP + 2 * HP colours)) words. */
#define TC_PREV_CAP 65536u

/* Allocation sites the refusal knob (tc_refuse) can target. */
enum { TC_SITE_EDGE, TC_SITE_GROW, TC_SITE_CONS, TC_SITE_STORE, TC_SITE_RING, TC_SITE_HELD, TC_SITE_N };

/* Colour/depth slots of a cell-record pipeline set. No FILL: cells are HP
 * faces, and the hole-fill backing is LP only. */
enum { TC_CP_WORLD, TC_CP_MARK, TC_CP_ALPHA, TC_CP_ADD, TC_CP_SUB,
       TC_CP_PREPASS, TC_CP_KEY, TC_CP_N };

typedef struct { uint32_t first, count; } tc_span;

/* One arena's device buffers. Immutable once published: the level-edge
 * replacement swaps the whole struct through the arena's atomic pointer, so
 * a present can never bind a new `vb` beside the outgoing `pv` (two separate
 * stores would leave that window open). The retire entry is the pair. */
typedef struct {
    rhi_buffer *vb;
    rhi_buffer *pv;                   /* anim arenas: parallel prev-tick buffer
                                       * near arenas: parallel ctx buffer      */
    uint32_t    cap;
    uint32_t    serial;               /* nonzero, distinct per pair made: the
                                       * identity a state's marks are checked
                                       * against (tc_state.mark_pair) */
    uint32_t    phys;                 /* capacity the buffers were made at; a
                                       * recycled pair runs at cap <= phys */
    uint64_t    bytes;                /* device bytes of vb and pv together */
} tc_pair;

/* Level-edge memory census and the counters of the edge's deferred device
 * work (OPENPETE_TC_RETIRE_SPREAD, OPENPETE_TC_EDGE_RECYCLE): s_tc.edgecen,
 * written by the device-owning thread, printed by shutdown before it zeroes
 * s_tc. */
typedef struct {
    uint64_t live_bytes, peak_bytes;  /* pairs and stores made and not freed  */
    uint32_t spread_pairs;            /* outgoing objects destroyed one per service call */
    uint32_t spread_span;             /* service calls the last retire took, from its floors passing */
    uint32_t spread_build;            /* rz_old_build of the retire spread_span measures */
    uint32_t spread_first;            /* service call at which that retire's floors passed */
    uint32_t recycle_hits;            /* replacement pairs taken from a spare   */
    uint32_t recycle_misses;          /* replacement pairs made fresh, a spare present */
    uint64_t recycle_bytes;           /* device bytes of the hits               */
    uint32_t spares_parked;           /* retired pairs kept as an arena's spare */
    uint32_t held_releases;           /* allocation failures answered by freeing held objects */
    uint32_t held_refusals;           /* of those, provoked by OPENPETE_TC_REFUSE site held */
} tc_edge_census;

/* One buffer of an arena and its allocator. Each buffer numbers its vertices
 * from 0 and is drawn by its own rhi_draw, so a chunk is addressed by the
 * (buffer, first) pair everywhere. */
typedef struct {
    _Atomic(tc_pair *) pair;          /* live buffers; swapped at a level edge,
                                       * NULL for an empty slot              */
    uint32_t    cap;                  /* builder's mirror of pair->cap         */
    uint32_t    high;                 /* high-water vertex count (draw range);
                                       * falls when the span below it frees  */
    tc_span    *free_list;            /* sorted by first; grows on demand     */
    int         free_n, free_cap;
    uint32_t    used;                 /* live verts (census) */
    uint32_t    chunks;               /* live chunk count (census) */
    /* Consolidation source (or a buffer sealed at a failed level edge): it
     * takes no allocation; a sealed buffer's released ranges also never
     * return to its free list. The buffer retires once its chunk count is
     * 0 and its floors pass (tc_source_retire). */
    uint8_t     source, sealed;
    uint8_t     zeroed;               /* chunks reached 0 as a source          */
    uint32_t    zero_build;           /* present_ctr when they did             */
} tc_buf;

typedef struct {
    tc_buf      buf[TC_BUF_SLOTS];
    _Atomic int n;                    /* highest occupied slot + 1; the draw
                                       * and the allocator skip a NULL pair   */
    int         ovf_state;            /* builder-owned; TC_OVF_*              */
    int         ovf_kind;             /* request outstanding: TC_REQ_*        */
    uint32_t    backoff, retry_at;    /* refusal backoff in builds; next retry */
    int         cons_live;            /* a consolidation's sources remain     */
    uint32_t    cons_ticks;           /* canonical ticks since its adoption   */
    uint8_t     cons_why;             /* TC_EXH_* bit of the last consolidation
                                       * request that could not be posted     */
    uint8_t     refused;              /* TC_EXH_GROW / CONS of the refusals since
                                       * the arena last installed a buffer    */
    uint8_t     exh_sites;            /* TC_EXH_* bits of this level's exhaustions */
    uint32_t    lvl_claims;           /* claims outside windows this level    */
} tc_arena;

typedef struct { uint8_t arena, buf; uint32_t first, count, stamp; } tc_defer;

/* std140 globals block (mirrors terrain.vert TerrainGlobals). C natural
 * layout == std140 here: every member is a 16-byte vec4/ivec4 run. */
typedef struct {
    float mrow0[4];    /* M[0][0..2], pad */
    float mrow1[4];    /* M[1][0..2], pad */
    float mrow2[4];    /* M[2][0..2], pad */
    int32_t cam_int_x4[4];  /* floor(cam x4) per axis, x4 = world/16 x 4 */
    float cam_frac_x4[4];   /* cam x4 - cam_int_x4, in [0, 1)            */
    float ofH[4];      /* ofx, ofy, H, poly-anim alpha */
    float aux[4];      /* near-plane view-Z, H_canon, env-anim colour alpha, fogscale */
    int32_t fog[4];    /* fogRefBase, gflags, cullRefS2, ot_terms */
    float sdv[128][4]; /* per-tile scroll V offsets (lq, hqA, hqB, 0) */
    /* Portal inner-rim cull frames (portal_rim_classify mirror): the CPU's
     * double frames rounded to float. rim_cnt.x = plane count (0 = test
     * off). */
    int32_t rim_cnt[4];
    int32_t hp_drop[4];    /* HP far-face drop: sum bound, corner bound (0 = sum alone) */
    float   frim_oxyH[4];
    float   frim_knobs[4];
    float   frim_abc[6][4];
    float   frim_O[6][4];
    float   frim_U[6][4];
    float   frim_W[6][4];
    float   frim_nrm[6][4];
    float   frim_bbox[6][4];
    float   frim_pv[96][4];  /* vec2 array: std140 stride 16 */
    /* HD texture-replacement fold: the continuous-fog ramp (ts, tb) +
     * entry count/enable, then the entry table (4 vec4 per entry: page
     * rect, atlas rect, fogA + filter class, fogB). Snapshotted from
     * openpete_native_hdfold_table at frame_begin. */
    float   hd_lad[4];
    float   hd_entry[OPENPETE_HDFOLD_MAX][16];
    /* Portal cover planes for the blended passes' fragment discard
     * (terrain.frag, portal_cover.glsl): active opening planes in screen
     * space. cov_poly holds one screen point per vec4 slot, 16 slots per
     * plane. */
    int32_t cov_cnt[4];    /* x = plane count (0 = test off); y = blended cover on */
    float   cov_margin[4]; /* x = cover margin M (world units)            */
    float   cov_abc[6][4]; /* screen inv-vz plane fit a, b, c; w = pn     */
    float   cov_bbox[6][4];/* outline screen bbox x0, y0, x1, y1          */
    float   cov_poly[96][4];/* outline points (px, py, 0, 0), 16 per plane */
    float   sdvc[128][4];  /* per-tile 4x4 near-cell row V offsets (rows 0-3) */
} tc_globals_ubo;

typedef struct {
    float o[4];             /* xyz origin, w flag bits                      */
    float gp[4], gq[4];     /* pack LP grade affine v' = p*v + q*255 (rgb);
                             * valid only with OPTC_SF_LPGRADE set in o.w;
                             * gp[3]: the bits of the colour table's first
                             * unit (OPTC_PREV_REC_NONE for none); gq[3]:
                             * the bits of the position-ring entry's first
                             * unit (OPTC_PREV_REC_NONE for none)            */
} tc_sector_ubo;

/* Both blocks ride one rhi_ubo with a uniform block size (the larger of the
 * two); everything must stay inside the 64 KB maxUniformBufferRange every
 * desktop implementation and llvmpipe provide. */
#define TC_UBO_BLOCK (sizeof(tc_globals_ubo) > sizeof(tc_sector_ubo) * OPTC_SLOTS \
                          ? sizeof(tc_globals_ubo)                                \
                          : sizeof(tc_sector_ubo) * OPTC_SLOTS)
_Static_assert(sizeof(tc_globals_ubo) <= 65536 &&
               sizeof(tc_sector_ubo) * OPTC_SLOTS <= 65536,
               "terrain-cache UBO block exceeds the 64 KB UBO-range floor");
/* The fragment mirror (terrain_shade.glsl) declares the 4 vec4 after mrow2
 * and the 264 vec4 between fog and hd_lad as padding. */
_Static_assert(offsetof(tc_globals_ubo, fog) == 7 * 16 &&
               offsetof(tc_globals_ubo, hd_lad) - offsetof(tc_globals_ubo, sdv) == 264 * 16,
               "terrain-cache globals: terrain_shade.glsl's padding no longer matches");

/* One build's complete retained frame state. The feeder STAGES the canonical
 * build's state and it PROMOTES to live one build later - the sub-tick
 * spine presents the previous canonical extraction (the canonical-lag leg),
 * and the retained draws must ride the same timeline or animated tiles and
 * moving cameras render one tick ahead of the CPU-fed scene around them.
 * Re-extracted presents (windowed intermediates + canon-lag re-extract)
 * publish immediately: their build IS the displayed state. */
/* Draw ranges of one state: per pass class and (arena, buffer), the
 * element spans holding a chunk the state's sector flags select for that
 * class, in ascending order, adjacent spans closer than the arena's merge
 * gap joined. Class TC_RC_DRAW serves pass_sel 0 and 2, class TC_RC_FILL
 * pass_sel 1 (tc_chunk_class_drawn). Built at frame_commit from the
 * builder's chunk registry. n < 0 means no list (a build still open, or
 * more spans than TC_RANGE_MAX): the state draws every buffer's whole
 * high-water range. Must stay the last member of tc_state, and `r` the last
 * member here: tc_state_copy copies only the n spans in use. */
#define TC_RANGE_MAX 4096
enum { TC_RC_DRAW, TC_RC_FILL, TC_RC_N };
typedef struct {
    int      n;
    uint16_t off[TC_RC_N][OPTC_ARENA_COUNT * TC_BUF_SLOTS + 1]; /* first span of (class, arena, buf) */
    tc_span  r[TC_RANGE_MAX];
} tc_ranges;

typedef struct {
    tc_globals_ubo g;
    tc_sector_ubo  sec[OPTC_SLOTS];
    uint32_t       stamp;             /* openpete_frame of the build, or a scene id */
    int            valid;
    uint32_t       build_serial;      /* present_ctr at the build: ranges freed at a
                                       * later serial may still be drawn by this state */
    /* Each buffer's high-water mark as the builder left it for this build
     * (tc_state_mark), and the serial of the pair that mark counts in (0 =
     * no buffer). The draw ranges are bounded by these, never by the live
     * marks the builder grows for later builds while a present draws this
     * one, and a buffer whose live pair is not the marked one holds none of
     * this build's chunks. */
    uint32_t       mark_high[OPTC_ARENA_COUNT][TC_BUF_SLOTS];
    uint32_t       mark_pair[OPTC_ARENA_COUNT][TC_BUF_SLOTS];
    tc_ranges      rl;                /* last: see tc_ranges */
} tc_state;

/* One chunk in the builder's registry: its element span in one arena
 * buffer and the sector slot, generation and tier (OPTC_FF_TIER_LP) its
 * elements carry in face_flags. `uniform` is 0 when the elements disagree
 * on any of the three, and such a chunk is drawn by every state and pass. */
typedef struct {
    uint32_t first, count;
    uint16_t slot;
    uint8_t  gen, uniform;
    uint8_t  lp;                      /* the elements are low-poly tier faces */
} tc_chunk;

/* Builder-owned chunk registry of one arena buffer, sorted by `first`. */
typedef struct {
    tc_chunk *c;
    int       n, cap;
} tc_chunk_list;

/* Eager scene ring: one state per scene id, keyed by the id the builder
 * stamped it with. A build of an id that already holds a state reuses that
 * state (the rebuild of a scene the ring refused), so the live states are at
 * most one per unretired scene. Sized at the scene ring plus the build in
 * progress plus the refused scene the accumulator holds, so a build always
 * finds a state no unretired scene carries. */
#define TC_RING (OPENPETE_NATIVE_SCENE_RING + 2)

/* The terrain cache's pipelines (tc_make_pipes); a member is NULL when its
 * create failed. */
typedef struct {
    rhi_pipeline *pipe_world;         /* opaque, plain                        */
    rhi_pipeline *pipe_world_mark;    /* opaque + stencil MARK (hole-fill on) */
    rhi_pipeline *pipe_alpha;
    rhi_pipeline *pipe_add;
    rhi_pipeline *pipe_sub;
    rhi_pipeline *pipe_fill;          /* stencil TEST_ZERO                    */
    rhi_pipeline *pipe_prepass;       /* pre-pass WORLD: footprint depth, packed depth blended into colour where it paints */
    rhi_pipeline *pipe_prepass_depth; /* pre-pass hole fill: depth only (colour off) */
    rhi_pipeline *pipe_key;           /* og OT-key emit (pass_sel 2)          */
    /* terrain_anim variants (binding-1 prev twin + vertex-stage lerp). */
    rhi_pipeline *pipe_a_world, *pipe_a_world_mark;
    rhi_pipeline *pipe_a_alpha, *pipe_a_add, *pipe_a_sub;
    rhi_pipeline *pipe_a_prepass, *pipe_a_prepass_depth, *pipe_a_key;
    rhi_pipeline *pipe_a_fill;        /* anim LP hole-fill: env-anim colour
                                       * sectors fill, poly-anim sectors do
                                       * not (their SF_FILLLP stays clear)   */
    /* terrain_near variants (binding-1 member-context twin + the vertex-stage
     * near-plane clip fan). Full set: near sectors carry every draw class. */
    rhi_pipeline *pipe_n_world, *pipe_n_world_mark;
    rhi_pipeline *pipe_n_alpha, *pipe_n_add, *pipe_n_sub;
    rhi_pipeline *pipe_n_fill, *pipe_n_prepass, *pipe_n_prepass_depth, *pipe_n_key;
    /* terrain_cells variants (instanced cell records): the near-variant
     * sectors' set (clip-fan stage) and the poly-anim sectors' set (lerp
     * stage), indexed by TC_CP_*. */
    rhi_pipeline *pipe_cn[TC_CP_N], *pipe_ca[TC_CP_N];
} tc_pipe_set;

/* The previous-tick store as one published unit: the set-2 bind reads the
 * buffer and its size through one acquired pointer, so it never pairs a
 * new buffer with an old size. */
typedef struct {
    rhi_buffer *buf;
    uint32_t    cap;                  /* units                                */
    uint32_t    serial;               /* nonzero, distinct per store made     */
} tc_store;

/* One device object the builder has unpublished and the device-owning
 * thread destroys once RHI_FRAMES_IN_FLIGHT service calls have passed: a
 * retired consolidation source or sealed buffer (pair) or a replaced
 * previous-tick store (store). */
typedef struct {
    tc_pair  *pair;
    tc_store *store;
    uint32_t  svc;                    /* service call count at the push       */
} tc_retire_ent;
#define TC_RETIRE_Q 1024

static struct {
    rhi_device   *dev;
    int           ready;
    tc_arena      arena[OPTC_ARENA_COUNT];
    rhi_ubo      *ubo;                /* block 0 = globals, block 1 = sectors */
    tc_pipe_set   ps;                 /* the pipelines (tc_make_pipes)        */
    uint32_t      cell_draws;         /* cell-arena buffer draws (census)     */
    uint64_t      cell_instances;     /* records in those buffers' high-water
                                       * ranges                               */
    rhi_texture  *hd_dummy;           /* 1x1 stand-in for the set-4 atlas and
                                       * for the set-2 prop key image         */
    _Atomic(tc_store *) prev_live;    /* previous-tick store (set 2, binding 1):
                                       * corner records and colour tables     */
    uint32_t      shadow_cap;         /* units prev_shadow holds (>= prev_cap) */
    uint32_t     *prev_shadow;        /* CPU copy of the store, prev_cap units:
                                       * the refill source of a replacement  */
    tc_buf        prev_alloc;         /* its allocator (pair unused)          */
    tc_defer      prev_defer[TC_DEFER_MAX]; /* released record runs (arena unused) */
    int           prev_defer_n;
    uint32_t      prev_fail;          /* refused record allocations (census)  */
    uint32_t      prev_defer_ovf;     /* releases past prev_defer, returned at once (census) */
    uint32_t      prev_cap;           /* store capacity in units              */
    int           prev_state;         /* TC_OVF_* of the store's growth       */
    uint32_t      prev_backoff, prev_retry_at;
    _Atomic int   prev_req;           /* builder -> device: grow the store    */
    _Atomic int   prev_done;          /* device -> builder: answered          */
    uint32_t      prev_want;          /* units asked for                      */
    tc_store     *prev_new;           /* answer (NULL = refused)              */
    tc_store     *rz_store;           /* the edge replacement's fresh store   */
    tc_store     *rz_old_store;       /* the store an edge adoption retired   */
    uint32_t      rz_store_cap;       /* units the edge replacement store holds */
    tc_state      staged;             /* canonical build awaiting its present */
    tc_state      live;               /* what presents render                 */
    tc_state      ring[TC_RING];      /* per-scene states (eager); stamp = scene id */
    /* ring[i]'s identity for the compose-side lookups (tc_state_for_stamp,
     * tc_dup_probe): build_serial << 32 | stamp, 0 while ring[i] holds no
     * state. The builder rewrites ring states while another thread composes
     * against a different one, so the compose side reads only these words;
     * the content of the state a scene's stamp names reaches it through
     * that scene's publish. */
    _Atomic uint64_t ring_key[TC_RING];
    tc_state      hold[OPENPETE_TERCACHE_HOLDS]; /* copies kept for held scenes (openpete_tercache_hold_take) */
    unsigned      hold_takes, hold_misses, hold_drops; /* hold census */
    int           ring_next;
    unsigned      ring_builds;        /* scene-id builds that took a ring state */
    unsigned      ring_reuses;        /* of those, rebuilds of an id holding a state */
    unsigned      ring_overwrites;    /* builds that overwrote a live scene's state */
    unsigned      ring_dup_selects;   /* selects of a stamp held by several states
                                       * that took an older build than the newest */
    tc_state      sel;                /* frame_select's snapshot of live: the
                                       * present's draws may execute VBLs after
                                       * selection (deferred screenshot
                                       * readback), by which time live has been
                                       * re-promoted - flushing live at draw
                                       * time would pair old chunks with a
                                       * fresher camera/fog/colour-generation
                                       * set (rung flips, sub-pixel skew) */
    tc_sector_ubo *build_target;      /* where sector_set writes this build   */
    tc_state     *build_state;        /* the state build_target belongs to    */
    tc_chunk_list reg[OPTC_ARENA_COUNT][TC_BUF_SLOTS]; /* builder's chunk registry */
    int           reg_broken;         /* a registry insert failed: no range lists */
    /* Submission census: per selected present, the elements the draws
     * submitted and the elements drawing each buffer's whole high-water
     * range would have submitted (vertices; cell records times their
     * per-instance vertices), and the draw calls issued. */
    uint64_t      cen_presents, cen_verts, cen_verts_whole, cen_draws;
    uint32_t      cen_nolist;         /* draws of a state with no range list  */
    int           draw_open;
    int  sel_src;      /* 0 closed, 1 scene ring, 2 live, 3 staged */          /* frame_select verdict for this present */
    uint64_t      ubo_flush_serial;   /* rhi_frame_serial of the last UBO flush
                                       * for the CURRENT selection (0 = none).
                                       * Keyed on the frame serial, not a
                                       * boolean: a capture path can render one
                                       * selection across SEVERAL rhi frames
                                       * (the HD-pending screenshot retry), and
                                       * each frame's ring slot needs its own
                                       * flush or the retained draws bind a
                                       * never-written slot (all-zero sector
                                       * flags = every vertex killed). */
    uint32_t      present_ctr;
    tc_defer      defer[TC_DEFER_MAX];
    int           defer_n;
    _Atomic uint32_t draws_last; /* accumulating: buffer draws since the last
                                  * build began (present thread adds, the
                                  * builder takes it at frame_begin)     */
    uint32_t      draws_prev;   /* the draws taken at the last frame_begin (census) */
    /* ---- level-edge arena replacement (see the header's contract) ----
     * One replacement is in flight at a time, as a three-state handshake
     * between the builder and the device-owning thread. IDLE: the builder
     * may post a request. READY: present_service has allocated rz_new and
     * the builder has not adopted yet. RETIRE: the builder has adopted and
     * rz_old awaits its drain floors. Each transition is a release store
     * paired with the other thread's acquire load, which is what publishes
     * the rz_new / rz_old arrays across the two threads. */
    _Atomic int      rz_req;      /* builder -> device: a level edge happened */
    _Atomic int      rz_state;
    _Atomic int      level_id;    /* the latched level, named by the log lines */
    uint32_t         alloc_epoch; /* steps at every buffer the arenas gain    */
    int              rz_alloc_logged;
    tc_pair         *rz_new[OPTC_ARENA_COUNT];
    /* The pairs one replacement retires: the arena's live buffers, plus the
     * slot at TC_BUF_SLOTS for a growth or consolidation buffer the device
     * answered that no frame_begin got to install. */
    tc_pair         *rz_old[OPTC_ARENA_COUNT][TC_BUF_SLOTS + 1];
    uint32_t         rz_old_build, rz_old_svc;  /* drain floors, at adoption  */
    /* ---- overflow buffers (see the header's contract) ----
     * Two bitmasks over the arenas carry one request each way. The builder
     * writes ovf_want under its request bit and reads ovf_new under the
     * answer bit; each bit is set with a release store and taken with an
     * acquiring exchange, which is what publishes the two arrays across the
     * threads. */
    _Atomic uint32_t ovf_req;     /* builder -> device: arenas wanting a buffer */
    _Atomic uint32_t ovf_done;    /* device -> builder: requests answered       */
    uint32_t         ovf_want[OPTC_ARENA_COUNT];   /* vertices asked for        */
    uint64_t         ovf_fulfil_ns[OPTC_ARENA_COUNT]; /* device time of the answer */
    uint32_t         ovf_req_svc[OPTC_ARENA_COUNT];   /* service call at request */
    uint32_t         ovf_req_build[OPTC_ARENA_COUNT]; /* build at request        */
    tc_pair         *ovf_new[OPTC_ARENA_COUNT];    /* answer (NULL = refused)   */
    int              ovf_fail_logged[OPTC_ARENA_COUNT];
    uint32_t         overflow_adopted[OPTC_ARENA_COUNT];
    _Atomic uint32_t build_ctr;   /* present_ctr, published for the device    */
    _Atomic uint32_t svc_calls, svc_fulfilled;
    _Atomic uint32_t draws_retired;
    uint32_t         alloc_fail[OPTC_ARENA_COUNT];
    /* ---- growth census (openpete_tercache_print_exit_row) ---- */
    int              rz_window;      /* builder: a level-edge request awaits adoption */
    int              rz_old_due;     /* device thread: the retired pairs passed their drain floors */
    tc_pair         *spare[OPTC_ARENA_COUNT];  /* device thread: a retired pair kept for the next edge (OPENPETE_TC_EDGE_RECYCLE) */
    tc_edge_census   edgecen;        /* device thread: the level-edge census */
    _Atomic uint32_t rz_failed;      /* device -> builder: refused replacements */
    uint32_t         rz_failed_seen; /* builder's copy of rz_failed            */
    uint32_t         bakes, window_bakes;
    uint32_t         claims[OPTC_ARENA_COUNT];
    uint32_t         edge_claims, grow_claims, retire_claims, prev_grow_claims;
    uint32_t         failed_edges, defer_overflow, dropped_spans;
    _Atomic uint32_t nonlocal_bufs;  /* device thread counts, builder reads    */
    uint32_t         bufs_max[OPTC_ARENA_COUNT];
    uint32_t         ranges_max;
    uint32_t         range_overflows; /* builds whose range lists overflowed TC_RANGE_MAX */
    uint64_t         class_dropped[TC_RC_N]; /* elements a class list left out by sector admission */
    uint32_t         consolidations, prev_grow, sealed_draws, sealed_allocs, migrate_ticks;
    /* Growth answered while a replacement retires: the device counts the
     * buffers and stores it answers in RETIRE (retire_answers) and any
     * answer that names a pair or store on the retire list; the builder
     * counts any install, chunk write, store write or store retirement that
     * names a pair made before the last adopted replacement (serial below
     * old_pair_floor) or the store that replacement retired
     * (old_store_serial). Both go to retire_old_touches, whose invariant
     * is 0. */
    _Atomic uint32_t retire_answers, retire_old_touches;
    uint32_t         old_pair_floor, old_store_serial;
    uint32_t         exhaust_warnings; /* exhaustion warning lines printed     */
    uint64_t         pinned_bytes;   /* peak bytes of emptied sources awaiting floors */
    /* builder -> device retire queue (single producer, single consumer) */
    tc_retire_ent    rq[TC_RETIRE_Q];
    _Atomic uint32_t rq_head, rq_tail;
    _Atomic int      stores_alive;   /* stores made and not yet freed (OPTC_STORES_ALIVE) */
    _Atomic uint32_t bind_refusals;  /* refused set-2 binds (display thread counts) */
    uint32_t         bind_refusals_seen;
    /* ---- content estimate (openpete_tercache_set_content) ----
     * est: the builder's per-arena estimate for the table being baked;
     * rz_est: the copy a level-edge request carries to the device thread,
     * written before the request's release store as rz_store_cap is.
     * The census keeps, per level, the estimate the level latched with,
     * the peak of each arena's summed high-water mark, and the growth
     * count at the latch. */
    uint32_t         est[OPTC_ARENA_COUNT], rz_est[OPTC_ARENA_COUNT];
    uint64_t         est_prev;            /* store units */
    int              est_have, est_level;
    uint32_t         lvl_est[OPTC_ARENA_COUNT], lvl_peak[OPTC_ARENA_COUNT];
    uint64_t         lvl_est_prev;
    uint32_t         lvl_prev_peak, lvl_grow0, lvl_sigs;
    uint32_t         window_skips, sig_changes;
    uint64_t         stale_bytes;
    uint32_t         budget_claims, budget_episodes, budget_walks_max, budget_open, budget_age_max;
    uint32_t         budget_starved;
    uint32_t         kflags;              /* OPTC_KEYS_* of the key images bound by the
                                           * openpete_tercache_draw in progress; 0 when
                                           * it binds none */
} s_tc;

static void tc_est_col(char *out, size_t cap, const uint32_t *v);
static void tc_arena_caps(uint32_t *out);
static void tc_est_census_level(void);

enum { TC_RZ_IDLE = 0, TC_RZ_READY = 1, TC_RZ_RETIRE = 2 };
/* Builder-side request state per arena. REQ: a growth or consolidation
 * request awaits its answer (the request-to-adoption window). BACKOFF: a
 * refusal waits retry_at before the next request. */
enum { TC_OVF_IDLE = 0, TC_OVF_REQ = 1, TC_OVF_BACKOFF = 2 };
/* What an arena's outstanding request asks for. */
enum { TC_REQ_GROW = 0, TC_REQ_CONS = 1 };
/* Why an arena ran out of room with nothing in flight (tc_exhaust): a
 * refused growth buffer, a refused consolidation buffer, a refused
 * level-edge replacement, a consolidation with no empty slot, or one
 * blocked by a previous consolidation's sources still migrating. */
enum { TC_EXH_GROW = 1, TC_EXH_CONS = 2, TC_EXH_EDGE = 4, TC_EXH_SLOTS = 8,
       TC_EXH_MIGRATING = 16 };

/* The last stats before teardown. shutdown zeroes the module's state, so a
 * census printed at process exit would otherwise read every counter as 0
 * whichever teardown ran first. */
static openpete_tercache_stats_t s_tc_final;
static int s_tc_final_valid;

/** @brief Add device bytes to the cache's live total and raise its peak.
 * @param b Bytes made. */
static void tce_add(uint64_t b)
{
    s_tc.edgecen.live_bytes += b;
    if (s_tc.edgecen.live_bytes > s_tc.edgecen.peak_bytes) s_tc.edgecen.peak_bytes = s_tc.edgecen.live_bytes;
}


/** @brief Load the current buffer pair of one arena buffer.
 *
 * Used by the chunk writes and the dead-stamp release. The draw path loads
 * the pointer itself (tc_draw_buf) so one draw reads one pair.
 * @param arena Arena index.
 * @param buf Buffer index within the arena.
 * @return The pair, or NULL when that buffer holds none. */
static const tc_pair *tc_live_pair(int arena, int buf)
{
    return atomic_load_explicit(&s_tc.arena[arena].buf[buf].pair, memory_order_acquire);
}

/** @brief Return the floor capacity of an arena: 16,384 vertices, or 1,024 records on a cell arena.
 * @param arena Arena index.
 * @return Capacity in vertices, or in records on a cell arena. */
static uint32_t tc_floor_cap(int arena)
{
    return OPTC_ARENA_IS_CELL(arena) ? 1024u : 16384u;
}

/** @brief Return OPENPETE_TC_INITIAL_CAP, read once, or 0 when unset or not positive.
 * @return The vertex count. */
static long tc_initial_cap_env(void)
{
    static long ov = -1;
    if (ov < 0) {
        const char *e = getenv("OPENPETE_TC_INITIAL_CAP");
        ov = (e && *e) ? strtol(e, NULL, 10) : 0;
        if (ov < 0) ov = 0;
    }
    return ov;
}

/** @brief Return the capacity an arena's primary buffer is allocated at.
 *
 * OPENPETE_TC_INITIAL_CAP, read once, wins: one vertex count for every
 * vertex arena and a sixteenth of it (at least 1) for every cell-record
 * arena; a record replaces 48 or 96 baked vertices, so the sixteenth still
 * binds after the vertex arenas do. A harness squeezes the primaries with
 * it so the growth path runs; growth steps are sized by it too. Without
 * it the size is the arena's content estimate (@p est,
 * openpete_tercache_set_content) raised to the floor, or the floor when no
 * estimate exists yet.
 * @param arena Arena index.
 * @param est The per-arena estimate, or NULL when none has been made.
 * @return Capacity in vertices, or in records on a cell arena. */
static uint32_t tc_initial_cap(int arena, const uint32_t *est)
{
    const long ov = tc_initial_cap_env();
    if (ov > 0) {
        if (OPTC_ARENA_IS_CELL(arena)) return ov >= 16 ? (uint32_t)(ov / 16) : 1u;
        return (uint32_t)ov;
    }
    const uint32_t f = tc_floor_cap(arena);
    return (est && est[arena] > f) ? est[arena] : f;
}

/** @brief Return the buffer count an arena may hold, OPENPETE_TC_BUF_MAX clamped to [1, TC_BUF_MAX].
 *
 * Read once; unset or empty gives TC_BUF_MAX. A harness lowers it so an
 * arena runs out of buffers early. Outside the knob registry.
 * @return The buffer count. */
static int tc_buf_max(void)
{
    static int v;
    if (!v) {
        const char *e = getenv("OPENPETE_TC_BUF_MAX");
        long x = (e && *e) ? strtol(e, NULL, 10) : TC_BUF_MAX;
        if (x < 1) x = 1;
        if (x > TC_BUF_MAX) x = TC_BUF_MAX;
        v = (int)x;
    }
    return v;
}

/** @brief Return OPENPETE_TC_PREV_CAP, the previous-tick store capacity a harness forces, in units.
 *
 * Read once. Outside the knob registry.
 * @return The units, or 0 when unset or below 1. */
static uint32_t tc_prev_cap_env(void)
{
    static int read;
    static uint32_t v;
    if (!read) {
        read = 1;
        const char *e = getenv("OPENPETE_TC_PREV_CAP");
        const long x = (e && *e) ? strtol(e, NULL, 10) : 0;
        v = x >= 1 ? (uint32_t)x : 0u;
    }
    return v;
}

/** @brief Return the previous-tick store's capacity at init, in units: OPENPETE_TC_PREV_CAP, else TC_PREV_CAP.
 *
 * A harness shrinks it so the store runs out; set, it also overrides the
 * level-edge estimate (openpete_tercache_request_resize), so the store
 * starts every level at the squeeze and grows from there.
 * @return Units. */
static uint32_t tc_prev_cap_init(void)
{
    const uint32_t v = tc_prev_cap_env();
    return v ? v : TC_PREV_CAP;
}

static long s_refuse_cnt[TC_SITE_N], s_refuse_skip[TC_SITE_N];

/** @brief Parse one item of OPENPETE_TC_REFUSE into the refusal tables.
 *
 * The item is `site:count`, optionally followed by `@skip`.
 * @param p The item's first character; parsing stops at a comma or the end. */
static void tc_refuse_item(const char *p)
{
    static const char *const names[TC_SITE_N] = { "edge", "grow", "cons", "store", "ring", "held" };
    for (int k = 0; k < TC_SITE_N; k++) {
        const size_t n = strlen(names[k]);
        if (strncmp(p, names[k], n) != 0 || p[n] != ':') continue;
        const char *q = p + n + 1;
        s_refuse_cnt[k] = *q == '*' ? -1 : strtol(q, NULL, 10);
        const char *at = strpbrk(q, "@,");
        if (at && *at == '@') s_refuse_skip[k] = strtol(at + 1, NULL, 10);
    }
}

/** @brief Decide whether the refusal knob refuses this allocation at an allocation site.
 *
 * OPENPETE_TC_REFUSE is a comma list of `site:count[@skip]`, site one of
 * edge (the level-edge replacement), grow (a growth buffer), cons (a
 * consolidation buffer), store (a previous-tick store replacement), ring
 * (a position-ring run, builder thread, openpete_tercache_ring_alloc) or
 * held (the first attempt of a pair or store made while retired objects
 * are held, tc_pair_make_held):
 * past the first @p skip allocations at the site, the next @p count are
 * refused, `*` meaning every one. Read once; device-owning thread only,
 * like every allocation it gates. Outside the knob registry; a harness
 * selects one site, which OPENPETE_RHI_FAIL_MEMTYPE (every allocation on a
 * memory type) cannot.
 * @param site A TC_SITE_* value.
 * @return 1 to refuse, else 0. */
static int tc_refuse(int site)
{
    static int parsed;
    static long seen[TC_SITE_N];
    long *const cnt = s_refuse_cnt, *const skip = s_refuse_skip;
    if (!parsed) {
        parsed = 1;
        const char *p = getenv("OPENPETE_TC_REFUSE");
        while (p && *p) {
            tc_refuse_item(p);
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    if (site < 0 || site >= TC_SITE_N || cnt[site] == 0) return 0;
    const long i = seen[site]++;
    if (i < skip[site]) return 0;
    return cnt[site] < 0 || i - skip[site] < cnt[site];
}

/** @brief Return the byte size of one element of an arena's main buffer.
 *
 * @param arena Arena index.
 * @return sizeof a cell record on the cell arenas (anim or near layout),
 *         else sizeof(rhi_terrain_vertex). */
static size_t tc_elem_size(int arena)
{
    if (OPTC_ARENA_IS_CELL_ANIM(arena)) return sizeof(rhi_terrain_cell_anim);
    if (OPTC_ARENA_IS_CELL(arena))      return sizeof(rhi_terrain_cell);
    return sizeof(rhi_terrain_vertex);
}

/** @brief Return the device bytes one buffer pair of an arena holds at a capacity.
 *
 * The stride is the element size plus the twin's (the prev-tick record on
 * the anim arenas, the near-context record on the near arenas). Feeds the
 * cap_bytes census of openpete_tercache_get_stats.
 * @param arena Arena index.
 * @param cap Buffer capacity in elements.
 * @return Bytes of the main buffer and its twin together. */
static uint64_t tc_pair_bytes(int arena, uint32_t cap)
{
    size_t stride = tc_elem_size(arena);
    if (OPTC_ARENA_IS_ANIM(arena))      stride += sizeof(rhi_terrain_prev);
    else if (OPTC_ARENA_IS_NEAR(arena)) stride += sizeof(rhi_terrain_nearctx);
    return (uint64_t)cap * stride;
}

/** @brief Destroy a buffer pair's device buffers and free the pair.
 *
 * Device-owning thread only. The caller must know no in-flight draw still
 * reads the buffers (see tc_arena_free_all).
 * @param dev Device the buffers were created on.
 * @param pr Pair to free; NULL is ignored. */
static void tc_pair_free(rhi_device *dev, tc_pair *pr)
{
    if (!pr) return;
    if (pr->vb) rhi_destroy_buffer(dev, pr->vb);
    if (pr->pv) rhi_destroy_buffer(dev, pr->pv);
    s_tc.edgecen.live_bytes -= pr->bytes;
    free(pr);
}

/** @brief Return the next pair serial of the process-wide counter, skipping 0.
 *
 * A pair made fresh and a spare a replacement recycles both take one, so
 * every pair of a replacement is newer than every outgoing pair.
 * @return The serial. */
static uint32_t tc_pair_serial_next(void)
{
    static _Atomic(uint32_t) s_serial;
    uint32_t v;
    do v = atomic_fetch_add_explicit(&s_serial, 1u, memory_order_relaxed) + 1u; while (v == 0);
    return v;
}

/** @brief Allocate one arena buffer pair on the device.
 *
 * Device-owning thread only. The twin is the anim arenas' previous-tick
 * buffer or the near arenas' emitted-tri context buffer, at the same element
 * count and its own stride; a base arena and a cell arena have none. A pair
 * missing any buffer it owes is freed. Each pair takes the next serial of a
 * process-wide counter, skipping 0.
 * @param dev Device.
 * @param arena Arena index.
 * @param cap Capacity in elements.
 * @return The pair, or NULL on an allocation failure. */
static tc_pair *tc_pair_make(rhi_device *dev, int arena, uint32_t cap)
{
    tc_pair *pr = (tc_pair *)calloc(1, sizeof(*pr));
    if (!pr) return NULL;
    pr->cap = pr->phys = cap;
    pr->serial = tc_pair_serial_next();
    pr->vb = rhi_create_vertex_buffer_device(dev, (size_t)cap * tc_elem_size(arena));
    if (pr->vb) pr->bytes += (uint64_t)cap * tc_elem_size(arena);
    if (OPTC_ARENA_IS_ANIM(arena))
        pr->pv = rhi_create_vertex_buffer_device(dev, (size_t)cap * sizeof(rhi_terrain_prev));
    else if (OPTC_ARENA_IS_NEAR(arena))
        pr->pv = rhi_create_vertex_buffer_device(dev, (size_t)cap * sizeof(rhi_terrain_nearctx));
    if (pr->pv) pr->bytes += tc_pair_bytes(arena, cap) - (uint64_t)cap * tc_elem_size(arena);
    tce_add(pr->bytes);
    const int twin_owed = OPTC_ARENA_IS_ANIM(arena) || OPTC_ARENA_IS_NEAR(arena);
    if (!pr->vb || (twin_owed && !pr->pv)) { tc_pair_free(dev, pr); return NULL; }
    if (!rhi_buffer_device_local(pr->vb) || (pr->pv && !rhi_buffer_device_local(pr->pv)))
        atomic_fetch_add_explicit(&s_tc.nonlocal_bufs, 1u, memory_order_relaxed);
    return pr;
}

/** @brief Allocate a previous-tick store of a capacity on the device.
 *
 * Device-owning thread only. Each store takes the next serial of a
 * process-wide counter, skipping 0.
 * @param dev Device.
 * @param cap Capacity in units of OPTC_PREV_REC_WORDS words.
 * @return The store, or NULL on an allocation failure. */
static tc_store *tc_store_make(rhi_device *dev, uint32_t cap)
{
    static uint32_t s_serial;
    tc_store *st = (tc_store *)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->cap = cap;
    if (++s_serial == 0) s_serial = 1;
    st->serial = s_serial;
    st->buf = rhi_create_storage_buffer_device(dev, (size_t)cap * OPTC_PREV_REC_WORDS * sizeof(uint32_t));
    if (!st->buf) { free(st); return NULL; }
    tce_add((uint64_t)cap * OPTC_PREV_REC_WORDS * sizeof(uint32_t));
    atomic_fetch_add_explicit(&s_tc.stores_alive, 1, memory_order_relaxed);
    if (!rhi_buffer_device_local(st->buf))
        atomic_fetch_add_explicit(&s_tc.nonlocal_bufs, 1u, memory_order_relaxed);
    return st;
}

/** @brief Destroy a previous-tick store and free its record.
 * @param dev Device the store was made on.
 * @param st The store; NULL is ignored. */
static void tc_store_free(rhi_device *dev, tc_store *st)
{
    if (!st) return;
    if (st->buf) {
        rhi_destroy_buffer(dev, st->buf);
        s_tc.edgecen.live_bytes -= (uint64_t)st->cap * OPTC_PREV_REC_WORDS * sizeof(uint32_t);
    }
    free(st);
    atomic_fetch_sub_explicit(&s_tc.stores_alive, 1, memory_order_relaxed);
}

/** @brief Free every buffer pair one arena owns and clear the pointers.
 *
 * The pairs are its live buffers and the ones its two handshakes may still
 * hold: the replacement's fulfilled-but-unadopted pair, its retire list, and
 * a growth or consolidation buffer the builder never installed. Each
 * buffer's free list is freed too. Teardown
 * (openpete_tercache_shutdown) is the only caller; while the cache runs,
 * each handshake frees its own pairs at the point that proves no frame reads
 * them (tc_retire_drain for the replacement, tc_pair_make's own cleanup for
 * an allocation that failed half-made).
 * @param dev Device.
 * @param arena Arena index. */
static void tc_arena_free_all(rhi_device *dev, int arena)
{
    for (int b = 0; b < TC_BUF_SLOTS; b++) {
        tc_buf *bf = &s_tc.arena[arena].buf[b];
        tc_pair_free(dev, atomic_load_explicit(&bf->pair, memory_order_relaxed));
        atomic_store_explicit(&bf->pair, NULL, memory_order_relaxed);
        free(bf->free_list);
        bf->free_list = NULL; bf->free_n = bf->free_cap = 0;
    }
    for (int b = 0; b <= TC_BUF_SLOTS; b++) {
        tc_pair_free(dev, s_tc.rz_old[arena][b]);
        s_tc.rz_old[arena][b] = NULL;
    }
    tc_pair_free(dev, s_tc.rz_new[arena]);  s_tc.rz_new[arena]  = NULL;
    tc_pair_free(dev, s_tc.ovf_new[arena]); s_tc.ovf_new[arena] = NULL;
    tc_pair_free(dev, s_tc.spare[arena]);   s_tc.spare[arena]   = NULL;
}

/* push-constant block (mirrors terrain.vert TerrainPC) */
typedef struct {
    float   x_base, x_span, y_base, y_span;
    float   disp_w, clip_h, band, freeze_wf;
    int32_t pass_sel, full_extent;
    int32_t mat_pass;      /* -1 = every face, 0 = stock, >0 = that material's pass */
    int32_t snap;          /* 1 = integer vertex precision (screen-point floor) */
    int32_t cover;         /* 1 = portal cover discard (terrain.frag) */
    int32_t pad_[3];       /* [1]: OPTC_KEYS_* of the pass's key images (terrain.frag
                            * kflags); terrain_faceid.frag reads all three as its words */
    float   fc[4];         /* gl_FragCoord -> PSX screen affine, read by the portal discards */
    uint32_t cls_mask[8];  /* mat_pass >= 0: bit c keeps the faces of resolution class c */
    int32_t depth_rev;     /* the bound pipeline's depth convention, set at push (tc_push) */
} tc_pc;
_Static_assert(sizeof(tc_pc) == 116, "tc_pc mirrors terrain.vert TerrainPC");
_Static_assert(offsetof(tc_pc, depth_rev) == 112, "tc_pc mirrors terrain.vert TerrainPC");
_Static_assert(sizeof(tc_pc) <= 128, "within the 128-byte push-constant minimum");

/** @brief Push a terrain block with depth_rev set to the bound pipeline's depth convention.
 *
 * terrain.vert selects its clip-space z from depth_rev (op_scene_clip), so
 * one compiled vertex stage serves the forward-class pre-pass and key
 * pipelines and the scene-class pipelines; the value is the one
 * specialization constant 0 gives the bound pipeline
 * (rhi_bound_depth_reversed). Every push of a terrain block goes through
 * here.
 * @param dev Device.
 * @param pc The block; depth_rev is overwritten in the pushed copy. */
static void tc_push(rhi_device *dev, const tc_pc *pc)
{
    tc_pc q = *pc;
    q.depth_rev = rhi_bound_depth_reversed(dev) ? 1 : 0;
    rhi_push_constants(dev, &q, sizeof q);
}

/* --vertex-precision integer, latched once per RHI frame (below) so every
 * arena draw of one present agrees. The renderer's snap state is live: a
 * mid-present flip would otherwise split a frame between a snapped and an
 * unsnapped half of the same terrain, along the sector-residency boundary. */
static int s_tc_snap;

int openpete_tercache_ready(void) { return s_tc.ready; }
int openpete_tercache_frame_open(void) { return s_tc.draw_open; }
int openpete_tercache_frame_source(void) { return s_tc.sel_src; }
void openpete_tercache_frame_slots(char *out, int cap)
{
    int off = 0;
    for (int i = 0; i < OPTC_SLOTS && off < cap - 24; i++) {
        union { float f; uint32_t u; } w; w.f = s_tc.sel.sec[i].o[3];
        if (!(w.u & OPTC_SF_DRAWN)) continue;
        off += snprintf(out + off, (size_t)(cap - off), " %d:%03x", i, (unsigned)(w.u & 0xFFF));
    }
    if (off == 0) out[0] = 0;
}
void openpete_tercache_frame_globals(char *out, int cap)
{
    const tc_globals_ubo *g = &s_tc.sel.g;
    snprintf(out, (size_t)cap, "cam=%.0f,%.0f,%.0f of=%.1f,%.1f H=%.1f near=%.1f fog=%d gfl=%08x cull=%d m0=%.3f,%.3f,%.3f m2=%.3f,%.3f,%.3f rim=%d cov=%d",
             (g->cam_int_x4[0] + (double)g->cam_frac_x4[0]) * 0.25,
             (g->cam_int_x4[1] + (double)g->cam_frac_x4[1]) * 0.25,
             (g->cam_int_x4[2] + (double)g->cam_frac_x4[2]) * 0.25,
             g->ofH[0], g->ofH[1], g->ofH[2], g->aux[0],
             g->fog[0], (unsigned)g->fog[1], g->fog[2], g->mrow0[0], g->mrow0[1], g->mrow0[2], g->mrow2[0], g->mrow2[1], g->mrow2[2],
             g->rim_cnt[0], g->cov_cnt[0]);
}
void openpete_tercache_frame_stats(int *drawn, int *hp, int *lp, int *nearvar, int gens[8])
{
    *drawn = *hp = *lp = *nearvar = 0;
    for (int g = 0; g < 8; g++) gens[g] = 0;
    if (!s_tc.draw_open) return;
    for (int i = 0; i < OPTC_SLOTS; i++) {
        union { float f; uint32_t u; } w; w.f = s_tc.sel.sec[i].o[3];
        if (!(w.u & OPTC_SF_DRAWN)) continue;
        (*drawn)++;
        if (w.u & OPTC_SF_DRAWHP) (*hp)++;
        if (w.u & OPTC_SF_DRAWLP) (*lp)++;
        if (w.u & OPTC_SF_NEARVAR) (*nearvar)++;
        gens[(w.u >> 12) & 7u]++;
    }
}

static const tc_state *tc_state_for_stamp(uint32_t stamp, int *src);
static void tc_state_copy(tc_state *dst, const tc_state *src);

/** @brief Test whether a retained state holds a live sector: a nonzero flag word on any slot but the dead one.
 * @param T The state.
 * @return 1 when a sector is published, else 0. */
static int tc_state_content(const tc_state *T)
{
    for (int i = 0; i < OPTC_SLOTS; i++)
        if (i != (int)OPTC_SLOT_DEAD && T->sec[i].o[3] != 0.0f) return 1;
    return 0;
}

int openpete_tercache_frame_content(void)
{
    if (!s_tc.ready) return -1;
    if (!s_tc.draw_open) return 0;
    return tc_state_content(&s_tc.sel);
}

int openpete_tercache_build_content(uint32_t stamp)
{
    if (!s_tc.ready) return -1;
    int src = 0;
    const tc_state *T = tc_state_for_stamp(stamp, &src);
    return T ? tc_state_content(T) : 0;
}

int openpete_tercache_build_drawn(uint32_t stamp)
{
    if (!s_tc.ready) return -1;
    int src = 0;
    const tc_state *T = tc_state_for_stamp(stamp, &src);
    if (!T) return 0;
    int n = 0;
    for (int i = 0; i < OPTC_SLOTS; i++) {
        union { float f; uint32_t u; } w; w.f = T->sec[i].o[3];
        n += (w.u & OPTC_SF_DRAWN) != 0;
    }
    return n;
}

void openpete_tercache_hold_take(int idx, uint32_t stamp)
{
    if (idx < 0 || idx >= OPENPETE_TERCACHE_HOLDS) return;
    s_tc.hold[idx].valid = 0;
    if (!s_tc.ready) return;
    int src = 0;
    const tc_state *T = tc_state_for_stamp(stamp, &src);
    if (!T || src == 4) { s_tc.hold_misses++; return; }
    tc_state_copy(&s_tc.hold[idx], T);
    s_tc.hold_takes++;
}

void openpete_tercache_hold_drop(int idx)
{
    if (idx >= 0 && idx < OPENPETE_TERCACHE_HOLDS) s_tc.hold[idx].valid = 0;
}

void openpete_tercache_hold_counts(unsigned *takes, unsigned *misses, unsigned *drops)
{
    if (takes) *takes = s_tc.hold_takes;
    if (misses) *misses = s_tc.hold_misses;
    if (drops) *drops = s_tc.hold_drops;
}

/** @brief Create one cache pipeline into its slot: now, or at the end of the caller's open pipeline batch.
 *
 * The create is the one rhi_create_pipeline makes; the slot is NULL until
 * it installs, and stays NULL when it fails. Under OPENPETE_TERRAIN_FACEID a
 * colour pipeline (fragment stage terrain.frag) takes terrain_faceid.frag
 * with blending off; its vertex stage, layout and every other state stay
 * the shipped pipeline's.
 * @param dev Device.
 * @param d Pipeline description, copied.
 * @param out The slot. */
static void tc_pipe(rhi_device *dev, const rhi_pipeline_desc *d, rhi_pipeline **out)
{
    rhi_pipeline_desc dd = *d;
    if (openpete_tercache_faceid() && dd.frag_spv && strcmp(dd.frag_spv, "terrain.frag.spv") == 0) {
        dd.frag_spv = "terrain_faceid.frag.spv";
        dd.blend = RHI_BLEND_NONE;
    }
    *out = NULL;
    (void)rhi_create_pipeline_slot(dev, &dd, NULL, 0, NULL, 0, (void **)out, NULL);
}

/* The vertex stage of each arena family (OPENPETE_MATERIAL_TER_VARIANTS
 * order: plain, anim, near, then the anim and near cell records): row 0 the
 * shipped stage, row 1 the TERRAIN_SECDELTA per-sector rounding control. */
enum { TC_V_PLAIN, TC_V_ANIM, TC_V_NEAR, TC_V_CELLS_ANIM, TC_V_CELLS_NEAR };
static const char *const k_tc_vert[2][OPENPETE_MATERIAL_TER_VARIANTS] = {
    { "terrain.vert.spv", "terrain_anim.vert.spv", "terrain_near.vert.spv",
      "terrain_cells_anim.vert.spv", "terrain_cells_near.vert.spv" },
    { "terrain_ctl.vert.spv", "terrain_anim_ctl.vert.spv", "terrain_near_ctl.vert.spv",
      "terrain_cells_anim_ctl.vert.spv", "terrain_cells_near_ctl.vert.spv" },
};

/** @brief Return the k_tc_vert row OPENPETE_TERRAIN_SECDELTA selects, latched at the first call.
 *
 * Set and not "0": row 1, the per-sector rounding control (a diagnostic
 * stage). Otherwise row 0, the shipped stage. Read through
 * openpete_config_device_value, so the launcher's describe, bring-up's
 * init and every material pipeline take the same stage.
 * @return 0 or 1. */
static int tc_stage_row(void)
{
    static int s_row = -1;
    if (s_row < 0) {
        const char *e = openpete_config_device_value("OPENPETE_TERRAIN_SECDELTA");
        s_row = (e && *e && *e != '0') ? 1 : 0;
    }
    return s_row;
}

int openpete_tercache_faceid(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        const char *e = getenv("OPENPETE_TERRAIN_FACEID");
        s_on = (e && *e && *e != '0');
    }
    return s_on;
}

/* The shipped stages compiled with TERRAIN_DEPTH_SPEC (k_tc_vert order):
 * the depth convention from specialization constant 0, fixed per pipeline,
 * in place of the push-constant select. A pipeline linked from libraries
 * shares one k_tc_vert pre-rasterization library across both depth classes;
 * its background optimization, and every whole create, compiles this stage
 * instead (rhi_pipeline_desc.vert_spv_whole), so the optimized pipeline
 * carries no select. */
static const char *const k_tc_vert_dspec[OPENPETE_MATERIAL_TER_VARIANTS] = {
    "terrain_dspec.vert.spv", "terrain_anim_dspec.vert.spv", "terrain_near_dspec.vert.spv",
    "terrain_cells_anim_dspec.vert.spv", "terrain_cells_near_dspec.vert.spv",
};

/** @brief Return the selected row's vertex stage of an arena family.
 *
 * @param v The family, a TC_V_* value.
 * @return The stage's SPIR-V name. */
static const char *tc_vert(int v)
{
    return k_tc_vert[tc_stage_row()][v];
}

/** @brief Return the whole-create vertex stage of an arena family (rhi_pipeline_desc.vert_spv_whole).
 *
 * Row 0 takes k_tc_vert_dspec; the diagnostic row 1 has no specialized
 * twin and keeps its select in every create.
 * @param v The family, a TC_V_* value.
 * @return The stage's SPIR-V name, or NULL. */
static const char *tc_vert_whole(int v)
{
    return tc_stage_row() == 0 ? k_tc_vert_dspec[v] : NULL;
}

/** @brief Build one cell-record pipeline set, one pipeline per TC_CP_* slot.
 *
 * The slots share terrain.frag and depth test and differ as the world
 * pipelines do: stencil MARK, the three blend equations with depth writes
 * off, the depth prepass, and the OT-key emit. Cell records draw no hole
 * fill, so the set holds no colour-off prepass slot. Each create is
 * made now, or at the end of the open pipeline batch (tc_cell_pipes_ok
 * then reads the set).
 * @param dev Device.
 * @param anim Nonzero for the anim record layout and stage, 0 for the near ones.
 * @param out Receives the pipelines; a failed slot is NULL. */
static void tc_make_cell_pipes(rhi_device *dev, int anim, rhi_pipeline *out[TC_CP_N])
{
    const int v = anim ? TC_V_CELLS_ANIM : TC_V_CELLS_NEAR;
    const rhi_pipeline_desc base = {
        .vert_spv = tc_vert(v), .vert_spv_whole = tc_vert_whole(v), .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_cells = true, .tex_ssbo_set2 = true,
        .terrain_anim = anim != 0, .terrain_near = anim == 0,
        .pair_set7 = true, .push_const_size = sizeof(tc_pc),
    };
    rhi_pipeline_desc d[TC_CP_N];
    for (int i = 0; i < TC_CP_N; i++) d[i] = base;
    d[TC_CP_MARK].stencil = RHI_STENCIL_MARK;
    d[TC_CP_ALPHA].blend = RHI_BLEND_ALPHA;    d[TC_CP_ALPHA].depth_write_off = true;
    d[TC_CP_ADD].blend   = RHI_BLEND_ADD;      d[TC_CP_ADD].depth_write_off = true;
    d[TC_CP_SUB].blend   = RHI_BLEND_SUBTRACT; d[TC_CP_SUB].depth_write_off = true;
    d[TC_CP_PREPASS].frag_spv = "terrain_prepass_cutout.frag.spv";   /* colour = packed depth */
    d[TC_CP_PREPASS].blend = RHI_BLEND_ALPHA;
    d[TC_CP_PREPASS].depth_class = RHI_DEPTH_FORWARD;
    d[TC_CP_KEY].frag_spv = "terrain_key_cache.frag.spv";
    d[TC_CP_KEY].depth_class = RHI_DEPTH_FORWARD;
    for (int i = 0; i < TC_CP_N; i++) tc_pipe(dev, &d[i], &out[i]);
}

/** @brief Report whether every slot of a cell-record pipeline set built.
 *
 * @param set The set tc_make_cell_pipes filled.
 * @return 1 when every slot holds a pipeline, else 0. */
static int tc_cell_pipes_ok(rhi_pipeline *const set[TC_CP_N])
{
    int ok = 1;
    for (int i = 0; i < TC_CP_N; i++) ok &= set[i] != NULL;
    return ok;
}

/** @brief Create every terrain cache pipeline into a set: now, or at the end of the caller's open pipeline batch.
 *
 * The world, anim, near and cell-record families, each in every draw
 * class. Inside rhi_pipeline_describe_begin each create is recorded
 * instead (openpete_tercache_describe).
 * @param dev Device.
 * @param ps Receives the pipelines; a failed slot is NULL. */
static void tc_make_pipes(rhi_device *dev, tc_pipe_set *ps)
{
    const rhi_pipeline_desc world_desc = {
        .vert_spv = tc_vert(TC_V_PLAIN), .vert_spv_whole = tc_vert_whole(TC_V_PLAIN),
        .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .pair_set7 = true, .tex_ssbo_set2 = true,
        .push_const_size = sizeof(tc_pc),
    };
    rhi_pipeline_desc d;
    d = world_desc;
    tc_pipe(dev, &d, &ps->pipe_world);
    d = world_desc; d.stencil = RHI_STENCIL_MARK;
    tc_pipe(dev, &d, &ps->pipe_world_mark);
    d = world_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_alpha);
    d = world_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_add);
    d = world_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_sub);
    d = world_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    tc_pipe(dev, &d, &ps->pipe_fill);
    /* Pre-pass pair: the WORLD faces write their footprint depth and blend
     * their packed depth into colour (the occlusion grid's source) at alpha
     * 1 where terrain.frag would paint and alpha 0 on a transparent texel
     * (terrain_prepass_cutout.frag); the hole-fill faces write depth only. */
    d = world_desc; d.frag_spv = "terrain_prepass_cutout.frag.spv";
    d.blend = RHI_BLEND_ALPHA; d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_prepass);
    d.blend = RHI_BLEND_NONE;
    d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_prepass_depth);
    d = world_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_key);
    /* Anim variants: identical state, the anim vertex stage + the
     * binding-1 prev twin. The FILL variant serves the env-anim colour
     * sectors' LP hole-fill backing; a poly-anim sector never publishes
     * SF_FILLLP, so the vertex stage kills its LP in that pass. */
    const rhi_pipeline_desc anim_desc = {
        .vert_spv = tc_vert(TC_V_ANIM), .vert_spv_whole = tc_vert_whole(TC_V_ANIM),
        .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_anim = true, .pair_set7 = true,
        .tex_ssbo_set2 = true,
        .push_const_size = sizeof(tc_pc),
    };
    d = anim_desc;
    tc_pipe(dev, &d, &ps->pipe_a_world);
    d = anim_desc; d.stencil = RHI_STENCIL_MARK;
    tc_pipe(dev, &d, &ps->pipe_a_world_mark);
    d = anim_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_a_alpha);
    d = anim_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_a_add);
    d = anim_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_a_sub);
    d = anim_desc; d.frag_spv = "terrain_prepass_cutout.frag.spv";
    d.blend = RHI_BLEND_ALPHA; d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_a_prepass);
    d.blend = RHI_BLEND_NONE;
    d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_a_prepass_depth);
    d = anim_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_a_key);
    d = anim_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    tc_pipe(dev, &d, &ps->pipe_a_fill);
    /* Near variants: identical state, the clip-fan vertex stage + the
     * binding-1 member-context twin. Includes FILL - near sectors keep the
     * LP hole-fill backing (unlike poly-anim, which is excluded from it). */
    const rhi_pipeline_desc near_desc = {
        .vert_spv = tc_vert(TC_V_NEAR), .vert_spv_whole = tc_vert_whole(TC_V_NEAR),
        .frag_spv = "terrain.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .terrain_near = true, .pair_set7 = true,
        .tex_ssbo_set2 = true,
        .push_const_size = sizeof(tc_pc),
    };
    d = near_desc;
    tc_pipe(dev, &d, &ps->pipe_n_world);
    d = near_desc; d.stencil = RHI_STENCIL_MARK;
    tc_pipe(dev, &d, &ps->pipe_n_world_mark);
    d = near_desc; d.blend = RHI_BLEND_ALPHA; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_n_alpha);
    d = near_desc; d.blend = RHI_BLEND_ADD; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_n_add);
    d = near_desc; d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_n_sub);
    d = near_desc; d.stencil = RHI_STENCIL_TEST_ZERO;
    tc_pipe(dev, &d, &ps->pipe_n_fill);
    d = near_desc; d.frag_spv = "terrain_prepass_cutout.frag.spv";
    d.blend = RHI_BLEND_ALPHA; d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_n_prepass);
    d.blend = RHI_BLEND_NONE;
    d.frag_spv = "terrain_prepass.frag.spv"; d.color_write_off = true;
    tc_pipe(dev, &d, &ps->pipe_n_prepass_depth);
    d = near_desc; d.frag_spv = "terrain_key_cache.frag.spv";
    d.depth_class = RHI_DEPTH_FORWARD;
    tc_pipe(dev, &d, &ps->pipe_n_key);

    /* Cell-record variants: the same states over the instanced cell stages. */
    tc_make_cell_pipes(dev, 0, ps->pipe_cn);
    tc_make_cell_pipes(dev, 1, ps->pipe_ca);
}

void openpete_tercache_describe(rhi_device *dev)
{
    tc_pipe_set ps;
    if (dev) tc_make_pipes(dev, &ps);
}

int openpete_tercache_init(rhi_device *dev, void (*batch)(int open))
{
    memset(&s_tc, 0, sizeof(s_tc));
    s_tc.dev = dev;
    if (!dev) return 0;
    if (batch) batch(1);

    tc_make_pipes(dev, &s_tc.ps);
    if (batch) batch(0);
    const int cells_ok = tc_cell_pipes_ok(s_tc.ps.pipe_cn) & tc_cell_pipes_ok(s_tc.ps.pipe_ca);

    int ok = cells_ok && s_tc.ps.pipe_world && s_tc.ps.pipe_world_mark && s_tc.ps.pipe_alpha &&
             s_tc.ps.pipe_add && s_tc.ps.pipe_sub && s_tc.ps.pipe_fill && s_tc.ps.pipe_prepass &&
             s_tc.ps.pipe_prepass_depth && s_tc.ps.pipe_key &&
             s_tc.ps.pipe_a_world && s_tc.ps.pipe_a_world_mark && s_tc.ps.pipe_a_alpha &&
             s_tc.ps.pipe_a_add && s_tc.ps.pipe_a_sub && s_tc.ps.pipe_a_prepass &&
             s_tc.ps.pipe_a_prepass_depth && s_tc.ps.pipe_a_key && s_tc.ps.pipe_a_fill &&
             s_tc.ps.pipe_n_world && s_tc.ps.pipe_n_world_mark && s_tc.ps.pipe_n_alpha &&
             s_tc.ps.pipe_n_add && s_tc.ps.pipe_n_sub && s_tc.ps.pipe_n_fill &&
             s_tc.ps.pipe_n_prepass && s_tc.ps.pipe_n_prepass_depth && s_tc.ps.pipe_n_key;
    /* No level has loaded yet, so every arena starts at the floor; the
     * first latch posts the replacement sized from the level's content. */
    for (int a = 0; ok && a < OPTC_ARENA_COUNT; a++) {
        tc_pair *pr = tc_pair_make(dev, a, tc_initial_cap(a, NULL));
        if (!pr) { ok = 0; break; }
        s_tc.arena[a].buf[0].cap = pr->cap;
        atomic_store_explicit(&s_tc.arena[a].buf[0].pair, pr, memory_order_relaxed);
        atomic_store_explicit(&s_tc.arena[a].n, 1, memory_order_relaxed);
        s_tc.bufs_max[a] = 1;
    }
    if (!ok) {
        fprintf(stderr, "[ter-gpu] init failed (pipeline or arena alloc) - "
                        "terrain cache disabled, CPU path stays\n");
        openpete_tercache_shutdown(dev);
        return 0;
    }
    /* Set-4 stand-in (init-time: uploads are illegal inside a render pass). */
    s_tc.hd_dummy = rhi_create_rgba8(dev, 1, 1);
    if (s_tc.hd_dummy) {
        const uint32_t px = 0;
        rhi_update_rgba8(dev, s_tc.hd_dummy, &px);
    }
    /* The UBO (block 0 globals, block 1 sectors) is a device object, so it
     * is made here on the device-owning thread rather than lazily by the
     * first build, which may run on the thread that runs the guest. */
    s_tc.ubo = rhi_create_ubo(dev, (uint32_t)TC_UBO_BLOCK, 2);
    if (!s_tc.ubo) return 0;
    s_tc.prev_cap = tc_prev_cap_init();
    tc_store *st = tc_store_make(dev, s_tc.prev_cap);
    atomic_store_explicit(&s_tc.prev_live, st, memory_order_release);
    s_tc.prev_shadow = (uint32_t *)calloc((size_t)s_tc.prev_cap * OPTC_PREV_REC_WORDS,
                                          sizeof(uint32_t));
    s_tc.shadow_cap = s_tc.prev_shadow ? s_tc.prev_cap : 0u;
    if (!st || !s_tc.prev_shadow || !s_tc.hd_dummy) {
        fprintf(stderr, "[ter-gpu] init failed (record store or set-2 stand-in) - "
                        "terrain cache disabled, CPU path stays\n");
        openpete_tercache_shutdown(dev);
        return 0;
    }
    s_tc.prev_alloc.cap = s_tc.prev_cap;
    s_tc.ready = 1;
    return 1;
}

/** @brief Return 0 when OPENPETE_TC_RANGE_DRAW=0 forces whole-range arena draws, else 1.
 *
 * Read once. The whole-range leg is the reference an A/B of the range
 * draws compares against: both must produce the same image. */
static int tc_range_draw_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("OPENPETE_TC_RANGE_DRAW");
        on = !(e && *e == '0');
    }
    return on;
}

static void tc_exhaust_warn(void);

static void tc_rb_free(rhi_device *dev);

/** @brief Print the level-edge census row (tercache-edge) to stderr.
 *
 * Called by shutdown after the arena and store frees and before s_tc is
 * zeroed, so live_bytes reads 0 unless a device object leaked. */
static void tc_edgecen_print(void)
{
    const tc_edge_census *e = &s_tc.edgecen;
    fprintf(stderr, "tercache-edge: live_bytes=%llu peak_bytes=%llu spread_pairs=%u spread_span=%u "
            "recycle_hits=%u recycle_misses=%u recycle_bytes=%llu spares_parked=%u "
            "held_releases=%u held_refusals=%u\n",
            (unsigned long long)e->live_bytes, (unsigned long long)e->peak_bytes,
            e->spread_pairs, e->spread_span, e->recycle_hits, e->recycle_misses,
            (unsigned long long)e->recycle_bytes, e->spares_parked, e->held_releases,
            e->held_refusals);
}

void openpete_tercache_shutdown(rhi_device *dev)
{
    if (!dev) return;
    if (s_tc.ready) { tc_exhaust_warn(); tc_est_census_level(); }
    openpete_tercache_get_stats(&s_tc_final);
    s_tc_final_valid = 1;
    if (s_tc.ring_builds)
        fprintf(stderr, "[tercache] ring: %u scene build(s), %u rebuild(s) of a held id, "
                "%u overwrite(s) of a live state, %u duplicate-stamp select(s) of an older build\n",
                s_tc.ring_builds, s_tc.ring_reuses, s_tc.ring_overwrites, s_tc.ring_dup_selects);
    {
        const char *e = getenv("OPENPETE_TC_SUBMIT_CENSUS");
        if (e && *e && *e != '0' && s_tc.cen_presents)
            fprintf(stderr, "[tercache] submit census: presents=%llu "
                    "verts_per_present=%.0f whole_range_verts_per_present=%.0f "
                    "draws_per_present=%.1f nolist_draws=%u range_draw=%d "
                    "class_dropped_draw=%llu class_dropped_fill=%llu range_overflows=%u\n",
                    (unsigned long long)s_tc.cen_presents,
                    (double)s_tc.cen_verts / (double)s_tc.cen_presents,
                    (double)s_tc.cen_verts_whole / (double)s_tc.cen_presents,
                    (double)s_tc.cen_draws / (double)s_tc.cen_presents,
                    s_tc.cen_nolist, tc_range_draw_on(),
                    (unsigned long long)s_tc.class_dropped[TC_RC_DRAW],
                    (unsigned long long)s_tc.class_dropped[TC_RC_FILL], s_tc.range_overflows);
    }
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        for (int b = 0; b < TC_BUF_SLOTS; b++) free(s_tc.reg[a][b].c);
    if (s_tc.hd_dummy) { rhi_destroy_texture(dev, s_tc.hd_dummy); s_tc.hd_dummy = NULL; }
    tc_rb_free(dev);
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) tc_arena_free_all(dev, a);
    rhi_pipeline **const named[] = {
        &s_tc.ps.pipe_world, &s_tc.ps.pipe_world_mark, &s_tc.ps.pipe_alpha, &s_tc.ps.pipe_add,
        &s_tc.ps.pipe_sub, &s_tc.ps.pipe_fill, &s_tc.ps.pipe_prepass, &s_tc.ps.pipe_key,
        &s_tc.ps.pipe_a_world, &s_tc.ps.pipe_a_world_mark, &s_tc.ps.pipe_a_alpha,
        &s_tc.ps.pipe_a_add, &s_tc.ps.pipe_a_sub, &s_tc.ps.pipe_a_prepass, &s_tc.ps.pipe_a_key,
        &s_tc.ps.pipe_a_fill,
        &s_tc.ps.pipe_n_world, &s_tc.ps.pipe_n_world_mark, &s_tc.ps.pipe_n_alpha,
        &s_tc.ps.pipe_n_add, &s_tc.ps.pipe_n_sub, &s_tc.ps.pipe_n_fill, &s_tc.ps.pipe_n_prepass,
        &s_tc.ps.pipe_n_key,
        &s_tc.ps.pipe_prepass_depth, &s_tc.ps.pipe_a_prepass_depth, &s_tc.ps.pipe_n_prepass_depth,
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (*named[i]) rhi_destroy_pipeline(dev, *named[i]);
    for (int i = 0; i < TC_CP_N; i++) {
        if (s_tc.ps.pipe_cn[i]) rhi_destroy_pipeline(dev, s_tc.ps.pipe_cn[i]);
        if (s_tc.ps.pipe_ca[i]) rhi_destroy_pipeline(dev, s_tc.ps.pipe_ca[i]);
    }
    if (s_tc.ubo)             rhi_destroy_ubo(dev, s_tc.ubo);
    tc_store_free(dev, atomic_load_explicit(&s_tc.prev_live, memory_order_relaxed));
    tc_store_free(dev, s_tc.prev_new);
    tc_store_free(dev, s_tc.rz_store);
    tc_store_free(dev, s_tc.rz_old_store);
    for (uint32_t i = atomic_load_explicit(&s_tc.rq_tail, memory_order_relaxed);
         i != atomic_load_explicit(&s_tc.rq_head, memory_order_relaxed); i++) {
        tc_pair_free(dev, s_tc.rq[i % TC_RETIRE_Q].pair);
        tc_store_free(dev, s_tc.rq[i % TC_RETIRE_Q].store);
    }
    free(s_tc.prev_shadow);
    free(s_tc.prev_alloc.free_list);
    if (s_tc.ready) tc_edgecen_print();
    memset(&s_tc, 0, sizeof(s_tc));
}

/* ---- allocator ---------------------------------------------------------- */

/** @brief Return a vertex range to one buffer's free list.
 *
 * The list stays sorted by first index, and the range coalesces with an
 * adjacent span on either side. The list grows when a range neither
 * coalesces nor fits; a growth that cannot allocate drops the range and
 * counts it in dropped_spans (unusable until the level-edge replacement
 * resets the buffer). A span that ends at the high-water mark leaves the
 * list and lowers the mark to its first element, so the allocation front
 * tracks the live content rather than the visit's peak.
 * @param a Buffer.
 * @param first First element of the range.
 * @param count Element count of the range. */
static void tc_free_insert(tc_buf *a, uint32_t first, uint32_t count)
{
    /* insert sorted + coalesce neighbours */
    int i = 0;
    while (i < a->free_n && a->free_list[i].first < first) i++;
    if (i > 0 && a->free_list[i-1].first + a->free_list[i-1].count == first) {
        a->free_list[i-1].count += count;
        if (i < a->free_n && a->free_list[i-1].first + a->free_list[i-1].count == a->free_list[i].first) {
            a->free_list[i-1].count += a->free_list[i].count;
            memmove(&a->free_list[i], &a->free_list[i+1],
                    (size_t)(a->free_n - i - 1) * sizeof(tc_span));
            a->free_n--;
        }
    } else if (i < a->free_n && first + count == a->free_list[i].first) {
        a->free_list[i].first = first;
        a->free_list[i].count += count;
    } else {
        if (a->free_n >= a->free_cap) {
            const int nc = a->free_cap ? a->free_cap * 2 : 64;
            tc_span *nl = (tc_span *)realloc(a->free_list, (size_t)nc * sizeof(*nl));
            if (!nl) { s_tc.dropped_spans++; return; }   /* leaked until level reset */
            a->free_list = nl; a->free_cap = nc;
        }
        memmove(&a->free_list[i+1], &a->free_list[i],
                (size_t)(a->free_n - i) * sizeof(tc_span));
        a->free_list[i].first = first;
        a->free_list[i].count = count;
        a->free_n++;
    }
    if (a->free_n > 0) {
        const tc_span *top = &a->free_list[a->free_n - 1];
        if (top->first + top->count == a->high) { a->high = top->first; a->free_n--; }
    }
}

/** @brief Return a released chunk range to its buffer's free list, unless the buffer is a source.
 *
 * A consolidation source takes no allocation, and a sealed buffer's ranges
 * may still be drawn by a held state until the buffer retires, so neither
 * list takes the range.
 * @param d The release. */
static void tc_defer_return(const tc_defer *d)
{
    tc_buf *rb = &s_tc.arena[d->arena].buf[d->buf];
    if (!rb->source) tc_free_insert(rb, d->first, d->count);
}

/** @brief Allocate a range in one buffer: first fit in its free list, else a grow of its high-water mark.
 *
 * Updates the buffer's live-element and chunk census on success.
 * @param a Buffer.
 * @param n Element count.
 * @param out_first Receives the range's first element.
 * @return 0 on success, -1 when this buffer has no room for the range. */
static int tc_buf_alloc(tc_buf *a, uint32_t n, uint32_t *out_first)
{
    for (int i = 0; i < a->free_n; i++) {           /* first fit */
        if (a->free_list[i].count < n) continue;
        *out_first = a->free_list[i].first;
        a->free_list[i].first += n;
        a->free_list[i].count -= n;
        if (a->free_list[i].count == 0) {
            memmove(&a->free_list[i], &a->free_list[i+1],
                    (size_t)(a->free_n - i - 1) * sizeof(tc_span));
            a->free_n--;
        }
        a->used += n; a->chunks++;
        return 0;
    }
    if (a->high + n > a->cap) return -1;            /* grow high-water */
    *out_first = a->high;
    a->high += n;
    a->used += n; a->chunks++;
    return 0;
}

/** @brief Return an arena's occupied element count: live chunks plus released ranges still in the defer window, sealed buffers excluded.
 *
 * A released range takes no allocation until its defer window passes, so a
 * sector that re-bakes every build holds its new chunk and up to
 * TC_DEFER_PRESENTS builds of its old ones; the live count alone
 * undercounts such an arena by that churn.
 * @param arena Arena index.
 * @return Elements. */
static uint64_t tc_arena_occupied(int arena)
{
    const tc_arena *a = &s_tc.arena[arena];
    uint64_t occ = 0;
    for (int b = 0; b < TC_BUF_SLOTS; b++)
        if (!a->buf[b].sealed && tc_live_pair(arena, b)) occ += a->buf[b].used;
    for (int i = 0; i < s_tc.defer_n; i++) {
        const tc_defer *d = &s_tc.defer[i];
        if (d->arena == arena && !a->buf[d->buf].sealed && tc_live_pair(arena, d->buf)) occ += d->count;
    }
    return occ;
}

/** @brief Return the elements a buffer can allocate now: the room above its high-water mark plus its free list.
 * @param bf Buffer.
 * @return Elements. */
static uint64_t tc_buf_room(const tc_buf *bf)
{
    uint64_t room = bf->cap - bf->high;
    for (int i = 0; i < bf->free_n; i++) room += bf->free_list[i].count;
    return room;
}

/** @brief Count an arena's occupied buffer slots and find its lowest empty one.
 * @param arena Arena index.
 * @param empty Receives the lowest empty slot, or -1 when every slot is taken.
 * @return Occupied slots that are not consolidation sources. */
static int tc_arena_slots(int arena, int *empty)
{
    const tc_arena *a = &s_tc.arena[arena];
    int k = 0;
    *empty = -1;
    for (int b = 0; b < TC_BUF_SLOTS; b++) {
        if (!tc_live_pair(arena, b)) { if (*empty < 0) *empty = b; continue; }
        if (!a->buf[b].source) k++;
    }
    return k;
}

/** @brief Post one growth or consolidation request for an arena to the device-owning thread.
 *
 * Builder thread. Records the service call and build counts the adoption
 * log measures against, and opens the arena's request window (TC_OVF_REQ).
 * @param arena Arena index.
 * @param kind TC_REQ_GROW or TC_REQ_CONS.
 * @param want Elements the buffer is to hold. */
static void tc_post_request(int arena, int kind, uint32_t want)
{
    tc_arena *a = &s_tc.arena[arena];
    s_tc.ovf_want[arena] = want;
    s_tc.ovf_req_svc[arena] = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed);
    s_tc.ovf_req_build[arena] = s_tc.present_ctr;
    a->ovf_kind = kind;
    a->ovf_state = TC_OVF_REQ;
    atomic_fetch_or_explicit(&s_tc.ovf_req, 1u << arena, memory_order_release);
}

/** @brief Ask for a consolidation buffer for an arena, making every buffer it holds a source.
 *
 * Builder thread. The buffer holds max(primary size, 2 x occupied, the
 * refused demand); occupied (tc_arena_occupied) counts live chunks and
 * deferred ranges and excludes sealed buffers. No cap applies: a buffer
 * below the occupied count refuses the migration it is made for and the
 * arena consolidates again; the sources' capacity bounds the occupied
 * count. From the request on, no
 * source takes an allocation, and each source retires once its chunks have
 * migrated (tc_source_retire). Refused, as by a backoff, when the arena has
 * no empty slot or a previous consolidation's sources still remain; the
 * reason is kept in cons_why for the exhaustion warning.
 * @param arena Arena index.
 * @param demand The refused chunk's element count, or 0.
 * @return 1 when the request was posted, else 0. */
static int tc_request_consolidation(int arena, uint32_t demand)
{
    tc_arena *a = &s_tc.arena[arena];
    int empty;
    (void)tc_arena_slots(arena, &empty);
    if (empty < 0 || a->cons_live) {
        a->cons_why = empty < 0 ? TC_EXH_SLOTS : TC_EXH_MIGRATING;
        return 0;
    }
    const uint64_t prim = tc_initial_cap(arena, s_tc.est_have ? s_tc.est : NULL);
    const uint64_t occ = tc_arena_occupied(arena);
    uint64_t want = 2u * occ;
    if (want < prim) want = prim;
    if (want < demand) want = demand;
    if (want > 0x7FFFFFFFu) want = 0x7FFFFFFFu;
    for (int b = 0; b < TC_BUF_SLOTS; b++)
        if (tc_live_pair(arena, b)) { a->buf[b].source = 1; a->buf[b].zeroed = 0; }
    a->cons_live = 1;
    a->cons_ticks = 0;
    fprintf(stderr, "[ter-gpu] arena %d consolidation request: %u elements (occupied %llu)\n",
            arena, (uint32_t)want, (unsigned long long)occ);
    tc_post_request(arena, TC_REQ_CONS, (uint32_t)want);
    return 1;
}

/** @brief Start or lengthen an arena's refusal backoff: 8 builds, doubling to 1024.
 * @param a The arena. */
static void tc_backoff(tc_arena *a)
{
    a->ovf_state = TC_OVF_BACKOFF;
    a->backoff = a->backoff ? (a->backoff < 1024u ? a->backoff * 2u : 1024u) : 8u;
    a->retry_at = s_tc.present_ctr + a->backoff;
}

/** @brief Record that an arena has neither a growth nor a consolidation buffer coming and start its refusal backoff.
 *
 * Builder thread. Until the backoff's retry, a chunk the arena refuses
 * stays claimed to the CPU walk and counts in `claims`; the level's
 * warning line (tc_exhaust_warn) names the arena, @p sites with the
 * refusals since the arena last installed a buffer, and those claims.
 * @param arena Arena index.
 * @param sites TC_EXH_* bits naming the refusals. */
static void tc_exhaust(int arena, unsigned sites)
{
    tc_arena *a = &s_tc.arena[arena];
    a->exh_sites |= (uint8_t)(sites | a->refused);
    tc_backoff(a);
}

/** @brief Print one warning line naming each arena that ran out of room during the level, then clear the level's record.
 *
 * Builder thread, at the level edge's resize request and at shutdown, so a
 * level that exhausted any arena prints exactly one line, naming per
 * arena the refusals (tc_exhaust) and the claims made outside every
 * request window during the level; a level with none prints nothing.
 * Counted in `exhaust_warnings`. */
static void tc_exhaust_warn(void)
{
    static const char *const names[] = { "grow", "cons", "edge", "slots", "migrating" };
    char line[2048];
    size_t at = 0;
    line[0] = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        tc_arena *ar = &s_tc.arena[a];
        const unsigned sites = ar->exh_sites;
        char why[64] = "";
        for (int k = 0; k < 5; k++) {
            const size_t l = strlen(why);
            if (sites & (1u << k)) snprintf(why + l, sizeof why - l, "%s%s", l ? "+" : "", names[k]);
        }
        if (sites && at < sizeof line) {
            const int n = snprintf(line + at, sizeof line - at, " arena %d refused %s claims %u;",
                                   a, why, ar->lvl_claims);
            if (n > 0) at += (size_t)n;
        }
        ar->exh_sites = 0;
        ar->lvl_claims = 0;
    }
    if (!line[0]) return;
    s_tc.exhaust_warnings++;
    fprintf(stderr, "[ter-gpu] warning: arena exhaustion at level %d:%s\n",
            atomic_load_explicit(&s_tc.level_id, memory_order_relaxed), line);
}

/** @brief Return the slot of the last buffer the allocator tries: the highest occupied slot that is not a source.
 *
 * chunk_alloc is first fit in slot order, so this buffer takes a chunk
 * only after every lower slot refused it, and its room is the arena's last
 * reserve. A slot reused after a source retires sits below it, so the
 * buffer is not always the most recently installed one.
 * @param arena Arena index.
 * @return The slot, or -1 when no buffer takes allocations. */
static int tc_newest_buf(int arena)
{
    const tc_arena *a = &s_tc.arena[arena];
    for (int b = atomic_load_explicit(&a->n, memory_order_relaxed) - 1; b >= 0; b--)
        if (tc_live_pair(arena, b) && !a->buf[b].source && !a->buf[b].sealed) return b;
    return -1;
}

/** @brief Return the capacity of an arena's buffers that take allocations: occupied slots that are neither sources nor sealed.
 * @param arena Arena index.
 * @return Elements. */
static uint64_t tc_arena_capacity(int arena)
{
    const tc_arena *a = &s_tc.arena[arena];
    uint64_t cap = 0;
    for (int b = 0; b < TC_BUF_SLOTS; b++)
        if (!a->buf[b].source && !a->buf[b].sealed && tc_live_pair(arena, b)) cap += a->buf[b].cap;
    return cap;
}

/** @brief Ask for one more buffer for an arena: growth below the buffer cap, consolidation at it.
 *
 * Builder thread, from a chunk every buffer of the arena refused (@p demand
 * > 0) or from an allocation into the last buffer the allocator tries
 * (tc_newest_buf) that left it under 25% of its capacity allocatable
 * (tc_buf_room) (@p demand 0). A growth buffer takes the arena's capacity
 * over its allocating buffers to max(primary size, 1.5 x that capacity),
 * and holds at least @p demand, the primary size being tc_initial_cap (the
 * arena's estimate, or OPENPETE_TC_INITIAL_CAP): from the primary size,
 * tc_buf_max() buffers hold 1.5^(tc_buf_max() - 1) primary sizes, and a
 * primary size that undercounts the content by up to that factor grows
 * without a consolidation. At that cap a refusal asks for a consolidation instead; the
 * watermark asks for nothing. Dropped while a request is outstanding or a
 * refusal's backoff runs. Every request is logged with the newest buffer's
 * high-water mark, live count and room.
 * @param arena Arena index.
 * @param demand Element count of the refused chunk, or 0 for the watermark. */
static void tc_request_grow(int arena, uint32_t demand)
{
    tc_arena *a = &s_tc.arena[arena];
    if (s_tc.rz_window || a->ovf_state == TC_OVF_REQ) return;   /* the edge replaces every buffer */
    if (a->ovf_state == TC_OVF_BACKOFF) {
        if ((int32_t)(s_tc.present_ctr - a->retry_at) < 0) return;
        a->ovf_state = TC_OVF_IDLE;
        if (demand && tc_request_consolidation(arena, demand)) return;
    }
    int empty;
    const int k = tc_arena_slots(arena, &empty);
    if (k >= tc_buf_max() || empty < 0) {
        if (demand && !tc_request_consolidation(arena, demand)) tc_exhaust(arena, a->cons_why);
        return;
    }
    const uint64_t prim = tc_initial_cap(arena, s_tc.est_have ? s_tc.est : NULL);
    const uint64_t held = tc_arena_capacity(arena);
    uint64_t step = held / 2u;                       /* to 1.5 x the capacity */
    if (prim > held + step) step = prim - held;      /* or to the primary size */
    if (demand > step) step = demand;
    if (step == 0u) step = 1u;
    const uint32_t want = step > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)step;
    const int nb = tc_newest_buf(arena);
    const tc_buf *top = nb >= 0 ? &a->buf[nb] : NULL;
    fprintf(stderr, "[ter-gpu] arena %d grow request: %s demand=%u want=%u buffers=%d "
                    "newest high=%u used=%u room=%llu cap=%u\n",
            arena, demand ? "refusal" : "watermark", demand, want, k,
            top ? top->high : 0u, top ? top->used : 0u,
            (unsigned long long)(top ? tc_buf_room(top) : 0u), top ? top->cap : 0u);
    tc_post_request(arena, TC_REQ_GROW, want);
}

/* ---- chunk registry and draw ranges -------------------------------------
 * The builder records every chunk it writes with the sector slot and
 * generation its elements carry, so that each committed state can list the
 * element spans its sector flags select and the draw can skip the rest of
 * the arena. A skipped span holds only elements the vertex stage would kill
 * for that state: a chunk whose slot is not drawn, whose generation is not
 * the published one, or whose plain/near family is not the one the slot's
 * NEARVAR flag picks, or a range no chunk occupies. */

/** @brief Return the index of the first registry entry at or after an element index.
 * @param l The registry list.
 * @param first Element index.
 * @return Insertion index in [0, l->n]. */
static int tc_reg_lower(const tc_chunk_list *l, uint32_t first)
{
    int lo = 0, hi = l->n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (l->c[mid].first < first) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/** @brief Record a written chunk in its buffer's registry, replacing an entry at the same first element.
 *
 * On an allocation failure the registry is left unchanged and every state
 * built afterwards draws its whole ranges (tc_ranges_build finds the
 * registry marked incomplete).
 * @param arena Arena index.
 * @param buf Buffer index.
 * @param c The chunk. */
static void tc_reg_add(int arena, int buf, tc_chunk c)
{
    tc_chunk_list *l = &s_tc.reg[arena][buf];
    const int i = tc_reg_lower(l, c.first);
    if (i < l->n && l->c[i].first == c.first) { l->c[i] = c; return; }
    if (l->n == l->cap) {
        const int ncap = l->cap ? l->cap * 2 : 256;
        tc_chunk *nc = (tc_chunk *)realloc(l->c, (size_t)ncap * sizeof(*nc));
        if (!nc) { s_tc.reg_broken = 1; return; }
        l->c = nc; l->cap = ncap;
    }
    memmove(&l->c[i + 1], &l->c[i], (size_t)(l->n - i) * sizeof(*l->c));
    l->c[i] = c;
    l->n++;
}

/** @brief Drop the registry entry of a freed chunk.
 * @param arena Arena index.
 * @param buf Buffer index.
 * @param first The chunk's first element. */
static void tc_reg_remove(int arena, int buf, uint32_t first)
{
    tc_chunk_list *l = &s_tc.reg[arena][buf];
    const int i = tc_reg_lower(l, first);
    if (i >= l->n || l->c[i].first != first) return;
    memmove(&l->c[i], &l->c[i + 1], (size_t)(l->n - i - 1) * sizeof(*l->c));
    l->n--;
}

/** @brief Empty every buffer's registry, keeping the storage. */
static void tc_reg_clear(void)
{
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        for (int b = 0; b < TC_BUF_SLOTS; b++) s_tc.reg[a][b].n = 0;
}

/** @brief Build a registry entry from a chunk's face_flags words.
 * @param first The chunk's first element.
 * @param count Element count.
 * @param ff First face_flags word.
 * @param stride Bytes between consecutive face_flags words.
 * @return The entry; uniform when every element's slot, generation and tier equal the first's. */
static tc_chunk tc_chunk_of(uint32_t first, int count, const uint8_t *ff, size_t stride)
{
    uint32_t f0;
    memcpy(&f0, ff, sizeof f0);
    tc_chunk c = { first, (uint32_t)count, (uint16_t)(f0 & OPTC_SLOT_DEAD),
                   (uint8_t)((f0 >> 29) & 7u), 1, (uint8_t)((f0 & OPTC_FF_TIER_LP) != 0u) };
    const uint32_t mask = OPTC_SLOT_DEAD | OPTC_FF_TIER_LP | 0xE0000000u;
    const uint32_t key = f0 & mask;
    for (int i = 1; i < count && c.uniform; i++) {
        uint32_t f;
        memcpy(&f, ff + (size_t)i * stride, sizeof f);
        if ((f & mask) != key) c.uniform = 0;
    }
    return c;
}

/** @brief Return 1 when a state may draw any element of a chunk.
 *
 * Mirrors the vertex stage's sector-level kills: OPTC_SF_DRAWN clear, a
 * generation other than the slot's published one, or a near-family arena
 * (the near and near-cell arenas) under a slot without OPTC_SF_NEARVAR, or
 * a plain-family arena under one with it.
 * @param T The state.
 * @param arena Arena index.
 * @param c The chunk. */
static int tc_chunk_selected(const tc_state *T, int arena, const tc_chunk *c)
{
    if (!c->uniform) return 1;
    union { float f; uint32_t u; } w;
    w.f = T->sec[c->slot].o[3];
    const uint32_t sf = w.u;
    if (!(sf & OPTC_SF_DRAWN) || ((sf >> 12) & 7u) != c->gen) return 0;
    const int near_family = OPTC_ARENA_IS_NEAR(arena) ||
                            (OPTC_ARENA_IS_CELL(arena) && !OPTC_ARENA_IS_CELL_ANIM(arena));
    return near_family == ((sf & OPTC_SF_NEARVAR) != 0);
}

/** @brief Return 1 when a pass class may draw any element of a selected chunk.
 *
 * Mirrors the vertex stage's sector admission (terrain.vert, the pass and
 * tier admission after the material-pass test): the fill class (pass_sel 1)
 * draws only low-poly faces of a sector with OPTC_SF_FILLLP, the draw class
 * (pass_sel 0 and 2) a tier's faces only under its OPTC_SF_DRAWLP or
 * OPTC_SF_DRAWHP. The flags are the state's published sector words, the
 * ones the stage reads. A chunk mixing slots, generations or tiers is
 * drawn by both classes.
 * @param T The state.
 * @param c The chunk, already selected (tc_chunk_selected).
 * @param cls TC_RC_DRAW or TC_RC_FILL. */
static int tc_chunk_class_drawn(const tc_state *T, const tc_chunk *c, int cls)
{
    if (!c->uniform) return 1;
    union { float f; uint32_t u; } w;
    w.f = T->sec[c->slot].o[3];
    if (cls == TC_RC_FILL) return c->lp && (w.u & OPTC_SF_FILLLP) != 0u;
    return (w.u & (c->lp ? OPTC_SF_DRAWLP : OPTC_SF_DRAWHP)) != 0u;
}

/** @brief Return the gap below which two selected spans of an arena are drawn as one.
 *
 * Joining spans draws the elements between them, which the vertex stage
 * kills as it would in a whole-range draw; the gap trades those elements
 * against a draw call.
 * @param arena Arena index.
 * @return Elements: 192 vertices, or 2 cell records. */
static uint32_t tc_range_gap(int arena)
{
    return OPTC_ARENA_IS_CELL(arena) ? 2u : 192u;
}

/** @brief Append one arena buffer's spans a pass class selects to a state's range list.
 * @param T The state.
 * @param cls TC_RC_DRAW or TC_RC_FILL.
 * @param arena Arena index.
 * @param buf Buffer index.
 * @return 0, or -1 when the list is full. */
static int tc_ranges_add_buf(tc_state *T, int cls, int arena, int buf)
{
    const tc_chunk_list *l = &s_tc.reg[arena][buf];
    const uint32_t gap = tc_range_gap(arena);
    int open = 0;
    uint32_t lo = 0, hi = 0;
    for (int i = 0; i < l->n; i++) {
        const tc_chunk *c = &l->c[i];
        if (!tc_chunk_selected(T, arena, c)) continue;
        if (!tc_chunk_class_drawn(T, c, cls)) {
            s_tc.class_dropped[cls] += c->count;
            continue;
        }
        if (open && c->first <= hi + gap) {
            if (c->first + c->count > hi) hi = c->first + c->count;
            continue;
        }
        if (open) {
            if (T->rl.n >= TC_RANGE_MAX) return -1;
            T->rl.r[T->rl.n++] = (tc_span){ lo, hi - lo };
        }
        open = 1; lo = c->first; hi = c->first + c->count;
    }
    if (open) {
        if (T->rl.n >= TC_RANGE_MAX) return -1;
        T->rl.r[T->rl.n++] = (tc_span){ lo, hi - lo };
    }
    return 0;
}

/** @brief Build a committed state's range lists, one per pass class, from the chunk registry and its sector flags.
 *
 * Leaves the lists absent (n = -1) when the registry is incomplete or the
 * spans of both classes overflow TC_RANGE_MAX, so the state draws whole
 * ranges (counted in range_overflows).
 * @param T The state. */
static void tc_ranges_build(tc_state *T)
{
    T->rl.n = 0;
    if (s_tc.reg_broken) { T->rl.n = -1; return; }
    for (int cls = 0; cls < TC_RC_N; cls++) {
        for (int a = 0; a < OPTC_ARENA_COUNT; a++)
            for (int b = 0; b < TC_BUF_SLOTS; b++) {
                T->rl.off[cls][a * TC_BUF_SLOTS + b] = (uint16_t)T->rl.n;
                if (tc_ranges_add_buf(T, cls, a, b) != 0) {
                    T->rl.n = -1;
                    s_tc.range_overflows++;
                    return;
                }
            }
        T->rl.off[cls][OPTC_ARENA_COUNT * TC_BUF_SLOTS] = (uint16_t)T->rl.n;
    }
    if ((uint32_t)T->rl.n > s_tc.ranges_max) s_tc.ranges_max = (uint32_t)T->rl.n;
}

/** @brief Copy a state, including only the range-list spans in use.
 * @param dst Destination.
 * @param src Source. */
static void tc_state_copy(tc_state *dst, const tc_state *src)
{
    const int n = src->rl.n > 0 ? src->rl.n : 0;
    memcpy(dst, src, offsetof(tc_state, rl.r) + (size_t)n * sizeof(tc_span));
}

/** @brief Record every arena buffer's high-water mark and pair serial into a state.
 *
 * Builder thread, the only writer of the marks and pairs it reads. Called
 * when a build opens its state and again at its commit, so a state carries
 * the marks covering every chunk its build allocated; buffers past the
 * arena's count record 0 for both.
 * @param T The state the build writes. */
static void tc_state_mark(tc_state *T)
{
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        const int n = atomic_load_explicit(&s_tc.arena[a].n, memory_order_relaxed);
        for (int b = 0; b < TC_BUF_SLOTS; b++) {
            const tc_pair *pr = b < n ? tc_live_pair(a, b) : NULL;
            T->mark_high[a][b] = pr ? s_tc.arena[a].buf[b].high : 0u;
            T->mark_pair[a][b] = pr ? pr->serial : 0u;
        }
    }
}


int openpete_tercache_chunk_alloc(int arena, int vert_count, uint32_t *out_first,
                                  int *out_buf)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT || vert_count <= 0)
        return -1;
    tc_arena *a = &s_tc.arena[arena];
    const int n = atomic_load_explicit(&a->n, memory_order_relaxed);
    const int newest = tc_newest_buf(arena);
    for (int b = 0; b < n; b++) {
        if (a->buf[b].source || a->buf[b].sealed || !tc_live_pair(arena, b)) continue;
        if (tc_buf_alloc(&a->buf[b], (uint32_t)vert_count, out_first) != 0) continue;
        if (a->buf[b].sealed) s_tc.sealed_allocs++;   /* unreachable: sealed is a source */
        *out_buf = b;
        /* Watermark: the last buffer the allocator tries, left with under
         * 25% of its capacity allocatable (room above the high-water mark
         * plus its free list), asks for the next buffer before a chunk is
         * refused. The high-water mark alone is not the fill: one chunk near
         * the top holds it there while churn cycles through the free list. */
        const tc_buf *nb = &a->buf[newest];
        if (b == newest && (uint64_t)nb->high * 4u > (uint64_t)nb->cap * 3u &&
            tc_buf_room(nb) * 4u < (uint64_t)nb->cap)
            tc_request_grow(arena, 0u);
        return 0;
    }
    s_tc.alloc_fail[arena]++;
    tc_request_grow(arena, (uint32_t)vert_count);
    if (s_tc.rz_window) s_tc.edge_claims++;
    else if (a->ovf_state == TC_OVF_REQ) s_tc.grow_claims++;
    else { s_tc.claims[arena]++; a->lvl_claims++; }
    if (atomic_load_explicit(&s_tc.rz_state, memory_order_relaxed) == TC_RZ_RETIRE)
        s_tc.retire_claims++;
    return -1;   /* full: caller claims the sector back to the CPU walk */
}

void openpete_tercache_chunk_write(int arena, int buf, uint32_t first,
                                   const rhi_terrain_vertex *v, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    const tc_pair *pr = tc_live_pair(arena, buf);
    if (!pr) return;
    if (pr->serial < s_tc.old_pair_floor)
        atomic_fetch_add_explicit(&s_tc.retire_old_touches, 1u, memory_order_relaxed);
    rhi_update_buffer_range(pr->vb, v,
                            (size_t)first * sizeof(rhi_terrain_vertex),
                            (size_t)count * sizeof(rhi_terrain_vertex));
    if (count > 0)
        tc_reg_add(arena, buf, tc_chunk_of(first, count, (const uint8_t *)&v[0].face_flags,
                                           sizeof(rhi_terrain_vertex)));
}

void openpete_tercache_chunk_write_prev(int arena, int buf, uint32_t first,
                                        const rhi_terrain_prev *p, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    const tc_pair *pr = tc_live_pair(arena, buf);
    if (!OPTC_ARENA_IS_ANIM(arena) || !pr || !pr->pv) return;
    rhi_update_buffer_range(pr->pv, p,
                            (size_t)first * sizeof(rhi_terrain_prev),
                            (size_t)count * sizeof(rhi_terrain_prev));
}

void openpete_tercache_chunk_write_ctx(int arena, int buf, uint32_t first,
                                       const rhi_terrain_nearctx *c, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT) return;
    const tc_pair *pr = tc_live_pair(arena, buf);
    if (!OPTC_ARENA_IS_NEAR(arena) || !pr || !pr->pv) return;
    rhi_update_buffer_range(pr->pv, c,
                            (size_t)first * sizeof(rhi_terrain_nearctx),
                            (size_t)count * sizeof(rhi_terrain_nearctx));
}

void openpete_tercache_chunk_write_cells(int arena, int buf, uint32_t first,
                                         const void *recs, int count)
{
    if (!s_tc.ready || !OPTC_ARENA_IS_CELL(arena)) return;
    const tc_pair *pr = tc_live_pair(arena, buf);
    if (!pr) return;
    const size_t sz = tc_elem_size(arena);
    rhi_update_buffer_range(pr->vb, recs, (size_t)first * sz, (size_t)count * sz);
    if (count > 0)
        tc_reg_add(arena, buf, tc_chunk_of(first, count, (const uint8_t *)recs +
                                           offsetof(rhi_terrain_cell, face_flags), sz));
}

void openpete_tercache_chunk_free(int arena, int buf, uint32_t first, int count)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT || count <= 0) return;
    if (buf < 0 || buf >= TC_BUF_SLOTS) return;
    tc_buf *a = &s_tc.arena[arena].buf[buf];
    tc_reg_remove(arena, buf, first);
    /* No in-place dead-stamp: a replaced chunk keeps its bytes untouched (a
     * host write would race in-flight frames reading the arena). Presents
     * that still show the pre-rebuild state draw it via its baked chunk
     * GENERATION (face_flags bits 29-31 vs the published slot generation);
     * newer presents kill it the same way. The range itself is reused only
     * after the defer window below. */
    a->used -= (uint32_t)count; a->chunks--;
    /* Defer the RANGE's reuse past the frames-in-flight window. */
    if (s_tc.defer_n < TC_DEFER_MAX) {
        s_tc.defer[s_tc.defer_n++] = (tc_defer){
            (uint8_t)arena, (uint8_t)buf, first, (uint32_t)count, s_tc.present_ctr };
    } else {
        if (!a->source) tc_free_insert(a, first, (uint32_t)count);   /* overflow: reuse now (rare) */
        s_tc.defer_overflow++;
    }
}

/** @brief Ask the device-owning thread for a larger previous-tick store.
 *
 * Builder thread, from an allocation that took the store past 75% of its
 * capacity (@p demand 0) or one it refused (@p demand the refused units).
 * The replacement holds cap + max(demand, the init capacity) units. Dropped
 * while a request is outstanding, during a level-edge window (the edge
 * replaces the store) or while a refusal's backoff runs.
 * @param demand Refused units, or 0 for the watermark. */
static void tc_store_request(uint32_t demand)
{
    if (s_tc.prev_state == TC_OVF_REQ || s_tc.rz_window) return;
    if (s_tc.prev_state == TC_OVF_BACKOFF &&
        (int32_t)(s_tc.present_ctr - s_tc.prev_retry_at) < 0) return;
    const uint32_t step = demand > tc_prev_cap_init() ? demand : tc_prev_cap_init();
    s_tc.prev_want = s_tc.prev_cap + step;
    s_tc.prev_state = TC_OVF_REQ;
    fprintf(stderr, "[ter-gpu] store grow request: %s demand=%u want=%u high=%u used=%u cap=%u\n",
            demand ? "refusal" : "watermark", demand, s_tc.prev_want, s_tc.prev_alloc.high,
            s_tc.prev_alloc.used, s_tc.prev_cap);
    atomic_store_explicit(&s_tc.prev_req, 1, memory_order_release);
}

int openpete_tercache_prev_alloc(int count, uint32_t *out_first)
{
    if (!s_tc.ready || count <= 0 || !s_tc.prev_shadow) return -1;
    if (tc_buf_alloc(&s_tc.prev_alloc, (uint32_t)count, out_first) != 0) {
        tc_store_request((uint32_t)count);
        if (s_tc.rz_window) s_tc.edge_claims++;
        else if (s_tc.prev_state == TC_OVF_REQ) s_tc.prev_grow_claims++;
        else s_tc.prev_fail++;
        return -1;
    }
    if ((uint64_t)s_tc.prev_alloc.high * 4u > (uint64_t)s_tc.prev_cap * 3u) tc_store_request(0u);
    return 0;
}

/* Position-ring census (openpete_tercache_ring_note), printed on the
 * tercache-ring exit row. */
static uint64_t s_ring_cnt[OPTC_RING_NOTE_N];
/* The previous-tick store's largest capacity and high-water mark seen at a
 * ring note, for the same row (the store is gone by the time it prints). */
static uint32_t s_ring_store_cap, s_ring_store_high;

int openpete_tercache_ring_alloc(int count, uint32_t *out_first)
{
    if (tc_refuse(TC_SITE_RING)) return -1;
    return openpete_tercache_prev_alloc(count, out_first);
}

uint32_t openpete_tercache_posring_max(void)
{
    return (uint32_t)(TC_RING + OPENPETE_TERCACHE_HOLDS + 1);
}

void openpete_tercache_ring_note(int kind, uint64_t n)
{
    if (kind < 0 || kind >= OPTC_RING_NOTE_N) return;
    if (s_tc.prev_cap > s_ring_store_cap) s_ring_store_cap = s_tc.prev_cap;
    if (s_tc.prev_alloc.high > s_ring_store_high) s_ring_store_high = s_tc.prev_alloc.high;
    if (kind == OPTC_RING_NOTE_UNITS || kind == OPTC_RING_NOTE_DEPTH)
        s_ring_cnt[kind] = n > s_ring_cnt[kind] ? n : s_ring_cnt[kind];
    else
        s_ring_cnt[kind] += n;
}

void openpete_tercache_prev_write(uint32_t first, const uint32_t *words, int count)
{
    if (!s_tc.ready || count <= 0 || !s_tc.prev_shadow) return;
    const tc_store *st = atomic_load_explicit(&s_tc.prev_live, memory_order_relaxed);
    const size_t rb = OPTC_PREV_REC_WORDS * sizeof(uint32_t);
    if (st->serial == s_tc.old_store_serial)
        atomic_fetch_add_explicit(&s_tc.retire_old_touches, 1u, memory_order_relaxed);
    /* The shadow takes every write: a replacement store is refilled from it,
     * never by reading the write-combined mapping back. */
    memcpy(s_tc.prev_shadow + (size_t)first * OPTC_PREV_REC_WORDS, words, (size_t)count * rb);
    rhi_update_buffer_range(st->buf, words, (size_t)first * rb, (size_t)count * rb);
}

/** @brief Push a device object onto the retire queue the device-owning thread drains.
 *
 * Builder thread. The caller unpublishes the object no later than the
 * push, so no build after it draws it; the drain destroys it once
 * RHI_FRAMES_IN_FLIGHT service calls have passed.
 * @param pr A pair, or NULL.
 * @param st A store, or NULL.
 * @return 1 when queued, 0 when the queue is full (the caller keeps the
 *         object). */
static int tc_retire_push(tc_pair *pr, tc_store *st)
{
    const uint32_t h = atomic_load_explicit(&s_tc.rq_head, memory_order_relaxed);
    const uint32_t t = atomic_load_explicit(&s_tc.rq_tail, memory_order_acquire);
    if (h - t >= TC_RETIRE_Q) return 0;
    s_tc.rq[h % TC_RETIRE_Q] = (tc_retire_ent){
        pr, st, atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed) };
    atomic_store_explicit(&s_tc.rq_head, h + 1u, memory_order_release);
    return 1;
}

/** @brief Destroy every queued retiree whose RHI_FRAMES_IN_FLIGHT service calls have passed.
 *
 * Device-owning thread, from every service call in any replacement state.
 * The queue is in push order, so the drain stops at the first entry still
 * inside its window.
 * @param svc Service call count including the current call. */
static void tc_retire_queue_drain(uint32_t svc)
{
    uint32_t t = atomic_load_explicit(&s_tc.rq_tail, memory_order_relaxed);
    const uint32_t h = atomic_load_explicit(&s_tc.rq_head, memory_order_acquire);
    while (t != h) {
        const tc_retire_ent *e = &s_tc.rq[t % TC_RETIRE_Q];
        if (svc - e->svc <= RHI_FRAMES_IN_FLIGHT) break;
        tc_pair_free(s_tc.dev, e->pair);
        tc_store_free(s_tc.dev, e->store);
        t++;
    }
    atomic_store_explicit(&s_tc.rq_tail, t, memory_order_release);
}

static int tc_retire_free(int one, int park);
static tc_store *tc_store_make_held(uint32_t cap);
static tc_pair *tc_pair_make_held(int arena, uint32_t cap);

/** @brief Size the CPU shadow of the store to a replacement store's capacity, zeroed, and return the units the allocator may hand out.
 *
 * Builder thread, at a level-edge adoption: the shadow mirrors every unit
 * the allocator can name, so it must hold the replacement's capacity
 * before any record lands in it. A failed reallocation keeps the old
 * shadow and limits the allocator to its units.
 * @param cap The replacement store's capacity in units.
 * @return The allocator's capacity in units. */
static uint32_t tc_shadow_fit(uint32_t cap)
{
    const size_t rb = OPTC_PREV_REC_WORDS * sizeof(uint32_t);
    if (cap > s_tc.shadow_cap) {
        uint32_t *sh = (uint32_t *)realloc(s_tc.prev_shadow, (size_t)cap * rb);
        if (!sh) return s_tc.shadow_cap;
        s_tc.prev_shadow = sh;
        s_tc.shadow_cap = cap;
    }
    memset(s_tc.prev_shadow, 0, (size_t)cap * rb);
    return cap;
}

/** @brief Install an answered previous-tick store replacement, or start the refusal's backoff.
 *
 * Builder thread, from frame_begin and frame_skip, after the last bake into
 * the old store. The shadow grows to the new capacity, the new store takes
 * the shadow's units up to the allocator's high-water mark (every record a
 * live, ring or held chunk names lives below it, deferred and held runs
 * included), and then publishes with release ordering, so the next
 * state's bind reads the refilled store. The old store retires through the
 * queue. The allocation epoch steps, so sectors claimed for want of room
 * retry. */
static void tc_store_adopt(void)
{
    if (!atomic_exchange_explicit(&s_tc.prev_done, 0, memory_order_acquire)) return;
    tc_store *st = s_tc.prev_new;
    s_tc.prev_new = NULL;
    if (st) {
        uint32_t *sh = (uint32_t *)realloc(s_tc.prev_shadow,
                                           (size_t)st->cap * OPTC_PREV_REC_WORDS * sizeof(uint32_t));
        if (!sh) { tc_retire_push(NULL, st); st = NULL; }
        else { s_tc.prev_shadow = sh; s_tc.shadow_cap = st->cap; }
    }
    if (!st) {
        s_tc.prev_state = TC_OVF_BACKOFF;
        s_tc.prev_backoff = s_tc.prev_backoff ? (s_tc.prev_backoff < 1024u ? s_tc.prev_backoff * 2u : 1024u) : 8u;
        s_tc.prev_retry_at = s_tc.present_ctr + s_tc.prev_backoff;
        return;
    }
    const size_t rb = OPTC_PREV_REC_WORDS * sizeof(uint32_t);
    const op_time_t t0 = op_now();   /* clock-guard: wall (refill cost) */
    memset(s_tc.prev_shadow + (size_t)s_tc.prev_cap * OPTC_PREV_REC_WORDS, 0,
           (size_t)(st->cap - s_tc.prev_cap) * rb);
    if (s_tc.prev_alloc.high)
        rhi_update_buffer_range(st->buf, s_tc.prev_shadow, 0, (size_t)s_tc.prev_alloc.high * rb);
    fprintf(stderr, "[ter-gpu] store of %u units adopted; refill of %u units %.3f ms\n",
            st->cap, s_tc.prev_alloc.high, (double)(op_now() - t0) / 1e6);
    tc_store *old = atomic_exchange_explicit(&s_tc.prev_live, st, memory_order_acq_rel);
    if (old->serial == s_tc.old_store_serial || st->serial == s_tc.old_store_serial)
        atomic_fetch_add_explicit(&s_tc.retire_old_touches, 1u, memory_order_relaxed);
    tc_retire_push(NULL, old);
    s_tc.prev_cap = st->cap;
    s_tc.prev_alloc.cap = st->cap;
    s_tc.prev_state = TC_OVF_IDLE;
    s_tc.prev_backoff = 0;
    s_tc.prev_grow++;
    s_tc.alloc_epoch++;
}

/** @brief Allocate the previous-tick store replacement the builder asked for and publish the answer.
 *
 * Device-owning thread; NULL answers a refusal (OPENPETE_TC_REFUSE site
 * store, or the device). Waits, leaving the request posted, while
 * OPTC_STORES_ALIVE stores exist. An answer made while a level-edge
 * replacement retires counts in retire_answers, and in
 * retire_old_touches when it is the store that replacement retired.
 * @param retiring Nonzero while the replacement is in the RETIRE state. */
static void tc_store_fulfil(int retiring)
{
    if (atomic_load_explicit(&s_tc.stores_alive, memory_order_relaxed) >= OPTC_STORES_ALIVE) return;
    if (!atomic_exchange_explicit(&s_tc.prev_req, 0, memory_order_acquire)) return;
    s_tc.prev_new = tc_refuse(TC_SITE_STORE) ? NULL : tc_store_make_held(s_tc.prev_want);
    if (retiring && s_tc.prev_new) {
        atomic_fetch_add_explicit(&s_tc.retire_answers, 1u, memory_order_relaxed);
        if (s_tc.prev_new == s_tc.rz_old_store ||
            (s_tc.rz_old_store && s_tc.prev_new->buf == s_tc.rz_old_store->buf))
            atomic_fetch_add_explicit(&s_tc.retire_old_touches, 1u, memory_order_relaxed);
    }
    if (!s_tc.prev_new)
        fprintf(stderr, "[ter-gpu] previous-tick store of %u units: allocation refused\n",
                s_tc.prev_want);
    atomic_store_explicit(&s_tc.prev_done, 1, memory_order_release);
}

void openpete_tercache_prev_free(uint32_t first, int count)
{
    if (!s_tc.ready || count <= 0) return;
    tc_buf *a = &s_tc.prev_alloc;
    a->used -= (uint32_t)count; a->chunks--;
    if (s_tc.prev_defer_n < TC_DEFER_MAX)
        s_tc.prev_defer[s_tc.prev_defer_n++] =
            (tc_defer){ 0u, 0u, first, (uint32_t)count, s_tc.present_ctr };
    else {
        s_tc.prev_defer_ovf++;
        s_tc.defer_overflow++;
        tc_free_insert(a, first, (uint32_t)count);
    }
}

/** @brief Return the record runs whose defer window has passed to the record allocator.
 *
 * The rule of the chunk releases in frame_begin: older than
 * TC_DEFER_PRESENTS builds and at or below the read floor, so no state that
 * can still draw the chunks naming a run is live.
 * @param have_floor Nonzero when floor_serial is valid.
 * @param floor_serial The oldest build serial a live state carries. */
static void tc_prev_release(int have_floor, uint32_t floor_serial)
{
    int w = 0;
    for (int i = 0; i < s_tc.prev_defer_n; i++) {
        const tc_defer *d = &s_tc.prev_defer[i];
        const int aged   = s_tc.present_ctr - d->stamp > TC_DEFER_PRESENTS;
        const int unread = !have_floor || (int32_t)(d->stamp - floor_serial) <= 0;
        if (aged && unread) tc_free_insert(&s_tc.prev_alloc, d->first, d->count);
        else s_tc.prev_defer[w++] = *d;
    }
    s_tc.prev_defer_n = w;
}

/* ---- level-edge arena replacement --------------------------------------- */

/* Builder thread: stores the level, then raises the replacement request. */
void openpete_tercache_request_resize(int level_id)
{
    if (!s_tc.ready) return;
    tc_exhaust_warn();
    atomic_store_explicit(&s_tc.level_id, level_id, memory_order_release);
    s_tc.rz_window = 1;
    s_tc.rz_store_cap = s_tc.prev_cap;
    memcpy(s_tc.rz_est, s_tc.est, sizeof s_tc.rz_est);
    if (s_tc.est_have && !tc_prev_cap_env() && s_tc.est_prev > s_tc.rz_store_cap)
        s_tc.rz_store_cap = s_tc.est_prev > UINT32_MAX / 2u ? UINT32_MAX / 2u : (uint32_t)s_tc.est_prev;
    atomic_store_explicit(&s_tc.rz_req, 1, memory_order_release);
}

uint32_t openpete_tercache_alloc_epoch(void) { return s_tc.alloc_epoch; }

int openpete_tercache_resize_ready(void)
{
    return s_tc.ready &&
        atomic_load_explicit(&s_tc.rz_state, memory_order_acquire) == TC_RZ_READY;
}

/** @brief Move answered but uninstalled growth or consolidation buffers onto the replacement's retire list.
 *
 * Builder thread, from tc_adopt_pending. A buffer the device answered but no
 * frame_begin installed has never been bound by a draw; it still retires
 * through the drain. The device answers no request while a replacement is
 * awaiting adoption and publishes every earlier answer before it raises
 * READY, so every answer raised before this point is visible here.
 * Outstanding unanswered requests are dropped. */
static void tc_collect_overflow_pending(void)
{
    const uint32_t mask = atomic_exchange_explicit(&s_tc.ovf_done, 0u,
                                                   memory_order_acquire);
    atomic_store_explicit(&s_tc.ovf_req, 0u, memory_order_relaxed);
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (!(mask & (1u << a))) continue;
        s_tc.rz_old[a][TC_BUF_SLOTS] = s_tc.ovf_new[a];
        s_tc.ovf_new[a] = NULL;
    }
}

/** @brief Return the device bytes of the buffers of an arena's consolidation that have emptied and await their floors.
 * @param arena Arena index.
 * @return Bytes. */
static uint64_t tc_pinned_bytes(int arena)
{
    uint64_t b = 0;
    for (int i = 0; i < TC_BUF_SLOTS; i++) {
        const tc_buf *bf = &s_tc.arena[arena].buf[i];
        if (bf->source && bf->zeroed && tc_live_pair(arena, i)) b += tc_pair_bytes(arena, bf->cap);
    }
    return b;
}

/** @brief Return 1 when a deferred release still names an arena buffer.
 * @param arena Arena index.
 * @param buf Buffer index.
 * @return 1 when named, else 0. */
static int tc_defer_names(int arena, int buf)
{
    for (int i = 0; i < s_tc.defer_n; i++)
        if (s_tc.defer[i].arena == arena && s_tc.defer[i].buf == buf) return 1;
    return 0;
}

static int tc_release_floor(uint32_t *floor_serial);

/** @brief Store an arena's buffer count as its highest occupied slot + 1 and record its occupied-slot peak.
 * @param a Arena index. */
static void tc_arena_recount(int a)
{
    int top = 0, occ = 0;
    for (int i = 0; i < TC_BUF_SLOTS; i++) {
        if (!tc_live_pair(a, i)) continue;
        top = i + 1;
        occ++;
    }
    atomic_store_explicit(&s_tc.arena[a].n, top, memory_order_release);
    if ((uint32_t)occ > s_tc.bufs_max[a]) s_tc.bufs_max[a] = (uint32_t)occ;
}


/** @brief Advance one buffer slot's source retirement: note when it empties, retire it once its floors pass.
 * @param a Arena index.
 * @param b Buffer slot.
 * @param have_floor Nonzero when @p floor_serial is valid.
 * @param floor_serial The release floor (tc_release_floor).
 * @return 1 while the slot holds a source that has not retired, else 0. */
static int tc_source_step(int a, int b, int have_floor, uint32_t floor_serial)
{
    tc_buf *bf = &s_tc.arena[a].buf[b];
    tc_pair *pr = atomic_load_explicit(&bf->pair, memory_order_relaxed);
    if (!pr || !bf->source) return 0;
    if (!bf->zeroed) {
        if (bf->chunks != 0 || tc_defer_names(a, b)) return 1;
        bf->zeroed = 1;
        bf->zero_build = s_tc.present_ctr;
    }
    const int aged = s_tc.present_ctr - bf->zero_build > TC_DEFER_PRESENTS;
    const int unread = !have_floor || (int32_t)(bf->zero_build - floor_serial) <= 0;
    if (!aged || !unread || !tc_retire_push(pr, NULL)) return 1;
    atomic_store_explicit(&bf->pair, NULL, memory_order_release);
    bf->source = bf->sealed = bf->zeroed = 0;
    bf->high = bf->used = bf->chunks = bf->cap = 0;
    bf->free_n = 0;
    s_tc.reg[a][b].n = 0;
    return 0;
}

/** @brief Retire every consolidation source and sealed buffer whose chunks are gone and whose floors have passed.
 *
 * Builder thread, from frame_begin after the deferred releases. A source
 * becomes empty when its chunk count is 0 and no deferred range names it;
 * that build is its zero serial. It retires once the release floor (the
 * oldest ring or held state) is past the zero serial and more than
 * TC_DEFER_PRESENTS builds have passed since it (the drain floor that
 * covers live, staged and the freeze-backdrop source): no state still drawn
 * can then name a chunk of it. Its pair leaves the slot (draws and
 * allocations skip a NULL pair, and a chunk's buffer index stays valid) and
 * goes to the retire queue, which waits RHI_FRAMES_IN_FLIGHT service calls
 * before destroying it (a full queue defers the retirement to a later
 * build); the slot is reusable from then on. The arena's
 * consolidation ends when no source remains. */
static void tc_source_retire(void)
{
    uint32_t floor_serial = 0;
    const int have_floor = tc_release_floor(&floor_serial);
    uint64_t pinned = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        tc_arena *ar = &s_tc.arena[a];
        if (!ar->cons_live) continue;
        int remain = 0;
        for (int b = 0; b < TC_BUF_SLOTS; b++)
            remain |= tc_source_step(a, b, have_floor, floor_serial);
        pinned += tc_pinned_bytes(a);
        tc_arena_recount(a);
        if (!remain) {
            ar->cons_live = 0;
            if (ar->cons_ticks > s_tc.migrate_ticks) s_tc.migrate_ticks = ar->cons_ticks;
        }
    }
    if (pinned > s_tc.pinned_bytes) s_tc.pinned_bytes = pinned;
}

/** @brief Apply the refusal policy to an arena whose growth or consolidation request was refused.
 *
 * A refused growth asks for a consolidation at once. A refused
 * consolidation returns its sources to service (sealed buffers excepted:
 * they stay sources until they retire) and starts a backoff, as does a
 * growth whose consolidation cannot be asked for.
 * @param a Arena index.
 * @param kind The refused request's TC_REQ_* kind. */
static void tc_adopt_refused(int a, int kind)
{
    tc_arena *ar = &s_tc.arena[a];
    ar->refused |= kind == TC_REQ_GROW ? TC_EXH_GROW : TC_EXH_CONS;
    if (kind == TC_REQ_GROW) {
        if (!tc_request_consolidation(a, s_tc.ovf_want[a])) tc_exhaust(a, ar->cons_why);
        return;
    }
    int sealed = 0;
    for (int b = 0; b < TC_BUF_SLOTS; b++) {
        if (!ar->buf[b].sealed) ar->buf[b].source = 0;
        sealed |= ar->buf[b].sealed;
    }
    ar->cons_live = sealed;
    tc_exhaust(a, 0u);
}

/** @brief Install an answered buffer in an arena slot and step the allocation epoch.
 *
 * Logs the request-to-adoption delay in service calls and builds and the
 * device's allocation time.
 * @param a Arena index.
 * @param slot The empty slot.
 * @param pr The answered pair.
 * @param kind TC_REQ_GROW or TC_REQ_CONS. */
static void tc_install_buf(int a, int slot, tc_pair *pr, int kind)
{
    tc_arena *ar = &s_tc.arena[a];
    tc_buf *b = &ar->buf[slot];
    if (pr->serial < s_tc.old_pair_floor)
        atomic_fetch_add_explicit(&s_tc.retire_old_touches, 1u, memory_order_relaxed);
    b->cap = pr->cap; b->high = 0; b->free_n = 0; b->used = 0; b->chunks = 0;
    b->source = b->sealed = b->zeroed = 0;
    s_tc.reg[a][slot].n = 0;
    atomic_store_explicit(&b->pair, pr, memory_order_release);
    tc_arena_recount(a);
    ar->backoff = 0;
    ar->refused = 0;
    if (kind == TC_REQ_CONS) s_tc.consolidations++;
    else s_tc.overflow_adopted[a]++;
    s_tc.alloc_epoch++;
    /* The window the claimed sectors spend on the CPU walk, in the two
     * clocks that bound it: the device thread's service calls and the
     * builder's builds. */
    fprintf(stderr, "[ter-gpu] arena %d %s buffer of %u elements adopted in slot %d "
                    "%u service calls / %u builds after the request; allocation %.3f ms\n",
            a, kind == TC_REQ_CONS ? "consolidation" : "growth", pr->cap, slot,
            atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed) - s_tc.ovf_req_svc[a],
            s_tc.present_ctr - s_tc.ovf_req_build[a], (double)s_tc.ovf_fulfil_ns[a] / 1e6);
}

/** @brief Install every answered growth or consolidation buffer in its arena's lowest empty slot, or apply the refusal policy.
 *
 * Builder thread, from frame_begin and frame_skip. The request window
 * closes either way. An answer with no empty slot left retires unused and
 * counts as a refusal. */
static void tc_adopt_overflow(void)
{
    const uint32_t mask = atomic_exchange_explicit(&s_tc.ovf_done, 0u,
                                                   memory_order_acquire);
    if (!mask) return;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (!(mask & (1u << a))) continue;
        tc_arena *ar = &s_tc.arena[a];
        tc_pair *pr = s_tc.ovf_new[a];
        s_tc.ovf_new[a] = NULL;
        const int kind = ar->ovf_kind;
        ar->ovf_state = TC_OVF_IDLE;
        int empty;
        (void)tc_arena_slots(a, &empty);
        if (pr && empty < 0 && tc_retire_push(pr, NULL)) pr = NULL;
        if (pr && empty >= 0) tc_install_buf(a, empty, pr, kind);
        else tc_adopt_refused(a, kind);
    }
}

/** @brief Find the oldest build serial a state still drawn may read ranges of: the release floor.
 *
 * A range freed at build serial F was drawn by states built before F. The
 * states still drawn are the ring states whose scene is unretired (eager)
 * and the held states (openpete_tercache_hold_take); nothing freed at or
 * after the oldest of their build serials may be reused yet. With neither
 * there is no floor, and the release condition is the build-count window
 * alone.
 * @param floor_serial Receives the floor when there is one.
 * @return 1 when a floor exists, else 0. */
static int tc_release_floor(uint32_t *floor_serial)
{
    int have = 0;
    for (int i = 0; i < TC_RING + OPENPETE_TERCACHE_HOLDS; i++) {
        const int held = i >= TC_RING;
        const tc_state *R = held ? &s_tc.hold[i - TC_RING] : &s_tc.ring[i];
        if (!R->valid || (!held && !openpete_native_scene_stamp_live(R->stamp))) continue;
        if (!have || (int32_t)(R->build_serial - *floor_serial) < 0) { *floor_serial = R->build_serial; have = 1; }
    }
    return have;
}

uint32_t openpete_tercache_build_serial(void)
{
    return s_tc.present_ctr;
}

int openpete_tercache_serial_released(uint32_t serial)
{
    uint32_t floor_serial = 0;
    if (s_tc.present_ctr - serial <= TC_DEFER_PRESENTS) return 0;
    return !tc_release_floor(&floor_serial) || (int32_t)(serial - floor_serial) <= 0;
}

/** @brief Seal the outgoing buffers and open a consolidation when the device refused a level-edge replacement.
 *
 * Builder thread, from frame_begin and frame_skip. The device counts each
 * refused replacement in rz_failed; a count the builder has not seen counts
 * a failed edge and closes the edge window. Every slot was freed at the
 * edge, so the outgoing buffers hold only ranges states drawn before the
 * edge may still read: each is sealed (no allocation, and its released
 * ranges never return to its free list, so the new level never writes a
 * range a held state draws) and becomes a consolidation source, and each
 * arena asks for a consolidation buffer at once, sized for the new level.
 * The new level bakes into it; its adoption steps the allocation epoch, so
 * the claims of the window re-bake. No hold drops. The sealed buffers
 * retire once their floors pass (tc_source_retire). The previous-tick
 * store stays. */
static void tc_note_failed_edge(void)
{
    const uint32_t f = atomic_load_explicit(&s_tc.rz_failed, memory_order_acquire);
    if (f == s_tc.rz_failed_seen) return;
    s_tc.failed_edges += f - s_tc.rz_failed_seen;
    s_tc.rz_failed_seen = f;
    if (atomic_load_explicit(&s_tc.rz_req, memory_order_relaxed)) return;   /* a newer edge is queued */
    s_tc.rz_window = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        tc_arena *ar = &s_tc.arena[a];
        for (int b = 0; b < TC_BUF_SLOTS; b++)
            if (tc_live_pair(a, b)) ar->buf[b].sealed = 1;
        /* An answer already in flight lands in an empty slot as the new
         * level's buffer; otherwise the consolidation asks for one. */
        if (ar->ovf_state != TC_OVF_REQ) {
            ar->ovf_state = TC_OVF_IDLE;
            ar->cons_live = 0;
            if (tc_request_consolidation(a, 0u)) continue;
            ar->backoff = 0;
            tc_exhaust(a, TC_EXH_EDGE | ar->cons_why);
        }
        for (int b = 0; b < TC_BUF_SLOTS; b++)
            if (ar->buf[b].sealed && !ar->buf[b].source) { ar->buf[b].source = 1; ar->buf[b].zeroed = 0; }
        ar->cons_live = 1;
        ar->cons_ticks = 0;
    }
}

/** @brief Adopt a fulfilled level-edge replacement.
 *
 * Builder thread, from frame_begin and frame_skip; no-op unless the
 * replacement is in the READY state. Publishes the fresh pairs and the
 * fresh previous-tick store, resets each arena's allocator and the store's,
 * collapses each arena back to one buffer (every growth, consolidation and
 * sealed buffer goes to the retire list with the outgoing primary), drops
 * the deferred-release lists (their ranges address the outgoing buffers),
 * steps the allocation epoch, records the drain floors, and hands the
 * outgoing pairs and store to the retire drain. */
static void tc_adopt_pending(void)
{
    if (atomic_load_explicit(&s_tc.rz_state, memory_order_acquire) != TC_RZ_READY) return;
    tc_collect_overflow_pending();
    /* Every pair made before the fresh ones is outgoing from here on: the
     * census's touch check compares serials against the lowest fresh one. */
    s_tc.old_pair_floor = UINT32_MAX;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        if (s_tc.rz_new[a]->serial < s_tc.old_pair_floor) s_tc.old_pair_floor = s_tc.rz_new[a]->serial;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        tc_arena *ar = &s_tc.arena[a];
        for (int b = 0; b < TC_BUF_SLOTS; b++)
            s_tc.rz_old[a][b] = atomic_load_explicit(&ar->buf[b].pair,
                                                     memory_order_relaxed);
        tc_buf *p = &ar->buf[0];
        p->cap = s_tc.rz_new[a]->cap;
        atomic_store_explicit(&p->pair, s_tc.rz_new[a], memory_order_release);
        /* The other slots are emptied before the count falls, and the draw
         * skips a NULL pair, so a present that read the old count draws
         * nothing extra rather than binding a pair now on the retire list. */
        for (int b = 1; b < TC_BUF_SLOTS; b++)
            atomic_store_explicit(&ar->buf[b].pair, NULL, memory_order_release);
        atomic_store_explicit(&ar->n, 1, memory_order_release);
        for (int b = 0; b < TC_BUF_SLOTS; b++) {
            tc_buf *bf = &ar->buf[b];
            bf->high = 0; bf->free_n = 0; bf->used = 0; bf->chunks = 0;
            bf->source = bf->sealed = bf->zeroed = 0;
            if (b) bf->cap = 0;
        }
        ar->ovf_state = TC_OVF_IDLE;
        ar->backoff = 0;
        ar->cons_live = 0;
        ar->refused = 0;
        s_tc.ovf_fail_logged[a] = 0;
        s_tc.rz_new[a] = NULL;
    }
    /* The store: records and colour tables were all released at the edge,
     * so the fresh store starts empty and its deferred runs are dropped. */
    s_tc.rz_old_store = atomic_exchange_explicit(&s_tc.prev_live, s_tc.rz_store,
                                                 memory_order_acq_rel);
    s_tc.old_store_serial = s_tc.rz_old_store ? s_tc.rz_old_store->serial : 0u;
    s_tc.rz_store = NULL;
    s_tc.prev_cap = s_tc.prev_alloc.cap = tc_shadow_fit(atomic_load_explicit(&s_tc.prev_live,
                                                                             memory_order_relaxed)->cap);
    s_tc.prev_alloc.high = s_tc.prev_alloc.used = s_tc.prev_alloc.chunks = 0;
    s_tc.prev_alloc.free_n = 0;
    s_tc.prev_defer_n = 0;
    s_tc.prev_state = TC_OVF_IDLE;
    s_tc.prev_backoff = 0;
    {   /* a store growth answered for the outgoing store retires unused */
        if (atomic_exchange_explicit(&s_tc.prev_done, 0, memory_order_acquire) && s_tc.prev_new) {
            tc_retire_push(NULL, s_tc.prev_new);
            s_tc.prev_new = NULL;
        }
        atomic_store_explicit(&s_tc.prev_req, 0, memory_order_relaxed);
    }
    s_tc.alloc_epoch++;
    /* Every deferred range addresses the outgoing pair's bytes. The fresh
     * pair has none of them allocated, so the list is dropped whole rather
     * than drained: re-inserting those spans into an empty free list would
     * hand the allocator ranges it already owns. */
    s_tc.defer_n = 0;
    /* A held state's ranges address the outgoing pairs too. */
    for (int i = 0; i < OPENPETE_TERCACHE_HOLDS; i++)
        if (s_tc.hold[i].valid) { s_tc.hold[i].valid = 0; s_tc.hold_drops++; }
    /* The registry described the outgoing buffers. */
    tc_reg_clear();
    s_tc.reg_broken = 0;
    s_tc.rz_old_build = s_tc.present_ctr;
    s_tc.rz_old_svc   = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed);
    s_tc.rz_window = 0;
    atomic_store_explicit(&s_tc.rz_state, TC_RZ_RETIRE, memory_order_release);
    {   /* the capacities the level starts on (the census's first-latch row) */
        uint32_t cap[OPTC_ARENA_COUNT];
        char cs[320];
        tc_arena_caps(cap);
        tc_est_col(cs, sizeof cs, cap);
        fprintf(stderr, "[tc-est] adopt level=%d cap=%s store=%u\n",
                atomic_load_explicit(&s_tc.level_id, memory_order_relaxed), cs, s_tc.prev_cap);
    }
}

/** @brief Read a 0/1 switch from the environment once into a first-use cache.
 * @param cache The switch's cache, -1 before the first read.
 * @param name Environment variable name.
 * @param dflt Value when the variable is unset or empty.
 * @return 0 when the value starts with '0', 1 for any other non-empty value, else dflt. */
static int tc_env_switch(OP_ONCE_INT *cache, const char *name, int dflt)
{
    int v = op_once_int_get(cache);
    if (v < 0) {
        const char *e = getenv(name);
        v = op_once_int_fill(cache, (e && *e) ? *e != '0' : dflt);
    }
    return v;
}

/** @brief Return whether OPENPETE_TC_RETIRE_SPREAD asks the level-edge retire to destroy one outgoing object per service call; read once.
 * @return 1 when set and not starting with '0', else 0. */
static int tc_retire_spread(void)
{
    static OP_ONCE_INT v = -1;
    return tc_env_switch(&v, "OPENPETE_TC_RETIRE_SPREAD", 0);
}

/* OPENPETE_TC_EDGE_RECYCLE when unset: 0 frees the outgoing level's pairs,
 * 1 keeps one per arena as a spare. The registry row's default follows. */
#define TC_EDGE_RECYCLE_DEFAULT 0

/** @brief Return whether OPENPETE_TC_EDGE_RECYCLE keeps a retired pair per arena for the next level edge's replacement; read once.
 * @return The variable's 0/1 value, TC_EDGE_RECYCLE_DEFAULT when unset. */
static int tc_edge_recycle(void)
{
    static OP_ONCE_INT v = -1;
    return tc_env_switch(&v, "OPENPETE_TC_EDGE_RECYCLE", TC_EDGE_RECYCLE_DEFAULT);
}

/** @brief Destroy the pairs and the store a replacement retired once both drain floors have passed.
 *
 * Device-owning thread, from openpete_tercache_present_service. The floors
 * count from the adoption: more than TC_DEFER_PRESENTS builds (frame_begin's
 * counter, the window a freed range already waits out, which covers the
 * one-build presentation lag) and more than RHI_FRAMES_IN_FLIGHT service
 * calls (the frames whose recorded command buffers may still bind the
 * outgoing buffers). Under OPENPETE_TC_RETIRE_SPREAD one object is
 * destroyed per call and the state stays RETIRE until the last; otherwise
 * all at once. Once every pair and the store are gone the handshake
 * returns to IDLE.
 * @param svc Service call count including the current call. */
static void tc_retire_drain(uint32_t svc)
{
    const uint32_t builds = atomic_load_explicit(&s_tc.build_ctr, memory_order_acquire);
    if (builds - s_tc.rz_old_build <= TC_DEFER_PRESENTS) return;
    if (svc - s_tc.rz_old_svc <= RHI_FRAMES_IN_FLIGHT) return;
    s_tc.rz_old_due = 1;
    const int spread = tc_retire_spread();
    if (s_tc.edgecen.spread_span == 0 || s_tc.rz_old_build != s_tc.edgecen.spread_build) {
        s_tc.edgecen.spread_build = s_tc.rz_old_build;
        s_tc.edgecen.spread_first = svc;
    }
    if (tc_retire_free(spread, tc_edge_recycle()) && spread) s_tc.edgecen.spread_pairs++;
    s_tc.edgecen.spread_span = svc - s_tc.edgecen.spread_first + 1u;
}

/** @brief Keep a retired pair as its arena's spare, the larger of it and the spare already kept.
 *
 * Device-owning thread; the pair has passed its drain floors, so no frame
 * binds it.
 * @param arena Arena index.
 * @param pr The retired pair.
 * @return 1 when a pair was destroyed (the smaller of the two), else 0. */
static int tc_spare_park(int arena, tc_pair *pr)
{
    tc_pair *sp = s_tc.spare[arena];
    s_tc.edgecen.spares_parked++;
    if (!sp) { s_tc.spare[arena] = pr; return 0; }
    if (pr->phys > sp->phys) { s_tc.spare[arena] = pr; pr = sp; }
    tc_pair_free(s_tc.dev, pr);
    return 1;
}

/** @brief Return the retire list's first occupied entry, or NULL when the list is empty.
 * @param arena Receives the entry's arena index.
 * @return The entry. */
static tc_pair **tc_retired_next(int *arena)
{
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        for (int b = 0; b <= TC_BUF_SLOTS; b++)
            if (s_tc.rz_old[a][b]) { *arena = a; return &s_tc.rz_old[a][b]; }
    return NULL;
}

/** @brief Remove the retired store and pairs, one destroyed object or all, and on the last return the handshake to IDLE.
 *
 * Device-owning thread, once the drain floors have passed (rz_old_due).
 * The store goes first: a store replacement waits while OPTC_STORES_ALIVE
 * stores exist, and the retired one counts. A pair kept as a spare costs
 * no device call and does not end a one-object call.
 * @param one Nonzero to return after the first destroyed object while any remain.
 * @param park Nonzero to keep each pair as its arena's spare (tc_spare_park).
 * @return 1 when an object was destroyed, else 0. */
static int tc_retire_free(int one, int park)
{
    int freed = 0, a = 0;
    if (s_tc.rz_old_store) {
        tc_store_free(s_tc.dev, s_tc.rz_old_store);
        s_tc.rz_old_store = NULL;
        freed = 1;
        if (one) return 1;
    }
    for (tc_pair **e; (e = tc_retired_next(&a)) != NULL; ) {
        tc_pair *pr = *e;
        *e = NULL;
        const int d = park ? tc_spare_park(a, pr) : (tc_pair_free(s_tc.dev, pr), 1);
        freed |= d;
        /* one destroyed object per service call; the state stays RETIRE */
        if (one && d && tc_retired_next(&a)) return 1;
    }
    s_tc.rz_old_due = 0;
    atomic_store_explicit(&s_tc.rz_state, TC_RZ_IDLE, memory_order_release);
    return freed;
}

/** @brief Return whether device objects are held past today's order: a due spread retire or a spare.
 * @return 1 when any is held, else 0. */
static int tc_held_exists(void)
{
    if (s_tc.rz_old_due) return 1;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        if (s_tc.spare[a]) return 1;
    return 0;
}

/** @brief Destroy every held object, the due retire and the spares, which restores today's order of frees.
 *
 * Device-owning thread, after an allocation failed while objects were held.
 * @param provoked Nonzero when OPENPETE_TC_REFUSE site held made the failure. */
static void tc_release_held(int provoked)
{
    s_tc.edgecen.held_releases++;
    if (provoked) s_tc.edgecen.held_refusals++;
    if (s_tc.rz_old_due) (void)tc_retire_free(0, 0);
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        tc_pair_free(s_tc.dev, s_tc.spare[a]);
        s_tc.spare[a] = NULL;
    }
}

/** @brief Make a pair, and on a failure while objects are held destroy them and retry once.
 *
 * Device-owning thread. OPENPETE_TC_REFUSE site held fails the first
 * attempt while objects are held, so the retry path runs on demand.
 * @param arena Arena index.
 * @param cap Capacity in elements.
 * @return The pair, or NULL when the retry failed too or nothing was held. */
static tc_pair *tc_pair_make_held(int arena, uint32_t cap)
{
    const int held = tc_held_exists();
    const int provoked = held && tc_refuse(TC_SITE_HELD);
    tc_pair *pr = provoked ? NULL : tc_pair_make(s_tc.dev, arena, cap);
    if (pr || !held) return pr;
    tc_release_held(provoked);
    return tc_pair_make(s_tc.dev, arena, cap);
}

/** @brief Make a previous-tick store, and on a failure while objects are held destroy them and retry once.
 * @param cap Capacity in store units.
 * @return The store, or NULL when the retry failed too or nothing was held. */
static tc_store *tc_store_make_held(uint32_t cap)
{
    const int held = tc_held_exists();
    const int provoked = held && tc_refuse(TC_SITE_HELD);
    tc_store *st = provoked ? NULL : tc_store_make(s_tc.dev, cap);
    if (st || !held) return st;
    tc_release_held(provoked);
    return tc_store_make(s_tc.dev, cap);
}

/** @brief Take an arena's spare for a replacement when its buffers hold a capacity, else NULL.
 *
 * The spare runs at the requested capacity, so its allocator, draws and
 * claims are the ones a fresh pair of that capacity has; the bytes past it
 * are never addressed. It takes a fresh serial, so it is newer than every
 * outgoing pair.
 * @param arena Arena index.
 * @param cap Requested capacity in elements.
 * @return The spare, or NULL. */
static tc_pair *tc_spare_take(int arena, uint32_t cap)
{
    tc_pair *sp = s_tc.spare[arena];
    if (!sp) return NULL;
    if (sp->phys < cap) { s_tc.edgecen.recycle_misses++; return NULL; }
    s_tc.spare[arena] = NULL;
    sp->cap = cap;
    sp->serial = tc_pair_serial_next();
    s_tc.edgecen.recycle_hits++;
    s_tc.edgecen.recycle_bytes += sp->bytes;
    return sp;
}

/** @brief Allocate one fresh pair per arena and a fresh previous-tick store for the stored level and mark the replacement READY.
 *
 * Device-owning thread. Each pair is sized by tc_initial_cap from the
 * estimate the request carried (rz_est); the store takes the capacity the
 * request carried. On any failure (the device's, or
 * OPENPETE_TC_REFUSE site edge) what was made is freed, the live pairs stay
 * in place, the state stays IDLE and rz_failed counts the refusal, from
 * which the builder seals the outgoing buffers and consolidates
 * (tc_note_failed_edge). */
static void tc_resize_fulfil(void)
{
    tc_pair *made[OPTC_ARENA_COUNT];
    const int level = atomic_load_explicit(&s_tc.level_id, memory_order_acquire);
    int ok = !tc_refuse(TC_SITE_EDGE);
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        const uint32_t want = tc_initial_cap(a, s_tc.rz_est);
        made[a] = ok ? tc_spare_take(a, want) : NULL;
        if (ok && !made[a]) made[a] = tc_pair_make_held(a, want);
        if (!made[a]) ok = 0;
    }
    tc_store *st = ok ? tc_store_make_held(s_tc.rz_store_cap) : NULL;
    if (!st) ok = 0;
    if (!ok) {
        atomic_fetch_add_explicit(&s_tc.rz_failed, 1u, memory_order_release);
        for (int a = 0; a < OPTC_ARENA_COUNT; a++) tc_pair_free(s_tc.dev, made[a]);
        if (!s_tc.rz_alloc_logged) {
            s_tc.rz_alloc_logged = 1;
            fprintf(stderr, "[ter-gpu] arena replacement at level %d: allocation failed, "
                            "the live buffers are sealed and consolidate\n", level);
        }
        return;
    }
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) s_tc.rz_new[a] = made[a];
    s_tc.rz_store = st;
    atomic_fetch_add_explicit(&s_tc.svc_fulfilled, 1u, memory_order_relaxed);
    atomic_store_explicit(&s_tc.rz_state, TC_RZ_READY, memory_order_release);
}

/** @brief Return whether a pair shares its object or a device buffer with a pair on the level-edge replacement's retire list.
 *
 * Device-owning thread, which owns rz_old while the replacement retires.
 * @param pr A freshly made pair.
 * @return 1 when @p pr or one of its buffers is on the list, else 0. */
static int tc_pair_on_retire_list(const tc_pair *pr)
{
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        for (int b = 0; b <= TC_BUF_SLOTS; b++) {
            const tc_pair *o = s_tc.rz_old[a][b];
            if (o && (o == pr || o->vb == pr->vb || (pr->pv && o->pv == pr->pv))) return 1;
        }
    return 0;
}

/** @brief Allocate one growth or consolidation buffer per requested arena and publish the answers.
 *
 * Device-owning thread. The answer is published NULL included: a refusal
 * (the device's, or OPENPETE_TC_REFUSE site grow or cons) is what starts the
 * builder's refusal policy. Reached in IDLE with no replacement requested,
 * and in RETIRE with none queued: the adoption moved every outgoing pair to
 * rz_old and zeroed ovf_req, so a request answered then names an arena of
 * the adopted replacement, and an answer the builder has not taken yet
 * cannot outlive the arena it belongs to. An answer made in RETIRE counts
 * in retire_answers, and in retire_old_touches when it names a pair on the
 * retire list. A failed allocation is logged once per arena.
 * @param retiring Nonzero while the replacement is in the RETIRE state. */
static void tc_overflow_fulfil(int retiring)
{
    const uint32_t mask = atomic_exchange_explicit(&s_tc.ovf_req, 0u,
                                                   memory_order_acquire);
    if (!mask) return;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (!(mask & (1u << a))) continue;
        const int site = s_tc.arena[a].ovf_kind == TC_REQ_CONS ? TC_SITE_CONS : TC_SITE_GROW;
        const op_time_t t0 = op_now();   /* clock-guard: wall (allocation cost) */
        tc_pair *pr = tc_refuse(site) ? NULL : tc_pair_make_held(a, s_tc.ovf_want[a]);
        s_tc.ovf_fulfil_ns[a] = (uint64_t)(op_now() - t0);   /* clock-guard: wall */
        if (!pr && !s_tc.ovf_fail_logged[a]) {
            s_tc.ovf_fail_logged[a] = 1;
            fprintf(stderr, "[ter-gpu] arena %d %s buffer of %u elements: "
                            "allocation refused\n",
                    a, site == TC_SITE_CONS ? "consolidation" : "growth", s_tc.ovf_want[a]);
        }
        if (retiring && pr) {
            atomic_fetch_add_explicit(&s_tc.retire_answers, 1u, memory_order_relaxed);
            if (tc_pair_on_retire_list(pr))
                atomic_fetch_add_explicit(&s_tc.retire_old_touches, 1u, memory_order_relaxed);
        }
        s_tc.ovf_new[a] = pr;
        atomic_fetch_or_explicit(&s_tc.ovf_done, 1u << a, memory_order_release);
    }
}

void openpete_tercache_present_service(void)
{
    if (!s_tc.ready || !s_tc.dev) return;
    const uint32_t svc = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed) + 1u;
    atomic_store_explicit(&s_tc.svc_calls, svc, memory_order_relaxed);
    { OP_PROF_SCOPE("tc.retireq"); tc_retire_queue_drain(svc); }
    int st = atomic_load_explicit(&s_tc.rz_state, memory_order_acquire);
    if (st == TC_RZ_RETIRE) {
        { OP_PROF_SCOPE("tc.retire"); tc_retire_drain(svc); }
        st = atomic_load_explicit(&s_tc.rz_state, memory_order_relaxed);
    }
    if (st == TC_RZ_READY) return;   /* the builder has not adopted yet */
    /* A replacement resizes every arena and collapses every buffer into it,
     * so a queued one takes precedence over an outstanding growth request,
     * and growth waits while one is queued behind a retiring one. */
    if (atomic_load_explicit(&s_tc.rz_req, memory_order_relaxed)) {
        /* The replacement makes a store: it waits while OPTC_STORES_ALIVE
         * exist, which the queued retirees' floors resolve within a few
         * calls. */
        if (st != TC_RZ_IDLE ||
            atomic_load_explicit(&s_tc.stores_alive, memory_order_relaxed) >= OPTC_STORES_ALIVE)
            return;
        if (atomic_exchange_explicit(&s_tc.rz_req, 0, memory_order_acquire)) {
            OP_PROF_SCOPE("tc.resize");
            tc_resize_fulfil();
        }
        return;
    }
    /* Growth is answered in IDLE and while a replacement retires: the
     * retiring pairs and store are only on rz_old and rz_old_store, which
     * no answer names (retire_old_touches). */
    { OP_PROF_SCOPE("tc.ovf"); tc_overflow_fulfil(st == TC_RZ_RETIRE); }
    { OP_PROF_SCOPE("tc.store"); tc_store_fulfil(st == TC_RZ_RETIRE); }
}

/* ---- content estimate (openpete_tercache_set_content) ------------------ */

/** @brief Format one per-arena column as comma-separated values.
 * @param out Receives the NUL-terminated column.
 * @param cap Capacity of @p out in bytes.
 * @param v The per-arena values. */
static void tc_est_col(char *out, size_t cap, const uint32_t *v)
{
    size_t at = 0;
    out[0] = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        const int n = snprintf(out + at, cap - at, "%s%u", a ? "," : "", v[a]);
        if (n < 0 || (size_t)n >= cap - at) break;
        at += (size_t)n;
    }
}

/** @brief Sum the capacity of an arena's buffers that take allocations (not sources, not sealed).
 * @param arena Arena index.
 * @return Capacity in vertices, or in records on a cell arena. */
static uint64_t tc_arena_open_cap(int arena)
{
    const tc_arena *a = &s_tc.arena[arena];
    uint64_t cap = 0;
    for (int b = 0; b < TC_BUF_SLOTS; b++)
        if (!a->buf[b].source && !a->buf[b].sealed && tc_live_pair(arena, b)) cap += a->buf[b].cap;
    return cap;
}

/** @brief Fill each arena's summed capacity over its live buffers.
 * @param out Receives OPTC_ARENA_COUNT capacities. */
static void tc_arena_caps(uint32_t *out)
{
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        uint64_t c = 0;
        for (int b = 0; b < TC_BUF_SLOTS; b++)
            if (tc_live_pair(a, b)) c += s_tc.arena[a].buf[b].cap;
        out[a] = c > UINT32_MAX ? UINT32_MAX : (uint32_t)c;
    }
}

/** @brief Total growth buffers adopted over the session.
 * @return The sum of overflow_adopted. */
static uint32_t tc_grow_total(void)
{
    uint32_t g = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) g += s_tc.overflow_adopted[a];
    return g;
}

/** @brief Print the `[tc-est] level` census line of the level being left: its estimate against each arena's peak.
 *
 * Builder thread, when set_content sees a new level id and at shutdown.
 * `peak` is the largest summed high-water mark an arena reached during the
 * level (what a capacity binds against), `grow` the growth buffers adopted
 * during it, `sigs` its same-id table signature changes. No-op before the
 * first level. */
static void tc_est_census_level(void)
{
    if (!s_tc.est_have) return;
    char e[320], p[320];
    tc_est_col(e, sizeof e, s_tc.lvl_est);
    tc_est_col(p, sizeof p, s_tc.lvl_peak);
    fprintf(stderr, "[tc-est] level=%d sigs=%u grow=%u est=%s peak=%s store_est=%llu store_peak=%u\n",
            s_tc.est_level, s_tc.lvl_sigs,
            tc_grow_total() - s_tc.lvl_grow0, e, p,
            (unsigned long long)s_tc.lvl_est_prev, s_tc.lvl_prev_peak);
}

/** @brief Saturate a 64-bit element count to the 31-bit range a buffer size takes.
 * @param v The count.
 * @return min(v, 2^31 - 1). */
static uint32_t tc_est_sat(uint64_t v)
{
    return v > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)v;
}

void openpete_tercache_set_content(int level_id, const openpete_tercache_content *c)
{
    if (!c) return;
    const uint64_t D = 2u + TC_DEFER_PRESENTS, N = TC_BUF_MAX;
    uint64_t e[OPTC_ARENA_COUNT];
    memset(e, 0, sizeof e);
    for (int k = 0; k < OPTC_CLASS_COUNT; k++) {
        e[k] = c->plain_v[k] + (D - 1u) * c->texroll_v[k];
        e[OPTC_ARENA_ANIM_BASE + k] = c->anim_v[k] + (D - 1u) * c->roll_v[k];
        e[OPTC_ARENA_NEAR_BASE + k] = 4u * (c->plain_v[k] + c->anim_v[k]
            + (D - 1u) * (c->animnear_v[k] + c->texroll_v[k])) / N;
    }
    for (int k = 0; k < OPTC_CLASS_COUNT - 1; k++)
        e[OPTC_ARENA_CELL_ANIM_BASE + k] = c->ca_r[k] + (D - 1u) * c->ca_roll_r[k];
    e[OPTC_ARENA_CN_OPAQUE] = 2u * c->cell_r / N;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        const uint32_t f = tc_floor_cap(a), v = tc_est_sat(e[a]);
        s_tc.est[a] = v > f ? v : f;
    }
    s_tc.est_prev = D * (c->prev_rec + c->split_anim_units) + c->coltab_units + c->split_units
                  + c->ring_units;
    if (!s_tc.est_have || level_id != s_tc.est_level) {
        tc_est_census_level();
        s_tc.est_level = level_id;
        s_tc.lvl_sigs = 0;
        s_tc.lvl_grow0 = tc_grow_total();
        memset(s_tc.lvl_peak, 0, sizeof s_tc.lvl_peak);
        s_tc.lvl_prev_peak = 0;
        uint32_t cap[OPTC_ARENA_COUNT];
        char cs[320], es[320];
        tc_arena_caps(cap);
        tc_est_col(cs, sizeof cs, cap);
        tc_est_col(es, sizeof es, s_tc.est);
        fprintf(stderr, "[tc-est] latch level=%d sectors=%u cap=%s est=%s store=%u store_est=%llu\n",
                level_id, c->sectors, cs, es, s_tc.prev_cap,
                (unsigned long long)s_tc.est_prev);
    } else {
        s_tc.lvl_sigs++;
    }
    memcpy(s_tc.lvl_est, s_tc.est, sizeof s_tc.lvl_est);
    s_tc.lvl_est_prev = s_tc.est_prev;
    s_tc.est_have = 1;
}

int openpete_tercache_bake_held(void)
{
    return s_tc.ready && s_tc.rz_window;
}

void openpete_tercache_note_window_skip(void) { s_tc.window_skips++; }

void openpete_tercache_note_budget_walk(uint32_t claims, uint32_t age_ticks, int starved)
{
    s_tc.budget_claims += claims;
    s_tc.budget_starved += starved != 0;
    if (age_ticks > s_tc.budget_age_max) s_tc.budget_age_max = age_ticks;
    if (claims) { s_tc.budget_open++; return; }
    if (!s_tc.budget_open) return;
    s_tc.budget_episodes++;
    if (s_tc.budget_open > s_tc.budget_walks_max) s_tc.budget_walks_max = s_tc.budget_open;
    s_tc.budget_open = 0;
}

void openpete_tercache_note_sig_change(uint64_t bytes)
{
    s_tc.sig_changes++;
    s_tc.stale_bytes += bytes;
}

/** @brief Track the level's per-arena peaks and ask for growth on any arena whose open capacity is below its estimate.
 *
 * Builder thread, from frame_begin after the adoptions; the peaks are not
 * taken while a level-edge request is open. The growth request
 * asks for the shortfall, at least the floor; it runs only with an
 * estimate made, no OPENPETE_TC_INITIAL_CAP (whose squeeze would otherwise
 * be undone), no level-edge request open, and the arena idle with a free
 * buffer slot under the cap. After a level edge every arena holds its
 * estimate, so this fires after a same-id table signature change whose
 * table needs more. */
static void tc_est_frame(void)
{
    /* Until the edge's adoption the buffers still describe the outgoing level. */
    for (int a = 0; !s_tc.rz_window && a < OPTC_ARENA_COUNT; a++) {
        uint32_t h = 0;
        for (int b = 0; b < TC_BUF_SLOTS; b++)
            if (tc_live_pair(a, b)) h += s_tc.arena[a].buf[b].high;
        if (h > s_tc.lvl_peak[a]) s_tc.lvl_peak[a] = h;
    }
    if (!s_tc.rz_window && s_tc.prev_alloc.high > s_tc.lvl_prev_peak)
        s_tc.lvl_prev_peak = s_tc.prev_alloc.high;
    if (!s_tc.est_have || tc_initial_cap_env() > 0 || s_tc.rz_window)
        return;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        tc_arena *ar = &s_tc.arena[a];
        if (ar->ovf_state != TC_OVF_IDLE) continue;
        const uint64_t cap = tc_arena_open_cap(a);
        if (cap >= s_tc.est[a]) continue;
        int empty;
        if (tc_arena_slots(a, &empty) >= tc_buf_max() || empty < 0) continue;
        uint64_t want = s_tc.est[a] - cap;
        if (want < tc_floor_cap(a)) want = tc_floor_cap(a);
        fprintf(stderr, "[ter-gpu] arena %d grow request: estimate want=%u cap=%llu est=%u\n",
                a, (uint32_t)want, (unsigned long long)cap, s_tc.est[a]);
        tc_post_request(a, TC_REQ_GROW, (uint32_t)want);
    }
}

/* ---- per-present protocol ---------------------------------------------- */

/** @brief Return whether OPENPETE_TC_DBG enables the terrain-cache debug log.
 *
 * Read once; any non-empty value other than one starting with `0` enables
 * it. */
static int tc_dbg(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_TC_DBG"); on = (e && *e && *e != '0'); }
    return on;
}
/** @brief Pick the eager ring state a scene-id build writes.
 *
 * A state already carrying @p stamp is reused: the id was built before and
 * its scene was refused by the full scene ring, so the id was not consumed
 * and the rebuild supersedes that build (one state per id, so selection
 * cannot find the refused build). Otherwise the first state from ring_next
 * on whose scene has retired is taken. When every state belongs to an
 * unretired scene, ring_next's state is overwritten and counted in
 * ring_overwrites; TC_RING's sizing keeps that branch unreachable.
 * @param stamp Scene-id stamp of the build (OPTC_STAMP_SCENE_BIT set).
 * @return Ring index of the state. */
static int tc_ring_pick(uint32_t stamp)
{
    s_tc.ring_builds++;
    for (int i = 0; i < TC_RING; i++)
        if (s_tc.ring[i].valid && s_tc.ring[i].stamp == stamp) { s_tc.ring_reuses++; return i; }
    int pick = -1;
    for (int k = 0; k < TC_RING && pick < 0; k++) {
        const int i = (s_tc.ring_next + k) % TC_RING;
        if (!s_tc.ring[i].valid || !openpete_native_scene_stamp_live(s_tc.ring[i].stamp)) pick = i;
    }
    if (pick < 0) { pick = s_tc.ring_next; s_tc.ring_overwrites++; }
    s_tc.ring_next = (pick + 1) % TC_RING;
    return pick;
}

/** @brief Pick a build's target state and run the promote rule.
 *
 * Shared prologue of frame_begin and frame_skip. An immediate build with a
 * scene stamp (OPTC_STAMP_SCENE_BIT) takes the ring entry tc_ring_pick
 * chooses. Any other immediate (re-extract) build writes
 * live directly and leaves staged in place. A canonical build stages, and
 * the previous staged state promotes to live (the extraction the next
 * presents show); a repeated stamp (the serial-retry path re-runs one build)
 * restages in place without a promote.
 * @param stamp Stamp of the build.
 * @param immediate Nonzero for an immediate build.
 * @return The state the build writes, stamped and marked valid. */
static tc_state *tc_begin_state(uint32_t stamp, int immediate)
{
    if (immediate && (stamp & OPTC_STAMP_SCENE_BIT)) {
        /* A scene-id build: its own ring entry, so every pending scene of a
         * burst composes against the terrain state extracted WITH it. */
        const int pick = tc_ring_pick(stamp);
        tc_state *T = &s_tc.ring[pick];
        if (tc_dbg()) fprintf(stderr, "[tc] state pick=%d for stamp=%08x (was %08x valid=%d) ctr=%u\n",
                              pick, stamp, T->stamp, T->valid, s_tc.present_ctr);
        T->stamp = stamp; T->valid = 1; T->build_serial = s_tc.present_ctr;
        atomic_store_explicit(&s_tc.ring_key[pick],
                              (uint64_t)T->build_serial << 32 | stamp, memory_order_release);
        return T;
    }
    if (immediate) {
        /* The staged canonical state SURVIVES: an intermediate re-extract
         * borrows live for its own present, and the next hook build's
         * promote restores the canonical timeline. */
        s_tc.live.stamp = stamp; s_tc.live.valid = 1; s_tc.live.build_serial = s_tc.present_ctr;
        return &s_tc.live;
    }
    if (s_tc.staged.valid && s_tc.staged.stamp != stamp)
        tc_state_copy(&s_tc.live, &s_tc.staged);   /* promote the previous build */
    s_tc.staged.stamp = stamp; s_tc.staged.valid = 1; s_tc.staged.build_serial = s_tc.present_ctr;
    return &s_tc.staged;
}

/** @brief Overwrite a released range's elements with the dead slot before the range becomes reusable.
 *
 * Every draw covers a whole buffer or spans joined across small gaps, and
 * selects per element by 3-bit generation, so a released range that lingers
 * unreused (the allocator can go elsewhere for many builds) still
 * rasterises whenever a later state's generation aliases its own mod 8; a
 * sector that rebuilds every build (animated vertex lighting) then z-fights
 * its current chunk with an older copy. frame_begin calls this after the
 * defer window guarantees no in-flight frame reads these bytes, the one
 * race-free moment (stamping at free time races the frames still reading
 * them). Each element is zeroed
 * with OPTC_SLOT_DEAD in face_flags; that slot is never published with
 * OPTC_SF_DRAWN, so the element is never drawn again. A vertex and a cell
 * record both carry face_flags, at their own offsets. Only the main buffer
 * is written.
 * @param d The deferred release. */
static void tc_dead_stamp(const tc_defer *d)
{
    enum { DEAD_N = 512 };
    static uint8_t s_dead[DEAD_N * sizeof(rhi_terrain_cell_anim)];
    static size_t s_dead_sz;          /* element size s_dead is laid out for */
    const size_t sz = tc_elem_size(d->arena);
    if (s_dead_sz != sz) {
        const size_t ffo = OPTC_ARENA_IS_CELL(d->arena)
                               ? offsetof(rhi_terrain_cell, face_flags)
                               : offsetof(rhi_terrain_vertex, face_flags);
        const uint32_t dead = OPTC_SLOT_DEAD;
        memset(s_dead, 0, sizeof(s_dead));
        for (int k = 0; k < DEAD_N; k++) memcpy(s_dead + (size_t)k * sz + ffo, &dead, sizeof dead);
        s_dead_sz = sz;
    }
    uint32_t left = d->count, at = d->first;
    /* NULL only for a slot whose source retired, which waits for every
     * deferred range naming it first. */
    const tc_pair *dp = tc_live_pair(d->arena, d->buf);
    if (!dp) return;
    while (left) {
        uint32_t n = left > DEAD_N ? DEAD_N : left;
        rhi_update_buffer_range(dp->vb, s_dead, (size_t)at * sz, (size_t)n * sz);
        at += n; left -= n;
    }
}

/** @brief Copy n doubles to floats, each rounded to nearest.
 *
 * @param dst Receives the floats.
 * @param src The doubles.
 * @param n The count. */
static void tc_f32_copy(float *dst, const double *src, int n)
{
    for (int i = 0; i < n; i++) dst[i] = (float)src[i];
}

/** @brief Fill the globals block's camera, projection and rim fields from the published globals.
 *
 * The camera is split per axis in x4 units (the baked corners' scale) by
 * openpete_tercache_cam_split. The scalars and the rim frames are the double
 * fields rounded to float, each once.
 * @param gu The block, zeroed by the caller.
 * @param g The published globals. */
static void tc_globals_f32(tc_globals_ubo *gu, const openpete_tercache_globals *g)
{
    const double cam[3] = { g->camx, g->camy, g->camz };
    for (int k = 0; k < 3; k++)
        openpete_tercache_cam_split(cam[k], &gu->cam_int_x4[k], &gu->cam_frac_x4[k]);
    const double ofH[4] = { g->ofx, g->ofy, g->H, g->vanim_alpha };
    const double aux[4] = { g->nearz, g->H_canon, g->canim_alpha, g->fogscale };
    tc_f32_copy(gu->ofH, ofH, 4);
    tc_f32_copy(gu->aux, aux, 4);
    const double oxyH[4] = { g->rim_ofx, g->rim_ofy, g->rim_H, 0.0 };
    tc_f32_copy(gu->frim_oxyH, oxyH, 4);
    tc_f32_copy(gu->frim_knobs, g->rim_knobs, 4);
    tc_f32_copy(&gu->frim_abc[0][0], &g->rim_abc[0][0], 24);
    tc_f32_copy(&gu->frim_O[0][0], &g->rim_O[0][0], 24);
    tc_f32_copy(&gu->frim_U[0][0], &g->rim_U[0][0], 24);
    tc_f32_copy(&gu->frim_W[0][0], &g->rim_W[0][0], 24);
    tc_f32_copy(&gu->frim_nrm[0][0], &g->rim_nrm[0][0], 24);
    tc_f32_copy(&gu->frim_bbox[0][0], &g->rim_bbox[0][0], 24);
    for (int i = 0; i < 96; i++) tc_f32_copy(gu->frim_pv[i], g->rim_pv[i], 2);
}

/** @brief Pack published globals into the std140 globals block: the matrix, the camera and projection fields, fog and flag words, scroll offsets, rim and cover planes, and the HD-fold table when asked.
 *
 * @param gu The block; zeroed here.
 * @param g The published globals.
 * @param hdfold 1 to snapshot the HD-fold entry table and ramp, 0 to leave them zero (no fold entry). */
static void tc_globals_pack(tc_globals_ubo *gu, const openpete_tercache_globals *g, int hdfold)
{
    memset(gu, 0, sizeof(*gu));
    for (int k = 0; k < 3; k++) {
        gu->mrow0[k] = g->mrow[0][k];
        gu->mrow1[k] = g->mrow[1][k];
        gu->mrow2[k] = g->mrow[2][k];
    }
    tc_globals_f32(gu, g);
    gu->fog[0] = g->fog_ref_base;
    gu->fog[1] = (int32_t)g->gflags;
    gu->fog[2] = g->cull_ref_s2;
    gu->fog[3] = (int32_t)g->ot_terms;
    gu->hp_drop[0] = g->hp_drop_sum_far;
    gu->hp_drop[1] = g->hp_drop_corner_min;
    memcpy(gu->sdv, g->scroll_dv, sizeof(gu->sdv));
    memcpy(gu->sdvc, g->scroll_dvc, sizeof(gu->sdvc));
    gu->rim_cnt[0]  = g->rim_n;
    if (hdfold) {   /* HD-fold entry table + continuous-fog ramp (level-lifetime state,
         * snapshotted per build like the rest of the globals). */
        int n = 0; float lad[2];
        const void *tab = openpete_native_hdfold_table(&n, lad);
        gu->hd_lad[0] = lad[0]; gu->hd_lad[1] = lad[1];
        gu->hd_lad[2] = (float)n;
        gu->hd_lad[3] = n > 0 ? 1.0f : 0.0f;
        if (n > 0)
            memcpy(gu->hd_entry, tab, (size_t)n * sizeof(gu->hd_entry[0]));
    }
    gu->cov_cnt[0]   = g->cov_n;
    gu->cov_cnt[1]   = g->cov_blend;
    gu->cov_margin[0] = (float)g->cov_margin;
    for (int pi = 0; pi < 6; pi++) {
        gu->cov_abc[pi][0] = (float)g->cov_abc[pi][0];
        gu->cov_abc[pi][1] = (float)g->cov_abc[pi][1];
        gu->cov_abc[pi][2] = (float)g->cov_abc[pi][2];
        gu->cov_abc[pi][3] = (float)g->cov_pn[pi];
        memcpy(gu->cov_bbox[pi], g->cov_bbox[pi], sizeof(gu->cov_bbox[pi]));
        for (int k = 0; k < 16; k++) {
            gu->cov_poly[pi * 16 + k][0] = g->cov_pxy[pi][k][0];
            gu->cov_poly[pi * 16 + k][1] = g->cov_pxy[pi][k][1];
        }
    }
}

void openpete_tercache_frame_begin(const openpete_tercache_globals *g,
                                   uint32_t stamp, int immediate)
{
    if (!s_tc.ready || !g) return;
    s_tc.present_ctr++;
    atomic_store_explicit(&s_tc.build_ctr, s_tc.present_ctr, memory_order_release);
    tc_note_failed_edge();
    tc_adopt_pending();
    tc_adopt_overflow();
    tc_store_adopt();
    tc_est_frame();
    s_tc.draws_prev = atomic_exchange_explicit(&s_tc.draws_last, 0u, memory_order_relaxed);
    /* release deferred ranges older than the in-flight window */
    int w = 0;
    uint32_t floor_serial = 0;
    const int have_floor = tc_release_floor(&floor_serial);
    for (int i = 0; i < s_tc.defer_n; i++) {
        const int aged   = s_tc.present_ctr - s_tc.defer[i].stamp > TC_DEFER_PRESENTS;
        const int unread = !have_floor || (int32_t)(s_tc.defer[i].stamp - floor_serial) <= 0;
        if (aged && unread) {
            tc_dead_stamp(&s_tc.defer[i]);
            if (tc_dbg()) fprintf(stderr, "[tc] release arena=%d first=%u count=%u freed_at=%u ctr=%u floor=%u\n",
                                  s_tc.defer[i].arena, s_tc.defer[i].first, s_tc.defer[i].count, s_tc.defer[i].stamp, s_tc.present_ctr, floor_serial);
            tc_defer_return(&s_tc.defer[i]);
        } else
            s_tc.defer[w++] = s_tc.defer[i];
    }
    if (tc_dbg()) {
        int live = 0;
        for (int i = 0; i < TC_RING; i++) live += s_tc.ring[i].valid && openpete_native_scene_stamp_live(s_tc.ring[i].stamp);
        fprintf(stderr, "[tc] begin stamp=%08x ctr=%u floor=%u have=%d live_states=%d released=%d held=%d\n",
                stamp, s_tc.present_ctr, floor_serial, have_floor, live, s_tc.defer_n - w, w);
    }
    s_tc.defer_n = w;
    tc_prev_release(have_floor, floor_serial);
    tc_source_retire();

    if (!s_tc.ubo) { s_tc.ready = 0; return; }   /* created at init: no device
                                                   * object is made from the
                                                   * builder's thread */
    /* STAGE into host structs only: the feeder runs outside the RHI frame,
     * and an outside-frame rhi_ubo_write fills every ring slot - racing an
     * in-flight frame's read. The live blocks flush on the present's first
     * draw (inside the frame, current slot only). */
    tc_state *T = tc_begin_state(stamp, immediate);
    tc_globals_ubo *gu = &T->g;
    tc_globals_pack(gu, g, 1);

    /* reset sector flags; origins are rewritten by sector_set */
    for (int i = 0; i < OPTC_SLOTS; i++) T->sec[i].o[3] = 0.0f;
    T->rl.n = -1;                         /* no range list until the commit */
    tc_state_mark(T);
    s_tc.build_target = T->sec;
    s_tc.build_state = T;
}

void openpete_tercache_frame_skip(uint32_t stamp, int immediate)
{
    /* A pure-CPU fallback build still occupies its slot in the promote
     * chain with an all-claimed (empty) state, so the present that renders
     * this build's scene draws no retained content. */
    if (!s_tc.ready) return;
    tc_note_failed_edge();
    tc_adopt_pending();
    tc_adopt_overflow();
    tc_store_adopt();
    tc_state *T = tc_begin_state(stamp, immediate);
    memset(&T->g, 0, sizeof(T->g));
    for (int i = 0; i < OPTC_SLOTS; i++) T->sec[i].o[3] = 0.0f;
    T->rl.n = -1;                         /* no range list until the commit */
    tc_state_mark(T);
    s_tc.build_target = T->sec;
    s_tc.build_state = T;
}

/** @brief Find the retained state built for a stamp: the eager scene ring, then live, then staged.
 *
 * The ring is searched only for a stamp carrying OPTC_STAMP_SCENE_BIT.
 * @param stamp Stamp of the scene.
 * @param src Receives 1 for a ring state, 2 for live, 3 for staged; left
 *        unchanged when no state matches.
 * @return The matching state, or NULL when the cache is not ready or no
 *         state carries @p stamp. */
static const tc_state *tc_state_for_stamp(uint32_t stamp, int *src)
{
    if (!s_tc.ready) return NULL;
    if (stamp & OPTC_STAMP_SCENE_BIT)
        for (int i = 0; i < TC_RING; i++)
            if ((uint32_t)atomic_load_explicit(&s_tc.ring_key[i], memory_order_acquire) == stamp) {
                *src = 1;
                return &s_tc.ring[i];
            }
    if (s_tc.live.valid && stamp == s_tc.live.stamp) { *src = 2; return &s_tc.live; }
    if (s_tc.staged.valid && stamp == s_tc.staged.stamp) { *src = 3; return &s_tc.staged; }
    for (int i = 0; i < OPENPETE_TERCACHE_HOLDS; i++)
        if (s_tc.hold[i].valid && stamp == s_tc.hold[i].stamp) { *src = 4; return &s_tc.hold[i]; }
    return NULL;
}

/** @brief Count a selection that found an older build of a stamp several ring states carry.
 *
 * Diagnostic. tc_state_for_stamp returns the lowest-index ring match; when
 * more than one valid ring state carries @p stamp and @p R is not the one
 * with the newest build serial, ring_dup_selects is incremented.
 * @param stamp Stamp of the composed scene.
 * @param R The state the selection returned. */
static void tc_dup_probe(uint32_t stamp, const tc_state *R)
{
    int n = 0, newer = 0;
    const uint32_t mine = (uint32_t)(atomic_load_explicit(&s_tc.ring_key[R - s_tc.ring],
                                                          memory_order_acquire) >> 32);
    for (int i = 0; i < TC_RING; i++) {
        const uint64_t k = atomic_load_explicit(&s_tc.ring_key[i], memory_order_acquire);
        if ((uint32_t)k != stamp) continue;
        n++;
        newer |= (int32_t)((uint32_t)(k >> 32) - mine) > 0;
    }
    if (n > 1 && newer) {
        s_tc.ring_dup_selects++;
        if (tc_dbg()) fprintf(stderr, "[tc] select stamp=%08x took an older build of %d\n", stamp, n);
    }
}

/* Selects the retained state whose stamp matches the composed scene. */
void openpete_tercache_frame_select(uint32_t present_stamp)
{
    /* present_stamp = the stamp of the SCENE being composed. The retained
     * terrain must be the same extraction as the CPU tris it composes with:
     * the eager ring first, then live (the lagged screenshot compose and the
     * sub-tick spine's immediate publishes both land here), then staged (a
     * depth-0 compose - the capture freeze-backdrop snapshot, a spine-off
     * windowed present - renders the CURRENT scene, whose terrain build sits
     * in staged until the next build's promote; drawing live there pairs
     * scene N with terrain N-1, and the camera moves between extractions, so
     * the skew bakes a whole-scene rim into the frozen backdrop). No other
     * state may stand in: a scene with no build of its own ran no feed. */
    int src = 0;
    const tc_state *R = tc_state_for_stamp(present_stamp, &src);
    if (R) {
        if (src == 1) tc_dup_probe(present_stamp, R);
        s_tc.draw_open = 1; tc_state_copy(&s_tc.sel, R); s_tc.sel_src = src;
    } else {
        s_tc.draw_open = 0; s_tc.sel_src = 0;
        if (tc_dbg() && s_tc.ready && s_tc.live.valid)
            fprintf(stderr, "[tc] select stamp=%08x no state (live=%08x staged=%08x%s)\n",
                    present_stamp, s_tc.live.stamp, s_tc.staged.valid ? s_tc.staged.stamp : 0u,
                    s_tc.staged.valid ? "" : " invalid");
    }
    if (s_tc.draw_open) s_tc.cen_presents++;
    s_tc.ubo_flush_serial = 0;
}

void openpete_tercache_sector_set(int slot, float ox, float oy, float oz,
                                  uint32_t sflags, const float *grade6, uint32_t coltab)
{
    if (!s_tc.build_target || slot < 0 || slot >= OPTC_SLOTS || slot == (int)OPTC_SLOT_DEAD)
        return;
    union { float f; uint32_t u; } w;
    w.u = sflags;
    s_tc.build_target[slot].o[0] = ox;
    s_tc.build_target[slot].o[1] = oy;
    s_tc.build_target[slot].o[2] = oz;
    s_tc.build_target[slot].o[3] = w.f;
    if (grade6) {
        for (int c = 0; c < 3; c++) {
            s_tc.build_target[slot].gp[c] = grade6[c];
            s_tc.build_target[slot].gq[c] = grade6[3 + c];
        }
    }
    w.u = OPTC_PREV_REC_NONE;   /* no position ring until openpete_tercache_sector_ring */
    s_tc.build_target[slot].gq[3] = w.f;
    w.u = coltab;   /* the colour table's first unit, read as the bits of gp.w */
    s_tc.build_target[slot].gp[3] = w.f;
}

void openpete_tercache_sector_ring(int slot, uint32_t first)
{
    if (!s_tc.build_target || slot < 0 || slot >= OPTC_SLOTS || slot == (int)OPTC_SLOT_DEAD)
        return;
    union { float f; uint32_t u; } w;
    w.u = first;   /* the ring entry's first unit, read as the bits of gq.w */
    s_tc.build_target[slot].gq[3] = w.f;
}

void openpete_tercache_frame_commit(void)
{
    /* Build staging is complete; promotion happens at the next frame_begin
     * (hook builds) and the UBO flush at the present's first draw. The
     * sector flags are final, so the state's draw ranges are listed now,
     * and its marks cover every chunk the build allocated. */
    if (s_tc.build_state) {
        tc_state_mark(s_tc.build_state);
        tc_ranges_build(s_tc.build_state);
    }
    s_tc.build_target = NULL;
    s_tc.build_state = NULL;
}

/* ---- draw --------------------------------------------------------------- */

/** @brief Return how many times each arena draw is issued (OPENPETE_TERRAIN_DUP_DRAW).
 *
 * Read once, clamped to 1..8, default 1. The measurement it serves: a
 * partial material claim costs one extra full-arena pass per material, and
 * N=2 prices that pass as an upper bound, since the duplicate also re-runs
 * the fragment stage, which a material pass mostly does not. */
static int tc_dup_draw(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("OPENPETE_TERRAIN_DUP_DRAW");
        n = (e && *e) ? atoi(e) : 1;
        if (n < 1) n = 1;
        if (n > 8) n = 8;
    }
    return n;
}

/* Which pass the arena draws are currently filtering for: -1 = every face
 * (no terrain material pass, and every depth/key pass), 0 = the stock
 * pass, >0 = one material's pass (its index + 1). Set around each group of
 * arena draws, with the resolution classes the pass keeps in
 * s_tc_mat_mask. */
static int s_tc_mat_pass = -1;
static uint32_t s_tc_mat_mask[8];

/** @brief Select the material pass the following arena draws filter for, with its resolution-class mask.
 *
 * @param pass -1 for every face, 0 for the stock pass, else a material
 *        pass id; a pass >= 0 loads its class mask from the material
 *        view (openpete_mods_material_terrain_pass_mask). */
static void tc_mat_pass_set(int pass)
{
    s_tc_mat_pass = pass;
    if (pass >= 0) (void)openpete_mods_material_terrain_pass_mask(pass, s_tc_mat_mask);
}
/* Material passes that fell through to the stock pipeline, counted per
 * draw pass: a pass whose material is dead, disabled, held, has no terrain
 * profile or no pipeline draws its faces stock. */
static unsigned s_tc_mat_stock;
/* Material passes run in this draw pass: the distinct members the baked
 * resolution classes resolved to this present. */
static unsigned s_tc_mat_passes;

/** @brief Return whether a pair is on the level-edge replacement's retire list.
 *
 * No draw may bind a retired pair; tc_draw_buf counts every draw that does
 * in draws_retired, which stays 0 while the invariant holds.
 * @param arena Arena index.
 * @param pr Pair about to be drawn.
 * @return 1 when a replacement is in the RETIRE state and lists @p pr, else 0. */
static int tc_pair_retired(int arena, const tc_pair *pr)
{
    if (atomic_load_explicit(&s_tc.rz_state, memory_order_acquire) != TC_RZ_RETIRE)
        return 0;
    for (int b = 0; b <= TC_BUF_SLOTS; b++)
        if (s_tc.rz_old[arena][b] == pr) return 1;
    return 0;
}

/** @brief Count a buffer draw against the two invariants the census reports.
 *
 * draws_retired counts a draw of a pair on the level-edge retire list (the
 * invariant is 0); sealed_draws counts a held state's draw of a buffer
 * sealed at a failed level edge, the case the seal exists for.
 * @param arena Arena index.
 * @param b The buffer.
 * @param pr The pair being drawn. */
static void tc_draw_census(int arena, const tc_buf *b, const tc_pair *pr)
{
    if (tc_pair_retired(arena, pr))
        atomic_fetch_add_explicit(&s_tc.draws_retired, 1u, memory_order_relaxed);
    if (b->sealed && s_tc.sel_src == 4) s_tc.sealed_draws++;
}

/** @brief Issue one face-identity arena draw over elements [first, first + n) of the bound buffer.
 *
 * Pushes @p pc with the three words terrain_faceid.frag reads in its
 * padding: the draw word (pass selector, arena, buffer), the element index
 * of the draw's first primitive and the elements per primitive step. A
 * vertex-list span is one draw (step 3); a cell-record span is one draw per
 * record (base record x @p per, step 1), since gl_PrimitiveID restarts with
 * each instance.
 * @param pc The draw's push constants.
 * @param arena Arena index.
 * @param bi Buffer index within the arena.
 * @param cells Nonzero for a cell-record arena.
 * @param per Vertices per cell record.
 * @param first First element (vertex, or record instance).
 * @param n Element count. */
static void tc_faceid_issue(const tc_pc *pc, int arena, int bi, int cells, uint32_t per,
                            uint32_t first, uint32_t n)
{
    tc_pc f = *pc;
    f.pad_[0] = (int32_t)(((uint32_t)pc->pass_sel << 16) | ((uint32_t)arena << 8) | (uint32_t)bi);
    if (!cells) {
        f.pad_[1] = (int32_t)first;
        f.pad_[2] = 3;
        tc_push(s_tc.dev, &f);
        rhi_draw(s_tc.dev, n, first);
        return;
    }
    f.pad_[2] = 1;
    for (uint32_t i = 0; i < n; i++) {
        f.pad_[1] = (int32_t)((first + i) * per);
        tc_push(s_tc.dev, &f);
        rhi_draw_instanced(s_tc.dev, per, 1, 0, first + i);
    }
}

/* Set while openpete_tercache_readback_record draws: every arena draw is
 * then issued once, as a plain draw. */
static int s_tc_rb_drawing;

/** @brief Issue one arena draw over elements [first, first + n) of the bound buffer, tc_dup_draw times.
 *
 * Under OPENPETE_TERRAIN_FACEID the draw is tc_faceid_issue's, once; a
 * readback draw (s_tc_rb_drawing) is issued once.
 * @param pc The draw's push constants (already pushed for the stock draw).
 * @param arena Arena index.
 * @param bi Buffer index within the arena.
 * @param cells Nonzero for a cell-record arena (instanced).
 * @param per Vertices per cell record.
 * @param first First element (vertex, or record instance).
 * @param n Element count. */
static void tc_issue(const tc_pc *pc, int arena, int bi, int cells, uint32_t per,
                     uint32_t first, uint32_t n)
{
    if (openpete_tercache_faceid() && !s_tc_rb_drawing)
        tc_faceid_issue(pc, arena, bi, cells, per, first, n);
    else
        for (int r = 0, rn = s_tc_rb_drawing ? 1 : tc_dup_draw(); r < rn; r++) {
            if (cells) rhi_draw_instanced(s_tc.dev, per, n, 0, first);
            else       rhi_draw(s_tc.dev, n, first);
        }
    s_tc.cen_verts += (uint64_t)n * (cells ? per : 1u);
    s_tc.cen_draws++;
}

/** @brief Draw one buffer of an arena: the selected state's spans in it, or the whole high-water range.
 *
 * Each buffer numbers its vertices from 0, and gl_VertexIndex includes the
 * draw's first vertex, so the vertex stage sees the same index in a span
 * draw as in a whole-range draw. Every vertex chunk holds whole triangles
 * and starts on a triangle boundary (allocations and frees are multiples
 * of 3), so a span that starts at a chunk assembles the same triangles. A
 * cell arena's buffer is an instance-rate record array: one instance per
 * record, the record's cell vertices per instance, a span selected by the
 * first instance. With a range list the draw covers the state's spans in
 * ascending order, clamped to the buffer's range, and so emits the
 * primitives a whole-range draw would rasterise, in the same order;
 * without one, or with OPENPETE_TC_RANGE_DRAW=0, it covers [0, high).
 * `high` is the selected state's mark for the buffer (tc_state_mark), not
 * the live mark the builder grows meanwhile; a buffer whose live pair is not
 * the one the state marked holds none of the state's chunks and is skipped.
 * Pushes @p pc, binds the pair (with its twin when it has one), and counts
 * the submission for the census.
 * @param arena Arena index.
 * @param bi Buffer index within the arena.
 * @param b Buffer.
 * @param pc Push constants for the draw. */
static void tc_draw_buf(int arena, int bi, const tc_buf *b, const tc_pc *pc)
{
    /* One load of the pair for the whole draw, and the range clamped to its
     * capacity. */
    const tc_pair *pr = atomic_load_explicit(&b->pair, memory_order_acquire);
    if (!pr || pr->serial != s_tc.sel.mark_pair[arena][bi]) return;
    uint32_t nverts = s_tc.sel.mark_high[arena][bi];   /* records, on a cell arena */
    if (nverts > pr->cap) nverts = pr->cap;
    if (nverts == 0) return;
    const int cells = OPTC_ARENA_IS_CELL(arena);
    const uint32_t per = OPTC_ARENA_IS_CELL_ANIM(arena) ? RHI_TERRAIN_CELL_VERTS_ANIM
                                                        : RHI_TERRAIN_CELL_VERTS_NEAR;
    const tc_ranges *rl = &s_tc.sel.rl;
    const int listed = rl->n >= 0 && tc_range_draw_on();
    const int k = arena * TC_BUF_SLOTS + bi;
    const uint16_t *off = rl->off[pc->pass_sel == 1 ? TC_RC_FILL : TC_RC_DRAW];
    if (listed && off[k] == off[k + 1]) {
        s_tc.cen_verts_whole += (uint64_t)nverts * (cells ? per : 1u);
        return;                           /* nothing selected in this buffer */
    }
    tc_draw_census(arena, b, pr);
    tc_push(s_tc.dev, pc);
    if (pr->pv) rhi_bind_vertex_buffer2(s_tc.dev, pr->vb, pr->pv);
    else        rhi_bind_vertex_buffer(s_tc.dev, pr->vb);
    s_tc.cen_verts_whole += (uint64_t)nverts * (cells ? per : 1u);
    if (!listed) {
        s_tc.cen_nolist++;
        tc_issue(pc, arena, bi, cells, per, 0, nverts);
    } else {
        for (int i = off[k]; i < off[k + 1]; i++) {
            const uint32_t lo = rl->r[i].first;
            if (lo >= nverts) break;
            const uint32_t hi = lo + rl->r[i].count < nverts ? lo + rl->r[i].count : nverts;
            tc_issue(pc, arena, bi, cells, per, lo, hi - lo);
        }
    }
    if (cells) { s_tc.cell_draws++; s_tc.cell_instances += nverts; }
    atomic_fetch_add_explicit(&s_tc.draws_last, 1u, memory_order_relaxed);
}

/** @brief Return an arena's high-water element count summed over its buffers, as the selected state marked them.
 *
 * tc_draw_anim's test for whether the arena has anything to draw. Reads the
 * selected state's marks (tc_state_mark), which the present thread owns,
 * not the live marks the builder writes.
 * @param arena Arena index.
 * @return The summed high-water count; 0 for an empty arena. */
static uint32_t tc_arena_high(int arena)
{
    uint32_t h = 0;
    for (int b = 0; b < TC_BUF_SLOTS; b++)
        h += s_tc.sel.mark_high[arena][b];
    return h;
}

/** @brief Fill a terrain push-constant block from a clip mapping.
 *
 * Zeroes the block, copies the clip mapping's extents, band and freeze
 * factor, the portal cover flag and fragment affine, and stamps the material
 * pass (s_tc_mat_pass) with its class mask (s_tc_mat_mask), the vertex
 * precision snap latched for this present (s_tc_snap) and the key-image
 * flags of the draw in progress (s_tc.kflags).
 * @param pc Out: the block to fill.
 * @param pass_sel Pass selector.
 * @param c Clip mapping; must not be NULL. */
static void tc_pc_fill(tc_pc *pc, int pass_sel, const openpete_tercache_clip *c)
{
    memset(pc, 0, sizeof(*pc));
    pc->x_base = c->x_base; pc->x_span = c->x_span;
    pc->y_base = c->y_base; pc->y_span = c->y_span;
    pc->disp_w = c->disp_w; pc->clip_h = c->clip_h;
    pc->band = c->band; pc->freeze_wf = c->freeze_wf;
    pc->pass_sel = pass_sel; pc->full_extent = c->full_extent;
    pc->mat_pass = s_tc_mat_pass;
    memcpy(pc->cls_mask, s_tc_mat_mask, sizeof pc->cls_mask);
    pc->snap = s_tc_snap;
    pc->cover = c->cover;
    pc->pad_[1] = (int32_t)s_tc.kflags;
    memcpy(pc->fc, c->fc, sizeof pc->fc);
}

/** @brief Draw every buffer of one arena with the bound pipeline.
 * @param arena Arena index.
 * @param pass_sel Pass selector pushed to the vertex stage.
 * @param c Clip mapping for the draw target. */
static void tc_draw_arena(int arena, int pass_sel, const openpete_tercache_clip *c)
{
    const tc_arena *a = &s_tc.arena[arena];
    tc_pc pc;
    tc_pc_fill(&pc, pass_sel, c);
    for (int b = 0, n = atomic_load_explicit(&a->n, memory_order_acquire); b < n; b++)
        tc_draw_buf(arena, b, &a->buf[b], &pc);
}

/** @brief Bind a variant pipeline and draw one arena with it.
 *
 * Serves the poly-anim, near-variant and cell-record arenas: the pipeline is
 * that variant of the slot's pipeline (same state, a different vertex stage
 * and binding-1 twin). Nothing is restored afterwards: the caller sequences
 * all draws for one slot and every arena draw re-pushes its own constants
 * and buffers. Skipped when @p p is NULL or the arena is empty.
 * @param p Pipeline to bind.
 * @param arena Arena index.
 * @param pass_sel Pass selector pushed to the vertex stage.
 * @param c Clip mapping for the draw target. */
static void tc_draw_anim(rhi_pipeline *p, int arena, int pass_sel,
                         const openpete_tercache_clip *c)
{
    if (!p || tc_arena_high(arena) == 0) return;
    rhi_bind_pipeline(s_tc.dev, p);
    tc_draw_arena(arena, pass_sel, c);
}

/* ---- material passes ----------------------------------------------------
 * A material is a draw-state change, so faces it claims cannot be drawn in
 * the same pass as the faces it does not. The arena stays whole: it is drawn
 * once per pass, and the vertex stage keeps only the faces whose resolution
 * class maps to that pass (pc.cls_mask). The stock pass runs first with
 * mat_pass 0, keeping the faces of class 0 and of every class that resolved
 * to stock.
 *
 * The world, fill, alpha, add and subtract colour slots take part, the blend
 * arenas included: each runs its stock pass, then one pass per resolved
 * member. The depth prepass and key slots draw every face. */

void *openpete_tercache_material_pipe(void **pslot, int variant, int slot,
                                      const uint32_t *frag, size_t frag_words,
                                      const char *owner, int mat_index, int chain_module,
                                      unsigned generation)
{
    if (*pslot == OPENPETE_POSTFX_PIPE_FAILED) return NULL;
    if (*pslot && *pslot != OPENPETE_POSTFX_PIPE_PENDING) return *pslot;
    if (!s_tc.dev || variant < 0 || variant >= OPENPETE_MATERIAL_TER_VARIANTS) return NULL;
    rhi_pipeline_desc d;
    memset(&d, 0, sizeof(d));
    d.vert_spv = tc_vert(variant);
    d.vert_spv_whole = tc_vert_whole(variant);
    d.blend = RHI_BLEND_NONE;
    d.depth_test = true;
    d.terrain_pipeline = true;
    d.terrain_anim = (variant == 1 || variant == 3);
    d.terrain_near = (variant == 2 || variant == 4);
    d.terrain_cells = (variant >= 3);
    d.tex_ssbo_set2 = true;   /* the stock layout: the cache binds set 2 once per draw */
    d.push_const_size = sizeof(tc_pc);
    switch (slot) {
        case 1: d.stencil = RHI_STENCIL_MARK; break;
        case 2: d.stencil = RHI_STENCIL_TEST_ZERO; break;
        /* A material inherits its group's compositing, so a blend slot's
         * pipeline differs from the stock one only in the fragment
         * stage - same equation, same depth-write rule. */
        case 3: d.blend = RHI_BLEND_ALPHA;    d.depth_write_off = true; break;
        case 4: d.blend = RHI_BLEND_ADD;      d.depth_write_off = true; break;
        case 5: d.blend = RHI_BLEND_SUBTRACT; d.depth_write_off = true; break;
        default: break;
    }
    const openpete_lazy_ref ref = {
        .kind = chain_module >= 0 ? OPENPETE_LAZY_CHAIN_TERRAIN : OPENPETE_LAZY_TERRAIN,
        .index = chain_module >= 0 ? chain_module : mat_index,
        .sub = variant * OPENPETE_MATERIAL_TER_SLOTS + slot, .generation = generation,
        .point = -1, .owner = owner,
    };
    const int rc = openpete_native_lazy_pipe(&d, NULL, 0, frag, frag_words, pslot, &ref);
    if (rc < 0)
        fprintf(stderr, "[tercache] material: terrain pipeline build "
                        "FAILED for %s (variant %d slot %d) - its faces "
                        "draw stock\n", owner ? owner : "?", variant, slot);
    return rc == 0 ? *pslot : NULL;
}

/** @brief Bind one pass's terrain material pipeline for a vertex variant and colour slot, or the stock pipeline.
 *
 * The material pipeline is built lazily, once per material (or combined
 * module, for a chain pass), variant and slot, and bound with the material
 * UBO pair at set 3, or for a chain pass the chain entry's terrain block
 * of the chain uniform set (openpete_native_material_chain_bind). @p stock is the
 * pipeline this pass's faces would have been drawn with had they not been
 * claimed; it is bound instead whenever the material cannot serve them (a
 * held or disabled owner, a pipeline that failed to build, a shader with
 * no terrain profile, no material UBO yet), and each
 * such fallback counts in s_tc_mat_stock. The pass always runs: the stock
 * pass has already dropped these faces, so skipping it would leave a hole in
 * the world rather than an unreshaded patch.
 * @param bucket Terrain material pass id: a material index + 1, or
 *        OPENPETE_MATERIAL_TER_PASS_CHAIN + terrain chain entry id.
 * @param variant Vertex variant: 0 plain, 1 anim, 2 near, 3 anim cells,
 *        4 near cells.
 * @param slot Colour slot: 0 world, 1 world with stencil mark, 2 fill,
 *        3 alpha, 4 add, 5 subtract.
 * @param stock Stock pipeline for the fallback, or NULL when no arena of
 *        this flavour draws in this slot.
 * @return 1 when a pipeline is bound, 0 when nothing is to be drawn. */
static int tc_bind_material(int bucket, int variant, int slot,
                            rhi_pipeline *stock)
{
    openpete_mod_material_terrain_t mg;
    int idx = variant * OPENPETE_MATERIAL_TER_SLOTS + slot;
    if (openpete_mods_material_terrain_bucket_gpu(bucket, idx, &mg) != 0)
        goto draw_stock;
    rhi_pipe_site_visit(RHI_PIPE_SITE_TERRAIN_MATERIAL, 1);
    if (!openpete_tercache_material_pipe(mg.pipeline_slot, variant, slot, mg.frag_spv,
                                         mg.frag_words, mg.owner, mg.index, mg.chain_module,
                                         mg.generation))
        goto draw_stock;
    {
        if (!openpete_native_material_ubo()) goto draw_stock;   /* no uniform source yet */
        if (mg.chain >= 0 && !openpete_native_material_chain_ready()) goto draw_stock;
        rhi_bind_pipeline(s_tc.dev, (rhi_pipeline *)*mg.pipeline_slot);
        if (mg.chain >= 0)
            openpete_native_material_chain_bind(s_tc.dev, mg.chain, &mg.frag_cdata);
        else
            openpete_native_material_bind(s_tc.dev, mg.uniform_slot, &mg.frag_cdata);
        return 1;
    }
draw_stock:
    if (!stock) return 0;       /* no arena of this flavour in this slot */
    s_tc_mat_stock++;
    rhi_bind_pipeline(s_tc.dev, stock);
    return 1;
}

/** @brief Draw the opaque world arenas of all five vertex variants for one material pass.
 *
 * Each variant carries the stock pipeline it would have used, so a material
 * that cannot serve its faces this present degrades to stock shading rather
 * than to a hole. The plain, anim and near variants draw their opaque and LP
 * arenas; the cell variants draw their opaque arena.
 * @param bucket Terrain material pass id.
 * @param slot Colour slot (0 world, 1 world with stencil mark).
 * @param use_mark Nonzero to fall back to the stencil-mark pipelines.
 * @param clip Clip mapping for the draw target. */
static void tc_draw_material_world(int bucket, int slot, int use_mark,
                                   const openpete_tercache_clip *clip)
{
    if (tc_bind_material(bucket, 0, slot,
                         use_mark ? s_tc.ps.pipe_world_mark : s_tc.ps.pipe_world)) {
        tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_LP,     0, clip);
    }
    if (tc_bind_material(bucket, 1, slot,
                         use_mark ? s_tc.ps.pipe_a_world_mark : s_tc.ps.pipe_a_world)) {
        tc_draw_arena(OPTC_ARENA_A_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_A_LP,     0, clip);
    }
    if (tc_bind_material(bucket, 2, slot,
                         use_mark ? s_tc.ps.pipe_n_world_mark : s_tc.ps.pipe_n_world)) {
        tc_draw_arena(OPTC_ARENA_N_OPAQUE, 0, clip);
        tc_draw_arena(OPTC_ARENA_N_LP,     0, clip);
    }
    const int cp = use_mark ? TC_CP_MARK : TC_CP_WORLD;
    if (tc_bind_material(bucket, 3, slot, s_tc.ps.pipe_ca[cp]))
        tc_draw_arena(OPTC_ARENA_CA_OPAQUE, 0, clip);
    if (tc_bind_material(bucket, 4, slot, s_tc.ps.pipe_cn[cp]))
        tc_draw_arena(OPTC_ARENA_CN_OPAQUE, 0, clip);
}

/** @brief Draw one blend class's arenas of all five vertex variants for one material pass.
 *
 * @param bucket Terrain material pass id.
 * @param slot Colour slot (3 alpha, 4 add, 5 subtract).
 * @param cls Blend arena class: OPTC_ARENA_ALPHA, OPTC_ARENA_ADD or
 *        OPTC_ARENA_SUB.
 * @param s_plain Stock pipeline of the plain arena.
 * @param s_anim Stock pipeline of the anim arena.
 * @param s_near Stock pipeline of the near arena.
 * @param clip Clip mapping for the draw target. */
static void tc_draw_material_blend(int bucket, int slot, int cls,
                                   rhi_pipeline *s_plain, rhi_pipeline *s_anim,
                                   rhi_pipeline *s_near,
                                   const openpete_tercache_clip *clip)
{
    const int cp = TC_CP_ALPHA + (cls - OPTC_ARENA_ALPHA);
    if (tc_bind_material(bucket, 0, slot, s_plain))
        tc_draw_arena(cls, 0, clip);
    if (tc_bind_material(bucket, 1, slot, s_anim))
        tc_draw_arena(OPTC_ARENA_ANIM_BASE + cls, 0, clip);
    if (tc_bind_material(bucket, 2, slot, s_near))
        tc_draw_arena(OPTC_ARENA_NEAR_BASE + cls, 0, clip);
    if (tc_bind_material(bucket, 3, slot, s_tc.ps.pipe_ca[cp]))
        tc_draw_arena(OPTC_ARENA_CELL_ANIM_BASE + cls, 0, clip);
    if (tc_bind_material(bucket, 4, slot, s_tc.ps.pipe_cn[cp]))
        tc_draw_arena(OPTC_ARENA_CELL_NEAR_BASE + cls, 0, clip);
}

/** @brief Run the material passes of one colour slot, after its stock passes.
 *
 * One pass per listed pass id, each setting s_tc_mat_pass to the id and
 * s_tc_mat_mask to the resolution classes that resolved to it, so the
 * vertex stage keeps only those faces. The list holds only the members
 * the baked classes resolved to this present, so the pass count is at
 * most the live terrain materials with baked faces: a class that resolved
 * to stock draws in the stock pass, and a material no class resolved to
 * draws no pass. The prepass and key slots run no material pass.
 * @param which OPTC_DRAW_* slot.
 * @param buckets Pass ids.
 * @param bn Number of pass ids.
 * @param use_mark Nonzero when the world slot uses the stencil-mark pipelines.
 * @param clip Clip mapping for the draw target. */
static void tc_draw_material_passes(int which, const int *buckets, int bn,
                                    int use_mark,
                                    const openpete_tercache_clip *clip)
{
    for (int k = 0; k < bn; k++) {
        const int b = buckets[k];
        tc_mat_pass_set(b);
        s_tc_mat_passes++;
        switch (which) {
        case OPTC_DRAW_WORLD:
            tc_draw_material_world(b, use_mark ? 1 : 0, use_mark, clip);
            break;
        case OPTC_DRAW_ALPHA:
            tc_draw_material_blend(b, 3, OPTC_ARENA_ALPHA,
                                   s_tc.ps.pipe_alpha, s_tc.ps.pipe_a_alpha,
                                   s_tc.ps.pipe_n_alpha, clip);
            break;
        case OPTC_DRAW_ADD:
            tc_draw_material_blend(b, 4, OPTC_ARENA_ADD,
                                   s_tc.ps.pipe_add, s_tc.ps.pipe_a_add,
                                   s_tc.ps.pipe_n_add, clip);
            break;
        case OPTC_DRAW_SUB:
            tc_draw_material_blend(b, 5, OPTC_ARENA_SUB,
                                   s_tc.ps.pipe_sub, s_tc.ps.pipe_a_sub,
                                   s_tc.ps.pipe_n_sub, clip);
            break;
        case OPTC_DRAW_FILL:
            if (tc_bind_material(b, 0, 2, s_tc.ps.pipe_fill))
                tc_draw_arena(OPTC_ARENA_LP,   1, clip);
            if (tc_bind_material(b, 1, 2, s_tc.ps.pipe_a_fill))
                tc_draw_arena(OPTC_ARENA_A_LP, 1, clip);
            if (tc_bind_material(b, 2, 2, s_tc.ps.pipe_n_fill))
                tc_draw_arena(OPTC_ARENA_N_LP, 1, clip);
            break;
        default: break;
        }
    }
}

/** @brief Write the selected state into this rhi frame's UBO ring slot on the frame's first terrain draw.
 *
 * One selection can render across several rhi frames (ubo_flush_serial), so
 * the flush runs once per frame serial; it also latches the vertex-snap
 * mode every draw of the frame pushes. */
static void tc_ubo_flush_frame(void)
{
    const int snap_now = openpete_native_vertex_snap_present();   /* 0 or 1 */
    const uint64_t fs = rhi_frame_serial(s_tc.dev);
    if (s_tc.ubo_flush_serial == fs) return;
    rhi_ubo_write(s_tc.ubo, 0, &s_tc.sel.g, sizeof(s_tc.sel.g));
    rhi_ubo_write(s_tc.ubo, 1, s_tc.sel.sec, sizeof(s_tc.sel.sec));
    s_tc.ubo_flush_serial = fs;
    s_tc_snap = snap_now;
}

/** @brief Return the GPU zone name of a terrain draw pass.
 * @param which Pass, an OPTC_DRAW_* value.
 * @return The zone name; "ter.gpu" for a value outside the passes. */
static const char *tc_zone_name(int which)
{
    static const char *k_zone[] = { "ter.world", "ter.alpha", "ter.add",
                                    "ter.sub", "ter.fill", "ter.prepass",
                                    "ter.prepass_depth", "ter.key" };
    return (which >= 0 && which <= OPTC_DRAW_KEY) ? k_zone[which] : "ter.gpu";
}

/** @brief Bind the scene's key-contest images at the sets terrain.frag declares.
 *
 * Same bind-at-pipe discipline as bind_scene_pipe: an absent image leaves
 * its set unbound, and fragments with vKoff == 0 never sample it.
 * @param keys The images; each NULL member is skipped. */
static void tc_bind_keys(const openpete_tercache_keys *keys)
{
    if (keys->hp)    rhi_bind_depth_tex_pair(s_tc.dev, keys->hp, keys->hp, 1);
    if (keys->actor) rhi_bind_depth_tex_pair(s_tc.dev, keys->actor, keys->actor, 7);
}

/** @brief Bind every input a terrain pass reads besides its pipeline.
 *
 * The VRAM page, the key-contest images, the prop key image with the
 * previous-corner record store at set 2, the HD-fold atlas at set 4 (the
 * init-time 1x1 dummy when no fold entry exists: terrain.frag references the
 * set statically and vHd == 0 fragments never sample it), and the two
 * terrain blocks at sets 5/6 - not the contract set 3, whose binding 1 is a
 * mod's own uniform block, and a descriptor set views one buffer per binding.
 * @param vram VRAM texture, or NULL to leave the binding.
 * @param keys Key-contest images, or NULL for a pass that contests nothing.
 * @return 0, or -1 when the set-2 bind was refused (counted in
 *         bind_refusals): the pass must draw nothing. */
static int tc_bind_draw_inputs(rhi_texture *vram, const openpete_tercache_keys *keys)
{
    if (vram) rhi_bind_vram(s_tc.dev, vram);
    if (keys) tc_bind_keys(keys);
    /* Set 2: the prop key image (the 1x1 stand-in when absent; fragments
     * with vKoff == 0 never sample it) beside the previous-corner records
     * the near stages read. Every cache pipeline holds this layout, so the
     * one bind serves every pipeline the pass switches to. */
    const tc_store *st = atomic_load_explicit(&s_tc.prev_live, memory_order_acquire);
    if (rhi_bind_texture_storage(s_tc.dev, (keys && keys->prop) ? keys->prop : s_tc.hd_dummy,
                                 st->buf,
                                 (uint32_t)(st->cap * OPTC_PREV_REC_WORDS * sizeof(uint32_t)),
                                 2, 0) != 0) {
        atomic_fetch_add_explicit(&s_tc.bind_refusals, 1u, memory_order_relaxed);
        return -1;
    }
    rhi_texture *at = openpete_native_hdfold_atlas();
    if (!at) at = s_tc.hd_dummy;
    if (at) rhi_bind_texture_set(s_tc.dev, at, 4);
    rhi_ubo_bind_at(s_tc.dev, s_tc.ubo, 5, 0);
    rhi_ubo_bind_at(s_tc.dev, s_tc.ubo, 6, 1);
    return 0;
}

void openpete_tercache_draw(int which, const openpete_tercache_clip *clip,
                            rhi_texture *vram, int use_mark,
                            const openpete_tercache_keys *keys)
{
    if (!s_tc.ready || !s_tc.draw_open || !s_tc.ubo) return;
    rhi_pipeline *p = NULL;
    switch (which) {
        case OPTC_DRAW_WORLD:   p = use_mark ? s_tc.ps.pipe_world_mark : s_tc.ps.pipe_world; break;
        case OPTC_DRAW_ALPHA:   p = s_tc.ps.pipe_alpha;   break;
        case OPTC_DRAW_ADD:     p = s_tc.ps.pipe_add;     break;
        case OPTC_DRAW_SUB:     p = s_tc.ps.pipe_sub;     break;
        case OPTC_DRAW_FILL:    p = s_tc.ps.pipe_fill;    break;
        case OPTC_DRAW_PREPASS: p = s_tc.ps.pipe_prepass; break;
        case OPTC_DRAW_PREPASS_DEPTH: p = s_tc.ps.pipe_prepass_depth; break;
        case OPTC_DRAW_KEY:     p = s_tc.ps.pipe_key;     break;
    }
    if (!p) return;
    s_tc.kflags = keys ? keys->flags : 0u;
    tc_ubo_flush_frame();
    rhi_gpu_zone_begin(s_tc.dev, tc_zone_name(which));
    rhi_bind_pipeline(s_tc.dev, p);
    if (tc_bind_draw_inputs(vram, keys) != 0) {   /* nothing may read a stale set 2 */
        rhi_gpu_zone_end(s_tc.dev);
        return;
    }
    /* Every colour slot runs its stock pass filtered (mat_pass 0, the
     * classes resolved to stock) and then one pass per listed member. The
     * depth and key passes shade nothing, so they draw every face
     * regardless of class. */
    int buckets[OPENPETE_MATERIAL_TER_BUCKET_MAX];
    const int bn = (which == OPTC_DRAW_PREPASS || which == OPTC_DRAW_PREPASS_DEPTH ||
                    which == OPTC_DRAW_KEY)
                       ? 0
                       : openpete_mods_material_terrain_bucket_list(
                             buckets, OPENPETE_MATERIAL_TER_BUCKET_MAX);
    tc_mat_pass_set(bn > 0 ? 0 : -1);
    s_tc_mat_stock = 0;
    s_tc_mat_passes = 0;
    const int cworld = use_mark ? TC_CP_MARK : TC_CP_WORLD;
    switch (which) {
        case OPTC_DRAW_WORLD:
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_anim(use_mark ? s_tc.ps.pipe_a_world_mark : s_tc.ps.pipe_a_world,
                         OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[cworld], OPTC_ARENA_CA_OPAQUE, 0, clip);
            tc_draw_anim(use_mark ? s_tc.ps.pipe_a_world_mark : s_tc.ps.pipe_a_world,
                         OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(use_mark ? s_tc.ps.pipe_n_world_mark : s_tc.ps.pipe_n_world,
                         OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[cworld], OPTC_ARENA_CN_OPAQUE, 0, clip);
            tc_draw_anim(use_mark ? s_tc.ps.pipe_n_world_mark : s_tc.ps.pipe_n_world,
                         OPTC_ARENA_N_LP,     0, clip);
            break;
        case OPTC_DRAW_ALPHA:
            tc_draw_arena(OPTC_ARENA_ALPHA, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_alpha, OPTC_ARENA_A_ALPHA, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_ALPHA], OPTC_ARENA_CA_ALPHA, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_alpha, OPTC_ARENA_N_ALPHA, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_ALPHA], OPTC_ARENA_CN_ALPHA, 0, clip);
            break;
        case OPTC_DRAW_ADD:
            tc_draw_arena(OPTC_ARENA_ADD,   0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_add, OPTC_ARENA_A_ADD, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_ADD], OPTC_ARENA_CA_ADD, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_add, OPTC_ARENA_N_ADD, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_ADD], OPTC_ARENA_CN_ADD, 0, clip);
            break;
        case OPTC_DRAW_SUB:
            tc_draw_arena(OPTC_ARENA_SUB,   0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_sub, OPTC_ARENA_A_SUB, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_SUB], OPTC_ARENA_CA_SUB, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_sub, OPTC_ARENA_N_SUB, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_SUB], OPTC_ARENA_CN_SUB, 0, clip);
            break;
        case OPTC_DRAW_FILL:
            /* LP only: poly-anim sectors never backstop-fill (their
             * SF_FILLLP is clear), env-anim colour and near ones do. */
            tc_draw_arena(OPTC_ARENA_LP,    1, clip);
            tc_draw_anim(s_tc.ps.pipe_a_fill, OPTC_ARENA_A_LP, 1, clip);
            tc_draw_anim(s_tc.ps.pipe_n_fill, OPTC_ARENA_N_LP, 1, clip);
            break;
        case OPTC_DRAW_PREPASS:
            /* The WORLD faces of the OPTC_DRAW_WORLD set. */
            tc_draw_arena(OPTC_ARENA_OPAQUE, 0, clip);
            tc_draw_arena(OPTC_ARENA_LP,     0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_prepass, OPTC_ARENA_A_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_PREPASS], OPTC_ARENA_CA_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_a_prepass, OPTC_ARENA_A_LP,     0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_prepass, OPTC_ARENA_N_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_PREPASS], OPTC_ARENA_CN_OPAQUE, 0, clip);
            tc_draw_anim(s_tc.ps.pipe_n_prepass, OPTC_ARENA_N_LP,     0, clip);
            break;
        case OPTC_DRAW_PREPASS_DEPTH:
            /* The hole-fill faces of the OPTC_DRAW_FILL set, depth only. */
            tc_draw_arena(OPTC_ARENA_LP,     1, clip);
            tc_draw_anim(s_tc.ps.pipe_a_prepass_depth, OPTC_ARENA_A_LP, 1, clip);
            tc_draw_anim(s_tc.ps.pipe_n_prepass_depth, OPTC_ARENA_N_LP, 1, clip);
            break;
        case OPTC_DRAW_KEY:
            /* keyed opaque faces only (blend arenas contest but never emit;
             * the vertex stage kills koff<=0.25 and fill-pass faces). */
            tc_draw_arena(OPTC_ARENA_OPAQUE, 2, clip);
            tc_draw_arena(OPTC_ARENA_LP,     2, clip);
            tc_draw_anim(s_tc.ps.pipe_a_key, OPTC_ARENA_A_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.ps.pipe_ca[TC_CP_KEY], OPTC_ARENA_CA_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.ps.pipe_a_key, OPTC_ARENA_A_LP,     2, clip);
            tc_draw_anim(s_tc.ps.pipe_n_key, OPTC_ARENA_N_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.ps.pipe_cn[TC_CP_KEY], OPTC_ARENA_CN_OPAQUE, 2, clip);
            tc_draw_anim(s_tc.ps.pipe_n_key, OPTC_ARENA_N_LP,     2, clip);
            break;
    }
    tc_draw_material_passes(which, buckets, bn, use_mark ? 1 : 0, clip);
    if (tc_dbg())
        fprintf(stderr, "[tc] %s mat_passes=%u stock_fallback=%u\n",
                tc_zone_name(which), s_tc_mat_passes, s_tc_mat_stock);
    s_tc_mat_pass = -1;
    rhi_gpu_zone_end(s_tc.dev);
}

/* ---- shared-vertex position readback ------------------------------------
 * OPENPETE_TERRAIN_READBACK (diagnostic, unregistered): the TERRAIN_READBACK
 * stage of each family draws every drawn vertex of the selected state as a
 * point at a pixel its element addresses (terrain.vert rb_emit), once with
 * its gl_Position bits and once with its readback key, into one RGBA32_UINT
 * target the harness pairs on the host (tools/terrain_readback.py). */

/* The TERRAIN_READBACK stages per arena family (k_tc_vert order): row 0 on
 * the shipped stage, row 1 on the TERRAIN_SECDELTA control. */
static const char *const k_tc_vert_rb[2][OPENPETE_MATERIAL_TER_VARIANTS] = {
    { "terrain_rb.vert.spv", "terrain_anim_rb.vert.spv", "terrain_near_rb.vert.spv",
      "terrain_cells_anim_rb.vert.spv", "terrain_cells_near_rb.vert.spv" },
    { "terrain_ctlrb.vert.spv", "terrain_anim_ctlrb.vert.spv",
      "terrain_near_ctlrb.vert.spv", "terrain_cells_anim_ctlrb.vert.spv",
      "terrain_cells_near_ctlrb.vert.spv" },
};
#define TC_RB_W      4096u      /* target width; terrain.vert RB_W */
#define TC_RB_HMAX   16384u     /* tallest target */
#define TC_RB_MAXENT (OPTC_ARENA_COUNT * TC_BUF_SLOTS)
#define TC_RB_KIND_CUT 3u       /* terrain.vert RB_CUT: a readback key's kind, a clip cut */

/* One drawn arena buffer of a recorded readback. */
typedef struct { uint32_t arena, buf, base, count, per; } tc_rb_entry;

/* One readback recording: its target and the arena buffers laid out in it. */
typedef struct {
    rhi_texture   *rt;
    uint32_t       h;           /* the target's height */
    int            armed;       /* recorded and not yet read */
    uint32_t       elems;       /* elements laid out */
    int            n;
    tc_rb_entry    ent[TC_RB_MAXENT];
} tc_rb_rec;

static struct {
    int            on;          /* -1 unread, else the latched switch */
    rhi_pipeline  *pipe[OPENPETE_MATERIAL_TER_VARIANTS];
    int            row;         /* k_tc_vert_rb row of the recording */
    tc_rb_rec      rec;         /* the OPENPETE_TERRAIN_READBACK recording */
} s_tc_rb = { .on = -1 };

/* The near-plane cut census's recording (openpete_tercache_nearcut_record):
 * the near-family arenas only. */
static tc_rb_rec s_tc_nc;

/** @brief Return the arena family (a TC_V_* value) of an arena.
 * @param arena Arena index.
 * @return Its family. */
static int tc_arena_family(int arena)
{
    if (OPTC_ARENA_IS_CELL_ANIM(arena)) return TC_V_CELLS_ANIM;
    if (OPTC_ARENA_IS_CELL(arena)) return TC_V_CELLS_NEAR;
    if (OPTC_ARENA_IS_NEAR(arena)) return TC_V_NEAR;
    return OPTC_ARENA_IS_ANIM(arena) ? TC_V_ANIM : TC_V_PLAIN;
}

/** @brief Return the readback pipeline of an arena family, created at its first use.
 *
 * The family's cache pipeline layout and vertex input, the TERRAIN_READBACK
 * stage of the recording's row, terrain_readback.frag, point
 * topology, an RGBA32_UINT colour target, no blend and no depth test.
 * @param fam The family, a TC_V_* value.
 * @param row The k_tc_vert_rb row.
 * @return The pipeline, or NULL when it failed to build. */
static rhi_pipeline *tc_rb_pipe(int fam, int row)
{
    if (s_tc_rb.pipe[fam]) return s_tc_rb.pipe[fam];
    const int anim = fam == TC_V_ANIM || fam == TC_V_CELLS_ANIM;
    const int near = fam == TC_V_NEAR || fam == TC_V_CELLS_NEAR;
    const rhi_pipeline_desc d = {
        .vert_spv = k_tc_vert_rb[row][fam], .frag_spv = "terrain_readback.frag.spv",
        .blend = RHI_BLEND_NONE,
        .terrain_pipeline = true, .terrain_anim = anim != 0, .terrain_near = near != 0,
        .terrain_cells = fam == TC_V_CELLS_ANIM || fam == TC_V_CELLS_NEAR,
        .pair_set7 = true, .tex_ssbo_set2 = true, .push_const_size = sizeof(tc_pc),
        .topology = RHI_TOPOLOGY_POINT_LIST, .color_format = RHI_COLOR_FORMAT_RGBA32_UINT,
    };
    s_tc_rb.pipe[fam] = rhi_create_pipeline(s_tc.dev, &d);
    return s_tc_rb.pipe[fam];
}

/** @brief Lay out the drawn arena buffers of the selected state in a recording's target.
 *
 * A buffer's elements are its high-water mark (records times cell vertices
 * per record on a cell arena), the range tc_draw_buf can draw from; each
 * buffer takes the next free element base. Fills the recording's ent, n and
 * elems.
 * @param r The recording.
 * @param near_only Nonzero to lay out the near-variant families alone (TC_V_NEAR, TC_V_CELLS_NEAR).
 * @return The pixels the layout needs (two per element). */
static uint64_t tc_rb_layout(tc_rb_rec *r, int near_only)
{
    r->n = 0;
    uint32_t base = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        const int fam = tc_arena_family(a);
        if (near_only && fam != TC_V_NEAR && fam != TC_V_CELLS_NEAR) continue;
        const int cells = OPTC_ARENA_IS_CELL(a);
        const uint32_t per = !cells ? 0u : OPTC_ARENA_IS_CELL_ANIM(a) ? RHI_TERRAIN_CELL_VERTS_ANIM
                                                                      : RHI_TERRAIN_CELL_VERTS_NEAR;
        for (int b = 0, nb = atomic_load_explicit(&s_tc.arena[a].n, memory_order_acquire);
             b < nb; b++) {
            const uint32_t high = s_tc.sel.mark_high[a][b];
            if (high == 0) continue;
            const uint32_t count = cells ? high * per : high;
            r->ent[r->n++] = (tc_rb_entry){ (uint32_t)a, (uint32_t)b, base, count, per };
            base += count;
        }
    }
    r->elems = base;
    return 2ull * base;
}

/** @brief Make a recording's target at least @p need pixels, TC_RB_W wide.
 * @param r The recording.
 * @param need Pixels the layout needs.
 * @return 1 when the target holds them, else 0. */
static int tc_rb_target(tc_rb_rec *r, uint64_t need)
{
    const uint64_t rows = (need + TC_RB_W - 1) / TC_RB_W;
    if (rows == 0 || rows > TC_RB_HMAX) return 0;
    if (r->rt && r->h >= rows) return 1;
    if (r->rt) rhi_destroy_texture_deferred(s_tc.dev, r->rt);
    r->h = (uint32_t)((rows + 255u) & ~255ull);
    if (r->h > TC_RB_HMAX) r->h = TC_RB_HMAX;
    r->rt = rhi_create_render_target_format(s_tc.dev, TC_RB_W, r->h, RHI_COLOR_FORMAT_RGBA32_UINT);
    return r->rt != NULL;
}

/** @brief Return whether the readback runs: OPENPETE_TERRAIN_READBACK set and not "0".
 * @return The k_tc_vert_rb row (the k_tc_vert row selected), or -1 when the readback is off. */
static int tc_rb_row(void)
{
    if (s_tc_rb.on < 0) {
        const char *e = getenv("OPENPETE_TERRAIN_READBACK");
        s_tc_rb.on = e && *e && *e != '0';
    }
    return s_tc_rb.on ? tc_stage_row() : -1;
}

/** @brief Draw one laid-out buffer twice, position pass then key pass.
 * @param e The buffer's layout entry.
 * @param clip The screen-to-clip mapping.
 * @param h The target's height. */
static void tc_rb_draw_entry(const tc_rb_entry *e, const openpete_tercache_clip *clip, uint32_t h)
{
    tc_pc pc;
    tc_pc_fill(&pc, 0, clip);
    pc.pad_[1] = (int32_t)e->base;
    pc.pad_[2] = (int32_t)(e->per | (h << 8));
    for (int mode = 0; mode < 2; mode++) {
        pc.pad_[0] = mode;
        tc_draw_buf((int)e->arena, (int)e->buf, &s_tc.arena[e->arena].buf[e->buf], &pc);
    }
}

/** @brief Destroy a recording's target.
 * @param dev Device.
 * @param r The recording. */
static void tc_rb_rec_free(rhi_device *dev, tc_rb_rec *r)
{
    if (r->rt) rhi_destroy_texture(dev, r->rt);
    memset(r, 0, sizeof *r);
}

/** @brief Destroy the readback targets and pipelines (cache shutdown).
 * @param dev Device. */
static void tc_rb_free(rhi_device *dev)
{
    tc_rb_rec_free(dev, &s_tc_rb.rec);
    tc_rb_rec_free(dev, &s_tc_nc);
    for (int i = 0; i < OPENPETE_MATERIAL_TER_VARIANTS; i++)
        if (s_tc_rb.pipe[i]) rhi_destroy_pipeline(dev, s_tc_rb.pipe[i]);
    memset(&s_tc_rb, 0, sizeof s_tc_rb);
    s_tc_rb.on = -1;
}

/** @brief Lay out and draw a recording of the selected retained state into its target, and arm it.
 *
 * Inside a frame, outside any render pass.
 * @param r The recording.
 * @param near_only Nonzero for the near-variant families alone.
 * @param row The k_tc_vert_rb row.
 * @param clip The screen-to-clip mapping the positions are computed under.
 * @param vram VRAM texture, or NULL to leave the binding.
 * @return 1 when recorded, 0 when the layout exceeds the tallest target. */
static int tc_rb_record(tc_rb_rec *r, int near_only, int row, const openpete_tercache_clip *clip,
                        rhi_texture *vram)
{
    if (!tc_rb_target(r, tc_rb_layout(r, near_only))) return 0;
    tc_ubo_flush_frame();
    rhi_begin_render_target(s_tc.dev, r->rt, 0.0f, 0.0f, 0.0f, 0.0f);
    s_tc_rb_drawing = 1;
    const int mat_was = s_tc_mat_pass;
    s_tc_mat_pass = -1;
    int bound = -1;
    for (int i = 0; i < r->n; i++) {
        const tc_rb_entry *e = &r->ent[i];
        const int fam = tc_arena_family((int)e->arena);
        rhi_pipeline *p = tc_rb_pipe(fam, row);
        if (!p) continue;
        if (fam != bound) {
            rhi_bind_pipeline(s_tc.dev, p);
            if (tc_bind_draw_inputs(vram, NULL) != 0) continue;
            bound = fam;
        }
        tc_rb_draw_entry(e, clip, r->h);
    }
    s_tc_mat_pass = mat_was;
    s_tc_rb_drawing = 0;
    rhi_end_render_target(s_tc.dev, r->rt);
    r->armed = 1;
    return 1;
}

/** @brief Read a recording's target back into a malloc'd buffer and disarm it.
 * @param r The recording.
 * @return The texels (four words each, row-major, TC_RB_W wide), owned by the caller, or NULL. */
static uint32_t *tc_rb_read(tc_rb_rec *r)
{
    if (!r->armed || !r->rt) return NULL;
    r->armed = 0;
    const size_t bytes = (size_t)TC_RB_W * r->h * 16u;
    uint32_t *tex = malloc(bytes);
    int w = 0, h = 0;
    if (!tex || !rhi_read_render_target_texels(s_tc.dev, r->rt, tex, bytes, &w, &h)) {
        free(tex);
        return NULL;
    }
    return tex;
}

int openpete_tercache_readback_record(const openpete_tercache_clip *clip, rhi_texture *vram)
{
    const int row = tc_rb_row();
    if (row < 0 || !s_tc.ready || !s_tc.draw_open || !s_tc.ubo || !clip) return 0;
    if (!tc_rb_record(&s_tc_rb.rec, 0, row, clip, vram)) {
        fprintf(stderr, "[tc-readback] %u elements exceed the %ux%u target\n",
                s_tc_rb.rec.elems, TC_RB_W, TC_RB_HMAX);
        return 0;
    }
    s_tc_rb.row = row + 1;
    fprintf(stderr, "[tc-readback] recorded buffers=%d elements=%u target=%ux%u row=%d\n",
            s_tc_rb.rec.n, s_tc_rb.rec.elems, TC_RB_W, s_tc_rb.rec.h, s_tc_rb.row);
    return 1;
}

int openpete_tercache_readback_write(const char *path)
{
    tc_rb_rec *r = &s_tc_rb.rec;
    if (!r->armed || !r->rt || !path) return 1;
    uint32_t *tex = tc_rb_read(r);
    if (!tex) {
        fprintf(stderr, "[tc-readback] read of the target failed\n");
        return -1;
    }
    FILE *f = fopen(path, "wb");
    const uint32_t hdr[6] = { 0x42524354u /* "TCRB" */, 1u, TC_RB_W, r->h,
                              (uint32_t)r->n, (uint32_t)s_tc_rb.row };
    const size_t words = (size_t)r->elems * 2u * 4u;
    int ok = f && fwrite(hdr, sizeof hdr, 1, f) == 1 &&
             fwrite(r->ent, sizeof(tc_rb_entry), (size_t)r->n, f) == (size_t)r->n &&
             fwrite(tex, sizeof(uint32_t), words, f) == words;
    if (f && fclose(f) != 0) ok = 0;
    free(tex);
    fprintf(stderr, "[tc-readback] %s %s elements=%u\n", ok ? "wrote" : "FAILED writing",
            path, r->elems);
    return ok ? 0 : -1;
}

int openpete_tercache_nearcut_record(const openpete_tercache_clip *clip, rhi_texture *vram)
{
    s_tc_nc.armed = 0;
    if (!s_tc.ready || !s_tc.draw_open || !s_tc.ubo || !clip) return 0;
    return tc_rb_record(&s_tc_nc, 1, tc_stage_row(), clip, vram);
}

int openpete_tercache_nearcut_read(uint32_t *cut, uint32_t *cut_view)
{
    *cut = *cut_view = 0;
    uint32_t *tex = tc_rb_read(&s_tc_nc);
    if (!tex) return -1;
    for (int i = 0; i < s_tc_nc.n; i++) {
        const tc_rb_entry *e = &s_tc_nc.ent[i];
        for (uint32_t t = 0; t + 3u <= e->count; t += 3u) {
            int live = 0, cutk = 0;
            float x[3], y[3];
            for (int k = 0; k < 3; k++) {
                const uint32_t *pos = tex + 8u * (size_t)(e->base + t + (uint32_t)k);
                const uint32_t *key = pos + 4;
                float v[4];
                memcpy(v, pos, sizeof v);
                if (pos[3] == 0u) continue;            /* killed: the stage wrote nothing */
                live++;
                x[k] = v[0] / v[3];
                y[k] = v[1] / v[3];
                cutk |= (key[3] & 0xFFu) == TC_RB_KIND_CUT;
            }
            if (live == 3 && cutk) {
                (*cut)++;
                *cut_view += (uint32_t)tc_tri_in_view(x, y);
            }
        }
    }
    free(tex);
    return 0;
}

void openpete_tercache_get_stats(openpete_tercache_stats_t *out)
{
    if (!out) return;
    if (!s_tc.ready && s_tc_final_valid) { *out = s_tc_final; return; }
    memset(out, 0, sizeof(*out));
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        const tc_arena *ar = &s_tc.arena[a];
        for (int b = 0, n = atomic_load_explicit(&ar->n, memory_order_relaxed); b < n; b++) {
            if (!atomic_load_explicit(&ar->buf[b].pair, memory_order_relaxed)) continue;
            out->chunks     += ar->buf[b].chunks;
            out->verts_used += ar->buf[b].used;
            out->verts_cap  += ar->buf[b].cap;
            out->high[a]    += ar->buf[b].high;
            out->cap[a]     += ar->buf[b].cap;
            out->cap_bytes  += tc_pair_bytes(a, ar->buf[b].cap);
        }
        out->alloc_fail[a]       = s_tc.alloc_fail[a];
        out->overflow_adopted[a] = s_tc.overflow_adopted[a];
    }
    out->draws_last_present = s_tc.draws_prev;
    out->frees_pending = (uint32_t)s_tc.defer_n;
    out->svc_calls     = atomic_load_explicit(&s_tc.svc_calls, memory_order_relaxed);
    out->svc_fulfilled = atomic_load_explicit(&s_tc.svc_fulfilled, memory_order_relaxed);
    out->draws_retired = atomic_load_explicit(&s_tc.draws_retired, memory_order_relaxed);
    out->cell_draws     = s_tc.cell_draws;
    out->cell_instances = s_tc.cell_instances;
    out->prev_high      = s_tc.prev_alloc.high;
    out->prev_cap       = s_tc.prev_alloc.cap;
    out->prev_fail      = s_tc.prev_fail;
    out->prev_defer_ovf = s_tc.prev_defer_ovf;
    out->bakes          = s_tc.bakes;
    out->window_bakes   = s_tc.window_bakes;
    memcpy(out->claims, s_tc.claims, sizeof out->claims);
    out->edge_claims    = s_tc.edge_claims;
    out->grow_claims    = s_tc.grow_claims;
    out->retire_claims  = s_tc.retire_claims;
    out->prev_grow_claims = s_tc.prev_grow_claims;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) out->grow += s_tc.overflow_adopted[a];
    out->failed_edges   = s_tc.failed_edges;
    out->consolidations = s_tc.consolidations;
    out->prev_grow      = s_tc.prev_grow;
    out->sealed_draws   = s_tc.sealed_draws;
    out->sealed_allocs  = s_tc.sealed_allocs;
    out->pinned_bytes   = s_tc.pinned_bytes;
    out->migrate_ticks  = s_tc.migrate_ticks;
    out->defer_overflow = s_tc.defer_overflow;
    out->hold_takes     = s_tc.hold_takes;
    out->hold_misses    = s_tc.hold_misses;
    out->hold_drops     = s_tc.hold_drops;
    out->nonlocal_bufs  = atomic_load_explicit(&s_tc.nonlocal_bufs, memory_order_relaxed);
    out->dropped_spans  = s_tc.dropped_spans;
    memcpy(out->bufs_max, s_tc.bufs_max, sizeof out->bufs_max);
    out->ranges_max     = s_tc.ranges_max;
    out->cen_nolist     = s_tc.cen_nolist;
    out->store_bind_refusals = atomic_load_explicit(&s_tc.bind_refusals, memory_order_relaxed);
    out->retire_answers     = atomic_load_explicit(&s_tc.retire_answers, memory_order_relaxed);
    out->retire_old_touches = atomic_load_explicit(&s_tc.retire_old_touches, memory_order_relaxed);
    out->exhaust_warnings   = s_tc.exhaust_warnings;
    out->window_skips       = s_tc.window_skips;
    out->sig_changes        = s_tc.sig_changes;
    out->stale_bytes        = s_tc.stale_bytes;
    out->budget_claims      = s_tc.budget_claims;
    out->budget_episodes    = s_tc.budget_episodes;
    out->budget_walks_max   = s_tc.budget_walks_max;
    out->budget_open        = s_tc.budget_open;
    out->budget_age_max     = s_tc.budget_age_max;
    out->budget_starved     = s_tc.budget_starved;
}

int openpete_tercache_take_bind_refusal(void)
{
    const uint32_t r = atomic_load_explicit(&s_tc.bind_refusals, memory_order_relaxed);
    if (r == s_tc.bind_refusals_seen) return 0;
    s_tc.bind_refusals_seen = r;
    return 1;
}

int openpete_tercache_buf_source(int arena, int buf)
{
    if (!s_tc.ready || arena < 0 || arena >= OPTC_ARENA_COUNT || buf < 0 || buf >= TC_BUF_SLOTS)
        return 0;
    return s_tc.arena[arena].buf[buf].source;
}

int openpete_tercache_migrating(void)
{
    if (!s_tc.ready) return 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        if (s_tc.arena[a].cons_live) return 1;
    return 0;
}

void openpete_tercache_migrate_tick(void)
{
    if (!s_tc.ready) return;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++)
        if (s_tc.arena[a].cons_live) s_tc.arena[a].cons_ticks++;
}

void openpete_tercache_note_bake(void)
{
    s_tc.bakes++;
    if (s_tc.rz_window) s_tc.window_bakes++;
}

/** @brief Format one census array as a comma-separated per-arena column.
 * @param out Receives the NUL-terminated column.
 * @param cap Capacity of @p out in bytes.
 * @param v The per-arena values.
 * @return The values' sum. */
static uint64_t tc_fmt_col(char *out, size_t cap, const uint32_t *v)
{
    size_t at = 0;
    uint64_t sum = 0;
    out[0] = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        sum += v[a];
        const int n = snprintf(out + at, cap - at, "%s%u", a ? "," : "", v[a]);
        if (n < 0 || (size_t)n >= cap - at) break;
        at += (size_t)n;
    }
    return sum;
}

void openpete_tercache_print_exit_row(void)
{
    openpete_tercache_stats_t st;
    openpete_tercache_get_stats(&st);
    char cl[256], bm[128];
    const uint64_t csum = tc_fmt_col(cl, sizeof cl, st.claims);
    (void)tc_fmt_col(bm, sizeof bm, st.bufs_max);
    fprintf(stderr, "tercache: bakes=%u grow=%u consolidations=%u failed_edges=%u prev_grow=%u "
            "prev_fail=%u claims_sum=%llu claims=%s edge_claims=%u grow_claims=%u "
            "retire_claims=%u prev_grow_claims=%u window_bakes=%u defer_overflow=%u "
            "hold_takes=%u hold_misses=%u hold_drops=%u sealed_draws=%u sealed_allocs=%u "
            "svc_fulfilled=%u draws_retired=%u pinned_bytes=%llu nonlocal_bufs=%u "
            "dropped_spans=%u migrate_ticks=%u stale_bytes=%llu bufs=%s ranges_max=%u/%d "
            "nolist=%u store_bind_refusals=%u retire_answers=%u retire_old_touches=%u "
            "exhaust_warnings=%u window_skips=%u sig_changes=%u budget_claims=%u "
            "budget_episodes=%u budget_walks_max=%u budget_open=%u budget_age_max=%u "
            "budget_starved=%u\n",
            st.bakes, st.grow, st.consolidations, st.failed_edges, st.prev_grow, st.prev_fail,
            (unsigned long long)csum, cl, st.edge_claims, st.grow_claims, st.retire_claims,
            st.prev_grow_claims, st.window_bakes, st.defer_overflow, st.hold_takes,
            st.hold_misses, st.hold_drops,
            st.sealed_draws, st.sealed_allocs, st.svc_fulfilled, st.draws_retired,
            (unsigned long long)st.pinned_bytes, st.nonlocal_bufs, st.dropped_spans,
            st.migrate_ticks, (unsigned long long)st.stale_bytes, bm, st.ranges_max,
            TC_RANGE_MAX, st.cen_nolist, st.store_bind_refusals, st.retire_answers,
            st.retire_old_touches, st.exhaust_warnings, st.window_skips, st.sig_changes,
            st.budget_claims, st.budget_episodes, st.budget_walks_max, st.budget_open,
            st.budget_age_max, st.budget_starved);
    fprintf(stderr, "tercache-ring: ring_writes=%llu ring_grow=%llu ring_fail=%llu "
            "ring_rebakes=%llu ring_units_max=%llu ring_depth_max=%llu store_cap_max=%u "
            "store_high_max=%u store_mib_max=%.2f\n",
            (unsigned long long)s_ring_cnt[OPTC_RING_NOTE_WRITE],
            (unsigned long long)s_ring_cnt[OPTC_RING_NOTE_GROW],
            (unsigned long long)s_ring_cnt[OPTC_RING_NOTE_FAIL],
            (unsigned long long)s_ring_cnt[OPTC_RING_NOTE_REBAKE],
            (unsigned long long)s_ring_cnt[OPTC_RING_NOTE_UNITS],
            (unsigned long long)s_ring_cnt[OPTC_RING_NOTE_DEPTH], s_ring_store_cap,
            s_ring_store_high,
            (double)s_ring_store_cap * OPTC_PREV_REC_WORDS * 4.0 / (1024.0 * 1024.0));
}

/* ---- one-image preview (openpete_tercache_preview_*) ---------------------- */

/* The preview's device objects: its own pipelines, target, VRAM image, a 1x1
 * stand-in for the set-2 and set-4 images, a one-record store for set 2,
 * and per batch a block set and the arenas' vertex buffers. */
struct openpete_tercache_preview_gpu {
    rhi_device *dev;
    rhi_pipeline *pipe[OPTC_PREVIEW_PIPES];
    rhi_texture *rt, *vram, *dummy;
    rhi_buffer *store;
    int nbatch;
    struct tc_preview_batch_gpu {
        rhi_ubo *ubo;
        rhi_buffer *vb[OPTC_PREVIEW_ARENAS];
        int n[OPTC_PREVIEW_ARENAS];
        rhi_buffer *nvb[OPTC_PREVIEW_ARENAS], *cxb[OPTC_PREVIEW_ARENAS];
        int nn[OPTC_PREVIEW_ARENAS];
        rhi_buffer *cb[2][OPTC_PREVIEW_CELL_CLASSES];   /* cell records, instance rate */
        int nc[2][OPTC_PREVIEW_CELL_CLASSES];
    } *b;
    openpete_tercache_clip clip;
    uint32_t w, h;
    float bg[3];
};

void openpete_tercache_preview_free(openpete_tercache_preview *p)
{
    if (!p) return;
    for (int i = 0; p->batch && i < p->nbatch; i++)
        for (int a = 0; a < OPTC_PREVIEW_ARENAS; a++) {
            free(p->batch[i].v[a]);
            free(p->batch[i].nv[a]);
            free(p->batch[i].cx[a]);
        }
    for (int i = 0; p->batch && i < p->nbatch; i++)
        for (int f = 0; f < 2; f++)
            for (int c = 0; c < OPTC_PREVIEW_CELL_CLASSES; c++) free(p->batch[i].crec[f][c]);
    free(p->batch);
    free(p->vram);
    memset(p, 0, sizeof(*p));
}

/** @brief Fill the preview's pipeline descriptions: four stages of four draw classes, in openpete_tercache_preview_jobs order.
 *
 * The stages are plain, near-variant, anim-layout cells and near cells; the
 * classes opaque, alpha, additive and subtractive, the blend classes with
 * depth writes off: the states tc_make_pipes and tc_make_cell_pipes give
 * them, with the cache's selected vertex stage.
 * @param d Receives OPTC_PREVIEW_PIPES descriptions. */
static void tc_preview_descs(rhi_pipeline_desc d[OPTC_PREVIEW_PIPES])
{
    static const struct { int v; bool near, cells, anim; } k_stage[4] = {
        { TC_V_PLAIN, false, false, false }, { TC_V_NEAR, true, false, false },
        { TC_V_CELLS_ANIM, false, true, true }, { TC_V_CELLS_NEAR, true, true, false },
    };
    static const rhi_blend_mode k_blend[4] = { RHI_BLEND_NONE, RHI_BLEND_ALPHA, RHI_BLEND_ADD,
                                               RHI_BLEND_SUBTRACT };
    for (int i = 0; i < OPTC_PREVIEW_PIPES; i++) {
        const int st = i / 4, c = i % 4;
        d[i] = (rhi_pipeline_desc){
            .vert_spv = tc_vert(k_stage[st].v), .vert_spv_whole = tc_vert_whole(k_stage[st].v),
            .frag_spv = "terrain.frag.spv",
            .blend = k_blend[c], .depth_write_off = c != 0, .depth_test = true,
            .terrain_pipeline = true, .terrain_near = k_stage[st].near,
            .terrain_cells = k_stage[st].cells, .terrain_anim = k_stage[st].anim,
            .pair_set7 = true, .tex_ssbo_set2 = true, .push_const_size = sizeof(tc_pc),
        };
    }
}

int openpete_tercache_preview_jobs(rhi_device *dev, int slot_contest, void *jobs[OPTC_PREVIEW_PIPES])
{
    for (int i = 0; i < OPTC_PREVIEW_PIPES; i++) jobs[i] = NULL;
    if (!dev) return -1;
    rhi_pipeline_desc d[OPTC_PREVIEW_PIPES];
    tc_preview_descs(d);
    const rhi_pipe_variant var = rhi_pipe_variant_current();
    const uint32_t epoch = rhi_pipe_variant_epoch();
    for (int i = 0; i < OPTC_PREVIEW_PIPES; i++) {
        rhi_compile_job *j = rhi_compile_job_new(dev, &d[i], NULL, 0, NULL, 0, var, 0, epoch);
        if (!j) {
            for (int k = 0; k < i; k++) rhi_compile_job_discard(dev, (rhi_compile_job *)jobs[k]);
            for (int k = 0; k < OPTC_PREVIEW_PIPES; k++) jobs[k] = NULL;
            return -1;
        }
        rhi_compile_job_set_slot_contest(j, slot_contest);
        jobs[i] = j;
    }
    return 0;
}

/** @brief Destroy a batch's block set and buffers; a NULL member is skipped.
 *
 * @param dev The device.
 * @param b The batch. */
static void tc_preview_batch_free(rhi_device *dev, struct tc_preview_batch_gpu *b)
{
    if (b->ubo) rhi_destroy_ubo(dev, b->ubo);
    rhi_buffer *const *bufs[] = { b->vb, b->nvb, b->cxb };
    for (size_t k = 0; k < sizeof bufs / sizeof bufs[0]; k++)
        for (int a = 0; a < OPTC_PREVIEW_ARENAS; a++)
            if (bufs[k][a]) rhi_destroy_buffer(dev, bufs[k][a]);
    for (int f = 0; f < 2; f++)
        for (int c = 0; c < OPTC_PREVIEW_CELL_CLASSES; c++)
            if (b->cb[f][c]) rhi_destroy_buffer(dev, b->cb[f][c]);
}

/** @brief Destroy a preview's device objects and free the record; NULL is ignored.
 *
 * The caller has waited for the device, or nothing was recorded.
 * @param g The objects. */
static void tc_preview_gpu_free(openpete_tercache_preview_gpu *g)
{
    if (!g) return;
    rhi_device *dev = g->dev;
    for (int i = 0; g->b && i < g->nbatch; i++) tc_preview_batch_free(dev, &g->b[i]);
    free(g->b);
    for (int i = 0; i < OPTC_PREVIEW_PIPES; i++) rhi_retire_pipeline(dev, g->pipe[i]);
    if (g->store) rhi_destroy_buffer(dev, g->store);
    if (g->rt) rhi_destroy_texture(dev, g->rt);
    if (g->vram) rhi_destroy_texture(dev, g->vram);
    if (g->dummy) rhi_destroy_texture(dev, g->dummy);
    free(g);
}

/** @brief Create and fill one batch's cell-record buffers, one per family and draw class that holds records.
 *
 * @param dev The device.
 * @param src The batch.
 * @param dst Receives the buffers and counts.
 * @return 0, or -1 when a buffer could not be made. */
static int tc_preview_cells_make(rhi_device *dev, const openpete_tercache_preview_batch *src,
                                 struct tc_preview_batch_gpu *dst)
{
    for (int f = 0; f < 2; f++)
        for (int c = 0; c < OPTC_PREVIEW_CELL_CLASSES; c++) {
            dst->nc[f][c] = src->nc[f][c];
            if (src->nc[f][c] <= 0) continue;
            const size_t rsz = f ? sizeof(rhi_terrain_cell) : sizeof(rhi_terrain_cell_anim);
            const size_t bytes = (size_t)src->nc[f][c] * rsz;
            dst->cb[f][c] = rhi_create_vertex_buffer(dev, bytes);
            if (!dst->cb[f][c]) return -1;
            rhi_update_buffer(dst->cb[f][c], src->crec[f][c], bytes);
        }
    return 0;
}

/** @brief Create one batch's block set and vertex buffers and fill them: the globals block, the sector block with the batch's origins and flags, and each arena's chunks.
 *
 * @param dev The device.
 * @param gu The packed globals block.
 * @param src The batch.
 * @param dst Receives the objects.
 * @return 0, or -1 when an object could not be made. */
static int tc_preview_batch_make(rhi_device *dev, const tc_globals_ubo *gu,
                                 const openpete_tercache_preview_batch *src,
                                 struct tc_preview_batch_gpu *dst)
{
    dst->ubo = rhi_create_ubo(dev, (uint32_t)TC_UBO_BLOCK, 2);
    if (!dst->ubo) return -1;
    static tc_sector_ubo sec[OPTC_SLOTS];   /* display owner only */
    memset(sec, 0, sizeof(sec));
    for (int i = 0; i < src->nsec && i < OPTC_PREVIEW_SLOTS; i++) {
        union { float f; uint32_t u; } w;
        w.u = src->flags[i];
        sec[i].o[0] = src->o[i][0];
        sec[i].o[1] = src->o[i][1];
        sec[i].o[2] = src->o[i][2];
        sec[i].o[3] = w.f;
        w.u = OPTC_PREV_REC_NONE;   /* no colour table */
        sec[i].gp[3] = w.f;
    }
    rhi_ubo_write(dst->ubo, 0, gu, sizeof(*gu));
    rhi_ubo_write(dst->ubo, 1, sec, sizeof(sec));
    for (int a = 0; a < OPTC_PREVIEW_ARENAS; a++) {
        dst->n[a] = src->n[a];
        if (src->n[a] <= 0) continue;
        const size_t bytes = (size_t)src->n[a] * sizeof(rhi_terrain_vertex);
        dst->vb[a] = rhi_create_vertex_buffer(dev, bytes);
        if (!dst->vb[a]) return -1;
        rhi_update_buffer(dst->vb[a], src->v[a], bytes);
    }
    for (int a = 0; a < OPTC_PREVIEW_ARENAS; a++) {
        dst->nn[a] = src->nn[a];
        if (src->nn[a] <= 0) continue;
        const size_t vb = (size_t)src->nn[a] * sizeof(rhi_terrain_vertex);
        const size_t cb = (size_t)src->nn[a] * sizeof(rhi_terrain_nearctx);
        dst->nvb[a] = rhi_create_vertex_buffer(dev, vb);
        dst->cxb[a] = rhi_create_vertex_buffer(dev, cb);
        if (!dst->nvb[a] || !dst->cxb[a]) return -1;
        rhi_update_buffer(dst->nvb[a], src->nv[a], vb);
        rhi_update_buffer(dst->cxb[a], src->cx[a], cb);
    }
    return tc_preview_cells_make(dev, src, dst);
}

openpete_tercache_preview_gpu *openpete_tercache_preview_prepare(rhi_device *dev,
                                                                 const openpete_tercache_preview *p,
                                                                 void *jobs[OPTC_PREVIEW_PIPES])
{
    openpete_tercache_preview_gpu *g = (openpete_tercache_preview_gpu *)calloc(1, sizeof(*g));
    int ok = g != NULL && dev != NULL && p != NULL;
    for (int i = 0; i < OPTC_PREVIEW_PIPES; i++) {
        rhi_compile_job *j = (rhi_compile_job *)jobs[i];
        jobs[i] = NULL;
        if (!ok || !j) { if (j) rhi_compile_job_discard(dev, j); ok = 0; continue; }
        g->pipe[i] = rhi_compile_job_install(dev, j);
        ok &= g->pipe[i] != NULL;
    }
    if (!g) return NULL;
    g->dev = dev;
    if (!ok) { tc_preview_gpu_free(g); return NULL; }
    g->clip = p->clip;
    g->w = p->w; g->h = p->h;
    memcpy(g->bg, p->bg, sizeof(g->bg));
    g->rt = rhi_create_render_target(dev, p->w, p->h);
    g->vram = rhi_create_vram(dev, 1024, 512);
    g->dummy = rhi_create_rgba8(dev, 1, 1);
    g->store = rhi_create_storage_buffer_device(dev, OPTC_PREV_REC_WORDS * sizeof(uint32_t));
    if (!g->rt || !g->vram || !g->dummy || !g->store) { tc_preview_gpu_free(g); return NULL; }
    rhi_update_vram(dev, g->vram, p->vram);
    const uint32_t px = 0;
    rhi_update_rgba8(dev, g->dummy, &px);
    static tc_globals_ubo gu;   /* display owner only */
    tc_globals_pack(&gu, &p->g, 0);
    g->b = (struct tc_preview_batch_gpu *)calloc((size_t)(p->nbatch > 0 ? p->nbatch : 1), sizeof(*g->b));
    if (!g->b) { tc_preview_gpu_free(g); return NULL; }
    g->nbatch = p->nbatch;
    for (int i = 0; i < p->nbatch; i++)
        if (tc_preview_batch_make(dev, &gu, &p->batch[i], &g->b[i]) != 0) {
            tc_preview_gpu_free(g);
            return NULL;
        }
    return g;
}

/** @brief Bind a pipeline and one batch's VRAM, storage, blocks and push constants for a preview draw.
 *
 * @param g The objects.
 * @param pipe The pipeline index.
 * @param i The batch.
 * @param pc The push constants.
 * @return 0, or -1 when the storage binding failed. */
static int tc_preview_bind(openpete_tercache_preview_gpu *g, int pipe, int i, const tc_pc *pc)
{
    rhi_bind_pipeline(g->dev, g->pipe[pipe]);
    rhi_bind_vram(g->dev, g->vram);
    if (rhi_bind_texture_storage(g->dev, g->dummy, g->store,
                                 (uint32_t)(OPTC_PREV_REC_WORDS * sizeof(uint32_t)), 2, 0) != 0)
        return -1;
    rhi_bind_texture_set(g->dev, g->dummy, 4);
    rhi_ubo_bind_at(g->dev, g->b[i].ubo, 5, 0);
    rhi_ubo_bind_at(g->dev, g->b[i].ubo, 6, 1);
    tc_push(g->dev, pc);
    return 0;
}

/** @brief Draw one cell family's records of one draw class in every batch, instanced: one instance per record, the stage's cell vertices per instance.
 *
 * @param g The objects.
 * @param fam 0 for the anim-layout records (pipelines 8-11), 1 for the near records (12-15).
 * @param c The draw class, OPTC_ARENA_OPAQUE through OPTC_ARENA_SUB.
 * @param pc The push constants.
 * @return The vertices drawn. */
static uint64_t tc_preview_draw_cells(openpete_tercache_preview_gpu *g, int fam, int c, const tc_pc *pc)
{
    const uint32_t per = fam ? RHI_TERRAIN_CELL_VERTS_NEAR : RHI_TERRAIN_CELL_VERTS_ANIM;
    uint64_t n = 0;
    for (int i = 0; i < g->nbatch; i++) {
        const int cnt = g->b[i].nc[fam][c];
        if (cnt <= 0 || tc_preview_bind(g, 8 + 4 * fam + c, i, pc) != 0) continue;
        rhi_bind_vertex_buffer(g->dev, g->b[i].cb[fam][c]);
        rhi_draw_instanced(g->dev, per, (uint32_t)cnt, 0, 0);
        n += (uint64_t)cnt * per;
    }
    return n;
}

/** @brief Draw one arena of every batch with one pipeline: the plain chunks, or with @p near the near-variant chunks and their member contexts.
 *
 * @param g The objects.
 * @param pipe The pipeline index.
 * @param a The arena, OPTC_ARENA_OPAQUE through OPTC_ARENA_LP.
 * @param near 1 for the near-variant chunks.
 * @param pc The push constants.
 * @return The vertices drawn. */
static uint64_t tc_preview_draw_arenas(openpete_tercache_preview_gpu *g, int pipe, int a, int near,
                                       const tc_pc *pc)
{
    uint64_t n = 0;
    for (int i = 0; i < g->nbatch; i++) {
        const int cnt = near ? g->b[i].nn[a] : g->b[i].n[a];
        if (cnt <= 0 || tc_preview_bind(g, pipe, i, pc) != 0) continue;
        if (near) rhi_bind_vertex_buffer2(g->dev, g->b[i].nvb[a], g->b[i].cxb[a]);
        else      rhi_bind_vertex_buffer(g->dev, g->b[i].vb[a]);
        rhi_draw(g->dev, (uint32_t)cnt, 0);
        n += (uint64_t)cnt;
    }
    return n;
}

uint64_t openpete_tercache_preview_record(openpete_tercache_preview_gpu *g)
{
    if (!g) return 0;
    tc_pc pc;
    memset(&pc, 0, sizeof(pc));
    pc.x_base = g->clip.x_base; pc.x_span = g->clip.x_span;
    pc.y_base = g->clip.y_base; pc.y_span = g->clip.y_span;
    pc.disp_w = g->clip.disp_w; pc.clip_h = g->clip.clip_h;
    pc.band = g->clip.band; pc.freeze_wf = g->clip.freeze_wf;
    pc.full_extent = g->clip.full_extent;
    pc.mat_pass = -1;   /* every face: no material pass */
    rhi_begin_render_target(g->dev, g->rt, g->bg[0], g->bg[1], g->bg[2], 0.0f);
    /* Opaque and LP first, then each blend class; per pass the plain
     * stage (pipelines 0-3), the near-variant stage (4-7), then the cell
     * records (8-15), the cache's own order within a class. */
    static const int k_pass[4][2] = { { 0, OPTC_ARENA_OPAQUE }, { 1, OPTC_ARENA_ALPHA },
                                      { 2, OPTC_ARENA_ADD }, { 3, OPTC_ARENA_SUB } };
    uint64_t n = 0;
    for (int near = 0; near < 2; near++) {
        n += tc_preview_draw_arenas(g, 4 * near, OPTC_ARENA_OPAQUE, near, &pc);
        n += tc_preview_draw_arenas(g, 4 * near, OPTC_ARENA_LP, near, &pc);
    }
    for (int fam = 0; fam < 2; fam++) n += tc_preview_draw_cells(g, fam, OPTC_ARENA_OPAQUE, &pc);
    for (int k = 1; k < 4; k++) {
        for (int near = 0; near < 2; near++)
            n += tc_preview_draw_arenas(g, k_pass[k][0] + 4 * near, k_pass[k][1], near, &pc);
        for (int fam = 0; fam < 2; fam++) n += tc_preview_draw_cells(g, fam, k_pass[k][1], &pc);
    }
    rhi_end_render_target(g->dev, g->rt);
    return n;
}

int openpete_tercache_preview_finish(openpete_tercache_preview_gpu *g, uint8_t *rgba, size_t cap)
{
    if (!g) return -1;
    int rc = 0;
    if (rgba) {
        int w = 0, h = 0;
        rc = rhi_read_render_target_raw(g->dev, g->rt, rgba, cap, &w, &h) && (uint32_t)w == g->w &&
                     (uint32_t)h == g->h ? 0 : -1;
    } else {
        rhi_wait_idle(g->dev);
    }
    tc_preview_gpu_free(g);
    return rc;
}
