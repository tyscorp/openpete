/* core/runtime/host_bugreport.c - the bug-report press: the key, the menu
 * request and the harness feed key, the sim-thread copy at a final yield,
 * the present-side screenshot and display reports, the main-thread device
 * facts, and the worker's lifecycle. See openpete_bugreport.h; the worker
 * itself is core/record/bugreport_write.c. */
#include "openpete_bugreport.h"
#include "openpete_thread_roles.h"

#include "../record/bugreport_internal.h"
#include "game_extras.h"
#include "host_loop_internal.h"
#include "run_internal.h"
#include "openpete.h"
#include "openpete_bugrec.h"
#include "openpete_bugrec_subtick.h"
#include "openpete_bugreport_replay.h"
#include "openpete_config.h"
#include "openpete_crash.h"
#include "openpete_devui.h"
#include "openpete_fiber.h"
#include "openpete_freecam.h"
#include "openpete_host_trace.h"
#include "openpete_interp.h"
#include "openpete_keymap.h"
#include "openpete_log.h"
#include "openpete_mods.h"
#include "openpete_native_render.h"
#include "openpete_osd.h"
#include "openpete_pace_log.h"
#include "openpete_paths.h"
#include "openpete_psx.h"
#include "openpete_rewind.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
#endif

#ifdef _WIN32
#define BR_ENVIRON _environ
#else
extern char** environ;
#define BR_ENVIRON environ
#endif

/* A fatal exit's bound on the in-flight bundle and on its own. */
#define BR_FATAL_WAIT_MS 30000u

static struct {
    SDL_AtomicInt requested;     /* openpete_bugreport_request */
    long          at_key;        /* OPENPETE_BUGREPORT_AT, or -1 */
    int           note_env;      /* OPENPETE_BUGREPORT_NOTE: -1 unset, 0 or 1 */
    int           harness_press; /* the due press came from OPENPETE_BUGREPORT_AT */
    const openpete_options_t* opts;
    SDL_ThreadID  sim_tid;       /* the thread openpete_bugreport_init ran on */
    br_job_t*     job;           /* the bundle in flight (sim-owned pointer) */
    SDL_Thread*   thr;
    br_job_t*     shot_job;      /* the bundle whose screenshot the present services owe */
    void*         main_job;      /* the bundle whose main-thread facts the pump owes (SDL atomic pointer) */
    unsigned long long presses, written, failed, refused;
    /* The last bundle's results, for the exit report. */
    char          last_path[1400];
    size_t        last_members, last_findings;
    uint64_t      last_bytes, last_replacements, last_press_us, last_display_us, last_write_ms;
} s_bp = { .at_key = -1, .note_env = -1 };

static void bundle_exit_flush(void);

void openpete_bugreport_init(const openpete_options_t* opts)
{
    const char* e = getenv("OPENPETE_BUGREPORT_AT");
    s_bp.at_key = (e && *e) ? strtol(e, NULL, 10) : -1;
    const char* n = getenv("OPENPETE_BUGREPORT_NOTE");
    s_bp.note_env = (n && *n) ? (*n != '0') : -1;
    s_bp.opts = opts;
    s_bp.sim_tid = SDL_GetCurrentThreadID();
    openpete_crash_set_exit_flush(OP_EXIT_FLUSH_BUGREPORT, bundle_exit_flush);
}

void openpete_bugreport_request(void) { SDL_SetAtomicInt(&s_bp.requested, 1); }

/* The one toast key of a bundle: SAVING holds under it until SAVED or
 * FAILED repaints it in place. */
#define BR_OSD_KEY "bugreport.saved"
/* The toast naming the members shipped flagged, beside SAVED. */
#define BR_OSD_FLAG_KEY "bugreport.flagged"

/** @brief Post the held SAVING toast once the job's note prompt has ended, then join a finished worker, repaint the toast with the result and log it, then free the job.
 *
 * The SAVING toast holds with OPENPETE_OSD_TIMEOUT_HOLD and every job ends
 * here, so the toast is always repainted with SAVED or FAILED and an
 * ordinary hold. A saved bundle with flagged members adds one toast and a
 * log line naming them (br_flag_line). A call without wait inside a speculative or replayed tick
 * returns at once.
 * @param wait Nonzero to wait for the worker; else return at once while it runs. */
static void bundle_reap(int wait) OP_REQUIRES(sim)
{
    /* openpete_osd_notify drops a post from a speculative or replayed tick;
     * a dropped SAVED would strand the held SAVING, so the reap waits. */
    if (!wait && (openpete_speculative || openpete_replaying)) return;
    br_job_t* j = s_bp.job;
    if (j && !j->saving_shown && SDL_GetAtomicInt(&j->noted)) {
        j->saving_shown = 1;
        openpete_osd_notify("host", BR_OSD_KEY, OPENPETE_OSD_TIMEOUT_HOLD, "BUG REPORT: SAVING...");
    }
    if (!j || (!wait && !SDL_GetAtomicInt(&j->finished))) return;
    SDL_WaitThread(s_bp.thr, NULL);
    s_bp.thr = NULL;
    s_bp.job = NULL;
    if (j->rc == 0) {
        s_bp.written++;
        OPL_INFO_F("bugreport", "wrote %s: %zu member(s), %llu bytes, %llu redaction(s), "
                   "%zu finding(s), %zu member(s) left out, %zu shipped flagged, press %llu us "
                   "on the sim thread, display %llu us, worker %llu ms; mods: %s; nothing was uploaded",
                   j->zip_path, j->members, (unsigned long long)j->zip_bytes,
                   (unsigned long long)j->replacements, j->findings, j->dropped, j->flagged,
                   (unsigned long long)j->press_us, (unsigned long long)j->display_us,
                   (unsigned long long)j->write_ms, j->mods_line[0] ? j->mods_line : "none");
        openpete_osd_notify("host", BR_OSD_KEY, 8000, "BUG REPORT: SAVED");
        /* The flagged members ship unchanged with the tester's identity in
         * them; one toast of its own, held longer, names them. */
        if (j->flag_line[0]) {
            OPL_WARN_F("bugreport", "%s", j->flag_line);
            openpete_osd_notify("host", BR_OSD_FLAG_KEY, 15000, "%s", j->flag_line);
        }
    } else {
        s_bp.failed++;
        OPL_ERROR_F("bugreport", "bundle %s not written (see the lines above)", j->zip_path);
        openpete_osd_notify("host", BR_OSD_KEY, 8000, "BUG REPORT FAILED - SEE THE LOG");
    }
    snprintf(s_bp.last_path, sizeof s_bp.last_path, "%s", j->zip_path);
    s_bp.last_members = j->members;
    s_bp.last_findings = j->findings;
    s_bp.last_bytes = j->zip_bytes;
    s_bp.last_replacements = j->replacements;
    s_bp.last_press_us = j->press_us;
    s_bp.last_display_us = j->display_us;
    s_bp.last_write_ms = j->write_ms;
    br_job_free(j);
}

int openpete_bugreport_due(int key_edge) OP_REQUIRES(sim)
{
    bundle_reap(0);
    int due = key_edge != 0;
    if (SDL_SetAtomicInt(&s_bp.requested, 0)) due = 1;
    s_bp.harness_press = 0;
    if (!due && s_bp.at_key >= 0 && (long)openpete_feed_key_now() >= s_bp.at_key) {
        s_bp.at_key = -1;
        s_bp.harness_press = 1;
        due = 1;
    }
    if (!due) return 0;
    if (s_bp.job) {
        s_bp.refused++;
        OPL_WARN_F("bugreport", "press refused: %s is still being written", s_bp.job->zip_path);
        /* Once SAVING shows, it already says so; before (the screenshot
         * and note prompt), the refusal gets its own word under the key,
         * which SAVING then repaints. */
        if (!s_bp.job->saving_shown)
            openpete_osd_notify("host", BR_OSD_KEY, 0, "BUG REPORT: STILL WRITING THE LAST ONE");
        return 0;
    }
    return 1;
}

/** @brief Create a directory when it does not exist.
 * @return 0 when it exists afterwards. */
static int br_mkdir(const char* dir)
{
    struct stat st;
    if (stat(dir, &st) == 0) return (st.st_mode & S_IFMT) == S_IFDIR ? 0 : -1;
#ifdef _WIN32
    return _mkdir(dir);
#else
    return mkdir(dir, 0755);
#endif
}

/** @brief Resolve the directory a bundle is written to: the bugreports data directory, created.
 *
 * Under a sandbox (a harness, a bug-report replay) the data directory lies
 * under the sandbox root (openpete_paths_sandbox), never among the user's
 * own bundles.
 * @param out Receives the directory, created.
 * @param cap Capacity of @p out.
 * @return 0, or -1 when no directory resolves (logged). */
static int bundle_out_dir(char* out, size_t cap)
{
    if (openpete_paths_data_dir("bugreports", out, cap, 1) == 0) return 0;
    OPL_ERROR_F("bugreport", "no bugreports data directory resolves");
    return -1;
}

/** @brief Name the bundle and create its staging directory under the bundle directory (bundle_out_dir).
 * @param j The job.
 * @return 0, or -1 when no directory could be made (logged). */
static int job_paths(br_job_t* j)
{
    SDL_Time now = 0;
    SDL_DateTime dt;
    memset(&dt, 0, sizeof dt);
    if (SDL_GetCurrentTime(&now)) (void)SDL_TimeToDateTime(now, &dt, true);
    const char* game = game_get_id();
    snprintf(j->game, sizeof j->game, "%s", game ? game : "unknown");
    snprintf(j->stem, sizeof j->stem, "bug-%s-%04d%02d%02d-%02d%02d%02d%s%s", j->game, dt.year,
             dt.month, dt.day, dt.hour, dt.minute, dt.second, j->fatal ? "-" : "",
             j->fatal ? j->fatal : "");
    if (bundle_out_dir(j->out_dir, sizeof j->out_dir) != 0) return -1;
    char leaf[128];
    snprintf(leaf, sizeof leaf, ".stage-%s", j->stem);
    openpete_paths_join(j->stage, sizeof j->stage, j->out_dir, leaf);
    char zleaf[128];
    snprintf(zleaf, sizeof zleaf, "%s.zip", j->stem);
    openpete_paths_join(j->zip_path, sizeof j->zip_path, j->out_dir, zleaf);
    openpete_paths_join(j->shot_path, sizeof j->shot_path, j->stage, "screen.png");
    if (br_mkdir(j->stage) != 0) {
        OPL_ERROR_F("bugreport", "cannot create %s: %s", j->stage, strerror(errno));
        return -1;
    }
    return 0;
}

/** @brief Write a report that takes a stream into a file of the staging directory.
 * @param j The job.
 * @param leaf The file name.
 * @param fn The report. */
static void stage_report(const br_job_t* j, const char* leaf, void (*fn)(FILE*))
{
    char path[1400];
    openpete_paths_join(path, sizeof path, j->stage, leaf);
    FILE* fp = fopen(path, "w");
    if (!fp) {
        OPL_WARN_F("bugreport", "cannot write %s: %s", path, strerror(errno));
        return;
    }
    fn(fp);
    fclose(fp);
}

/* A growable text buffer for the press's own text members. */
typedef struct { char* b; size_t n, cap; } tbuf_t;

/** @brief Append formatted text to a buffer; an allocation failure leaves it as it was. */
static void tb_printf(tbuf_t* t, const char* fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    const size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
    if (t->n + len + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        while (cap < t->n + len + 1) cap *= 2;
        char* g = (char*)realloc(t->b, cap);
        if (!g) return;
        t->b = g;
        t->cap = cap;
    }
    memcpy(t->b + t->n, line, len);
    t->n += len;
    t->b[t->n] = '\0';
}

/** @brief Build summary.txt: game, counters, level, player position, free camera, pause, runahead, rewind and the modset tag.
 * @param cpu The guest register file. */
static char* press_summary(const CPUState* cpu) OP_REQUIRES(sim)
{
    tbuf_t t = {0};
    const char* name = game_get_display_name();
    tb_printf(&t, "game=%s\ngame_name=%s\n", game_get_id() ? game_get_id() : "",
              name ? name : "");
    tb_printf(&t, "frame=%u\nvbl=%d\nfeed_key=%u\nconsume=%u\nconsume_write=%u\n", openpete_frame,
              SDL_GetAtomicInt(&openpete_vbl), openpete_feed_key_now(), openpete_consume,
              openpete_consume_write);
    tb_printf(&t, "park_ra=0x%08X\npark_sp=0x%08X\n", openpete_yield_site.ra,
              openpete_yield_site.sp);
    int32_t x = 0, y = 0, z = 0, yaw = 0, level = -1;
    const int pose = game_read_player_pose((CPUState*)cpu, &x, &y, &z, &yaw, &level);
    tb_printf(&t, "level=%d\n", pose >= 0 ? level : -1);
    if (pose >= 0) tb_printf(&t, "player=(%d, %d, %d)\nplayer_yaw=%d\n", x, y, z, yaw);
    else tb_printf(&t, "player=none\n");
    tb_printf(&t, "gameplay_live=%d\n", game_is_gameplay_live((CPUState*)cpu));
    tb_printf(&t, "freecam=%d\npaused=%d\nrunahead=%d\nrewind=%d\n",
              openpete_freecam_active() ? 1 : 0, host_session_paused() ? 1 : 0,
              openpete_runahead_get(), openpete_rewind_enabled());
    const char* tag = openpete_mods_modset_order_tag();
    tb_printf(&t, "modset=%s\n", tag && *tag ? tag : "(stock)");
    tb_printf(&t, "game_state=(no per-game report)\n");
    return t.b;
}

/** @brief Build env.txt: every set OPENPETE_* variable, one NAME=value per line. */
static char* press_env(void)
{
    tbuf_t t = {0};
    tb_printf(&t, "%s", "");
    for (char** e = BR_ENVIRON; e && *e; e++)
        if (!strncmp(*e, "OPENPETE_", 9)) tb_printf(&t, "%s\n", *e);
    return t.b;
}

/** @brief Write one config row's effective value as a TOML line.
 * @param t The buffer.
 * @param r The row. */
static void cfg_row_line(tbuf_t* t, const openpete_mods_cfg_row_t* r)
{
    switch (r->type) {
    case 'b': tb_printf(t, "%s = %s\n", r->key, r->ival ? "true" : "false"); break;
    case 'i': tb_printf(t, "%s = %lld\n", r->key, (long long)r->ival); break;
    case 'd': tb_printf(t, "%s = %.17g\n", r->key, r->fval); break;
    default: {
        tb_printf(t, "%s = \"", r->key);
        for (const char* c = r->sval ? r->sval : ""; *c; c++) {
            if (*c == '"' || *c == '\\') tb_printf(t, "\\%c", *c);
            else tb_printf(t, "%c", *c);
        }
        tb_printf(t, "\"\n");
    }
    }
}

/** @brief Record one loaded mod: id, version, state, directory, loaded object and its shipped config.toml text.
 *
 * The config text holds each declared non-secret row's effective value; a
 * secret row is a comment naming its key. Undeclared keys of the user's
 * file are listed by the worker, which reads the file.
 * @param m The record to fill.
 * @param i Index into the loaded mod table.
 * @return 1 when filled. */
static int press_mod(br_mod_t* m, int i) OP_REQUIRES(sim)
{
    openpete_mods_menu_info_t info;
    const char *dir = NULL, *obj = NULL;
    if (!openpete_mods_menu_info(i, &info) || !openpete_mods_bundle_paths(i, &dir, &obj)) return 0;
    snprintf(m->id, sizeof m->id, "%s", info.id);
    snprintf(m->version, sizeof m->version, "%s", info.version ? info.version : "");
    snprintf(m->dir, sizeof m->dir, "%s", dir ? dir : "");
    snprintf(m->obj, sizeof m->obj, "%s", obj ? obj : "");
    m->enabled = info.enabled;
    m->mutative = info.sim_mutative;
    const int rows = openpete_mods_cfg_row_count(i);
    tbuf_t t = {0};
    tb_printf(&t, "# The declared [[config]] rows' effective values at the press; a row\n"
                  "# marked secret and every undeclared key are named, not valued.\n");
    m->declared = rows > 0 ? (char**)calloc((size_t)rows, sizeof *m->declared) : NULL;
    for (int r = 0; r < rows && m->declared; r++) {
        openpete_mods_cfg_row_t row;
        if (!openpete_mods_cfg_row(i, r, &row)) continue;
        m->declared[m->ndeclared++] = strdup(row.key);
        if (row.secret) tb_printf(&t, "# %s: secret, value withheld\n", row.key);
        else cfg_row_line(&t, &row);
    }
    m->cfg = t.b;
    return 1;
}

/** @brief Record every loaded mod (press_mod) and the enabled ones' ids for the toast.
 * @param j The job. */
static void press_mods(br_job_t* j) OP_REQUIRES(sim)
{
    const int n = openpete_mods_count();
    if (n <= 0) return;
    j->mods = (br_mod_t*)calloc((size_t)n, sizeof *j->mods);
    if (!j->mods) return;
    for (int i = 0; i < n; i++) {
        if (!press_mod(&j->mods[j->nmods], i)) continue;
        const br_mod_t* m = &j->mods[j->nmods++];
        if (m->enabled) {
            const size_t at = strlen(j->mods_line);
            snprintf(j->mods_line + at, sizeof j->mods_line - at, "%s%s", at ? ", " : "", m->id);
        }
    }
}

/** @brief Copy the run's argv and disc path into the job. */
static void press_args(br_job_t* j, const openpete_options_t* opts)
{
    if (!opts) return;
    snprintf(j->disc_path, sizeof j->disc_path, "%s", opts->disc_path ? opts->disc_path : "");
    if (opts->handoff_argc <= 0 || !opts->handoff_argv) return;
    j->argv = (char**)calloc((size_t)opts->handoff_argc, sizeof *j->argv);
    if (!j->argv) return;
    for (int i = 0; i < opts->handoff_argc; i++)
        if (opts->handoff_argv[i]) j->argv[j->argc++] = strdup(opts->handoff_argv[i]);
}

/** @brief Copy the recorder's replay anchor, the segment from it, the hash rows since it and the taint since it.
 * @param j The job. */
static void press_anchor(br_job_t* j) OP_REQUIRES(sim)
{
    const openpete_bugrec_anchor_t* a = openpete_bugrec_pick();
    if (a && openpete_bugrec_anchor_copy(&j->anchor, a) == 0) {
        j->have_anchor = 1;
        if (openpete_bugrec_segment_bk2(a, &j->segment, &j->segment_len, &j->segment_rows) != 0) {
            j->segment = NULL;
            j->segment_len = j->segment_rows = 0;
        }
        openpete_bugrec_taint_since(a, j->taint);
    }
    j->hash_stride = openpete_bugrec_hash_stride();
    j->hash_n = openpete_bugrec_hash_rows_copy(a ? a->frame : openpete_frame, &j->hash_rows);
}

/** @brief Copy the host-trace, pace (30 s and coarse), input, hang, sub-tick present and audio feed event rings.
 *
 * The audio feed's counters are read under its mutex, except at a fatal
 * exit taken off the sim's thread, where a stuck thread may hold it; its
 * event ring is read without a lock.
 * @param j The job. */
static void press_rings(br_job_t* j) OP_REQUIRES(sim)
{
    const uint8_t* trace = NULL;
    const size_t nt = host_trace_ring_records(&trace);
    if (nt && (j->trace = (uint8_t*)malloc(nt * HT_REC_BYTES))) {
        memcpy(j->trace, trace, nt * HT_REC_BYTES);
        j->trace_n = nt;
    }
    if (openpete_pace_ring_image(OP_PACE_RING_WINDOW_NS, &j->pace, &j->pace_len, &j->pace_n) != 0) {
        j->pace = NULL;
        j->pace_len = j->pace_n = 0;
    }
    if (openpete_pace_coarse_image(&j->pace_coarse, &j->pace_coarse_len, &j->pace_coarse_rows) != 0) {
        j->pace_coarse = NULL;
        j->pace_coarse_len = j->pace_coarse_rows = 0;
    }
    j->input_n = openpete_bugrec_input_snapshot(&j->input);
    /* At a fatal exit the dump the watchdog just wrote holds these rings. */
    if (!j->fatal) j->hang_len = openpete_hang_rings_image(&j->hang);
    if (openpete_bugthumbs_enabled()) j->thumbs_n = openpete_bugthumbs_copy(&j->thumbs);
    j->subtick_n = openpete_bugrec_subtick_image(&j->subtick, &j->subtick_len);
    j->audio = openpete_audio_feed_snapshot(!j->fatal || !strcmp(j->exit_thread, "sim"), 1);
    j->press_present_ns = (int64_t)openpete_interp_present_now();
}

/** @brief Run the worker on its thread.
 * @param p The job.
 * @return 0. */
static int bundle_worker(void* p)
{
    OP_ROLE_BIND(bugreport);
    br_job_t* j = (br_job_t*)p;
    br_job_write(j);
    SDL_SetAtomicInt(&j->finished, 1);
    return 0;
}

int openpete_bugreport_press(const CPUState* cpu, const openpete_options_t* opts) OP_REQUIRES(sim)
{
    const uint64_t t0 = SDL_GetTicksNS();
    s_bp.presses++;
    br_job_t* j = (br_job_t*)calloc(1, sizeof *j);
    if (!j || job_paths(j) != 0 || openpete_bugrec_capture(&j->press, cpu) != 0) {
        OPL_ERROR_F("bugreport", "press failed: %s", j ? "no staging directory or state capture" : "out of memory");
        openpete_osd_notify("host", BR_OSD_KEY, 8000, "BUG REPORT FAILED - SEE THE LOG");
        if (j) openpete_bugrec_anchor_release(&j->press);
        free(j);
        s_bp.failed++;
        return -1;
    }
    j->t_press_ns = t0;
    /* The note is for a person: a harness press asks for it only under
     * OPENPETE_BUGREPORT_NOTE=1, any other press unless it is 0. */
    j->note_wanted = s_bp.harness_press ? s_bp.note_env == 1 : s_bp.note_env != 0;
    press_anchor(j);
    press_rings(j);
    openpete_log_capture_begin();
    host_session_press_reports();
    j->reports = openpete_log_capture_end(&j->reports_len);
    stage_report(j, "config.txt", openpete_config_report);
    stage_report(j, "keymap.txt", openpete_keymap_report);
    openpete_bugknobs_poll(opts);
    j->knobs = openpete_bugknobs_text();
    j->warps = openpete_bugrec_warps_text();
    j->summary = press_summary(cpu);
    j->env = press_env();
    host_session_pace_env(j->pace_env, sizeof j->pace_env);
    press_mods(j);
    press_args(j, opts);
    snprintf(j->log_path, sizeof j->log_path, "%s", openpete_log_file_path());
    j->press_us = (SDL_GetTicksNS() - t0) / 1000u;
    /* The answers the other threads owe: the present services' image and
     * display reports, the pump's device facts. */
    s_bp.shot_job = j;
    SDL_SetAtomicPointer(&s_bp.main_job, j);
    s_bp.job = j;
    s_bp.thr = SDL_CreateThread(bundle_worker, "openpete-bugreport", j);
    if (!s_bp.thr) {
        OPL_ERROR_F("bugreport", "worker thread not created: %s; writing on the sim thread",
                    SDL_GetError());
        SDL_SetAtomicInt(&j->shot, BR_ABSENT);
        SDL_SetAtomicInt(&j->display, BR_ABSENT);
        s_bp.shot_job = NULL;
        bundle_worker(j);
        s_bp.job = NULL;
        bundle_reap(1);
        return 0;
    }
    OPL_INFO_F("bugreport", "press at frame %u (feed key %u): writing %s; anchor %s, %zu segment "
               "row(s), %zu hash row(s); %llu us on the sim thread", openpete_frame,
               openpete_feed_key_now(), j->zip_path,
               j->have_anchor ? "held" : "none", j->segment_rows, j->hash_n,
               (unsigned long long)j->press_us);
    return 0;
}

int openpete_bugreport_take_shot(char* out, size_t cap) OP_REQUIRES(sim)
{
    br_job_t* j = s_bp.shot_job;
    if (!j || SDL_GetAtomicInt(&j->shot) != BR_PENDING) return 0;
    snprintf(out, cap, "%s", j->shot_path);
    j->shot_t0_ns = SDL_GetTicksNS();
    return 1;
}

void openpete_bugreport_display_service(int shot_ok) OP_REQUIRES(sim)
{
    br_job_t* j = s_bp.shot_job;
    if (!j) return;
    s_bp.shot_job = NULL;
    openpete_log_capture_begin();
    host_session_display_reports();
    size_t n = 0;
    char* txt = openpete_log_capture_end(&n);
    char dev[4096];
    const int dn = openpete_native_render_enabled() ? openpete_native_render_device_report(dev, sizeof dev) : 0;
    if (dn > 0 && txt && (size_t)dn < sizeof dev) {
        char* g = (char*)realloc(txt, n + (size_t)dn + 1);
        if (g) {
            memcpy(g + n, dev, (size_t)dn + 1);
            txt = g;
            n += (size_t)dn;
        }
    }
    j->display_us = (SDL_GetTicksNS() - (j->shot_t0_ns ? j->shot_t0_ns : SDL_GetTicksNS())) / 1000u;
    j->display_txt = txt;
    j->display_len = n;
    if (!SDL_CompareAndSwapAtomicInt(&j->display, BR_PENDING, BR_DONE)) {
        free(txt);
        j->display_txt = NULL;
        j->display_len = 0;
    }
    (void)SDL_CompareAndSwapAtomicInt(&j->shot, BR_PENDING, shot_ok ? BR_DONE : BR_ABSENT);
}

/** @brief Append the monitors and the SDL video and audio drivers to a buffer; main thread. */
static void main_displays(tbuf_t* t)
{
    tb_printf(t, "sdl: version=%d video_driver=%s audio_driver=%s\n", SDL_GetVersion(),
              SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none",
              SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none");
    SDL_AudioSpec spec;
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames))
        tb_printf(t, "audio: default playback format=0x%04x channels=%d rate=%d frames=%d\n",
                  (unsigned)spec.format, spec.channels, spec.freq, frames);
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    for (int i = 0; ids && i < n; i++) {
        const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(ids[i]);
        const char* name = SDL_GetDisplayName(ids[i]);
        tb_printf(t, "display id=%u name=\"%s\" mode=%dx%d refresh=%.3f density=%.2f scale=%.2f\n",
                  (unsigned)ids[i], name ? name : "", m ? m->w : 0, m ? m->h : 0,
                  m ? (double)m->refresh_rate : 0.0, m ? (double)m->pixel_density : 0.0,
                  (double)SDL_GetDisplayContentScale(ids[i]));
    }
    SDL_free(ids);
}

void openpete_bugreport_main_poll(void)
{
    br_job_t* j = (br_job_t*)SDL_GetAtomicPointer(&s_bp.main_job);
    if (!j || !SDL_CompareAndSwapAtomicPointer(&s_bp.main_job, j, NULL)) return;
    char path[1400];
    openpete_paths_join(path, sizeof path, j->stage, "devices.txt");
    FILE* fp = fopen(path, "w");
    if (fp) {
        openpete_bugrec_devices_write_allowlisted(fp);
        fclose(fp);
    }
    tbuf_t t = {0};
    main_displays(&t);
    j->main_txt = t.b;
    j->main_len = t.n;
    SDL_SetAtomicInt(&j->main_facts, BR_DONE);
}

void openpete_bugreport_shutdown(void) OP_REQUIRES(sim)
{
    br_job_t* j = s_bp.job;
    if (!j) return;
    /* No present follows: the image and the display reports are absent. */
    if (s_bp.shot_job == j) {
        s_bp.shot_job = NULL;
        (void)SDL_CompareAndSwapAtomicInt(&j->display, BR_PENDING, BR_ABSENT);
        (void)SDL_CompareAndSwapAtomicInt(&j->shot, BR_PENDING, BR_ABSENT);
    }
    /* No tester answers a note at exit: the worker finalizes without one. */
    openpete_devui_note_end();
    OPL_INFO_F("bugreport", "waiting for %s", j->zip_path);
    bundle_reap(1);
}

void openpete_bugreport_report(void)
{
    openpete_bugthumbs_report();
    if (!s_bp.presses && !s_bp.refused) return;
    OPL_INFO_F("bugreport", "census: %llu press(es), %llu written, %llu failed, %llu refused "
               "in flight; last %s: %zu member(s), %llu bytes, %llu redaction(s), %zu finding(s), "
               "press %llu us, display %llu us, worker %llu ms",
               s_bp.presses, s_bp.written, s_bp.failed, s_bp.refused,
               s_bp.last_path[0] ? s_bp.last_path : "-", s_bp.last_members,
               (unsigned long long)s_bp.last_bytes, (unsigned long long)s_bp.last_replacements,
               s_bp.last_findings, (unsigned long long)s_bp.last_press_us,
               (unsigned long long)s_bp.last_display_us, (unsigned long long)s_bp.last_write_ms);
}

/** @brief Copy a fatal exit's sim-side state into its job on the exiting thread: the replay window, the rings, the summary, the mods, the pacing environment and, on the sim's own thread, the subsystem reports.
 *
 * Runs on the thread that is exiting, after the movie's flush, which keeps
 * the input log's lock for good: the lock is recursive, so only the thread
 * holding it can read the segment. On the sim's own thread no guest code
 * runs during the copy: the host and game fibers share that OS thread, and
 * the exit is taken either on the host fiber or on the game fiber inside a
 * guest call (a hang-watchdog trip or stall request taken at an entry or
 * loop poll, or the CdSync spin limit), which suspends the guest mid-tick,
 * so the copy is consistent but need not sit at a tick boundary. Off it (the
 * stall monitor's fallback dump on its own thread, taken when the game
 * fiber reached no entry or poll within the handoff and may still be
 * executing guest code, or a fatal exit on another thread) nothing stops
 * the sim, so the anchor, rings, summary and mod rows copied here may be
 * torn; the subsystem reports are not run there, since the running sim may
 * hold what they read.
 * @param j The job. */
static void fatal_collect(br_job_t* j)
{
    OP_TSA_SUPPRESS_BEGIN("fatal exit: the sim's state is copied unlocked; off the sim thread the copy may be torn")
    press_anchor(j);
    press_rings(j);
    if (!strcmp(j->exit_thread, "sim")) {
        openpete_log_capture_begin();
        host_session_press_reports();
        j->reports = openpete_log_capture_end(&j->reports_len);
    } else {
        j->reports = strdup("(not run: the exit was taken off the sim's thread)\n");
        j->reports_len = j->reports ? strlen(j->reports) : 0;
    }
    j->summary = press_summary(&g_game.cpu);
    press_mods(j);
    host_session_pace_env(j->pace_env, sizeof j->pace_env);
    OP_TSA_SUPPRESS_END
}

/** @brief Collect a fatal exit's collectors and write its bundle: config, keymap, environment variables, argv, then the worker's write.
 *
 * Runs on its own thread while the exiting thread waits, so a collector or
 * a write that blocks cannot keep the process from exiting.
 * @param p The job.
 * @return 0. */
static int fatal_worker(void* p)
{
    OP_ROLE_BIND(bugreport);
    br_job_t* j = (br_job_t*)p;
    stage_report(j, "config.txt", openpete_config_report);
    stage_report(j, "keymap.txt", openpete_keymap_report);
    j->knobs = openpete_bugknobs_text();
    j->warps = openpete_bugrec_warps_text();
    j->env = press_env();
    press_args(j, s_bp.opts);
    snprintf(j->log_path, sizeof j->log_path, "%s", openpete_log_file_path());
    j->press_us = (SDL_GetTicksNS() - j->t_press_ns) / 1000u;
    SDL_SetAtomicPointer(&s_bp.main_job, j);
    br_job_write(j);
    SDL_SetAtomicInt(&j->finished, 1);
    return 0;
}

/** @brief Wait for a job's worker to set its finished flag, up to a bound.
 * @param j The job.
 * @param ms The bound.
 * @return 1 when it finished. */
static int fatal_wait(br_job_t* j, unsigned ms)
{
    const uint64_t end = SDL_GetTicksNS() + (uint64_t)ms * 1000000u;
    while (!SDL_GetAtomicInt(&j->finished)) {
        if (SDL_GetTicksNS() >= end) return 0;
        SDL_Delay(5);
    }
    return 1;
}

/** @brief The bug-report slot of the fatal exit flush: write a bundle of the run that is exiting.
 *
 * Waits for a bundle already in flight first (its answers are bounded),
 * copies the sim-side state on this thread (fatal_collect), then runs the
 * collectors and the write on a worker thread, waiting up to
 * BR_FATAL_WAIT_MS for each, so a collector that blocks cannot keep the
 * process from exiting. The bundle is left unfreed; the process exits. */
static void bundle_exit_flush(void)
{
    br_job_t* inflight = s_bp.job;
    if (inflight && !fatal_wait(inflight, BR_FATAL_WAIT_MS))
        OPL_WARN_F("bugreport", "fatal exit: %s still being written after %u ms", inflight->zip_path,
                   BR_FATAL_WAIT_MS);
    br_job_t* j = (br_job_t*)calloc(1, sizeof *j);
    if (!j) return;
    const char* why = openpete_crash_exit_reason();
    j->fatal = why && *why ? why : "fatal";
    j->exit_thread = SDL_GetCurrentThreadID() == s_bp.sim_tid ? "sim" : "other";
    j->t_press_ns = SDL_GetTicksNS();
    snprintf(j->note_state, sizeof j->note_state, "%s", "fatal exit");
    if (job_paths(j) != 0) return;
    SDL_SetAtomicInt(&j->shot, BR_ABSENT);
    SDL_SetAtomicInt(&j->display, BR_ABSENT);
    fatal_collect(j);
    OPL_INFO_F("bugreport", "fatal exit (%s) on the %s thread: writing %s; anchor %s, %zu segment "
               "row(s), %zu hash row(s); %llu us collecting", j->fatal, j->exit_thread, j->zip_path,
               j->have_anchor ? "held" : "none", j->segment_rows, j->hash_n,
               (unsigned long long)((SDL_GetTicksNS() - j->t_press_ns) / 1000u));
    SDL_Thread* t = SDL_CreateThread(fatal_worker, "openpete-bugreport-fatal", j);
    if (!t) {
        OPL_ERROR_F("bugreport", "fatal exit: worker thread not created: %s", SDL_GetError());
        return;
    }
    if (!fatal_wait(j, BR_FATAL_WAIT_MS)) {
        SDL_DetachThread(t);
        OPL_ERROR_F("bugreport", "fatal exit: %s not finished after %u ms; exiting without it",
                    j->zip_path, BR_FATAL_WAIT_MS);
        return;
    }
    SDL_WaitThread(t, NULL);
    if (j->rc == 0)
        OPL_INFO_F("bugreport", "wrote %s at the fatal exit (%s): %zu member(s), %llu bytes, "
                   "%llu redaction(s), %zu finding(s), %zu member(s) left out, %zu shipped flagged, "
                   "worker %llu ms; nothing was uploaded",
                   j->zip_path, j->fatal, j->members, (unsigned long long)j->zip_bytes,
                   (unsigned long long)j->replacements, j->findings, j->dropped, j->flagged,
                   (unsigned long long)j->write_ms);
    else
        OPL_ERROR_F("bugreport", "fatal exit: bundle %s not written (see the lines above)", j->zip_path);
    if (j->rc == 0 && j->flag_line[0]) OPL_WARN_F("bugreport", "%s", j->flag_line);
}

/* Documented at the declaration in bugreport_internal.h. */
int br_main_withdraw(br_job_t* j)
{
    return SDL_CompareAndSwapAtomicPointer(&s_bp.main_job, j, NULL) ? 1 : 0;
}
