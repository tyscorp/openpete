// games/spyro-1/mods/mod_wad_overlay.h - level-segment asset overlay.
// See mod_wad_overlay.c. Consumers: game_extras_impl.c (configure, as the
// runtime descriptor's mod_disc_configure; cursor save and restore),
// psyq_hooks.cpp (setloc tracking, apply at the CdRead chokepoint),
// mod_level_ops.c (read_level_data).

#ifndef SPYRO1_MOD_WAD_OVERLAY_H
#define SPYRO1_MOD_WAD_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Records the disc path the WAD TOC is read from and arms a lazy parse.
 *
 * Ignored for a NULL or empty path, and when the same path is already
 * parsed. Otherwise the overlay goes pending; the TOC is read on the first
 * mod_wad_apply or spyro1_mod_wad_read_level_data after a mod asset root
 * exists.
 * @param disc_path The .cue or .bin path of the user's disc. */
void spyro1_mod_wad_configure(const char* disc_path);
/** @brief Records the disc head sector a CdlSetloc positioned.
 *
 * Logs the sector when OPENPETE_WADMOD_TRACE is set. The next mod_wad_apply
 * starts its read at this sector.
 * @param sector The absolute disc sector. */
void mod_wad_setloc(uint32_t sector);
/** @brief Serves override bytes into a just-completed CdRead's guest buffer and advances the tracked head.
 *
 * Parses the disc's WAD TOC on first use once an asset root exists. For
 * each level data or overlay segment the read overlaps, the serving file
 * is resolved once per level load, at the read that starts at the
 * segment's first sector (or at the first read into an unlatched
 * segment), and held for every later read of that segment; a mod toggle
 * therefore takes effect at the next load. The logical level is the one
 * whose guest TOC entry (g_NextLevelId, then g_LevelId) points at the
 * segment, and that entry's length is the size contract; a declared
 * custom level's files are tried before the asset roots. A served data
 * override longer than the stock entry extends the segment's range to the
 * file's length.
 * @param host_buf Host pointer to the read's destination buffer.
 * @param sectors The number of 2048-byte data sectors read. */
void mod_wad_apply(uint8_t* host_buf, int sectors);
/** @brief Reads a level's stock data WAD entry host-side from the user's disc image.
 *
 * Reads the entry the disc's own WAD TOC names, independent of any
 * override or guest TOC edit, so a mod can borrow another level's data at
 * runtime without shipping derived game data.
 * @param level_id The logical level id.
 * @param buf Receives the entry bytes; NULL probes the length only.
 * @param cap The capacity of `buf`.
 * @return The entry byte length, or -1 when the TOC is unavailable, the
 *     level has no data entry, `cap` is too small, or a sector read fails. */
int spyro1_mod_wad_read_level_data(int level_id, void* buf, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO1_MOD_WAD_OVERLAY_H */
