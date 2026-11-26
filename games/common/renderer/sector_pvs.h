/* games/common/renderer/sector_pvs.h - the Spyro 2/3 sector PVS lookup.
 *
 * The level's potentially-visible-set octree (Spyro 2 func_80048D18, Spyro 3
 * func_8002D0D8) is keyed on the camera position shifted right 11, so one
 * leaf cell spans 2048 world units on each axis. Each node on the descent
 * hides its first strictly increasing run of sector bytes and marks the rest
 * visible. The lookup is evaluated host-side over a copy of the tree bytes, at
 * any position, and writes no guest memory. */

#ifndef OPENPETE_SECTOR_PVS_H
#define OPENPETE_SECTOR_PVS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The edge of one PVS leaf cell in world units (the lookup's shift of 11). */
#define SECTOR_PVS_CELL 2048

/** @brief Computes the sectors the PVS octree marks visible from a position.
 *
 * From the root at depth 7 each node clears its first, strictly increasing
 * run of sector bytes and marks the rest visible; the descent takes the child
 * octant the position falls in and ends at depth 0 or at a node without
 * children. A missing child, a zero-length node or a read past len makes every
 * sector visible, as the game's walk does.
 * @param b The bytes holding the tree.
 * @param len Their length.
 * @param tree The offset of the root node in b.
 * @param pos The position, world units.
 * @param vis Receives 256 bytes, 0xFF for a visible sector, 0 for a hidden one.
 * @param open Receives 1 when the walk ended on an all-visible exit (an
 *        empty or -1 tree, a missing child, a malformed node), 0 when a node
 *        decided the list; NULL when not wanted.
 * @return The number of visible sectors. */
int sector_pvs_lookup(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t pos[3],
                      uint8_t vis[256], int* open);

/** @brief Looks the tree up one lattice step from a position, open outside the 128-cell lattice.
 *
 * The point is `p` plus `d` cells on each axis; outside world 0 to 262143 on
 * an axis it is open without a lookup (the descent's coordinate would wrap
 * into another cell).
 * @param b The bytes holding the tree.
 * @param len Their length.
 * @param tree The offset of the root node in b.
 * @param p The position, world units.
 * @param d The step in cells.
 * @param vis Receives the point's 256 visibility bytes, all 0 at an open point.
 * @return 1 when the point is open, else 0. */
int sector_pvs_lookup_step(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t p[3], const int d[3],
                           uint8_t vis[256]);

/** Lattice points of the 3x3x3 ring around a cell: index (dx+1)*9 + (dy+1)*3
 * + (dz+1), the centre at SECTOR_PVS_CENTRE, the opposite of k at 26 - k. */
#define SECTOR_PVS_RING   27
#define SECTOR_PVS_CENTRE 13

/** @brief Looks the tree up at the 27 lattice points one cell around `p`.
 *
 * A point is open when its lookup takes the all-visible exit or lies outside
 * the tree's 128-cell lattice (world 0 to 262143 on each axis, past which the
 * descent's coordinate would wrap into another cell); an open point's list
 * is all 0.
 * @param b The bytes holding the tree.
 * @param len Their length.
 * @param tree The offset of the root node in b.
 * @param p The position, world units.
 * @param nb Receives each point's 256 visibility bytes.
 * @param open Receives 1 for an open point, else 0. */
void sector_pvs_ring(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t p[3],
                     uint8_t nb[SECTOR_PVS_RING][256], uint8_t open[SECTOR_PVS_RING]);

/** @brief Marks in `out` the sectors one of the 13 opposite pairs of a ring admits.
 *
 * A pair (k, 26 - k) admits a sector both points list; with `open_agrees`
 * also a sector one point lists while the other is open. An open point
 * admits nothing alone. `out` is only set, never cleared.
 * @param nb The ring's lists (sector_pvs_ring).
 * @param open The ring's open flags.
 * @param open_agrees Nonzero counts an open partner as listing the sector.
 * @param n The sector count to test, at most 256.
 * @param out The set to add to, 0xFF per admitted sector. */
void sector_pvs_pairs(const uint8_t nb[SECTOR_PVS_RING][256], const uint8_t open[SECTOR_PVS_RING],
                      int open_agrees, uint32_t n, uint8_t out[256]);

/** The column rule's reach: the cells straight above a position tested for
 * an open one, and the cells straight below whose lists it admits. */
#define SECTOR_PVS_COLUMN_GATE  2
#define SECTOR_PVS_COLUMN_DEPTH 4

/** @brief Admits what the cells straight below a position list when a cell straight above it is open.
 *
 * When one of the SECTOR_PVS_COLUMN_GATE cells straight above `p`'s cell (+Z)
 * is open, every sector one of the SECTOR_PVS_COLUMN_DEPTH cells straight
 * below it lists is set in `out`. A camera at the top of the tree looks down
 * past a cell whose list was computed from below the surface; the cells under
 * it list what that view reaches. `out` is only set, never cleared.
 * @param b The bytes holding the tree.
 * @param len Their length.
 * @param tree The offset of the root node in b.
 * @param p The position, world units.
 * @param n The sector count to test, at most 256.
 * @param out The set to add to, 0xFF per admitted sector.
 * @return The sectors set that `out` did not hold, -1 when no cell above is open. */
int sector_pvs_column(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t p[3], uint32_t n,
                      uint8_t out[256]);

struct CPUState;

/* A host copy of one PVS tree and the key it was taken under. */
typedef struct {
    uint8_t* b;           /* the tree bytes (NULL: no tree, or the lookup is off) */
    uint32_t len, cap;
    uint32_t ptr, size;   /* the tree pointer and its size word */
    uint32_t flag;        /* the lookup flag, env.base + 0x10 (0: lookup off) */
    int32_t  level, area; /* level id and sub-area word at the snapshot */
    uint32_t state;       /* the game-state word at the snapshot */
    uint64_t hash;        /* FNV-1a over the tree bytes */
    unsigned gen;         /* bumped when the tree content changes */
} sector_pvs_tree_t;

/** @brief Re-copies a PVS tree when its key changed, and re-hashes it then or when the game-state word changed.
 *
 * The key is the tree pointer, its size word, the lookup flag at
 * env.base + 0x10, the level id and the sub-area word (env.area). A load
 * that reuses the buffer at the same address and size changes the
 * game-state word on its way in and out, so a change of that word
 * re-hashes the tree; a hash change bumps the generation. A zero flag or an
 * absent tree empties the copy (the game draws every sector).
 * @param T The snapshot.
 * @param cpu The guest CPU state, read only.
 * @param ptr The tree pointer (the word at env.occ_groups for the terrain,
 *        at cyc.occ_groups for the sky).
 * @return 1 when the key or state changed (a refresh), else 0. */
int sector_pvs_tree_refresh(sector_pvs_tree_t* T, struct CPUState* cpu, uint32_t ptr);

/* The --sector-visibility filled set over two ticks for one tree. */
typedef struct {
    uint8_t cur[256], prev[256];  /* C' of the current and the previous tick */
    uint8_t u[256];               /* U, the drawn set */
    int32_t cur_cell[3], prev_cell[3];
    unsigned cur_gen, prev_gen;
    int cur_ok, prev_ok;
    int column;                   /* nonzero: C' also takes sector_pvs_column */
    int col_added;                /* the sectors the column added to C' at the last recompute (-1: gate closed) */
} sector_pvs_filled_t;

/* sector_pvs_filled_step's result bits. */
#define SECTOR_PVS_COMPUTED 1   /* C' was recomputed (a new cell or tree) */
#define SECTOR_PVS_UNIONED  2   /* the previous tick's C' joined U */

/** @brief Advances the filled set one tick: C' at the tick camera's cell, U over two ticks.
 *
 * C' (`base` plus the opposite-pair admissions, an open partner agreeing,
 * plus sector_pvs_column's admissions when F->column is set) is recomputed
 * when the cell or the tree generation changed, else the previous tick's C'
 * carries over. U is C' united with the previous tick's
 * C' when the two cells are within Chebyshev distance 1 and the generation
 * did not change. Out-of-lattice points are open (sector_pvs_ring).
 * @param F The filled state.
 * @param T The tree snapshot, with a tree.
 * @param p The tick camera position, world units.
 * @param n The sector count, at most 256.
 * @param base The game's verdict, 0xFF per visible sector (read on a recompute).
 * @return SECTOR_PVS_COMPUTED and SECTOR_PVS_UNIONED bits. */
int sector_pvs_filled_step(sector_pvs_filled_t* F, const sector_pvs_tree_t* T, const int32_t p[3],
                           uint32_t n, const uint8_t base[256]);

/** @brief Drops both ticks' sets (no tree), so the next step neither reuses nor unites them.
 * @param F The filled state. */
void sector_pvs_filled_reset(sector_pvs_filled_t* F);

#ifdef __cplusplus
}
#endif

#endif
