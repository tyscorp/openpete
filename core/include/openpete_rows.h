/* openpete_rows.h - growth of a flat row table held as a typed pointer and a capacity.
 *
 * A loader that appends parsed rows to `T* rows` with a separate count and
 * capacity asks for room before each append; the table either holds the
 * rows asked for or is left exactly as it was, so a failed growth never
 * loses the block or leaves a capacity the block does not have. Tables
 * with a census or a stable-address need use mod_vec.h instead.
 */
#ifndef OPENPETE_ROWS_H
#define OPENPETE_ROWS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/** @brief Grow a row table to hold at least @p need rows, doubling its capacity from @p first.
 *
 * @p rows and @p *cap are unchanged when the table already holds @p need
 * rows (the table is returned), and when the request overflows a 32-bit
 * row count or a size_t byte count or the allocation fails (NULL is
 * returned; the caller still owns @p rows).
 * @param rows The table, or NULL while @p *cap is 0.
 * @param cap Its capacity in rows, raised on a growth.
 * @param need Rows the table must hold, at least 1.
 * @param elem Row size in bytes, nonzero.
 * @param first Capacity of the first allocation, nonzero.
 * @return The table, moved or not, or NULL on failure. */
static inline void* openpete_rows_reserve(void* rows, uint32_t* cap, uint64_t need, size_t elem, uint32_t first) {
    if (need <= *cap) return rows;
    uint64_t n = *cap ? *cap : first;
    while (n < need) n *= 2u;
    if (n > UINT32_MAX || n > SIZE_MAX / elem) return NULL;
    void* grown = realloc(rows, (size_t)n * elem);
    if (!grown) return NULL;
    *cap = (uint32_t)n;
    return grown;
}

#endif
