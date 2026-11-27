/* games/common/renderer/sector_pvs.c - see sector_pvs.h. */
#include "renderer/sector_pvs.h"

#include <stdlib.h>

#include "game_layout.h"
#include "psx_runtime_internal.h"

#include <string.h>

/** @brief Walks one PVS node's sector bytes: the first strictly increasing run hidden, the rest visible, 0xFF bytes skipped.
 * @param b The tree bytes.
 * @param v The first sector byte's offset.
 * @param end The node's end offset.
 * @param vis The visibility table. */
static void spvs_node(const uint8_t* b, uint32_t v, uint32_t end, uint8_t vis[256]) {
    if (v < end) {
        uint32_t a = b[v];
        while (v != end && a != 0xFFu) {
            vis[a] = 0;
            const uint32_t prev = a;
            if (v + 1u >= end) { v++; break; }
            a = b[v + 1u];
            v++;
            if ((int32_t)prev - (int32_t)a >= 0) break;
        }
    }
    for (; v < end; v++)
        if (b[v] != 0xFFu) vis[b[v]] = 0xFF;
}

/** @brief Steps the descent's node coordinates one level down and returns the octant the position falls in.
 * @param p The position shifted right 11, x y z.
 * @param c The node coordinates, doubled and advanced into the octant.
 * @param depth The depth being left, 7 at the root.
 * @return The octant, x bit 4, y bit 2, z bit 1. */
static uint32_t spvs_octant(const uint32_t p[3], uint32_t c[3], int depth) {
    uint32_t oct = 0;
    for (int a = 0; a < 3; a++) {
        c[a] <<= 1;
        if ((int64_t)p[a] - (int64_t)((((uint64_t)c[a] + 1u) << depth) >> 1) >= 0) { c[a]++; oct |= 4u >> a; }
    }
    return oct;
}

/** @brief Returns the offset of a node's child in an octant: past the node's children offset, then past each lower present sibling's length byte.
 * @param b The tree bytes.
 * @param len Their length.
 * @param t The node's offset.
 * @param mask The node's child mask.
 * @param oct The octant, present in the mask.
 * @return The child's offset; past len when the bytes end. */
static uint32_t spvs_child(const uint8_t* b, uint32_t len, uint32_t t, uint32_t mask, uint32_t oct) {
    t += (uint32_t)b[t + 2u] | ((uint32_t)b[t + 3u] << 8);
    for (uint32_t i = 0; i < oct && t < len; i++)
        if (mask & (1u << i)) t += b[t];
    return t;
}

/** @brief Descends the tree to the node that decides a position, filling its visibility table and the node's cell span.
 * @param b The tree bytes.
 * @param len Their length.
 * @param t The root node's offset.
 * @param pos The position, world units.
 * @param vis Receives 256 bytes, 0xFF for a visible sector, 0 for a hidden one; all 0xFF on an open exit.
 * @param leaf Receives the deciding node's first cell x y z and its edge in cells; untouched on an open exit.
 * @return 1 on an all-visible exit (an empty or -1 tree, a missing child, a malformed node), 0 when a node decided. */
static int spvs_descend(const uint8_t* b, uint32_t len, uint32_t t, const int32_t pos[3], uint8_t vis[256],
                        uint32_t leaf[4]) {
    memset(vis, 0xFF, 256);
    if (len < 4u || t > len - 4u
        || ((uint32_t)b[t] | ((uint32_t)b[t + 1u] << 8) | ((uint32_t)b[t + 2u] << 16)
            | ((uint32_t)b[t + 3u] << 24)) == 0xFFFFFFFFu)
        return 1;
    memset(vis, 0, 256);
    const uint32_t p[3] = { (uint32_t)pos[0] >> 11, (uint32_t)pos[1] >> 11, (uint32_t)pos[2] >> 11 };
    uint32_t c[3] = { 0, 0, 0 };
    int depth = 7;
    for (; ; depth--) {
        if (t > len - 4u || b[t] == 0u || t + b[t] > len) { memset(vis, 0xFF, 256); return 1; }
        const uint32_t mask = b[t + 1u], end = t + b[t];
        spvs_node(b, t + (depth ? (mask ? 4u : 2u) : 1u), end, vis);
        if (depth == 0 || mask == 0) break;
        const uint32_t oct = spvs_octant(p, c, depth);
        if (!(mask & (1u << oct))) { memset(vis, 0xFF, 256); return 1; }
        t = spvs_child(b, len, t, mask, oct);
    }
    for (int a = 0; a < 3; a++) leaf[a] = c[a] << depth;
    leaf[3] = 1u << depth;
    return 0;
}

int sector_pvs_lookup(const uint8_t* b, uint32_t len, uint32_t t, const int32_t pos[3],
                      uint8_t vis[256], int* open) {
    uint32_t leaf[4];
    const int op = spvs_descend(b, len, t, pos, vis, leaf);
    if (open) *open = op;
    if (op) return 256;
    int n = 0;
    for (int i = 0; i < 256; i++) n += vis[i] == 0xFF;
    return n;
}

int sector_pvs_lookup_step(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t p[3], const int d[3],
                           uint8_t vis[256]) {
    int32_t q[3];
    for (int a = 0; a < 3; a++) {
        q[a] = p[a] + d[a] * SECTOR_PVS_CELL;
        if (q[a] < 0 || q[a] >= 128 * SECTOR_PVS_CELL) { memset(vis, 0, 256); return 1; }
    }
    int open = 1;
    sector_pvs_lookup(b, len, tree, q, vis, &open);
    if (open) memset(vis, 0, 256);
    return open;
}

void sector_pvs_ring(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t p[3],
                     uint8_t nb[SECTOR_PVS_RING][256], uint8_t open[SECTOR_PVS_RING]) {
    for (int k = 0; k < SECTOR_PVS_RING; k++) {
        const int d[3] = { k / 9 - 1, (k / 3) % 3 - 1, k % 3 - 1 };
        open[k] = (uint8_t)sector_pvs_lookup_step(b, len, tree, p, d, nb[k]);
    }
}

int sector_pvs_column(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t p[3], uint32_t n,
                      uint8_t out[256]) {
    uint8_t vis[256];
    int gate = 0;
    for (int h = 1; h <= SECTOR_PVS_COLUMN_GATE && !gate; h++) {
        const int d[3] = { 0, 0, h };
        gate = sector_pvs_lookup_step(b, len, tree, p, d, vis);
    }
    if (!gate) return -1;
    if (n > 256u) n = 256u;
    int added = 0;
    for (int k = 1; k <= SECTOR_PVS_COLUMN_DEPTH; k++) {
        const int d[3] = { 0, 0, -k };
        if (sector_pvs_lookup_step(b, len, tree, p, d, vis)) continue;
        for (uint32_t i = 0; i < n; i++) {
            if (!vis[i] || out[i]) continue;
            out[i] = 0xFF;
            added++;
        }
    }
    return added;
}

int sector_pvs_leaf_ring(const uint8_t* b, uint32_t len, uint32_t tree, const int32_t p[3], uint32_t n,
                         uint8_t out[256]) {
    uint8_t nb[SECTOR_PVS_RING][256], open[SECTOR_PVS_RING], add[256];
    uint32_t leaf[4];
    for (int a = 0; a < 3; a++)
        if (p[a] < 0 || p[a] >= 128 * SECTOR_PVS_CELL) return -1;
    if (spvs_descend(b, len, tree, p, add, leaf) || leaf[3] < 2u) return -1;
    static const int z[3] = { 0, 0, 0 };
    for (int k = 0; k < SECTOR_PVS_RING; k++) {
        const int d[3] = { k / 9 - 1, (k / 3) % 3 - 1, k % 3 - 1 };
        int32_t q[3];
        for (int a = 0; a < 3; a++) {
            const int32_t c = d[a] == 0 ? p[a] >> 11
                              : d[a] > 0 ? (int32_t)(leaf[a] + leaf[3]) : (int32_t)leaf[a] - 1;
            q[a] = c * SECTOR_PVS_CELL;
        }
        open[k] = (uint8_t)sector_pvs_lookup_step(b, len, tree, q, z, nb[k]);
    }
    if (n > 256u) n = 256u;
    memset(add, 0, sizeof add);
    sector_pvs_pairs((const uint8_t (*)[256])nb, open, 1, n, add);
    int added = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!add[i] || out[i]) continue;
        out[i] = 0xFF;
        added++;
    }
    return added;
}

void sector_pvs_pairs(const uint8_t nb[SECTOR_PVS_RING][256], const uint8_t open[SECTOR_PVS_RING],
                      int open_agrees, uint32_t n, uint8_t out[256]) {
    if (n > 256u) n = 256u;
    for (int k = 0; k < SECTOR_PVS_CENTRE; k++) {
        const int o = SECTOR_PVS_RING - 1 - k;
        const int ao = open_agrees && open[k], bo = open_agrees && open[o];
        for (uint32_t i = 0; i < n; i++)
            if ((nb[k][i] && (nb[o][i] || bo)) || (nb[o][i] && ao)) out[i] = 0xFF;
    }
}

int sector_pvs_tree_refresh(sector_pvs_tree_t* T, struct CPUState* cpu, uint32_t ptr) {
    const uint32_t flag = game_layout.env.base ? cpu->read_word(game_layout.env.base + 0x10u) : 0u;
    const uint32_t size = (ptr >= 0x80000004u) ? cpu->read_word(ptr - 4u) : 0u;
    const int32_t level = game_layout.level_id ? (int32_t)cpu->read_word(game_layout.level_id) : 0;
    const int32_t area = game_layout.env.area ? (int32_t)cpu->read_word(game_layout.env.area) : 0;
    const uint32_t state = game_layout.game_state ? cpu->read_word(game_layout.game_state) : 0u;
    const int key_same = T->ptr == ptr && T->size == size && T->flag == flag && T->level == level
                         && T->area == area;
    if (key_same && T->state == state && (T->b || !flag || size < 9u)) return 0;
    T->ptr = ptr; T->size = size; T->flag = flag; T->level = level; T->area = area; T->state = state;
    if (!flag || ptr < 0x80000004u || size < 9u) {
        if (T->b || T->len) T->gen++;
        free(T->b);
        T->b = NULL;
        T->len = T->cap = 0;
        T->hash = 0;
        return 1;
    }
    const uint32_t len = size > 262144u ? 262144u : size;
    if (len > T->cap) {
        uint8_t* nb = (uint8_t*)realloc(T->b, len);
        if (!nb) return 1;
        T->b = nb;
        T->cap = len;
    }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t k = 0; k < len; k++) {
        T->b[k] = cpu->read_byte(ptr + k);
        h = (h ^ T->b[k]) * 1099511628211ull;
    }
    if (h != T->hash || len != T->len) T->gen++;
    T->hash = h;
    T->len = len;
    return 1;
}

int sector_pvs_filled_step(sector_pvs_filled_t* F, const sector_pvs_tree_t* T, const int32_t p[3],
                           uint32_t n, const uint8_t base[256]) {
    const int32_t cell[3] = { p[0] >> 11, p[1] >> 11, p[2] >> 11 };
    int r = 0;
    memcpy(F->prev, F->cur, sizeof F->prev);
    memcpy(F->prev_cell, F->cur_cell, sizeof F->prev_cell);
    F->prev_gen = F->cur_gen;
    F->prev_ok = F->cur_ok;
    /* The game's verdict changes only with the cell; recompute on a new cell
     * or tree, else the previous tick's C' is this tick's too. */
    if (!F->cur_ok || F->cur_gen != T->gen || memcmp(cell, F->cur_cell, sizeof cell)) {
        uint8_t nb[SECTOR_PVS_RING][256], open[SECTOR_PVS_RING];
        memcpy(F->cur, base, sizeof F->cur);
        sector_pvs_ring(T->b, T->len, 0, p, nb, open);
        sector_pvs_pairs((const uint8_t (*)[256])nb, open, 1, n, F->cur);
        F->col_added = F->column ? sector_pvs_column(T->b, T->len, 0, p, n, F->cur) : -1;
        F->leaf_added = F->leaf ? sector_pvs_leaf_ring(T->b, T->len, 0, p, n, F->cur) : -1;
        memcpy(F->cur_cell, cell, sizeof cell);
        F->cur_gen = T->gen;
        F->cur_ok = 1;
        r |= SECTOR_PVS_COMPUTED;
    }
    memcpy(F->u, F->cur, sizeof F->u);
    int near = F->prev_ok && F->prev_gen == F->cur_gen;
    for (int a = 0; a < 3 && near; a++) near = abs(F->prev_cell[a] - F->cur_cell[a]) <= 1;
    if (near) {
        for (int i = 0; i < 256; i++) F->u[i] |= F->prev[i];
        r |= SECTOR_PVS_UNIONED;
    }
    return r;
}

void sector_pvs_filled_reset(sector_pvs_filled_t* F) {
    F->cur_ok = F->prev_ok = 0;
}
