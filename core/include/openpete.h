// core/include/openpete.h - runtime entry point + program options.
//
// The game entry (games/common/game_main.cpp) fills in `openpete_options_t`
// and calls `openpete_run()`. All command-line parsing, window setup,
// screenshot logic, and the run loop live in core/.

#ifndef OPENPETE_H
#define OPENPETE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "openpete_perf_stats.h"   /* OPENPETE_PERF_* levels for opts.perf_stats */
#include "openpete_overlay_anchor.h" /* openpete_anchor_t for opts.perf_position */

#ifdef __cplusplus
extern "C" {
#endif

/* Exit codes. */
#define OPENPETE_EXIT_OK           0  /* clean termination */
#define OPENPETE_EXIT_PANIC        1  /* panic / crash / timeout */
#define OPENPETE_EXIT_DIVERGENCE   2  /* state-divergence detected */
#define OPENPETE_EXIT_SETUP_ERROR  3  /* disc not found, unknown flag, etc. */

/* Highest accepted --ssaa factor. The supersampled extent drives FIVE
 * device-local render targets - the scene target plus the four key-contest
 * targets (terrain/moby/actor key depth + the moby rank RT) - and each carries
 * a colour image AND a depth image, ~60 bytes per supersampled pixel on a
 * D32_SFLOAT_S8_UINT device. Since the factor is PER AXIS, the footprint grows
 * as N²: at 1080p, 4× is ~2.0 GB but 8× is ~8.0 GB, which exceeds an 8 GB card
 * outright. maxImageDimension2D alone is no guard (32768 permits 16× at 1080p,
 * ~32 GB), and drivers that oversubscribe device-local memory turn the overrun
 * into multi-second passes and a lost device rather than a clean allocation
 * failure the render-target step-down could catch. */
#define OPENPETE_SSAA_MAX 4

/* Log format. */
typedef enum {
    OPENPETE_LOG_PLAIN = 0,
    OPENPETE_LOG_JSON  = 1,
} openpete_log_format_t;

/* Active renderer / display mode (--renderer). VK (the native Vulkan renderer)
 * is the DEFAULT visible display; the PsyCross GL window is started hidden and,
 * in pure-vk mode, does NOT rasterize the ordering table at all (the native
 * renderer draws everything from gamestate). PSYCROSS selects the legacy GL
 * path as the sole visible window. AB/DIFF bring up the native window AND keep
 * PsyCross rendering (to its hidden FBO) so the native output can be compared
 * against it:
 *   VK   - native renderer is the sole, full-window display (default).
 *   PSYCROSS - legacy: PsyCross GL drives the single visible window.
 *   AB   - native | PsyCross side-by-side, for visual comparison.
 *   DIFF - native renderer live full-window; --screenshot-on-frame writes a
 *          per-pixel colour diff of native vs the PsyCross reference.
 * All native modes are pure display consumers - they never perturb gameplay
 * state. Native modes are ignored under --headless (PsyCross renders instead),
 * so the oracle / determinism gates (which run headless) are unaffected by the
 * default. */
typedef enum {
    /* PsyCross GL rasterizes; the native Vulkan window DISPLAYS its frame
     * (read back per present). No native scene is extracted. */
    OPENPETE_RENDERER_PSYCROSS = 0,
    /* Default: the native Vulkan renderer both rasterizes and displays. */
    OPENPETE_RENDERER_VK,
} openpete_renderer_t;

/* Vertex screen-coordinate precision (`--vertex-precision`). The native renderer
 * projects world geometry to sub-pixel FLOAT screen coordinates for smooth motion
 * (the PGXP-equivalent default). INTEGER snaps each 3D-world vertex to the PSX
 * 512x240 integer pixel grid before rasterization, reproducing the original
 * hardware's characteristic polygon "wobble"/jitter (the GTE stored screen X/Y as
 * int16). Display-only: it rounds only the emitted display geometry - never
 * gameplay-readable RAM - and only the 3D-world layers (2D HUD/overlay stays
 * smooth). Depth and UVs are untouched, so the true Z-buffer and perspective-
 * correct texturing are kept. */
typedef enum {
    OPENPETE_VERTEX_PRECISION_SUBPIXEL = 0,  /* default: sub-pixel float, smooth motion */
    OPENPETE_VERTEX_PRECISION_INTEGER,       /* PSX integer-grid snap (polygon wobble) */
} openpete_vertex_precision_t;

/* Texture/colour interpolation mode (`--texture-mapping`). The native renderer
 * reconstructs the homogeneous W from depth so the rasterizer interpolates
 * UV + gouraud colour perspective-correct (the modern default). AFFINE emits
 * w=1 instead (scene vertex spec-constant 1): the rasterizer interpolates
 * screen-linearly, exactly the PSX GPU's behaviour - the authentic texture
 * swim/warp on large near polygons (the game's own near-terrain subdivide,
 * which existed to tame it, still runs). Display-only; depth values are
 * identical in both modes. Boot-latched (pipelines bake the constant). */
typedef enum {
    OPENPETE_TEXTURE_MAPPING_PERSPECTIVE = 0,  /* default: perspective-correct */
    OPENPETE_TEXTURE_MAPPING_AFFINE,           /* PSX screen-linear interpolation */
} openpete_texture_mapping_t;

/* Sector level-of-detail policy (`--lod original|extended`). ORIGINAL is the
 * authentic PSX behaviour: distance-based LOD swaps + the stock fog curve.
 * EXTENDED collapses the sector LOD band so every visible sector draws from
 * its high-poly mesh at full quality, out to the cull distance and beyond
 * (the classic "see detail far away" look), fogged on the same --fog-scale
 * curve as ORIGINAL.
 * Native renderer only - the feeders derive detail from gamestate, never
 * perturbing gameplay-readable RAM (in PsyCross reference mode EXTENDED warns
 * and is ignored). */
typedef enum {
    OPENPETE_LOD_ORIGINAL = 0,  /* default: authentic PSX LOD band + fog curve */
    OPENPETE_LOD_EXTENDED,      /* full detail at any distance (was --no-lod) */
} openpete_lod_mode_t;

/* Distance-fog form (`--fog original|per-pixel|off`). ORIGINAL is the game's
 * depth cue: the per-vertex DPCS fade and the per-face CLUT-row ladder.
 * PER_PIXEL keeps the vertex fade and evaluates the ladder at each pixel's
 * depth on the game's curve. OFF draws no depth cue. AUTO (the unset
 * sentinel) follows --lod on every read: ORIGINAL under original LOD,
 * PER_PIXEL under extended. Native renderer only; it selects host-side
 * colours and never writes gameplay-readable RAM. */
typedef enum {
    OPENPETE_FOG_AUTO = -1,
    OPENPETE_FOG_ORIGINAL = 0,
    OPENPETE_FOG_PER_PIXEL,
    OPENPETE_FOG_OFF,
} openpete_fog_mode_t;

/* Sector candidate-set policy (`--sector-visibility auto|game|frustum|filled`). GAME is
 * the faithful path: the sector set the game's own visibility pass and
 * occlusion groups produce, which hides everything outside the room the camera
 * sits in and everything outside the 4:3 frame. FRUSTUM admits every
 * sector whose bounding sphere passes the frustum test at the displayed
 * half-widths, hidden or not, and lets the depth buffer decide what is seen: a
 * deliberate departure from the original presentation that fills the side
 * wedges of a frame wider than 4:3 and draws the geometry the original hides
 * from an inside-geometry view. The admitted set is still culled by the
 * frustum test, the per-sector tier verdicts, the retained GPU cache and the
 * native occlusion pre-pass. Display-only: the visibility flags are read, never
 * written. FILLED admits, on a game whose verdict is a position-keyed PVS,
 * the game's verdict plus the sectors an opposite pair of neighbour cells
 * around the tick camera lists (an all-visible neighbour counting as
 * agreeing, never alone), within the frustum; elsewhere it is FRUSTUM. AUTO
 * (default) is the value saved for every game and runs FILLED on a game
 * whose verdict is a position-keyed PVS (Spyro 2 and 3), FRUSTUM elsewhere
 * (Spyro 1). Native renderer only. */
typedef enum {
    OPENPETE_SECTOR_VIS_UNSET = -1, /* options only: not yet resolved */
    OPENPETE_SECTOR_VIS_GAME = 0,  /* the game's visibility verdict */
    OPENPETE_SECTOR_VIS_FRUSTUM,   /* every sector inside the frustum */
    OPENPETE_SECTOR_VIS_FILLED,    /* the game's verdict with its holes filled */
    OPENPETE_SECTOR_VIS_AUTO,      /* default: FILLED or FRUSTUM by the game */
} openpete_sector_visibility_t;

/* --cadence: how the eager planner places the in-between scenes of a tick's
 * span on a display grid (vsync on) when the span affords fewer scenes than
 * the grid has slots. even plans every level-th slot, a whole divisor, so
 * every scene is held equally long. fill plans as many scenes as the span
 * affords and spreads them over the slots, so a span one scene short repeats
 * one refresh instead of halving the rate. Without vsync the spans have no
 * grid and both values plan the same. Presentation only: no guest state. */
typedef enum {
    OPENPETE_CADENCE_UNSET = -1,  /* options only: not yet resolved */
    OPENPETE_CADENCE_EVEN = 0,    /* default: every level-th slot */
    OPENPETE_CADENCE_FILL,        /* every affordable scene, spread over the slots */
} openpete_cadence_t;

/* All command-line options live here. The struct tag exists so
 * headers can forward-declare it without pulling this whole file in. */
typedef struct openpete_options {
    /* Identity (filled by per-game main; rest from CLI) */
    const char* display_name;     /* window title etc. */

    /* Run-loop control */
    int frame_limit;              /* 0 = unlimited */
    int max_runtime_seconds;      /* 0 = unlimited */
    bool headless;                /* no on-screen window (offscreen video driver) */

    /* Render-cadence target. -1 = unset (the default): resolved at startup
     * to the display's refresh rate for windowed runs, 0 for headless
     * (run_recomp_fiber, before the pacer is seeded). 0 = match sim
     * (condvar-paced ticker, the historical behavior). >0 = wall-clock
     * dual-pacer: sim still yields at the native PSX 60 Hz vblank, input
     * pump runs at this Hz, and present fires when the game produced new
     * draw commands. */
    int render_fps;

    /* --tick-scale F (default 1.0): wall-clock SPEED multiplier for the whole
     * engine - scales the sim cadence AND the render cadence by the same factor,
     * so the number of interpolated sub-tick presents PER sim tick is invariant.
     * 0.5 = half-speed slow motion (sim 14.985 Hz, present rate halved), 2.0 =
     * double speed. A debug/inspection aid for high-FPS interpolation: slow the
     * world down without changing how many intermediate frames render between
     * ticks, so each interpolated frame is easy to scrutinize. Display/pacing
     * only - every sim tick still computes byte-identical gameplay state (the
     * tick LOGIC and count are unchanged; only their wall-clock spacing scales),
     * so it cannot perturb gameplay vs PSX. Inert under --headless (which runs
     * unpaced) and when there is no window. Must be > 0. */
    double tick_scale;

    /* SDL window size override. 0/0 = use the per-game default (640x480
     * for Spyro). The window *height* drives the internal render resolution:
     * PsyCross sizes its present FBO from g_windowHeight (× the disp/clip
     * overscan ratio) in GR_BeginScene and rasterizes the GTE-projected
     * geometry directly into it with a real depth buffer - a taller window
     * genuinely renders the scene at higher resolution, not an upscale
     * (empirically ~5x the edge detail at 4x height). Window width is decoupled:
     * the FBO width is derived from height × aspect, so it only affects the
     * letterbox/pillarbox margins of the final present-blit to the window. */
    int window_w;
    int window_h;
    /* --window maximized: the window opens maximized (window_w/h stay 0,
     * the size it restores to is the default). 1 = maximized, 0 = not. */
    int window_maximized;

    /* The launcher window's first state. --launcher-fullscreen 0|1
     * (OPENPETE_LAUNCHER_FULLSCREEN): -1 = unset, the launcher opens
     * windowed. --launcher-window WxH|maximized (OPENPETE_LAUNCHER_WINDOW):
     * launcher_window_maximized -1 = unset, the launcher takes --window's
     * size and state when set, else 1280x800; 0 = launcher_window_w x
     * launcher_window_h;
     * 1 = maximized. A fullscreen toggle, resize or maximize made in the
     * launcher replaces fullscreen, window_w/h and window_maximized of the
     * game it boots (openpete_launcher_handoff). */
    int launcher_fullscreen;
    int launcher_window_w;
    int launcher_window_h;
    int launcher_window_maximized;

    /* Vertical FOV of the visible band in degrees, native renderer only
     * (0 = the game's projection, 55° vertical). Applied through
     * openpete_native_set_fov: the feeders project through a scaled H, the
     * GTE and every gameplay-readable byte stay canonical. */
    float fov_deg;

    /* Target display aspect ratio (width / height), native renderer only -
     * the Hor+ widening happens purely at the display layer from canonical
     * 4:3 producers. 0 = auto (`--aspect auto`, the default): the render
     * aspect follows the window / fullscreen shape live, filling it with no
     * bars (clamped ≥ 4:3 - Hor+ only ADDS world). >0 = fixed (`--aspect
     * W:H`): sizes the launch window to match and letterboxes/pillarboxes
     * whenever the window shape differs. */
    float aspect_w_over_h;

    /* --lod original|extended: sector level-of-detail policy (see
     * openpete_lod_mode_t above). ORIGINAL keeps the authentic PSX LOD band;
     * EXTENDED collapses it so every visible sector is drawn from its
     * high-poly mesh out to the cull distance and beyond. The native feeders
     * derive detail from gamestate and do not perturb gameplay-readable RAM.
     * `--no-lod` is a deprecated alias for `--lod extended`. */
    openpete_lod_mode_t lod;

    /* --sector-visibility auto|game|frustum|filled: the sector candidate set
     * (see openpete_sector_visibility_t above). Default AUTO. Set from
     * OPENPETE_SECTOR_VISIBILITY when no flag is given. */
    openpete_sector_visibility_t sector_visibility;

    /* --cadence even|fill (openpete_cadence_t above). Default EVEN. Set from
     * OPENPETE_CADENCE when no flag is given; live through
     * openpete_set_cadence. */
    openpete_cadence_t cadence;

    /* Fog-distance multiplier (`--fog-scale N`). Scales every channel of the
     * terrain fog model together (DPCS vertex curve, CLUT-row palette ladder,
     * near-tile select, fog-saturation LP handoff): N = 1 is the authentic
     * PSX curve (saturation at the LOD distance), larger N saturates N x
     * farther - the classic "see detail far away" look; ~100 pushes fog off
     * entirely. 0 = auto: 1 under either --lod mode. */
    float fog_scale;

    /* --fog original|per-pixel|off (openpete_fog_mode_t above). Default
     * OPENPETE_FOG_AUTO: the mode follows --lod until a value is chosen. */
    openpete_fog_mode_t fog_mode;

    /* Vertex screen-coordinate precision (`--vertex-precision subpixel|integer`).
     * SUBPIXEL (default) keeps the native renderer's smooth sub-pixel projection;
     * INTEGER snaps 3D-world vertices to the PSX 512x240 pixel grid to reproduce
     * the original polygon wobble. Display-only (see openpete_vertex_precision_t). */
    openpete_vertex_precision_t vertex_precision;

    /* Raster output quantize (`--raster-quantize N`). N > 0 resamples the
     * composed 3D scene (pre-HUD) on the virtual PSX raster at N x scale - each
     * 512N x 240N cell takes ONE point sample (N=1 = authentic 240p), matching
     * the hardware rasterizer's single sample per output pixel: no downsample
     * filter, so textures and polygon edges stay hard. An integer MULTIPLIER of
     * the PSX grid rather than a row count, so every setting stays aligned with
     * the INTEGER vertex snap's 512x240 lattice by construction: snapped edges
     * land on cell boundaries, and the snap's silhouette/T-junction seams
     * (<= ~1 PSX pixel) are bounded to one cell, where a per-cell depth vote
     * moves the sample point off the leaking sliver. -1 = auto (1 when
     * vertex_precision is INTEGER, else off); 0 = off. Display-only; native
     * renderer only. */
    int raster_quantize;

    /* Extraction worker threads (`--extract-threads N`). Data-parallel fan-out
     * of the native render extraction (terrain sector walk) over a persistent
     * worker pool; results merge in item order, so output is byte-identical
     * to serial. 0 = auto (logical cores / 2, clamped to [1, 8]); 1 = serial
     * (the pre-pool code path, no threads created). Boot-latched;
     * display-only (workers read guest RAM only inside the extraction
     * window - see openpete_workpool.h contracts). */
    int extract_threads;

    /* PS1 15-bit colour (`--dither 0|1`). 1 = quantise the composed 3D scene
     * to RGB555 with the GPU's 4x4 ordered dither matrix on the PSX 512x240
     * framebuffer lattice - the authentic banding+crosshatch pair the native
     * float pipeline removed. Applied as an engine postfx below any mod chain
     * (a CRT filter sees the dithered image, as composite video did).
     * Display-only; native renderer only. Default 0. */
    int dither;

    /* Hide the player (`--hide-player 0|1`). 1 = the native scene drops the
     * avatar, its fire breath, its ground shadow, the companion (moby,
     * drop shadow, glow ring) and the particles their updates spawn; every
     * other draw, the HUD and the game state are unchanged. Display-only;
     * native renderer only. Default 0. */
    int hide_player;

    /* Hide the HUD (`--hide-hud 0|1`, env OPENPETE_HIDE_HUD; the flag wins).
     * 1 = the native compose skips the flat 2D HUD layer and the HUD3D
     * layer; the overlay quads, the fade and the game state are unchanged.
     * Display-only; native renderer only. Default 0; -1 = unset between
     * openpete_options_init and the end of the parse. */
    int hide_hud;

    /* Moby classes not drawn (`--hide-classes LIST`, env
     * OPENPETE_HIDE_CLASSES; the flag wins): a comma list of class ids
     * (strtoul base 0, at most 32). The native moby feed drops every face of
     * a listed class and the shadow feed its drop shadow. Display-only;
     * native renderer only. NULL = none. */
    const char* hide_classes;

    /* Untextured view (`--notex 0|1`, env OPENPETE_NOTEX; the flag wins).
     * 1 = textured scene fragments output the raw gouraud vertex colour;
     * cutout silhouettes keep their texture alpha. Display-only; native
     * renderer only. Default 0; -1 = unset between openpete_options_init and
     * the end of the parse. */
    int notex;

    /* Wireframe view (`--wireframe 0|1`, env OPENPETE_WIREFRAME; the flag
     * wins). 1 = every 3D scene draw rasterizes as triangle edges coloured
     * by the gouraud vertex lighting (the NOTEX colouring); the 2D layers
     * stay filled. Display-only; native renderer only. Default 0; -1 =
     * unset between openpete_options_init and the end of the parse. */
    int wireframe;

    /* Reference-renderer raster (`--psycross-native-raster 0|1`). 1 renders the
     * PsyCross scene at the console's own 512x240 display grid rather than a
     * square-pixel 4:3 grid derived from the window height, so a screenshot or
     * a --record capture carries one texel per PSX pixel - the authentic
     * raster and its aliasing, not a supersampled resample of it. The window
     * blit scales it up, so only the sampling grid changes. Ignored unless
     * PsyCross rasterizes (--renderer psycross). */
    int psycross_native_raster;

    /* Texture/colour interpolation (`--texture-mapping perspective|affine`).
     * AFFINE = the PSX GPU's screen-linear interpolation (authentic texture
     * swim). Boot-latched; display-only (see openpete_texture_mapping_t). */
    openpete_texture_mapping_t texture_mapping;

    /* Terrain sector-mesh GPU cache (`--terrain-cache 0|1`, EXPERIMENTAL,
     * default OFF): retain terrain as per-sector GPU meshes and move the
     * camera transform / fog / cull into the vertex shader, so the per-present
     * CPU cost is O(sectors) instead of O(faces). Display-only; falls back to
     * the CPU walk per present whenever an unsupported mode is active (debug
     * envs, portal previews, HD texture packs, mod material selectors, A/B
     * reference modes, software Vulkan, --vertex-precision integer).
     * -1 = unset between openpete_options_init and the end of the parse. */
    int         terrain_cache;

    /* Disc image resident in memory (default on): PsyCross reads the .bin
     * from a host array filled by a background loader instead of fread on
     * the sim thread. --disc-cache 0 / OPENPETE_DISC_CACHE=0 = the file
     * path, for A/B. Display/timing-only; bytes identical. -1 = unset
     * between openpete_options_init and the end of the parse. */
    int         disc_cache;

    /* VBLs of guest time every level load waits, spent by deferring the
     * completion of the load's disc reads (games/common/psyq/psyq_thunks.cpp).
     * -1 = unset: the game's own default (game_get_load_delay_default).
     * 0 = instant. Written into a recorded bk2's header; replay under a
     * different value is refused. --load-delay / OPENPETE_LOAD_DELAY. */
    int         load_delay;

    /* Screenshot. Pass `--screenshot-on-frame N[,N2,...]` to fire on the
     * first present at or past each listed value of the feed key
     * (openpete_feed_key_now); the path template can contain `{n}` to
     * substitute the listed value per shot. Output extension `.png` writes PNG via
     * stb_image_write; any other extension writes BMP via SDL_SaveBMP. */
#define OPENPETE_MAX_SCREENSHOT_FRAMES 64
    int         screenshot_frames[OPENPETE_MAX_SCREENSHOT_FRAMES];
    int         screenshot_frame_count;   /* 0 = disabled */
    /* `--screenshot-on-tick N[,...]` fills the same targets but keys them
     * on `openpete_canon_tick` (canonical presents = game draw-ticks)
     * instead of the feed key. Tick-keyed captures sample
     * the SAME sim moment run-to-run - the frame<->tick mapping drifts with
     * host timing (see openpete_fiber.h) - so the visual-regression gates
     * use ticks. Mutually exclusive with --screenshot-on-frame. */
    bool        screenshot_ticks_keyed;
    const char* screenshot_output;        /* path template; default ./screenshot.png */

    /* Multi-present capture for the transform-interp visual regression
     * harness (`scripts/check_interp_visual.sh`): once the sim reaches
     * `screenshot_presents_start`, capture the next
     * `screenshot_presents_count` *presents* (canonical + intermediates)
     * with sequential indices in `{p}`. Distinct from
     * `screenshot_on_frame` which fires once per sim frame on the
     * canonical present only. Used to compare alpha-1.0 (canonical) vs
     * alpha<1.0 (intermediate) outputs at different --render-fps
     * settings - the latter is the only way to observe whether the
     * interp delta is reaching the GL submit. */
    int         screenshot_presents_start;  /* -1 = disabled */
    int         screenshot_presents_count;  /* default 1 */

    /* `--screenshot-native-only`: capture the native `-vk` variant ALONE -
     * skip the PsyCross OT rasterization (`openpete_psycross_renders`, which a
     * screenshot request otherwise forces on for the `-og` reference) and the
     * `-og`/`-vk` composition. Gameplay-neutral (the render->gameplay
     * channel writes come from the recompiled render funcs, upstream of OT
     * rasterization; this is also the normal windowed pure-vk play path). The
     * presentation-golden corpus's default capture path - its single biggest
     * speedup, since the discarded reference otherwise costs a full GL OT walk
     * per screenshot frame. */
    bool        screenshot_native_only;

    /* `--screenshot-intermediate-on-frame N[,...]`: deterministic sub-tick
     * (alpha = 0.5) native captures, headless-capable. At each listed VBL the
     * present clock is forced to the tick midpoint (curr_tick_ns + period/2) and
     * the re-extract callback is invoked once, so the shot lands on the exact
     * interpolated pose the sub-tick spine would present - reproducibly, with no
     * wall-clock pacing. Keyed on the feed key, like --screenshot-on-frame.
     * Implies --screenshot-native-only capture. Fills a SEPARATE target list so a
     * run can carry canonical and intermediate legs together. */
    int         screenshot_intermediate_frames[OPENPETE_MAX_SCREENSHOT_FRAMES];
    int         screenshot_intermediate_count;   /* 0 = disabled */

    /* `--screenshot-overlays [0|1]` (env OPENPETE_SCREENSHOT_OVERLAYS): the
     * recomposed screenshot (headless and intermediate captures) also draws
     * the chrome - settings menu, OSD toasts, stats readout, version line -
     * over the scene, as the live present does. -1 = unset (resolved to 0). */
    int         screenshot_overlays;

    /* Main-RAM dump - writes the 2MB g_ram to disk once the feed key
     * reaches N. Used as the openpete side of the PCSX-Redux comparison
     * loop (see scripts/pcsx-redux-ram-dump.lua). */
    int         ram_dump_on_frame;     /* -1 = disabled */
    const char* ram_dump_output;       /* path; default ./ram.bin */

    /* Scheduled savestate ops - fire once at the savestate quiescent point
     * (game fiber yielded) when the feed key reaches N.
     * Headless-friendly: this is the autonomous test vehicle for the
     * restore-correctness checks (save->tick->restore->tick hash A/B).
     * Load re-anchors both counters exactly like the hotkey path. */
    int         state_save_on_frame;   /* -1 = disabled */
    const char* state_save_output;     /* path; default ./frameN.state */
    int         state_load_on_frame;   /* -1 = disabled */
    const char* state_load_input;      /* path; required when load armed */

    /* Savestate stress harnesses (restore-at-yield checks; headless):
     * state_stress N       - every N ticked iterations: in-memory save +
     *                        immediate quiet restore. A clean run and a
     *                        stressed run must produce identical
     *                        --gameplay-hash-log timelines.
     * state_stress_rewind K- save, run K more iterations, restore, repeat
     *                        (the runahead rollback shape). The repeated
     *                        window's hash rows must self-match pass 1. */
    int         state_stress;          /* 0 = disabled */
    int         state_stress_rewind;   /* 0 = disabled */

    /* Rewind. rewind_window: seconds of keyframe ring
     * (0 = off). rewind_hold_frame/iters: headless harness - simulate
     * holding the rewind key for N iterations at frame A. */
    int         rewind_window;         /* 0 = disabled */
    int         rewind_hold_frame;     /* -1 = disabled */
    int         rewind_hold_iters;

    /* Runahead: lead the presented timeline by N sim
     * ticks to collapse internal input latency (measured L_submit =
     * 2 ticks; N=2 collapses it fully). 0 = off. Arms only when every
     * enabled mod declares a runahead capability. */
    int         runahead;              /* 0 = disabled; clamped 0..3 */

    /* Seed g_ram from a 2 MB file before the recomp dispatches. Used to
     * pin-prime openpete with PCSX-Redux's RAM at the same instant - gives
     * the recompiler a byte-identical starting state, so any subsequent
     * divergence is purely the recomp's fault, not BIOS / _96_init / heap-
     * init asymmetry between the two engines. NULL = disabled. */
    const char* seed_ram_path;

    /* Seed CPUState (32 GPRs + pc/hi/lo + cop0 + GTE) from a 528-byte file
     * captured by scripts/pcsx-redux-ram-dump.lua. Applied AFTER seed-ram
     * and after the boot's EXE-header init, so it overrides everything.
     * Lets openpete start from the exact register state pcsxr was in at
     * the same PC - the partner of --seed-ram for register-aligned diffs.
     * NULL = disabled. File format: see core/runtime/run.c (load_cpu_seed). */
    const char* seed_cpu_path;

    /* Halt at recompiled-function entry. When the recomp's per-function
     * intercept (psx_override_dispatch) sees this PSX vaddr, dump 2MB RAM
     * to ram_dump_output and exit cleanly. 0 = disabled. Pair with the
     * PCSX-Redux Lua dumper using the same PC to capture both engines at
     * the *same* logical instant (function-entry boundary). A VBL count
     * names different instants on the two sides: the Lua dumper counts
     * vblank events inside the BIOS vblank interrupt, openpete counts
     * VSync(0) calls. */
    uint32_t halt_at_pc;

    /* Optional CPU-state dump at halt-at-pc. NULL = disabled. Format
     * matches scripts/pcsx-redux-ram-dump.lua's CSv1 (528 bytes:
     * magic + 32 GPRs + pc/hi/lo + cop0 + GTE). Used to align stack-
     * region diffs: knowing both sides' sp at the dump instant lets a
     * post-process zero "below sp" before bytewise comparison. */
    const char* halt_cpu_dump_output;

    /* Halt on the Nth (1-indexed) call to halt_at_pc instead of the
     * first. Use to land the dump at a specific frame boundary by
     * pinning to a per-frame game function. 0 or 1 = dump on first
     * hit; 2+ = skip preceding hits. */
    uint32_t halt_hit_target;

    /* Logging */
    openpete_log_format_t log_format;
    const char* log_level_spec;        /* e.g. "cd:debug,*:info" or NULL */
    /* --log-file PATH / --no-log-file (which sets "off"). NULL = unset, so
     * $OPENPETE_LOG_FILE and then the default state-dir path apply. */
    const char* log_file;

    /* Disc / data inputs. */
    const char* disc_path;
    const char* exe_path;          /* Direct PSX EXE bytes (overrides disc) */
    const char* ingest_path;       /* --ingest: copy this disc image into
                                       the content-addressed library next
                                       to the exe, then boot it. */
    const char* game_selector;     /* --game ID: the game this session
                                       runs; a disc naming another game
                                       refuses the launch. NULL = none. */
    const char* extract_recomp_inputs_dir; /* Extract PSX.EXE + overlay
                                       blobs from --disc into this dir and
                                       exit (first-boot pipeline stage /
                                       carve-equivalence gate). */
    const char* recomp_module_path; /* Shared-linkage builds: override the
                                       recomp-module path (default: platform
                                       library name next to the exe). Ignored
                                       by static builds. */

    /* Bring-up smoke test: skip the run loop and instead boot the
     * PSX EXE, dispatch a single recompiled function at this address, and
     * exit reporting cpu->v0. 0 = disabled. */
    uint32_t test_recomp_call_addr;
    uint32_t test_recomp_expected_v0;  /* 0 = don't check */
    uint32_t test_recomp_gp;            /* override cpu->gp before dispatch (0 = leave alone) */
    uint32_t probe_addr;                /* read 4 bytes at this PSX vaddr post-dispatch (0 = off) */
    bool     test_recomp_segv;          /* deliberately fault to verify crash handler */

    /* Main-loop driver. Dispatches the function at this PSX vaddr on a
     * dedicated fiber; PSYQ thunks (VSync etc.) yield back to the host fiber,
     * which presents via PsyCross and advances `openpete_frame` until exit. */
    uint32_t run_recomp_addr;

    /* Infra-triangle (--debug-triangle). When set, the host fiber emits a
     * hardcoded POLY_F3 each frame between PsyX_BeginScene and
     * PsyX_EndScene. Validates the full GL render path independently of the
     * game's own draw code. */
    bool debug_triangle;

    /* Active renderer / display mode (--renderer psycross|vk). Default
     * OPENPETE_RENDERER_PSYCROSS = the original behavior (PsyCross owns the one
     * window, no native Vulkan window). The other modes open the native
     * renderer's Vulkan window and start PsyCross hidden. Pure display
     * selection - never perturbs gameplay state; native modes are no-ops under
     * --headless. See openpete_renderer_t above. */
    openpete_renderer_t renderer;

    /* The renderer this game starts with when --renderer is absent, which is
     * also the one print_help marks as the default. A game with no native
     * renderer walks sets both this and `renderer` through
     * openpete_options_set_default_renderer, before parsing, so an explicit
     * --renderer still wins and the help text still names what it will get. */
    openpete_renderer_t default_renderer;

    /* Set by the launcher's hand-off (openpete_launcher_handoff) when it
     * could not create the Vulkan device in a windowed run. Bring-up then
     * creates no device, PsyCross keeps its GL path, and its GL window is
     * the display. Not a knob: no flag or env var sets it. Display-only. */
    int native_unavailable;

    /* --ssaa N (default 1): supersampling factor for the native vk LIVE window.
     * The offscreen render target is allocated at N× the swapchain extent per
     * axis (N² samples/pixel) and the present blit box-downsamples to the
     * window. 1 = no-op (the offscreen matches the swapchain, a 1:1 blit).
     * Range 1..OPENPETE_SSAA_MAX, further capped to the GPU's max image size
     * (so a large value degrades gracefully). Display-only - no gameplay effect;
     * under --headless and on --renderer psycross. The native_rtt that backs
     * the -vk screenshots + DIFF mode stays at reference resolution
     * so the A/B-vs-PsyCross comparison stays 1:1 pixel-exact. */
    int ssaa;

    /* --resolution window|N|WxH (default window): the native scene's base
     * size, packed by openpete_render_res_pack (0 = window). resolution_set
     * is 1 when a flag or the config key gave an N or WxH value; headless
     * captures compose through a base only then. */
    int resolution;
    int resolution_set;

    /* Performance-stats overlay level on the native vk window, one of the
     * OPENPETE_PERF_* values: off, the FPS counter (the default), or the
     * counter plus a pacing readout. --perf-stats at launch; the O menu's
     * SHOW FPS row and the M overlay's Performance stats row move it live.
     * Display-only; ignored under --headless / psycross. */
    int perf_stats;

    /* Which corner/edge the performance-stats block anchors to, an
     * openpete_anchor_t (openpete_overlay_anchor.h). --perf-position; the M
     * overlay's row moves it live. Display-only. */
    int perf_position;

    /* On-screen display (toast notifications). `osd` 0/1; `osd_position` an
     * openpete_anchor_t; `osd_timeout_ms` the default dwell in INTEGER
     * MILLISECONDS. The user-facing flag is in seconds (--osd-timeout 3.0)
     * because that is how a person thinks about a toast; everything past the
     * parser is ms, converted exactly once, right here. Display-only. */
    int      osd;
    int      osd_position;
    uint32_t osd_timeout_ms;

    /* --version-osd 0|1: the release build's two-second version line at the
     * bottom left. A distributed build that is not exactly a release keeps its
     * line up for the whole session and this does not suppress it - the line
     * is what names the build in a screenshot of it - and a build that is not
     * a distribution build draws none either way. Display-only. */
    int version_osd;

    /* --vsync 0|1: native vk swapchain present mode. 1 = FIFO (vsync ON, present
     * locked to the monitor refresh - the default); 0 = a vsync-OFF mode
     * (immediate/mailbox) for low latency / uncapped-throughput benchmarking;
     * -1 = unset (defer to the OPENPETE_PRESENT_MODE env, else FIFO). Display-only;
     * ignored under --headless / psycross. */
    int vsync;

    /* --gpu INDEX|NAME: the Vulkan physical device the native renderer uses,
     * an enumeration index or a case-insensitive substring of its device
     * name. NULL = unset (OPENPETE_VK_DEVICE, else the automatic pick that
     * prefers a discrete GPU). Read at device creation only. */
    const char* gpu;

    /* --fullscreen 0|1: launch the native window in borderless-desktop
     * fullscreen (same mode Alt+Enter / double-click / the O-menu DISPLAY row
     * toggle live). Default 0 = windowed. Display-only; ignored under
     * --headless / psycross (warned). */
    int fullscreen;

    /* --render-paths CSV (debug): restrict rendering to the named render paths
     * (e.g. "cyclorama,spyro") in BOTH renderers, so a path can be compared in
     * isolation. PsyCross drops the other paths' geometry prims during the OT
     * walk (presentation-only - never writes game RAM, so gameplay state is
     * unaffected); the native renderer feeds only the selected paths. The path
     * NAMES are game-specific, so core only carries the raw string; the per-game
     * impl parses it. NULL/empty = all paths (no filtering). */
    const char* render_paths;

    /* --devui-open 0|1 (debug): open the dev config overlay (the M panel) as
     * soon as it initializes, instead of waiting for the M key. Windowed
     * native-vk runs only - the overlay is never created headless or under
     * --renderer psycross, where the flag logs that it is inert. -1 = unset
     * (defer to the OPENPETE_DEVUI_OPEN env, else closed). Boot-latched:
     * openpete_devui_init reads it once. Display-only. */
    int devui_open;

    /* --devui-inject-mouse X,Y (debug): post this cursor position into the
     * dev overlay's imgui IO before every frame, so an unattended run can
     * park the pointer over a widget and read the hover state out of the
     * log. Windowed native-vk runs only. NULL/empty = unset (defer to
     * OPENPETE_DEVUI_INJECT_MOUSE, else no injection). Boot-latched:
     * openpete_devui_init parses it once. Display-only. */

    /* BizHawk .bk2 movie replay. Full pad-bitstream fidelity (holds,
     * releases, gamepad, analog). NULL = disabled. */
    const char* bk2_path;

    /* Map bk2 frame 0 to value `bk2_frame_offset` of the counter the
     * movie's rows are indexed by (the feed key, or openpete_frame for a
     * movie recorded one row per yield). Negative is legal; unset
     * (bk2_frame_offset_set false) means 0. */
    int  bk2_frame_offset;
    bool bk2_frame_offset_set;

    /* When set, exit after the last bk2 frame instead of holding the last
     * input forever. */
    bool bk2_stop_on_end;

    /* Force-mute all audio (SPU SFX + XA CD music + music WAV override)
     * at the OpenAL layer via PsyX_SPUAL_SetMute + PsyX_XA_ForceGain +
     * music_override_set_forced_gain. Engine-level, so no game-specific
     * globals or per-frame stamping is needed. Defaults to false (audio
     * on) - the shipping game has sound; pass --mute 1 (or set it in
     * openpete.toml) to silence a dev/CI build. Note this only clamps gain
     * on already-created AL sources; it never gates device/context init,
     * so a headless run with no audio device is silent either way. */
    bool mute;

    /* Preset the game's own in-game volume sliders at boot, on the scale its
     * options menu uses (0 = OFF .. 10 = max). -1 leaves each slider alone,
     * which is the default: the game picks its own value (and a loaded save
     * file restores the player's).
     *
     * Unlike --mute these are *gameplay-side* values, written into the game's
     * volume globals by the per-game audio-volume feature at a tick boundary.
     * The preset survives the clobbers the game does on its own (title-screen
     * init, save-file load) and is released the moment the player edits that
     * slider in the game's options menu. */
    int sfx_volume;     /* --sfx   0..10, -1 = leave the game's slider alone */
    int music_volume;   /* --music 0..10, -1 = leave the game's slider alone */

    /* --vibration: controller rumble, 0 (off) .. 10 (max), -1 = leave the
     * game's own vibration setting alone (the default, which means ON - the
     * game enables actuators at init and a save file restores the player's
     * choice).
     *
     * Two jobs from one value, the same shape as the sliders above: the
     * per-game vibration feature stamps the game's own enable flag at a tick
     * boundary, and the host scales motor amplitude by N/10 on top. The guest
     * toggle has no intensity of its own, so without the host scale a 3 and a
     * 10 would feel identical.
     *
     * 0 silences the motors host-side and writes NOTHING to guest RAM - the
     * game's own vibration setting keeps reading whatever the player chose.
     * That is deliberate: 0 is the value every unattended launcher sets, and a
     * guest write there would leave the oracle and determinism gates measuring
     * a quietly modified game. */
    int vibration;      /* --vibration 0..10, -1 = leave the game's setting alone */
    /* --mouse-sensitivity: multiplier on the freecam/camlock mouse look
     * (angle units per count = OPENPETE_FREECAM_MOUSE_K * this); < 0 = unset
     * (1.0). */
    float mouse_sensitivity;
    /* --mouse 0|1: the host mouse channel switch. 0 drops every real mouse
     * event and refuses pointer capture, so the pointer over the window is an
     * ordinary desktop cursor; the scripted injection stream is unaffected.
     * -1 = unset (on). Host-side only: nothing here reaches guest RAM. */
    int mouse;
    /* --input-lock: ignore every physical host input device (keyboard, mouse,
     * gamepads); --bk2 replay and injected events still drive the run and
     * closing the window still exits. -1 = unset (off). Host-side only. */
    int input_lock;
    /* --background: create the window at its normal size and visible, but
     * without activating it, without focusable input, and pushed behind the
     * other windows where the platform supports it. Presents, pacing and the
     * screenshot readback path are unchanged; the window simply never takes
     * over the screen or the keyboard. Wins over --fullscreen. -1 = unset
     * (off). Ignored with --headless. Host-side only. */
    int background;
    /* --pad-unfocused: keep accepting gamepad input, and driving rumble,
     * while the window does not have focus. -1 = unset (off). Host-side
     * only: it changes which SDL events arrive, never guest RAM. */
    int pad_unfocused;
    /* --stick-response linear|console: how a gamepad stick axis becomes
     * the PSX stick byte the game reads (openpete_stick_response_t). -1 =
     * unset (console). It changes the pad bytes the guest reads, as a
     * different controller would. */
    int stick_response;

    /* Content for the "<bk2_record_path>.args" sidecar written next to a
     * --bk2-record capture. Holds the launch flags passed *directly* on
     * the command line (excluding every --bk2-* flag and anything sourced
     * from openpete.toml) so replaying the recording via --bk2 re-applies
     * the same scenario. NULL = don't write a sidecar. Heap-owned and
     * intentionally leaked for program lifetime (built in
     * openpete_options_parse). */
    const char* bk2_record_args;

    /* Record live pad input (keyboard + gamepad, merged by PsyCross) into
     * a BizHawk-format .bk2 zip at this path. Sister of --bk2 replay;
     * combinable with it (the recorded frames include the replayed ones,
     * so a session can start with a canned .bk2 prefix and extend live
     * from where it ends). NULL = disabled. The file is written at exit,
     * so SIGTERM/Ctrl-C against the run still produces a usable movie as
     * long as core/runtime/run.c reaches its cleanup block.
     *
     * When --bk2-record is passed with no path, openpete_options_parse
     * resolves a default here: "<bk2 data dir>/<timestamp>.bk2" (and
     * mkdir -p's that directory). Heap-owned in that case (leaked for
     * program lifetime); otherwise it points at the CLI token. */
    const char* bk2_record_path;

    /* Scheduled fast-forward windows: forces openpete_unlock_fps = 1
     * while the feed key is inside any [lo,hi] range. Used by the
     * BK2 determinism harness to inject pacing variation without
     * requiring a human to hold Tab. NULL = disabled. Format:
     *   "100-200,500-600"
     * Inclusive on both ends. Values are the feed key's, the counter bk2
     * rows are indexed by. */
    const char* unlock_frames_spec;
#define OPENPETE_UNLOCK_RANGES_MAX 32
    struct { int lo, hi; } unlock_ranges[OPENPETE_UNLOCK_RANGES_MAX];
    int  unlock_ranges_count;

    /* Position log: every yield where the game is past fast-boot, append
     * one CSV line `frame,vbl,x,y,z` with g_Spyro.m_Position to this
     * path. Used for cross-run determinism diffs. NULL = disabled. The
     * Spyro address is hardcoded in run.c (game-specific) - when we add
     * a second game we'll route this through game_extras. */
    const char* position_log_path;

    /* --gameplay-hash-log PATH (default NULL): per-yield, hash the
     * gameplay-readable region defined by games/<game>/manifest/gameplay_state_regions.toml
     * with FNV1a-64 and append `frame,vbl,hex_hash` to PATH. The
     * regions are loaded statically (compiled-in copy of the manifest)
     * so the runtime has zero runtime-config requirement. Used by
     * tests/harness/check_determinism_matrix.sh as the primary determinism
     * gate - the per-position log is a coarse proxy that misses leaks
     * not yet propagated to Spyro's m_Position. NULL = disabled. */
    const char* gameplay_hash_log_path;

    /* PCSX-Redux oracle verifier. When set, opens a .psxtrace produced by
     * scripts/pcsx-redux-trace.lua; on every dispatch of psxtrace_anchor_pc
     * the verifier consumes the next trace record and value-equality
     * compares its per-region hashes against openpete's live
     * gameplay_hash_compute_ex(). The anchor PC defaults to whatever PC
     * is recorded in the trace's first record (so the same anchor
     * scripts/pcsx-redux-trace.lua used). First mismatch is logged with
     * (frame, region_index, region_name, oracle_hash, op_hash); the run
     * exits non-zero unless --psxtrace-keep-going is set. NULL = disabled.
     * Format spec in tools/psxtrace_format.py. */
    const char* psxtrace_verify_path;
    bool        psxtrace_keep_going;
    uint32_t    psxtrace_anchor_pc;        /* 0 = inherit from trace      */
    uint32_t    psxtrace_anchor_hit;       /* 0/1 = first hit; N = Nth    */

    /* --frame-time-log PATH (default NULL): write one 32-byte record
     * per present (frame, slip_count, op_now_ns, deadline_ns, slack_ns)
     * for the pacing measurement harness. Off by default. Read by
     * scripts/measure-pacing.py. */
    const char* frame_time_log_path;
    /* --pace-log PATH: the unified pacing event log (every VBL, every
     * present, on one timebase). Superset of --frame-time-log; see
     * openpete_pace_log.h. */
    const char* pace_log_path;

    /* --profile-out PATH (default NULL): stream a binary `.opp` performance
     * trace (CPU scope spans + GPU zones + census, keyed per present /
     * canonical tick). Off by default. See core/include/openpete_profile.h,
     * scripts/profile-to-trace.py, scripts/profile-summary.py. */
    const char* profile_out_path;

    /* --record PATH: capture gameplay video. True VFR - one encoded frame
     * per game-presented frame, PTS = wall-clock capture time, so playback
     * matches real game cadence (frame drops show as fewer frames/sec).
     * Frames are read from the present FBO at internal resolution (no
     * letter/pillarbox bars). Links libav* in-process. NULL = disabled.
     * Container auto-picked by extension (.mkv recommended; .mp4 supported).
     * Written incrementally + trailer on shutdown, so Ctrl-C still yields a
     * playable file. */
    const char* record_path;
    const char* record_encoder;   /* libavcodec video encoder. Default libx264. */
    int         record_crf;       /* x264/x265 quality. Default 23. */
    int         record_gop;       /* keyframe interval, frames. Default 120. */
    const char* record_pixfmt;    /* encoder pixfmt. Default yuv420p. */

    /* Audio capture into the recording. The mixed SPU PCM is pulled via an
     * OpenAL loopback device (only while recording; normal playback is
     * unaffected) and encoded as a second stream. */
    bool        record_no_audio;     /* --no-record-audio: video-only */
    const char* record_audio_encoder; /* libavcodec audio encoder. Default aac. */

    /* --record-fps-overlay 0|1: draw the native vk FPS counter INTO captured
     * video (--record). Off by default so recordings show the clean game image;
     * 1 burns the overlay in (verification captures). -1 = unset (defer to the
     * OPENPETE_FPS_IN_RECORD env, else off). Display-only. */
    int         record_fps_overlay;

    /* --record-cfr RATE: capture at a constant frame rate instead of true VFR.
     * RATE must be an integer multiple of the NTSC sim-tick rate (1125000/37609
     * = 29.913052...): 29.913, 59.826, 119.652, ... Any other value is rejected,
     * because a non-integer ratio makes the sub-tick interpolator emit an
     * uneven number of intermediate presents per tick - the very cadence jitter
     * CFR exists to remove. Rounded spellings (30, 60, 120) are accepted and
     * snapped to their true neighbour. 0 = off (VFR).
     *
     * The mode is more than a timestamping change: it drives the host clock
     * from the pacer (op_virtual_clock_enable) so exactly RATE/29.913 presents
     * are produced per sim tick regardless of how fast the machine renders, and
     * stamps frame N at exactly N/RATE. Two captures of the same window at
     * different rates therefore cover the identical span of game time.
     * Wall-clock never enters the output. */
/* Largest accepted --record-cfr multiple of the tick rate (16x = 478.6 fps).
 * Presents-per-tick above this is well past any display and just multiplies
 * encode cost. */
#define OPENPETE_RECORD_CFR_MAX_MULT 16
    double      record_cfr;

    /* --record-frames N: finish the recording (and exit) after exactly N
     * encoded frames. With --record-cfr this pins the captured window to an
     * exact game-time length: N / RATE seconds. 0 = unlimited. */
    int         record_frames;

    /* --texpack-dir PATH: directory of HD texture-replacement PNGs (the texture
     * pack). NULL = unset (defer to the OPENPETE_TEXPACK_DIR env, else the
     * built-in <exe>/assets/textures/). Native renderer only; display-only,
     * never perturbs gameplay state. */
    const char* texpack_dir;
    /* --sfxpack-dir PATH: directory of drop-in sound replacements (WAVs
     * named by the sfx census); NULL = env OPENPETE_SFXPACK_DIR /
     * <exe>/assets/sfx. Mods' assets/sfx/ always join. */
    const char* sfxpack_dir;

    /* --controller-db PATH: an extra SDL controller-mapping database, loaded
     * after the shipped and per-user ones and so outranking both. NULL =
     * unset (defer to the OPENPETE_CONTROLLER_DB env). Host input only;
     * a mapping decides which physical button reaches the decode path, never
     * what the guest reads for a given decoded button. */
    const char* controller_db;

    /* --mods-dir PATH: mods root (each subdir with a mod.toml is a mod;
     * enabled.txt fixes the order = priority). NULL = unset (defer to
     * env OPENPETE_MODS_DIR, else <exe>/mods). A missing directory means
     * zero mods and a byte-identical modless run. */
    const char* mods_dir;

    /* --mods ID[,ID...]: enabled modset override - replaces the mods dir's
     * enabled.txt entirely, same semantics (order IS priority, later =
     * higher). An empty list ("--mods \"\"") enables nothing, i.e. a
     * one-flag vanilla run without touching the install. NULL = unset
     * (defer to env OPENPETE_MODS, else enabled.txt, else all discovered
     * mods in lexicographic order). */
    const char* mods_enabled;

    /* --scan-guard 0|1 (OPENPETE_SCAN_GUARD): 1 (default) has the game's
     * guest-byte guard restore and log a mod's store into the code ranges
     * Spyro 3's anti-piracy scans hash; 0 turns the guard's compares off,
     * so such a store persists. No effect on a game without a guard.
     * -1 = unset between openpete_options_init and the end of the parse. */
    int         scan_guard;

    /* --mod-hot-reload [0|1] (OPENPETE_MOD_HOT_RELOAD, on unless empty or
     * starting with '0'): 1 has the mod loader poll every entered mod's
     * sources, config.toml and shaders/ every 64th tick boundary and reload
     * a changed mod in place; 0 (default) loads each mod once. Latched by
     * openpete_mods_init. -1 = unset between openpete_options_init and the
     * end of the parse. */
    int         mod_hot_reload;

    /* --compile-mods: utility mode - compile EVERY mod with src/ in the
     * resolved mods dir (ignoring enabled.txt) against <exe>/sdk through
     * the loader's own key/stamp/compile path, then exit (nonzero if any
     * compile failed). The cmake `spyro1_mods` build step for native
     * presets; also pre-warms a dist install's compile-on-boot cache. */
    int         compile_mods;

    /* --dump-sfx-pack DIR: utility mode - walk every sample on --disc
     * (and the custom-level Data files in --dump-sfx-wads, comma list)
     * and write the sfx pack census (WAVs named by content hash +
     * index.txt) to DIR, then exit. */
    const char* dump_sfx_pack_dir;
    const char* dump_sfx_wads;

    /* openpete.toml utility modes - resolve, do the one thing, exit 0.
     * Handled by game main alongside --compile-mods, so the whole config
     * stack is already resolved by the time they run. */
    int         help;            /* --help / -h: the help was printed */
    int         config_report;   /* --config-report: print the resolution table */
    int         keys_report;     /* --keys-report: print the resolved keymap */

    /* Host-backed memory card (save/load). The game reaches the card only
     * through the PSYQ MemCard* wrappers, which openpete backs with real
     * 128 KiB .mcr images in the `cards` directory openpete_paths.h
     * resolves. Pure host-side save I/O - never touches gameplay-readable
     * state. See openpete_memcard.h. */
    const char* card_dir;      /* --card-dir PATH: dir holding cardN.mcr (NULL = the resolved cards dir; env OPENPETE_CARD_DIR) */
    const char* card1_path;    /* --card1 PATH: explicit port-A image (NULL = <card_dir>/card1.mcr) */
    const char* card2_path;    /* --card2 PATH: explicit port-B image (NULL = <card_dir>/card2.mcr) */
    int         card_a_present;/* card in port A (default 1) */
    int         card_b_present;/* card in port B (default 1) */
    int         card_unformatted; /* --card-unformatted: provision a blank card so the in-game format UI runs (default 0 = pre-formatted) */
    int         adopt_saves;   /* --adopt-saves: re-tag each save (card sidecar on games without save records) to the running modset at first card access */
    int         save_mismatch_toast; /* --save-mismatch-toast: OSD toast at a load of a save written under another modset */
    const char* resume_handoff;/* --resume-handoff: process-handoff checkpoint to restore at first quiescence */
    const char* bug_report_replay; /* --bug-report-replay: the bundle (zip or extracted directory) this session replays */
    unsigned    bug_report_replay_beyond; /* --bug-report-replay-beyond: ticks run past the bundle's last recorded tick (default 0) */
    int         handoff_argc;  /* original argv snapshot for the handoff exec */
    char**      handoff_argv;

    /* Skip the title screen / save-file picker and load directly into a
     * specific level. The game's natural post-intro path puts the title
     * screen in TSM_Demo + TSD_Level mode, which loads whatever
     * g_LevelId is set to. We hook that exact moment in the per-game
     * impl, override g_LevelId/g_NextLevelId, and let the state machine
     * do its normal flyby + load - no state corruption. 0 = disabled.
     * Spyro 1 level IDs: 10/20/30/40/50/60 are homeworlds, +1..3 are
     * regular levels, +4 is the flight/speedway level, 63 is Gnasty's
     * lair. Splash sequence is auto-skipped (host-yield is bypassed until
     * the first DrawOTag), so this loads as fast as the EXE allows. */
    int skip_to_level;

    /* --skip-to-cutscene C: a non-level target (Spyro 1: the credits rolls)
     * the per-game impl enters from a live level; combined with
     * --skip-to-level it enters from that level, else from the game's
     * default. 0 = disabled. */
    int skip_to_cutscene;

    /* --skip-to-area L:A: level L's sub-area A, entered through the game's
     * own sub-area transition once the level is live (Spyro 3). Replaces
     * --skip-to-level's level. 0 = disabled. */
    int skip_to_area_level;
    int skip_to_area;

    /* --cheats a,b,...: comma-separated names of the game's OWN built-in
     * cheat codes to apply automatically, so the pad code never has to be
     * entered. The per-game feature module owns the name table and applies
     * the effects once gameplay is live (the globals they write don't exist
     * before a level loads, and New Game / save-load rewrite them). NULL =
     * none. Deliberately gameplay-affecting - the same class as the debug
     * free camera, not a presentation flag. */
    const char* cheats;
} openpete_options_t;

/** @brief Zero opts, then set every field whose default is not zero.
 *
 * Unset tri-state fields take -1 (OPENPETE_SECTOR_VIS_UNSET for
 * sector_visibility) so a later consumer can tell a typed value from the
 * default. No environment variable is read here: openpete_options_parse
 * applies the env spellings after the flags, so an openpete.toml [env]
 * entry reaches them too.
 * @param opts The options block to initialize. */
void openpete_options_init(openpete_options_t* opts);

/** @brief Set the game's starting renderer and the one the help text calls the default.
 *
 * Writes both renderer and default_renderer. Call between
 * openpete_options_init and openpete_options_parse, so a typed --renderer
 * still wins.
 * @param opts The options block.
 * @param r The game's default renderer. */
void openpete_options_set_default_renderer(openpete_options_t* opts,
                                           openpete_renderer_t r);

/** @brief Parse the command line and the game-neutral configuration tiers into opts, writing nothing to the process environment.
 *
 * The parse that runs before the game is identified: as
 * openpete_options_parse over openpete_config_apply_neutral, so the
 * [game.*], [disc.*] and [keys] tables are not read, the [env] winners are
 * read through openpete_config_env without being set, the flags that stand
 * for an env var (--freecam-pose, --player-pose, --freecam-path) do not set
 * it, and a --bk2-record request is not completed. The caller runs
 * openpete_options_parse on fresh options after identification.
 * @param opts The options block, initialized by openpete_options_init.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return As openpete_options_parse. */
int  openpete_options_parse_neutral(openpete_options_t* opts, int argc, char** argv);

/** @brief Parse the command line into opts.
 *
 * The full parse, run once after the game is identified (the game id and
 * the identified disc are set on the configuration layer first).
 * argv[0] is the program name. The parse splices, left to right, the
 * openpete.toml flags, the <bk2>.args sidecar of a --bk2 movie, and the
 * typed arguments, then walks them with the last write winning. After the
 * walk it resolves the default --bk2-record path and its sidecar, then
 * applies the env spellings of the flags that were not typed and resolves
 * notex, wireframe, terrain_cache, disc_cache, scan_guard, mod_hot_reload
 * and sector_visibility to their defaults (load_delay stays -1 for run.c). The EXE-source check is left to
 * openpete_options_require_exe_source. --help prints the help, sets
 * opts->help and returns OPENPETE_EXIT_OK at once; the caller exits 0. A
 * value refused by type or range fails the parse when typed or from a bk2
 * sidecar; from the config file or an env spelling it warns and keeps the
 * default, unless openpete_options_set_strict is on or --config-report was
 * passed, which fail it.
 * @param opts The options block, initialized by openpete_options_init.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR on an unknown flag,
 *         a bad value, or an unreadable args file; the caller propagates it. */
int  openpete_options_parse(openpete_options_t* opts, int argc, char** argv);

/** @brief Make a refused config-file or env value fail openpete_options_parse instead of warning and keeping the default.
 *
 * The surface dump's selfcheck parses in strict mode, so a declared default
 * the parser refuses cannot pass by falling back.
 * @param on 1 for strict, 0 for the boot behaviour. */
void openpete_options_set_strict(int on);

/** @brief Copy in to out, resolving each unset field whose unset and resolved values behave the same at every consumer.
 *
 * mouse -1 becomes 1, input_lock, background, pad_unfocused and
 * launcher_fullscreen -1 become 0,
 * mouse_sensitivity -1 becomes 1.0, load_delay -1 becomes
 * game_get_load_delay_default(), and notex, wireframe, terrain_cache,
 * disc_cache, scan_guard, mod_hot_reload and sector_visibility take their
 * built-in defaults when still unset.
 * Every runtime consumer reads the raw fields; this serves the surface
 * dump's selfcheck, which compares a parse of a flag at its declared default
 * with a parse without it.
 * @param in Parsed options.
 * @param out Receives the resolved copy; may alias nothing in in. */
void openpete_options_effective(const openpete_options_t* in,
                                openpete_options_t* out);

/** @brief A parser flag with no registry row and no schema alias, as the surface dump lists it. */
typedef struct {
    const char* flag;          /* "--frame-limit" */
    int         takes_arg;     /* knob_flag_arg_t: 0 required, 1 none, 2 optional */
    const char* build_cond;    /* preprocessor symbol gating its --help line, or NULL */
    const char* block;         /* --help block: "release", "preamble", "debug", "none" */
    const char* default_note;  /* how the program decides its default, or NULL */
    const char* help_arg;      /* --help value placeholder ("N", "PATH"), or NULL */
    unsigned short help_order; /* position inside the block, ascending */
    const char* help_text;     /* --help description, '\n' per line break; NULL = not printed */
} openpete_help_only_t;

/** @brief The help-only flag table.
 *
 * @param count Receives the entry count.
 * @return The static table. */
const openpete_help_only_t* openpete_options_help_only(size_t* count);

/** @brief Check that a disc or EXE path is set.
 *
 * Call after the disc library resolves, since --ingest or an installed
 * library entry can supply the disc after parse. --test-segv needs no EXE
 * and always passes.
 * @param opts The parsed options.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR after printing
 *         the ways to name a game. */
int openpete_options_require_exe_source(const openpete_options_t* opts);

/** @brief Resolve the game window's size: --window, else the game's default, widened for --aspect.
 *
 * The default is game_default_window_w/h. With --aspect and no --window on
 * a native renderer the width becomes the default height times the aspect
 * times game_display_crop_factor(), so the cropped source fills the
 * window; the PsyCross reference renderer keeps the 4:3 default. The
 * launcher creates its window at this size and bring-up uses it, so the
 * hand-off resizes nothing.
 * @param opts The options.
 * @param out_w Receives the width in pixels.
 * @param out_h Receives the height in pixels. */
void openpete_game_window_size(const openpete_options_t* opts, int* out_w, int* out_h);

/** @brief Run the program: boot the guest and drive the host loop until exit.
 *
 * Acquires the `sim` role on the calling thread (openpete_thread_roles.h),
 * initializes the clock and seals its init group and the game layout, and
 * releases `sim` at return. Initializes logging and the stderr log mirror, sets the extraction worker
 * pool's requested size (its threads spawn on first parallel use), installs
 * the crash handler, then runs the recompiled-call smoke
 * test when one is requested. PsyCross builds dispatch the guest entry on a
 * fiber, with the host loop on a worker thread when the thread split
 * applies. Builds without PsyCross open an SDL window that only clears and
 * presents until quit, --frame-limit or --max-seconds.
 * @param opts The parsed options.
 * @return One of OPENPETE_EXIT_*. */
int  openpete_run(const openpete_options_t* opts);

/** @brief Ask for a clean shutdown from any thread.
 *
 * Sets the cooperative flag the host loop polls once per iteration, so
 * teardown runs through the normal drain instead of exiting from inside a
 * scene. Callers include the settings menu's exit row and the RHI on an
 * unrecoverable device loss. */
void openpete_request_shutdown(void);

/** @brief Report whether the run is to stop: a shutdown request, or an expired --max-runtime-seconds deadline.
 *
 * Read from any thread. The host loop quits on either at its next
 * iteration; code that runs the guest without returning to the host loop
 * (the fast-boot VBL path) polls this to hand control back to it.
 * @return Nonzero once a shutdown is requested or the deadline has passed. */
int openpete_shutdown_requested(void);

/** @brief Set the render-fps target of present pacing at runtime.
 *
 * Queues the target; at the top of its next iteration the host loop takes
 * it on its own thread, sets the render period to 1 s / fps divided by the tick scale,
 * re-anchors the sim and render deadlines to now (no catch-up burst), and
 * enables sub-tick interpolation when the render rate exceeds the VBL rate.
 * 0 selects the match-sim pacer: the render period is the VBL period and
 * interpolation is off. Pacing only; writes no gameplay-readable state.
 * @param fps Presents per second; a negative value is read as 0. */
void openpete_set_render_fps(int fps);

/** @brief Return the render-fps target the pacer drives.
 *
 * The resolved launch value (an unset --render-fps is the display refresh
 * rate when windowed, 0 headless) or the value openpete_set_render_fps last
 * set. The settings menu seeds its FPS row from it.
 * @return Presents per second, or 0 for the match-sim pacer. */
int openpete_get_render_fps(void);

/** @brief Set the eager planner's cadence policy at runtime.
 *
 * Every later span plan reads it (openpete_get_cadence); the span being
 * produced keeps its plan. Host state only: writes no gameplay-readable
 * state. Logs a change.
 * @param cadence OPENPETE_CADENCE_FILL, or anything else for OPENPETE_CADENCE_EVEN. */
void openpete_set_cadence(int cadence);

/** @brief Return the eager planner's cadence policy, OPENPETE_CADENCE_EVEN or OPENPETE_CADENCE_FILL.
 * @return The policy the next span plan uses. */
int openpete_get_cadence(void);

/* The tick scale's range: --tick-scale, the toml tick_scale key, the live
 * knob and openpete_set_tick_scale clamp a positive factor into it. The
 * upper bound keeps the scaled VBL period above zero nanoseconds. */
#define OPENPETE_TICK_SCALE_MIN 0.05
#define OPENPETE_TICK_SCALE_MAX 8.0

/** @brief Set the engine speed factor, slowing or speeding the sim and presents together.
 *
 * At the top of its next iteration the host loop divides the VBL period by
 * the factor, re-derives the tick, render and slip periods from it and
 * re-anchors its deadlines. Pacing only: the sim executes identical ticks
 * on a stretched wall clock, and an unlocked or headless run ignores the
 * deadlines.
 * @param scale The factor, clamped to [OPENPETE_TICK_SCALE_MIN, OPENPETE_TICK_SCALE_MAX]. */
void   openpete_set_tick_scale(double scale);
/** @brief Return the tick scale last requested or applied; callable from any thread.
 *
 * @return The factor. */
double openpete_get_tick_scale(void);

/** @brief Request a runahead depth in ticks at runtime.
 *
 * The host loop re-arms at the top of its next iteration. Arming checks
 * that every enabled mod declares a runahead capability and allocates the
 * speculation snapshot; a refusal sets the depth to 0. A change of depth
 * resets the interpolation ring, so presents snap instead of pairing
 * states from the two timelines. openpete_runahead_get returns the applied
 * depth.
 * @param n Depth, clamped to 0..3; 0 disables runahead. */
void openpete_runahead_set(int n);
/** @brief Request a rewind window in seconds at runtime.
 *
 * The host loop applies it at the top of its next iteration through
 * openpete_rewind_configure, which frees and reallocates the keyframe ring
 * and its snapshots; the loop also writes those snapshots, so the resize
 * runs on its thread and never under a capture. Applying clears the rewind
 * history.
 * @param seconds Window length; 0 or less disables rewind. */
void openpete_rewind_window_set(int seconds);
/** @brief Request the sim paused or resumed, as the pause key does.
 *
 * The host loop applies it where it reads the pause key, with the same
 * audio mute and rumble silence; a request that matches the current state
 * changes nothing.
 * @param on Nonzero pauses, 0 resumes. */
void openpete_sim_pause_request(int on);
/** @brief Request one tick while the sim is paused, as the step key does.
 *
 * The host loop consumes the request at its next iteration; a request made
 * while the sim runs is dropped there. */
void openpete_sim_step_request(void);
/** @brief Report whether the host loop has the sim paused; callable from any thread.
 *
 * @return 1 while paused, else 0. */
int  openpete_sim_paused(void);
/** @brief Return the runahead depth the host loop applied; callable from any thread.
 *
 * @return Depth in ticks, 0 when disarmed. */
int  openpete_runahead_get(void);

/** @brief Mute or unmute host audio at runtime.
 *
 * The request is applied at the top of the host loop's next iteration,
 * on its thread. It sets the gains --mute applies at boot: the SPU mixer mute, XA gain,
 * music override gain and mod one-shot sounds, all held muted while either
 * this baseline or the skip-to-level fast-forward mute is set. Leaving the
 * pause toggle re-applies the baseline to the SPU mute. Host audio only.
 * @param on Nonzero to mute. */
void openpete_set_mute(int on);
/** @brief Return the mute baseline last requested or applied; callable from any thread.
 *
 * @return 1 when muted. */
int  openpete_get_mute(void);

/* Live in-game volume-slider presets (0..10, -1 = leave the game's slider
 * alone) - the --sfx / --music values, re-settable at runtime. These hold a
 * host-side REQUEST only: the per-game audio-volume feature reads them on its
 * own tick hook and performs every guest write there, so a UI-thread edit
 * never touches gameplay-readable RAM off-schedule. */
/** @brief Set the requested in-game sound-effects slider level, the --sfx value.
 *
 * Stores a host-side request only; the per-game audio-volume feature writes
 * the game's volume on its tick hook.
 * @param v Level 0..10, values above 10 read as 10; negative stores -1,
 *        which leaves the game's slider alone. */
void openpete_set_sfx_volume(int v);
/** @brief Return the requested in-game sound-effects slider level.
 *
 * @return 0..10, or -1 to leave the game's slider alone. */
int  openpete_get_sfx_volume(void);
/** @brief Set the requested in-game music slider level, the --music value.
 *
 * Stores a host-side request only; the per-game audio-volume feature writes
 * the game's volume on its tick hook.
 * @param v Level 0..10, values above 10 read as 10; negative stores -1,
 *        which leaves the game's slider alone. */
void openpete_set_music_volume(int v);
/** @brief Return the requested in-game music slider level.
 *
 * @return 0..10, or -1 to leave the game's slider alone. */
int  openpete_get_music_volume(void);

/** @brief Set the requested controller-vibration strength, the --vibration value.
 *
 * Stores a host-side request with openpete_rumble_set_strength: the
 * per-game vibration feature writes the game's enable flag on its tick
 * hook, and the pump thread scales motor amplitude by it.
 * @param v Strength 0..10, values above 10 read as 10; negative stores -1,
 *        which leaves the game's own setting alone. */
void openpete_set_vibration(int v);
/** @brief Return the requested controller-vibration strength.
 *
 * @return The value openpete_rumble_get_strength holds: 0..10, or -1 when
 *         unset. */
int  openpete_get_vibration(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_H */
