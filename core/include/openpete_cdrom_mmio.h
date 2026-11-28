/* openpete_cdrom_mmio.h - the CD-ROM controller's registers for a game that
 * drives the controller itself (core/psx/cdrom_mmio.c). */
#ifndef OPENPETE_CDROM_MMIO_H
#define OPENPETE_CDROM_MMIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Read one CD-ROM controller register.
 *
 * Under an active replay, the reference's next read of that register in
 * the current record answers first. Otherwise the model answers: 0x1F801800
 * the index and the FIFO status bits, 0x1F801801 the next response byte,
 * 0x1F801802 0 (sector data reaches the game through libcd's thunks), and
 * 0x1F801803 the interrupt flag or enable register, by index, with the top
 * three bits set.
 * @param phys Physical address.
 * @param out Receives the byte.
 * @return 1 when @p phys is one of 0x1F801800 to 0x1F801803, else 0. */
int  openpete_cdrom_mmio_read8(uint32_t phys, uint8_t* out);
/** @brief Write one CD-ROM controller register: index, command, parameter, interrupt enable, or acknowledge.
 *
 * @param phys Physical address.
 * @param value Byte written.
 * @return 1 when @p phys is one of 0x1F801800 to 0x1F801803, else 0. */
int  openpete_cdrom_mmio_write8(uint32_t phys, uint8_t value);

/** @brief Advance the model's VBL count and raise the queued interrupt now due.
 *
 * At most one interrupt is raised per call: the queue's head, when it is
 * due and no interrupt is unacknowledged.
 * @param vbls VBLs elapsed since the previous call. */
void openpete_cdrom_mmio_vbl(uint32_t vbls);

/** @brief Load a golden's `.cdirq` sibling and switch the controller to replay.
 *
 * Reads `<trace_path>.cdirq`, which starts with a `psxtrace-cdirq` line,
 * into its command (C), interrupt (I) and register-read (R) rows;
 * malformed rows are skipped. An allocation failure releases the rows
 * read so far and leaves the controller out of replay.
 * @param trace_path The golden's path.
 * @return 1 when the file was found and loaded, else 0. */
int  openpete_cdrom_mmio_open(const char* trace_path);
/** @brief Set the lowest instruction address of the game's own controller code.
 *
 * Replay rows whose issuing address is below it, or in the BIOS ROM, are
 * the library's or the BIOS's traffic: libcd's calls are thunked on the
 * host, so the game-command replay skips them.
 * @param pc Instruction address. */
void openpete_cdrom_mmio_set_raw_pc_min(uint32_t pc);

/** @brief Copy the reference's response to libcd's next command of one byte in the current record.
 *
 * For a thunk that answers a query the drive answered on the reference.
 * Library rows (issued below the raw-site minimum) of earlier records are
 * commands openpete never issued and are skipped. The first interrupt's
 * response is copied; the row's last interrupt sets what the
 * libcd_last_done and libcd_last_irq queries return; the row is then
 * consumed.
 * @param cmd Command byte.
 * @param out Receives the response, up to 16 bytes.
 * @param n_out Receives the response length.
 * @return 1 when a row with at least one interrupt was consumed; 0 when
 *         none matches or replay is not active. */
int  openpete_cdrom_mmio_libcd_response(uint8_t cmd, uint8_t* out, uint32_t* n_out);
/** @brief Return the VBL of the last interrupt of the command the last openpete_cdrom_mmio_libcd_response() hit consumed, its completion on the reference.
 *
 * @return The VBL, 0 before any hit. */
uint32_t openpete_cdrom_mmio_libcd_last_done_vbl(void);
/** @brief Return the record that last interrupt was recorded in, between the anchor hits of that index and the next.
 *
 * @return The record index, 0 before any hit. */
uint32_t openpete_cdrom_mmio_libcd_last_done_rec(void);
/** @brief Return the status byte of that last interrupt's response, or of the first response when the last is empty.
 *
 * @return The status byte, 0 before any hit. */
uint8_t  openpete_cdrom_mmio_libcd_last_done_status(void);
/** @brief Report one interrupt of the command the last openpete_cdrom_mmio_libcd_response() hit consumed.
 *
 * @param idx 0-based interrupt index.
 * @param rec Receives the record it was recorded in.
 * @param vbl Receives its VBL.
 * @param irq Receives its cause (INT number).
 * @param status Receives its response's first byte, 0 for an empty one.
 * @return 1, or 0 past the command's interrupts or before any hit. */
int      openpete_cdrom_mmio_libcd_last_irq(uint32_t idx, uint32_t* rec, uint32_t* vbl, uint8_t* irq, uint8_t* status);

/** @brief Return the loaded disc image's sector count, 0 when none is loaded.
 *
 * The table answer to GetTD 0 is this count + 150 as the lead-out.
 * @return PsyCross's g_cdNumFrames, floored at 0. */
uint32_t openpete_cdrom_mmio_disc_sectors(void);

/* The controller's register state and interrupt queue, fixed layout, as a
 * savestate carries it: a game that drives the controller itself polls the
 * flag register across VBLs, so an interrupt queued before a save is raised
 * after the load at the VBL count it was due. The replay cursors are not
 * carried: a load is refused while the reference's schedule runs. */
#define OPENPETE_CDROM_MMIO_FIFO  16
#define OPENPETE_CDROM_MMIO_QUEUE 8
typedef struct {
    uint32_t due;                               /* VBL count it may be raised from */
    uint8_t  irq, n;                            /* INT number, response length */
    uint8_t  resp[OPENPETE_CDROM_MMIO_FIFO];
    uint8_t  pad[2];
} openpete_cdrom_mmio_pending_t;
typedef struct {
    uint32_t vbl;                               /* the model's VBL count */
    uint32_t param_n, resp_n, resp_head, queue_n;
    uint8_t  index, irq_enable, irq_flag, last_cmd;
    uint8_t  param[OPENPETE_CDROM_MMIO_FIFO];
    uint8_t  resp[OPENPETE_CDROM_MMIO_FIFO];
    openpete_cdrom_mmio_pending_t queue[OPENPETE_CDROM_MMIO_QUEUE];
} openpete_cdrom_mmio_state_t;

/** @brief Copy the controller's register state, response FIFO and interrupt queue into @p out.
 *
 * @param out Receives the state; every byte is written, padding zeroed. */
void openpete_cdrom_mmio_state_save(openpete_cdrom_mmio_state_t* out);
/** @brief Replace the controller's register state, response FIFO and interrupt queue with @p in.
 *
 * Counts beyond the FIFO or queue capacity are clamped to it.
 * @param in The state a save wrote. */
void openpete_cdrom_mmio_state_load(const openpete_cdrom_mmio_state_t* in);

/** @brief Log the controller's exit counters.
 *
 * Prints nothing when the game issued no command and no library query was
 * answered. Otherwise reports commands (replayed from the reference, answered
 * from the table), register reads (replayed, answered by the model, the
 * reference's unmade) and library queries (answered, unmade). */
void openpete_cdrom_mmio_summary(void);

#ifdef __cplusplus
}
#endif
#endif
