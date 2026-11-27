/* rhi_mip.c - CPU construction of RGBA8 mip chains (see rhi.h). */

#include "rhi.h"

#include <stdlib.h>
#include <string.h>

uint32_t rhi_mip_levels(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    for (uint32_t m = width > height ? width : height; m > 1; m >>= 1) levels++;
    return levels;
}

size_t rhi_rgba8_mip_chain_bytes(uint32_t width, uint32_t height)
{
    size_t bytes = 0;
    const uint32_t levels = rhi_mip_levels(width, height);
    for (uint32_t i = 0; i < levels; i++) {
        bytes += (size_t)width * height * 4u;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    return bytes;
}

/* One destination texel's taps along an axis: the first source texel and the
 * weights of four consecutive source texels in 1/256, summing to 256. A tap
 * past the span's end carries weight 0 and reads the last texel. */
typedef struct { uint32_t idx[4]; uint32_t w[4]; } mip_taps;

/** @brief Compute the box taps of a span of src texels resampled to dst texels, quantized to 1/256.
 *
 * Destination texel i covers source [i*src/dst, (i+1)*src/dst). Measured in
 * units of 1/dst of a source texel it covers [i*src, (i+1)*src) and source
 * texel t covers [t*dst, (t+1)*dst), so each overlap is an exact integer;
 * the overlaps are scaled to 1/256 by rounding and the largest absorbs the
 * remainder, so the weights sum to exactly 256. A halving (src < 4*dst)
 * touches at most four texels; an even span gets 128 and 128.
 * @param src Source span.
 * @param dst Destination span, max(1, src / 2).
 * @param taps Receives dst entries. */
static void mip_axis_taps(uint32_t src, uint32_t dst, mip_taps *taps)
{
    for (uint32_t i = 0; i < dst; i++) {
        const uint64_t lo = (uint64_t)i * src, hi = (uint64_t)(i + 1) * src;
        const uint32_t first = (uint32_t)(lo / dst);
        mip_taps *t = &taps[i];
        uint32_t sum = 0, big = 0;
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t at = first + k;
            const uint64_t a = (uint64_t)at * dst, b = a + dst;
            const uint64_t l = a > lo ? a : lo, h = b < hi ? b : hi;
            const uint64_t ov = (at < src && h > l) ? h - l : 0u;
            t->idx[k] = at < src ? at : src - 1u;
            t->w[k] = (uint32_t)((ov * 256u + src / 2u) / src);
            sum += t->w[k];
            if (t->w[k] > t->w[big]) big = k;
        }
        t->w[big] = t->w[big] + 256u - sum;
    }
}

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define MIP_SSE2 1
#endif

/** @brief Weight one source row through the horizontal taps into one row of 16-bit sums, four per output texel.
 *
 * Each sum is w0*a + w1*b + w2*c over the tap texels' channel; the weights
 * sum to 256, so a sum is at most 255 * 256 and fits 16 bits exactly. The
 * taps of one output texel are consecutive texels (mip_axis_taps), so with
 * SSE2 the first two load as one 8-byte read and the third as a 4-byte
 * read, and one 16-bit multiply per pair forms the four channel sums.
 * @param s Source row, sw texels RGBA8, sw >= 2.
 * @param tx Horizontal taps, dw entries.
 * @param out Receives dw * 4 sums.
 * @param dw Output width. */
static void mip_row_h(const uint8_t *s, const mip_taps *tx, uint16_t *out, uint32_t dw)
{
#ifdef MIP_SSE2
    const __m128i zero = _mm_setzero_si128();
    for (uint32_t x = 0; x < dw; x++) {
        const mip_taps *t = &tx[x];
        const __m128i ab = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(const void *)(s + t->idx[0] * 4u)), zero);
        int32_t c32;
        memcpy(&c32, s + t->idx[2] * 4u, 4);
        const __m128i c = _mm_unpacklo_epi8(_mm_cvtsi32_si128(c32), zero);
        const __m128i w01 = _mm_set_epi16((short)t->w[1], (short)t->w[1], (short)t->w[1], (short)t->w[1],
                                          (short)t->w[0], (short)t->w[0], (short)t->w[0], (short)t->w[0]);
        const __m128i w2 = _mm_set_epi16(0, 0, 0, 0, (short)t->w[2], (short)t->w[2], (short)t->w[2], (short)t->w[2]);
        __m128i sum = _mm_add_epi16(_mm_mullo_epi16(ab, w01), _mm_mullo_epi16(c, w2));
        sum = _mm_add_epi16(sum, _mm_srli_si128(sum, 8));
        _mm_storel_epi64((__m128i *)(void *)(out + (size_t)x * 4u), sum);
    }
#else
    for (uint32_t x = 0; x < dw; x++) {
        const mip_taps *t = &tx[x];
        for (uint32_t c = 0; c < 4u; c++)
            out[x * 4u + c] = (uint16_t)(t->w[0] * s[t->idx[0] * 4u + c] + t->w[1] * s[t->idx[1] * 4u + c] +
                                         t->w[2] * s[t->idx[2] * 4u + c]);
    }
#endif
}

/** @brief Blend three rows of 16-bit horizontal sums into one output row, rounding once: d = (w0*r0 + w1*r1 + w2*r2 + 32768) >> 16.
 *
 * With SSE2, eight values per step: a 16x16 multiply's low and high halves
 * interleave into 32-bit products, which accumulate exactly (at most
 * 255 * 256 * 256 + 32768). The tail goes one value at a time.
 * @param r Three rows of n sums.
 * @param w Their weights, summing to 256.
 * @param d Output row.
 * @param n Values per row. */
static void mip_row_v(const uint16_t *const r[3], const uint32_t w[3], uint8_t *d, size_t n)
{
    size_t x = 0;
#ifdef MIP_SSE2
    const __m128i round = _mm_set1_epi32(32768);
    for (; x + 8u <= n; x += 8u) {
        __m128i lo32 = round, hi32 = round;
        for (int k = 0; k < 3; k++) {
            const __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(r[k] + x));
            const __m128i wk = _mm_set1_epi16((short)w[k]);
            const __m128i pl = _mm_mullo_epi16(v, wk), ph = _mm_mulhi_epu16(v, wk);
            lo32 = _mm_add_epi32(lo32, _mm_unpacklo_epi16(pl, ph));
            hi32 = _mm_add_epi32(hi32, _mm_unpackhi_epi16(pl, ph));
        }
        const __m128i q = _mm_packs_epi32(_mm_srli_epi32(lo32, 16), _mm_srli_epi32(hi32, 16));
        _mm_storel_epi64((__m128i *)(void *)(d + x), _mm_packus_epi16(q, q));
    }
#endif
    for (; x < n; x++) d[x] = (uint8_t)((32768u + w[0] * r[0][x] + w[1] * r[1][x] + w[2] * r[2][x]) >> 16);
}

/** @brief Write one level of a chain from a level with both spans even, as the rounded mean of each 2x2 block.
 *
 * The same values mip_level's separable pass computes with weights 128 and
 * 128 on both axes, in one pass without the row scratch.
 * @param src Level above, sw x (2 * dh) RGBA8.
 * @param sw Its width, even.
 * @param dst Receives the dw x dh level.
 * @param dw Destination width, sw / 2.
 * @param dh Destination height. */
static void mip_level_even(const uint8_t *src, uint32_t sw, uint8_t *dst, uint32_t dw, uint32_t dh)
{
    const size_t stride = (size_t)sw * 4u;
    const uint32_t lo = 0x00FF00FFu;
    for (uint32_t y = 0; y < dh; y++) {
        const uint8_t *a = src + (size_t)(2u * y) * stride, *b = a + stride;
        uint8_t *d = dst + (size_t)y * dw * 4u;
        for (uint32_t x = 0; x < dw; x++) {
            /* Two 16-bit lanes per word (channels 0 and 2, then 1 and 3):
             * four bytes sum to at most 1020, so no lane carries. */
            uint32_t p[4];
            memcpy(&p[0], a + 8u * x, 4); memcpy(&p[1], a + 8u * x + 4u, 4);
            memcpy(&p[2], b + 8u * x, 4); memcpy(&p[3], b + 8u * x + 4u, 4);
            const uint32_t even = (p[0] & lo) + (p[1] & lo) + (p[2] & lo) + (p[3] & lo) + 0x00020002u;
            const uint32_t odd = ((p[0] >> 8) & lo) + ((p[1] >> 8) & lo) + ((p[2] >> 8) & lo) +
                                 ((p[3] >> 8) & lo) + 0x00020002u;
            const uint32_t q = ((even >> 2) & lo) | (((odd >> 2) & lo) << 8);
            memcpy(d + 4u * x, &q, 4);
        }
    }
}

/** @brief Write one level of a chain as the box-filtered average of the level above it.
 *
 * Separable and exact in integers: each source row is weighted through
 * the horizontal taps into 16-bit sums (mip_row_h), and each output row
 * blends the three rows its vertical taps name and rounds once
 * (round-half-up) to 8 bits (mip_row_v). Weights are in 1/256 per axis; an
 * even span uses 128 and 128, and an all-even halving takes the 2x2 path
 * (mip_level_even), which computes the same values. Horizontal rows live in
 * a three-slot ring keyed by source row, since consecutive output rows
 * share their boundary row.
 * @param src Level above, sw x sh RGBA8.
 * @param sw Its width.
 * @param sh Its height.
 * @param dst Receives the dw x dh level.
 * @param dw Destination width.
 * @param dh Destination height.
 * @param tx Horizontal taps scratch, dw entries.
 * @param ty Vertical taps scratch, dh entries.
 * @param ring Row scratch, 3 * dw * 4 entries. */
static void mip_level(const uint8_t *src, uint32_t sw, uint32_t sh,
                      uint8_t *dst, uint32_t dw, uint32_t dh,
                      mip_taps *tx, mip_taps *ty, uint16_t *ring)
{
    if (!(sw & 1u) && !(sh & 1u)) { mip_level_even(src, sw, dst, dw, dh); return; }
    mip_axis_taps(sw, dw, tx);
    mip_axis_taps(sh, dh, ty);
    /* An odd span n halves to n2 = (n - 1) / 2 at a footprint of 2 + 1/n2
     * texels starting at fraction i/n2 <= 1 - 1/n2, so it ends at or before
     * the third texel: three taps, the fourth always weighs 0. */
    const size_t n = (size_t)dw * 4u;
    uint32_t slot_row[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    for (uint32_t y = 0; y < dh; y++) {
        const mip_taps *t = &ty[y];
        const uint16_t *r[3];
        for (uint32_t k = 0; k < 3u; k++) {
            const uint32_t row = t->idx[k], slot = row % 3u;
            uint16_t *h = ring + slot * n;
            if (slot_row[slot] != row) {
                if (sw >= 2u) mip_row_h(src + (size_t)row * sw * 4u, tx, h, dw);
                else for (uint32_t c = 0; c < 4u; c++) h[c] = (uint16_t)(256u * src[(size_t)row * 4u + c]);
                slot_row[slot] = row;
            }
            r[k] = h;
        }
        const uint32_t w[3] = { t->w[0], t->w[1], t->w[2] };
        mip_row_v(r, w, dst + (size_t)y * n, n);
    }
}

bool rhi_rgba8_build_mips(uint8_t *chain, uint32_t width, uint32_t height)
{
    if (!chain || !width || !height) return false;
    const uint32_t levels = rhi_mip_levels(width, height);
    if (levels < 2) return true;
    const uint32_t w1 = width > 1 ? width / 2 : 1;
    mip_taps *tx = malloc(sizeof(mip_taps) * w1);
    mip_taps *ty = malloc(sizeof(mip_taps) * (height > 1 ? height / 2 : 1));
    uint16_t *ring = malloc(sizeof(uint16_t) * 3u * (size_t)w1 * 4u);
    const bool ok = tx && ty && ring;
    uint8_t *src = chain;
    uint32_t sw = width, sh = height;
    for (uint32_t i = 1; ok && i < levels; i++) {
        const uint32_t dw = sw > 1 ? sw / 2 : 1, dh = sh > 1 ? sh / 2 : 1;
        uint8_t *dst = src + (size_t)sw * sh * 4u;
        mip_level(src, sw, sh, dst, dw, dh, tx, ty, ring);
        src = dst; sw = dw; sh = dh;
    }
    free(tx); free(ty); free(ring);
    return ok;
}
