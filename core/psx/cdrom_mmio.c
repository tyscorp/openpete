/* cdrom_mmio.c - the CD-ROM controller's registers, 0x1F801800..0x1F801803,
 * for a game that drives the controller itself instead of through libcd.
 *
 * libcd's calls are thunked (games/common/psyq/psyq_thunks.cpp) and never reach
 * these registers; a game's own controller code does: it writes a command
 * byte and its parameters, polls the interrupt flag register for the INT
 * number the controller raised, reads the response FIFO and acknowledges.
 * The model keeps the index register, both FIFOs, the interrupt enable and
 * flag registers, and a queue of interrupts due at a VBL count: a command
 * answers with an INT3 and, for the commands with a second response, an
 * INT2 later; an interrupt due while another is unacknowledged waits for
 * the acknowledgement, as the controller's does. An interrupt the drive
 * raises during a VBL lands after the game's work of that VBL (the work
 * runs at the vblank, the drive answers milliseconds later), so one
 * raised at VBL V becomes visible from the vblank that ends V.
 *
 * Under a golden's `.cdirq` sibling (the reference's controller traffic:
 * every command with the writing instruction's address, every interrupt
 * with its response, and every register read by the game's own code with
 * the value it saw) the game's reads replay the reference's: the k-th
 * read of a register in a record returns what the reference's k-th read
 * of it in that record returned, so where inside a vblank the drive
 * answered needs no model; commands issued from the game's own code must
 * match the reference's next such command byte, and their interrupts
 * feed the model's queue for the reads no row covers. Rows from the
 * library's code (an address below the raw-site minimum the game names)
 * are libcd's own traffic and are skipped. Without the sibling the model
 * answers from a table: motor on, the disc's track list and lead-out from
 * the loaded image, the reference's measured latencies for the second
 * responses.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openpete_log.h"
#include "openpete_psxtrace.h"
#include "openpete_cdrom_mmio.h"
#include "openpete_rows.h"

#define CD_FIFO  OPENPETE_CDROM_MMIO_FIFO
#define CD_QUEUE OPENPETE_CDROM_MMIO_QUEUE
#define CD_RESP  OPENPETE_CDROM_MMIO_FIFO

typedef openpete_cdrom_mmio_pending_t cd_pending_t;

static uint8_t  s_index = 0;
static uint8_t  s_param[CD_FIFO]; static uint32_t s_param_n = 0;
static uint8_t  s_resp[CD_FIFO];  static uint32_t s_resp_n = 0, s_resp_head = 0;
static uint8_t  s_irq_enable = 0, s_irq_flag = 0;
static cd_pending_t s_queue[CD_QUEUE]; static uint32_t s_queue_n = 0;
static uint32_t s_vbl = 0;            /* the model's VBL count (the VBL bodies openpete ran) */
static uint32_t s_commands = 0, s_replayed = 0, s_defaulted = 0;
static uint32_t s_raw_pc_min = 0xFFFFFFFFu;
static uint8_t  s_last_cmd = 0;

/* The reference's rows: a command with the interrupts that followed it. */
typedef struct { uint32_t rec, vbl, pc; uint8_t cmd, nparam, param[CD_FIFO]; uint32_t first_irq, irq_count; } cd_row_cmd_t;
typedef struct { uint32_t rec, vbl; uint8_t irq, n, resp[CD_RESP]; } cd_row_irq_t;
static cd_row_cmd_t* s_rc = NULL; static uint32_t s_rc_n = 0, s_rc_next = 0;
static cd_row_irq_t* s_ri = NULL; static uint32_t s_ri_n = 0;
typedef struct { uint32_t rec; uint8_t off, value; } cd_row_read_t;
static cd_row_read_t* s_rr = NULL; static uint32_t s_rr_n = 0, s_rr_next = 0;
static uint32_t s_reads_replayed = 0, s_reads_model = 0, s_reads_unmade = 0;
static int s_replay = 0;

void openpete_cdrom_mmio_set_raw_pc_min(uint32_t pc) { s_raw_pc_min = pc; }

/* Capacities of the three row tables while the sidecar loads. */
typedef struct { uint32_t cmd, irq, read; } cd_row_caps_t;

/** @brief Append a `C` row (a command, its parameters and the index of its first interrupt) to s_rc.
 *
 * A row with fewer than six fields is skipped; parameters past the FIFO
 * size are dropped.
 * @param line The row.
 * @param caps The tables' capacities, raised on a growth.
 * @return 0 when the table could not grow, else 1. */
static int cd_row_cmd_add(const char* line, cd_row_caps_t* caps) {
    unsigned idx, rec, vbl, pc, cmd, n; int off = 0;
    if (sscanf(line, "C %u %u %u %x %x %u%n", &idx, &rec, &vbl, &pc, &cmd, &n, &off) != 6) return 1;
    cd_row_cmd_t* grown = openpete_rows_reserve(s_rc, &caps->cmd, s_rc_n + 1ull, sizeof(*s_rc), 256u);
    if (!grown) return 0;
    s_rc = grown;
    cd_row_cmd_t* r = &s_rc[s_rc_n++];
    memset(r, 0, sizeof(*r));
    r->rec = rec; r->vbl = vbl; r->pc = pc; r->cmd = (uint8_t)cmd; r->nparam = (uint8_t)(n > CD_FIFO ? CD_FIFO : n);
    const char* p = line + off;
    for (unsigned i = 0; i < r->nparam; i++) { unsigned b; int k = 0; if (sscanf(p, " %x%n", &b, &k) != 1) break; r->param[i] = (uint8_t)b; p += k; }
    r->first_irq = s_ri_n;
    return 1;
}

/** @brief Append an `I` row (an interrupt and its response) to s_ri and count it on the last command.
 *
 * A row with fewer than five fields is skipped; response bytes past the
 * FIFO size are dropped.
 * @param line The row.
 * @param caps The tables' capacities, raised on a growth.
 * @return 0 when the table could not grow, else 1. */
static int cd_row_irq_add(const char* line, cd_row_caps_t* caps) {
    unsigned idx, rec, vbl, irq, n; int off = 0;
    if (sscanf(line, "I %u %u %u %u %u%n", &idx, &rec, &vbl, &irq, &n, &off) != 5) return 1;
    cd_row_irq_t* grown = openpete_rows_reserve(s_ri, &caps->irq, s_ri_n + 1ull, sizeof(*s_ri), 1024u);
    if (!grown) return 0;
    s_ri = grown;
    cd_row_irq_t* r = &s_ri[s_ri_n++];
    memset(r, 0, sizeof(*r));
    r->rec = rec; r->vbl = vbl; r->irq = (uint8_t)irq; r->n = (uint8_t)(n > CD_RESP ? CD_RESP : n);
    const char* p = line + off;
    for (unsigned i = 0; i < r->n; i++) { unsigned b; int k = 0; if (sscanf(p, " %x%n", &b, &k) != 1) break; r->resp[i] = (uint8_t)b; p += k; }
    if (s_rc_n) s_rc[s_rc_n - 1].irq_count++;
    return 1;
}

/** @brief Append an `R` row (a register read by the game's own code and the value it saw) to s_rr.
 *
 * A row with fewer than six fields is skipped.
 * @param line The row.
 * @param caps The tables' capacities, raised on a growth.
 * @return 0 when the table could not grow, else 1. */
static int cd_row_read_add(const char* line, cd_row_caps_t* caps) {
    unsigned idx, rec, vbl, pc, off, value;
    if (sscanf(line, "R %u %u %u %x %u %x", &idx, &rec, &vbl, &pc, &off, &value) != 6) return 1;
    cd_row_read_t* grown = openpete_rows_reserve(s_rr, &caps->read, s_rr_n + 1ull, sizeof(*s_rr), 4096u);
    if (!grown) return 0;
    s_rr = grown;
    s_rr[s_rr_n].rec = rec; s_rr[s_rr_n].off = (uint8_t)off; s_rr[s_rr_n].value = (uint8_t)value;
    s_rr_n++;
    return 1;
}

/** @brief Release the reference's command, interrupt and read rows and fail the sidecar load on an allocation failure.
 * @param fp The open sidecar, closed here.
 * @param path Its path, for the message.
 * @return 0, the load's failure value. */
static int cd_rows_fail(FILE* fp, const char* path) {
    OPL_ERROR_F("cdrom", "controller replay: out of memory at %u commands, %u interrupts, %u reads of %s; replay off",
                s_rc_n, s_ri_n, s_rr_n, path);
    fclose(fp);
    free(s_rc); free(s_ri); free(s_rr);
    s_rc = NULL; s_ri = NULL; s_rr = NULL;
    s_rc_n = s_ri_n = s_rr_n = 0;
    return 0;
}

int openpete_cdrom_mmio_open(const char* trace_path) {
    char path[4096];
    snprintf(path, sizeof(path), "%s.cdirq", trace_path);
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    char line[512];
    if (!fgets(line, sizeof(line), fp) || strncmp(line, "psxtrace-cdirq", 14) != 0) { fclose(fp); return 0; }
    cd_row_caps_t caps = { 0, 0, 0 };
    s_rc = openpete_rows_reserve(NULL, &caps.cmd, 1u, sizeof(*s_rc), 256u);
    s_ri = openpete_rows_reserve(NULL, &caps.irq, 1u, sizeof(*s_ri), 1024u);
    s_rr = openpete_rows_reserve(NULL, &caps.read, 1u, sizeof(*s_rr), 4096u);
    if (!s_rc || !s_ri || !s_rr) return cd_rows_fail(fp, path);
    while (fgets(line, sizeof(line), fp)) {
        const int ok = line[0] == 'C' ? cd_row_cmd_add(line, &caps)
                     : line[0] == 'I' ? cd_row_irq_add(line, &caps)
                     : line[0] == 'R' ? cd_row_read_add(line, &caps) : 1;
        if (!ok) return cd_rows_fail(fp, path);
    }
    fclose(fp);
    s_replay = 1;
    OPL_INFO_F("cdrom", "controller replay: %u commands, %u interrupts, %u reads by the game's own code from %s", s_rc_n, s_ri_n, s_rr_n, path);
    return 1;
}

/** @brief Load the response FIFO and rewind its read position.
 *
 * @param b Response bytes.
 * @param n Byte count; clamped to the FIFO size. */
static void resp_set(const uint8_t* b, uint32_t n) {
    if (n > CD_FIFO) n = CD_FIFO;
    memcpy(s_resp, b, n); s_resp_n = n; s_resp_head = 0;
}

/** @brief Set the flag register to a queued interrupt's INT number and load its response.
 *
 * The caller removes the entry from the queue.
 * @param p Queue entry. */
static void raise_irq(const cd_pending_t* p) {
    s_irq_flag = p->irq;
    resp_set(p->resp, p->n);
}

/** @brief Raise the queue's head interrupt when it is due and no interrupt is unacknowledged.
 *
 * Raising sets the flag register and loads the response FIFO; the head
 * then leaves the queue. */
static void queue_service(void) {
    if (s_irq_flag != 0 || s_queue_n == 0) return;
    if ((int32_t)(s_queue[0].due - s_vbl) > 0) return;
    raise_irq(&s_queue[0]);
    memmove(&s_queue[0], &s_queue[1], (s_queue_n - 1) * sizeof(s_queue[0]));
    s_queue_n--;
}

/** @brief Append an interrupt due at a VBL count to the queue.
 *
 * A full queue drops it with a warning. The response is truncated to 16 bytes.
 * @param due  VBL count from which the interrupt may be raised.
 * @param irq  INT number.
 * @param resp Response bytes.
 * @param n    Response length. */
static void queue_push(uint32_t due, uint8_t irq, const uint8_t* resp, uint32_t n) {
    if (s_queue_n == CD_QUEUE) { OPL_WARN_F("cdrom", "interrupt queue full; dropping INT%u", irq); return; }
    cd_pending_t* p = &s_queue[s_queue_n++];
    p->due = due; p->irq = irq; p->n = (uint8_t)(n > CD_RESP ? CD_RESP : n);
    memcpy(p->resp, resp, p->n);
}

/* The table the model answers from without the reference's rows: the
 * status byte (motor on, 0x02) and the second-response latencies the
 * reference showed for this drive, in VBLs after the command. */
#define CD_STAT_MOTOR_ON 0x02u
static const struct { uint8_t cmd; uint32_t int2_delay; } k_second_response[] = {
    { 0x0A, 48 },   /* Init */
    { 0x1E, 30 },   /* ReadTOC */
    { 0x1A,  2 },   /* GetID */
    { 0x16, 33 },   /* SeekP */
    { 0x15, 33 },   /* SeekL */
    { 0x09,  2 },   /* Pause */
    { 0x08,  2 },   /* Stop */
    { 0x0C,  2 },   /* SetSession */
    { 0x07,  2 },   /* MotorOn */
};

/** @brief Queue a command's interrupts from the model's table.
 *
 * Queues an INT3 due one VBL later whose first byte is the motor-on status;
 * GetTN and GetTD (track 1 at 150 sectors, else the lead-out from the
 * disc's sector count, minute and second in BCD) append their bytes. Test
 * sub-function 0x05 answers 0x82, 0, 0 and 0x20 answers the four bytes
 * 0x94, 0x09, 0x19, 0xC0, neither with the motor-on status. A command in
 * k_second_response also queues an INT2 after its measured delay; GetID's
 * carries the SCEA license bytes.
 * @param cmd Command byte. */
static void issue_default(uint8_t cmd) {
    uint8_t r[CD_RESP]; uint32_t n = 0;
    r[n++] = CD_STAT_MOTOR_ON;
    switch (cmd) {
    case 0x13: r[n++] = 0x01; r[n++] = 0x01; break;                       /* GetTN: one track */
    case 0x14: {                                                          /* GetTD: track start, MSF BCD */
        uint32_t lba = (s_param_n && s_param[0] != 0) ? 150u : openpete_cdrom_mmio_disc_sectors() + 150u;
        uint32_t m = lba / 4500u, sec = (lba / 75u) % 60u;
        r[n++] = (uint8_t)(((m / 10u) << 4) | (m % 10u));
        r[n++] = (uint8_t)(((sec / 10u) << 4) | (sec % 10u));
        break;
    }
    case 0x19:                                                            /* Test */
        if (s_param_n && s_param[0] == 0x05) { r[0] = 0x82; r[n++] = 0; r[n++] = 0; }   /* SCEx counts: none read */
        else if (s_param_n && s_param[0] == 0x20) { n = 0; r[n++] = 0x94; r[n++] = 0x09; r[n++] = 0x19; r[n++] = 0xC0; }
        break;
    default: break;
    }
    queue_push(s_vbl + 1u, 3, r, n);
    for (size_t i = 0; i < sizeof(k_second_response) / sizeof(k_second_response[0]); i++) {
        if (k_second_response[i].cmd != cmd) continue;
        uint8_t r2[CD_RESP]; uint32_t n2 = 0;
        r2[n2++] = CD_STAT_MOTOR_ON;
        if (cmd == 0x1A) { r2[n2++] = 0x00; r2[n2++] = 0x20; r2[n2++] = 0x00; r2[n2++] = 'S'; r2[n2++] = 'C'; r2[n2++] = 'E'; r2[n2++] = 'A'; }
        queue_push(s_vbl + k_second_response[i].int2_delay + 1u, 2, r2, n2);
    }
    s_defaulted++;
}

/** @brief Consume the reference's next game-issued command row and queue its interrupts.
 *
 * Skips rows issued below the raw-site minimum or from a ROM address. A
 * command beyond the last row, or one whose byte differs from the row's,
 * reports a scheduling failure and returns 0. Each interrupt row is queued
 * at the current VBL count plus its VBL offset from the command, plus one.
 * @param cmd Command byte the game wrote.
 * @return 1 when a matching row was consumed, else 0. */
static int issue_replay(uint8_t cmd) {
    /* The game's own sites lie in main RAM at or above the minimum; the
     * BIOS's boot commands (ROM addresses) and libcd's are skipped. */
    while (s_rc_next < s_rc_n && (s_rc[s_rc_next].pc < s_raw_pc_min || (s_rc[s_rc_next].pc & 0x1FFFFFFFu) >= 0x00200000u)) s_rc_next++;
    if (s_rc_next >= s_rc_n) {
        openpete_sched_fail("CD-ROM command 0x%02X beyond the reference's %u rows", cmd, s_rc_n);
        return 0;
    }
    const cd_row_cmd_t* r = &s_rc[s_rc_next++];
    if (r->cmd != cmd) {
        openpete_sched_fail("CD-ROM command 0x%02X, the reference issued 0x%02X (record %u)", cmd, r->cmd, r->rec);
        return 0;
    }
    for (uint32_t i = 0; i < r->irq_count; i++) {
        const cd_row_irq_t* q = &s_ri[r->first_irq + i];
        queue_push(s_vbl + (q->vbl - r->vbl) + 1u, q->irq, q->resp, q->n);
    }
    s_replayed++;
    return 1;
}

/** @brief Handle a command byte written to the command register.
 *
 * Counts it, answers it from the reference's rows when replay is active and
 * the scheduler is running (else from the table), clears the parameter FIFO
 * and raises the queue's head when it is due. Logs the command when
 * OPENPETE_CDROM_LOG is set.
 * @param cmd Command byte. */
static void issue(uint8_t cmd) {
    s_commands++;
    s_last_cmd = cmd;
    if (getenv("OPENPETE_CDROM_LOG"))
        OPL_INFO_F("cdrom", "command 0x%02X (%u params) at vbl %u: replay=%d sched=%d next_row=%u/%u raw_min=0x%08X",
                   cmd, s_param_n, s_vbl, s_replay, openpete_sched_active(), s_rc_next, s_rc_n, s_raw_pc_min);
    if (!(s_replay && openpete_sched_active() && issue_replay(cmd))) issue_default(cmd);
    s_param_n = 0;
    queue_service();
}

void openpete_cdrom_mmio_vbl(uint32_t vbls) {
    s_vbl += vbls;
    queue_service();
}

/** @brief Answer a register read with the reference's next read of that register in the current record.
 *
 * Rows of earlier records are reads openpete never made and are skipped.
 * The matched row is consumed in place; rows before it for other
 * registers stay. The model follows the answer: a flag-register value
 * with an interrupt raises the queue's head when none is unacknowledged,
 * and a response-FIFO read advances the FIFO.
 * @param off Register offset, 0 to 3.
 * @param out Receives the byte.
 * @return 1 when a row answered; 0 when replay is not active or no row of
 *         the current record matches. */
static int read_replay(uint32_t off, uint8_t* out) {
    if (!s_replay || !s_rr_n || !openpete_sched_active()) return 0;
    const uint32_t cur_rec = openpete_sched_current_record();
    while (s_rr_next < s_rr_n && s_rr[s_rr_next].rec < cur_rec) { s_rr_next++; s_reads_unmade++; }
    uint32_t k = s_rr_next;
    while (k < s_rr_n && s_rr[k].rec == cur_rec && s_rr[k].off != off) k++;
    if (k >= s_rr_n || s_rr[k].rec != cur_rec) { s_reads_model++; return 0; }
    /* Rows before it are reads of other registers the game makes in its
     * own order; they stay for their registers. The row is consumed in
     * place. */
    *out = s_rr[k].value;
    s_rr[k].rec = 0xFFFFFFFFu;
    while (s_rr_next < s_rr_n && s_rr[s_rr_next].rec == 0xFFFFFFFFu) s_rr_next++;
    s_reads_replayed++;
    /* The model follows the reference: an interrupt the reference showed
     * is the queue's head raised now; a response byte consumed advances
     * the FIFO. */
    if (off == 3 && (*out & 7u) != 0 && s_irq_flag == 0 && s_queue_n) { raise_irq(&s_queue[0]); memmove(&s_queue[0], &s_queue[1], (s_queue_n - 1) * sizeof(s_queue[0])); s_queue_n--; }
    if (off == 1 && s_resp_head < s_resp_n) s_resp_head++;
    return 1;
}

int openpete_cdrom_mmio_read8(uint32_t phys, uint8_t* out) {
    if (phys >= 0x1F801800u && phys <= 0x1F801803u && read_replay(phys - 0x1F801800u, out)) return 1;
    switch (phys) {
    case 0x1F801800u:
        *out = (uint8_t)(s_index | (s_param_n == 0 ? 0x08u : 0u) | (s_param_n < CD_FIFO ? 0x10u : 0u) |
                         (s_resp_head < s_resp_n ? 0x20u : 0u));
        return 1;
    case 0x1F801801u:
        *out = (s_resp_head < s_resp_n) ? s_resp[s_resp_head++] : 0u;
        return 1;
    case 0x1F801802u:
        *out = 0u;   /* data FIFO: sector data reaches the game through libcd's thunks */
        return 1;
    case 0x1F801803u:
        *out = (uint8_t)(0xE0u | ((s_index & 1u) ? s_irq_flag : s_irq_enable));
        return 1;
    default:
        return 0;
    }
}

int openpete_cdrom_mmio_write8(uint32_t phys, uint8_t v) {
    switch (phys) {
    case 0x1F801800u:
        s_index = v & 3u;
        return 1;
    case 0x1F801801u:
        if (s_index == 0) issue(v);
        return 1;
    case 0x1F801802u:
        if (s_index == 0) { if (s_param_n < CD_FIFO) s_param[s_param_n++] = v; }
        else if (s_index == 1) s_irq_enable = v & 0x1Fu;
        return 1;
    case 0x1F801803u:
        if (s_index == 1) {
            s_irq_flag &= (uint8_t)~(v & 0x1Fu);
            if (v & 0x40u) s_param_n = 0;
            queue_service();
        }
        return 1;
    default:
        return 0;
    }
}

void openpete_cdrom_mmio_state_save(openpete_cdrom_mmio_state_t* out) {
    memset(out, 0, sizeof *out);
    out->vbl = s_vbl;
    out->param_n = s_param_n; out->resp_n = s_resp_n; out->resp_head = s_resp_head; out->queue_n = s_queue_n;
    out->index = s_index; out->irq_enable = s_irq_enable; out->irq_flag = s_irq_flag; out->last_cmd = s_last_cmd;
    memcpy(out->param, s_param, sizeof out->param);
    memcpy(out->resp, s_resp, sizeof out->resp);
    for (uint32_t i = 0; i < s_queue_n; i++) {
        out->queue[i].due = s_queue[i].due;
        out->queue[i].irq = s_queue[i].irq;
        out->queue[i].n = s_queue[i].n;
        memcpy(out->queue[i].resp, s_queue[i].resp, sizeof out->queue[i].resp);
    }
}

void openpete_cdrom_mmio_state_load(const openpete_cdrom_mmio_state_t* in) {
    s_vbl = in->vbl;
    s_param_n = in->param_n > CD_FIFO ? CD_FIFO : in->param_n;
    s_resp_n = in->resp_n > CD_FIFO ? CD_FIFO : in->resp_n;
    s_resp_head = in->resp_head > s_resp_n ? s_resp_n : in->resp_head;
    s_queue_n = in->queue_n > CD_QUEUE ? CD_QUEUE : in->queue_n;
    s_index = in->index; s_irq_enable = in->irq_enable; s_irq_flag = in->irq_flag; s_last_cmd = in->last_cmd;
    memcpy(s_param, in->param, sizeof s_param);
    memcpy(s_resp, in->resp, sizeof s_resp);
    memset(s_queue, 0, sizeof s_queue);
    memcpy(s_queue, in->queue, s_queue_n * sizeof s_queue[0]);
}

/* The reference's response to the library's next command of this byte in
 * the current record, for a thunk that answers a query the drive answered
 * (a position query's eight bytes). Rows of earlier records are commands
 * openpete never issued. */
static uint32_t s_lib_next = 0, s_lib_replayed = 0, s_lib_unmade = 0, s_lib_last_done = 0, s_lib_last_done_rec = 0;
static uint8_t  s_lib_last_done_status = 0;
uint32_t openpete_cdrom_mmio_libcd_last_done_vbl(void) { return s_lib_last_done; }
uint32_t openpete_cdrom_mmio_libcd_last_done_rec(void) { return s_lib_last_done_rec; }
static uint32_t s_lib_last_row = 0xFFFFFFFFu;
int openpete_cdrom_mmio_libcd_last_irq(uint32_t idx, uint32_t* rec, uint32_t* vbl, uint8_t* irq, uint8_t* status) {
    if (s_lib_last_row == 0xFFFFFFFFu || idx >= s_rc[s_lib_last_row].irq_count) return 0;
    const cd_row_irq_t* q = &s_ri[s_rc[s_lib_last_row].first_irq + idx];
    *rec = q->rec; *vbl = q->vbl; *irq = q->irq; *status = q->n ? q->resp[0] : 0u;
    return 1;
}
uint8_t  openpete_cdrom_mmio_libcd_last_done_status(void) { return s_lib_last_done_status; }
int openpete_cdrom_mmio_libcd_response(uint8_t cmd, uint8_t* out, uint32_t* n_out) {
    if (!s_replay || !openpete_sched_active()) return 0;
    const uint32_t cur_rec = openpete_sched_current_record();
    while (s_lib_next < s_rc_n && s_rc[s_lib_next].rec < cur_rec) {
        if (s_rc[s_lib_next].pc < s_raw_pc_min && s_rc[s_lib_next].cmd == cmd) s_lib_unmade++;
        s_lib_next++;
    }
    uint32_t k = s_lib_next;
    while (k < s_rc_n && s_rc[k].rec == cur_rec && !(s_rc[k].pc < s_raw_pc_min && s_rc[k].cmd == cmd)) k++;
    if (k >= s_rc_n || s_rc[k].rec != cur_rec || s_rc[k].irq_count == 0) return 0;
    const cd_row_irq_t* q = &s_ri[s_rc[k].first_irq];
    memcpy(out, q->resp, q->n);
    *n_out = q->n;
    s_lib_last_row = k;
    {
        const cd_row_irq_t* last = &s_ri[s_rc[k].first_irq + s_rc[k].irq_count - 1u];
        s_lib_last_done = last->vbl;
        s_lib_last_done_rec = last->rec;
        s_lib_last_done_status = last->n ? last->resp[0] : q->resp[0];
    }
    s_rc[k].cmd = 0xFFu;   /* consumed */
    while (s_lib_next < s_rc_n && s_rc[s_lib_next].cmd == 0xFFu) s_lib_next++;
    s_lib_replayed++;
    return 1;
}

extern int g_cdNumFrames;   /* PsyCross's frame count of the loaded image */
uint32_t openpete_cdrom_mmio_disc_sectors(void) { return g_cdNumFrames > 0 ? (uint32_t)g_cdNumFrames : 0u; }

void openpete_cdrom_mmio_summary(void) {
    if (!s_commands && !s_lib_replayed) return;
    OPL_INFO_F("cdrom", "controller: %u commands from the game's own code (%u replayed from the reference, %u answered from the table); "
               "reads: %u replayed, %u answered by the model, %u of the reference's unmade; "
               "library queries: %u answered from the reference's responses, %u of the reference's unmade",
               s_commands, s_replayed, s_defaulted, s_reads_replayed, s_reads_model, s_reads_unmade,
               s_lib_replayed, s_lib_unmade);
}
