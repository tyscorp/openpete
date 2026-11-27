/* core/mods/mod_modset.c - the save modset tags: the order-free set form
 * that cards carry and the ordered form that savestates carry, the order
 * hash, and the savestate comparison of a stored tag against the running
 * modset (contract in openpete_mods.h). Pure code: no loader state. */
#include "openpete_mods.h"

#include "openpete_fnv1a64.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "#" and 16 hex digits: the order hash that ends an ordered tag. */
#define MS_HASH_CHARS 17

/* Appender over a caller buffer: writes what fits, counts everything. */
typedef struct {
    char*  out;
    size_t cap;
    size_t len;
} ms_buf_t;

/** @brief Append a string, counting every byte whether or not it fits. */
static void ms_puts(ms_buf_t* b, const char* s) {
    for (; s && *s; s++) {
        if (b->out && b->len + 1 < b->cap) b->out[b->len] = *s;
        b->len++;
    }
}

/** @brief NUL-terminate the buffer at the written length or its last byte, and return the full length. */
static size_t ms_finish(ms_buf_t* b) {
    if (b->out && b->cap) b->out[b->len < b->cap ? b->len : b->cap - 1] = '\0';
    return b->len;
}

/** @brief Append the set form of @p n members, ids sorted. */
static void ms_set(ms_buf_t* b, const openpete_modset_member_t* m, int n) {
    int* ix = n > 0 ? (int*)malloc((size_t)n * sizeof *ix) : NULL;
    if (n > 0 && !ix) {
        ms_puts(b, "(modset tag allocation failed)");
        return;
    }
    for (int i = 0; i < n; i++) {
        int j = i - 1;
        while (j >= 0 && strcmp(m[ix[j]].id, m[i].id) > 0) {
            ix[j + 1] = ix[j];
            j--;
        }
        ix[j + 1] = i;
    }
    for (int i = 0; i < n; i++) {
        if (i) ms_puts(b, "+");
        ms_puts(b, m[ix[i]].id);
        ms_puts(b, "@");
        ms_puts(b, m[ix[i]].version);
    }
    free(ix);
}

size_t openpete_modset_format_set(char* out, size_t cap,
                                  const openpete_modset_member_t* m, int n) {
    ms_buf_t b = {out, cap, 0};
    ms_set(&b, m, n);
    return ms_finish(&b);
}

size_t openpete_modset_format_order(char* out, size_t cap,
                                    const openpete_modset_member_t* m, int n,
                                    uint64_t order_hash) {
    ms_buf_t b = {out, cap, 0};
    char hash[MS_HASH_CHARS + 1];
    if (n <= 0) return ms_finish(&b);
    snprintf(hash, sizeof hash, "#%016" PRIx64, order_hash);
    ms_puts(&b, OPENPETE_MODSET_ORDER_PREFIX);
    ms_set(&b, m, n);
    ms_puts(&b, hash);
    return ms_finish(&b);
}

uint64_t openpete_modset_order_hash(const openpete_modset_event_t* ev,
                                    int n) {
    uint64_t h = openpete_fnv1a64_init();
    for (int i = 0; i < n; i++) {
        /* Each field at a fixed width, the id length first: the byte
         * stream names one event sequence only. */
        const uint32_t idn = (uint32_t)strlen(ev[i].id);
        const uint32_t w[4] = {(uint32_t)ev[i].kind, idn, ev[i].addr,
                               ev[i].overlay};
        for (int k = 0; k < 4; k++) {
            const uint8_t le[4] = {(uint8_t)w[k], (uint8_t)(w[k] >> 8),
                                   (uint8_t)(w[k] >> 16),
                                   (uint8_t)(w[k] >> 24)};
            h = openpete_fnv1a64_update(h, le, 4);
        }
        h = openpete_fnv1a64_update(h, ev[i].id, idn);
    }
    return h;
}

openpete_modset_verdict_t openpete_modset_compare(const char* saved,
                                                  const char* now_order,
                                                  const char* now_set) {
    const size_t pl = strlen(OPENPETE_MODSET_ORDER_PREFIX);
    saved     = saved ? saved : "";
    now_order = now_order ? now_order : "";
    now_set   = now_set ? now_set : "";
    if (strncmp(saved, OPENPETE_MODSET_ORDER_PREFIX, pl) != 0)
        return strcmp(saved, now_set) == 0 ? OPENPETE_MODSET_SET_MATCH
                                           : OPENPETE_MODSET_SET_DIFFERS;
    if (strcmp(saved, now_order) == 0) return OPENPETE_MODSET_MATCH;
    /* The set part sits between the prefix and the fixed-width hash. */
    const size_t n = strlen(saved);
    if (n <= pl + MS_HASH_CHARS || saved[n - MS_HASH_CHARS] != '#')
        return OPENPETE_MODSET_SET_DIFFERS;
    const size_t set_n = n - pl - MS_HASH_CHARS;
    return (strlen(now_set) == set_n &&
            strncmp(saved + pl, now_set, set_n) == 0)
               ? OPENPETE_MODSET_ORDER_DIFFERS
               : OPENPETE_MODSET_SET_DIFFERS;
}
