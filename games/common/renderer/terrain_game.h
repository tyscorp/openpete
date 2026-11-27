/* terrain_game.h - the per-game data the shared terrain feeder consults.
 *
 * The walk in renderer/terrain.c reads every engine address from game_layout;
 * what it cannot read from RAM is a game's own data: the portal-sky opening
 * planes the portal feeder derives (the OT-conformance clamp), the frozen list
 * of LP faces allowed to back-fill sightlines, the level-name spelling of the
 * fill-tag verdict files, and authored-data art corrections. A game supplies
 * them through `terrain_game`; a game with none of them names no table and
 * binds the inert provider (games/common/game_host_desc.c). */
#ifndef OPENPETE_COMMON_RENDERER_TERRAIN_GAME_H
#define OPENPETE_COMMON_RENDERER_TERRAIN_GAME_H
#include <stddef.h>
#include <stdint.h>
#include "psx_runtime_internal.h"
#include "renderer/native_gte.h"     /* ngte_view: the caller's projection view */
#include "openpete_thread_roles.h"   /* OP_SLOT */
#ifdef __cplusplus
extern "C" {
#endif

/* One active portal-sky opening plane, in screen space: 1/view-Z of the
 * opening plane at pixel (x, y) is a·x + b·y + c (full-world view-Z), valid
 * where the result is > 0 (the plane is in front of the camera along that
 * pixel's ray). x0..y1 is the exact min/max screen bbox of the projected
 * outline (no inflation) - the conformance clamp only considers faces
 * touching it. When an outline point projects behind the camera the bbox
 * degenerates to full-screen. */
typedef struct {
    uint32_t portal;                /* owning portal record (identity/debug) */
    double   a, b, c;               /* screen-space inv-vz field of the plane */
    double   back_vz;               /* farthest outline view-Z (the sky's behind-
                                     * ring spill depth base) */
    double   ot_vz;                 /* 128 x the sky's mirrored .s OT bucket B
                                     * (mean outline SZ >> 7, +64 bump) - the
                                     * depth the .s actually inserts the sky at.
                                     * Terrain faces whose OT mean sorts behind
                                     * THIS are drawn before the sky in the walk
                                     * (sky covers them whole-face). 0 if
                                     * unavailable. */
    double   push_vz;               /* world-unit view-Z push the sky RENDERS
                                     * with (the .s's +64-slot far-bucket bump =
                                     * 8192; 0 for near portals). Sky-relative
                                     * depth constructions - the conformance
                                     * sink target - must add it so covered
                                     * faces still land just BEHIND the sky. */
    float    x0, y0, x1, y1;        /* outline screen bbox (broadphase) */
    /* The projected outline POLYGON. The conformance sink is confined to
     * faces whose screen centroid is INSIDE it - that is where the sky
     * actually rasters (fan/dome) and wins; sinking a face outside it just
     * punches a hole to whatever world geometry is behind. */
    float    px[16], py[16];
    int      pn;
    /* The outline in VIEW space, transformed straight from the portal's world
     * points without going through the projection. The screen arrays above
     * exist only when EVERY outline point is in front of the camera; view
     * space is a rigid transform of world space, so these are always valid
     * and any distance or containment measured from them is the world-space
     * answer. */
    double   vx[16], vy[16], vz[16];
    int      vn;                    /* outline points in view space (0 = none) */
    /* What the terrain feed does with a face the rim classifier accepts
     * under this plane. 0: drops it whole. 1: keeps it and draws it through
     * the renderer's inner-rim pipelines, whose fragment stage discards it
     * inside the screen outline (px, py) only, so the preview paints there
     * and the face stays drawn outside the opening. */
    int      rim_fragment;
} openpete_portal_skyplane;

typedef struct terrain_game {
    /* True while a portal fed sky within the last couple of presents. */
    int   (*portal_sky_recent)(void) OP_SLOT(sim);
    /* Fill out[] with the opening planes of every portal whose sky was drawn
     * on the current or immediately-previous canonical frame; returns the
     * count (0 when no portal sky is active). */
    int   (*portal_sky_planes)(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max) OP_SLOT(sim);
    /* Plane-cover near margin M in world units: inside an opening's footprint
     * the preview covers everything at or behind plane − M. */
    float (*portal_plane_margin)(void);
    /* The frozen LP fill-face list: 1 when (level id, sector, LP face) may
     * back-fill sightlines no authored HP covers. */
    int   (*fill_tag)(int level, int sec, int face);
    /* The level's name as the fill-tag verdict files spell it. */
    void  (*level_name)(int level, char* out, size_t cap);
    /* Authored-data colour corrections keyed on (level id, sector, LP face):
     * 1 and *rgb (0x00BBGGRR) when the face is corrected. */
    int   (*lp_artfix)(int level, int sec, int face, uint32_t* rgb);
    /* 1 when the world draw now running is followed, in the same draw, by
     * the portal draw whose capture portal_sky_planes reads, and that draw
     * has not run yet this tick: the terrain's native build and feed then
     * wait for terrain_native_flush_deferred, so both read the capture this
     * tick's draw produces. NULL = never. */
    int   (*env_feed_waits_for_portals)(CPUState* cpu);

    /* 1 when the world tick N stopped drawing is still whole in guest RAM and
     * VRAM, by the game's own load sequence, so a held camera cut may walk it
     * for tick N-1. NULL: a held cut keeps tick N's drawn verdict. */
    int   (*world_outlives_draw)(CPUState* cpu);
} terrain_game_t;

/* The running game's table: game_runtime_install (game_host_desc.h) copies
 * the registered descriptor's table here before any feeder runs. */
extern terrain_game_t terrain_game;

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_TERRAIN_GAME_H */
