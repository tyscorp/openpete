/* tests/unit/mod_save_sidecar_test.c - the per-card .modsave file
 * (core/mods/mod_save.c) on disk.
 *
 *   unreadable  a sidecar that exists but cannot be read (mode 0200) at
 *               the mount and at the save: a save leaves it byte-identical;
 *               once readable again, the next save merges the file's
 *               snapshots with the ones made meanwhile.
 *   sequence    two processes mount one card's sidecar; A saves index 1,
 *               then B saves index 2: the file holds both, and index 0
 *               from before. Both save index 3 (A, then B): B's snapshot
 *               first, then A's.
 *   concurrent  two processes each save 40 snapshots to their own index
 *               at once: each index holds its process's newest MS_CAP.
 *
 * The module is included whole so the cases build ops and read tables with
 * its own helpers; the atomic write and the lock are the real ones, other
 * host services are stubs. Usage: mod_save_sidecar_test <scratch dir>. */

#include "../../core/mods/mod_save.c"

#include <stddef.h>
#include <sys/stat.h>
#include <sys/wait.h>

int      openpete_speculative = 0;
unsigned openpete_frame = 0u;

int openpete_memcard_file_offset(int slot, const char* name) { (void)slot; (void)name; return -1; }
int openpete_memcard_file_view(int slot, const char* name, const uint8_t** out, size_t* len) {
    (void)slot; (void)name; (void)out; (void)len;
    return -1;
}
int openpete_memcard_held_path(int slot, char* out, size_t cap) { (void)slot; (void)out; (void)cap; return 0; }
int openpete_memcard_mismatch_toast(void) { return 0; }
const char* openpete_memcard_modset_tag(int slot) { (void)slot; return ""; }
int openpete_mods_bypass_pending(uint32_t addr) { (void)addr; return 0; }
int openpete_mods_chain_has_owner(uint32_t addr, const char* owner_id) { (void)addr; (void)owner_id; return 0; }
int openpete_mods_count(void) { return 0; }
/* A save descriptor with three records: card_mounted reads sidecars only for a game that has one. */
static const char* const k_names[] = { "BASLUS-00000", NULL };
static openpete_save_desc_t s_desc = { .record_count = 3, .file_names = k_names };
static game_mod_desc_t s_game = { .save_desc = &s_desc };
const game_mod_desc_t* openpete_mods_game_desc(void) { return &s_game; }
void openpete_mods_guard_enter(const char* mod_id) { (void)mod_id; }
void openpete_mods_guard_leave(void) {}
/** @brief Stub: open a dispatch site.
 * @param ctx the site's kind.
 * @return the enclosing site, always NONE here. */
openpete_mods_site_t openpete_mods_site_open(openpete_mods_ctx ctx) {
    (void)ctx;
    const openpete_mods_site_t none = {OPENPETE_MODS_CTX_NONE, 0, 0};
    return none;
}
/** @brief Stub: close a dispatch site.
 * @param prev the value the open returned. */
void openpete_mods_site_close(openpete_mods_site_t prev) { (void)prev; }
const char* openpete_mods_id(int i) { (void)i; return ""; }
const char* openpete_mods_id_of(openpete_mod_t* self) { (void)self; return ""; }
int openpete_mods_is_enabled(const char* id) { (void)id; return 0; }
const char* openpete_mods_modset_tag(void) { return ""; }
void openpete_osd_notify(const char* source, const char* key, uint32_t timeout_ms, const char* fmt, ...) {
    (void)source; (void)key; (void)timeout_ms; (void)fmt;
}
void* openpete_psx_to_host_ptr(uint32_t addr) { (void)addr; return NULL; }

static int s_fail;
static char s_card[1024], s_side[1100];

#define CHECK(cond, ...) do {                                             \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);  \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_fail = 1; } \
} while (0)

/** @brief Commit a snapshot at card slot 0, save @p idx: hash bytes @p tag, lineage bytes @p who, one record of mod @p who's id. */
static void save(int idx, uint8_t tag, uint8_t who) {
    uint8_t h[32], lin[16];
    memset(h, tag, sizeof h);
    memset(lin, who, sizeof lin);
    ms_snap_t* sn = snap_new(h, lin, "");
    const char id[] = { 'm', 'o', 'd', (char)('A' + who), '\0' };
    snap_add(sn, id, 1u, 0u, &tag, 1u);
    const ms_qop_t op = { Q_SNAP, 0, idx, sn };
    commit_op(&op);
}

/** @brief Mount the test card in slot 0 (reads its sidecar). */
static void mount(void) { openpete_modsave_card_mounted(0, s_card, 0); }

/** @brief The sidecar file's bytes; NULL when it cannot be read. */
static uint8_t* file_bytes(size_t* len) {
    uint8_t* p = NULL;
    return sidecar_read(s_side, &p, len) == 1 ? p : NULL;
}

/** @brief The hash tags of the sidecar file's snapshots at @p idx, newest first, as a string ("" for none, "?" when unreadable or unparseable); eight calls' results stay valid at once. */
static const char* tags_at(int idx) {
    static char ring[8][MS_CAP + 1];
    static int next;
    char* out = ring[next++ & 7];
    size_t len = 0;
    uint8_t* p = file_bytes(&len);
    ms_table_t* d = (ms_table_t*)calloc(1, sizeof *d);
    if (!p || !d || sidecar_parse(d, p, len) != 0) { free(p); if (d) table_clear(d); free(d); return "?"; }
    uint32_t n = 0;
    for (; n < d->idx[idx].n; n++) out[n] = (char)d->idx[idx].s[n]->hash[0];
    out[n] = '\0';
    table_clear(d);
    free(d);
    free(p);
    return out;
}

/** @brief Remove the sidecar and its lock file. */
static void reset_files(void) {
    char lk[1200];
    snprintf(lk, sizeof lk, "%s.lock", s_side);
    remove(s_side);
    remove(lk);
}

/** @brief An unreadable sidecar is never written over and is merged into once readable. */
static void case_unreadable(void) {
    reset_files();
    mount();
    save(0, 'a', 0);
    size_t len0 = 0;
    uint8_t* before = file_bytes(&len0);
    CHECK(before != NULL, "unreadable: no sidecar after the first save");
    chmod(s_side, 0200);
    mount();
    CHECK(s_sc[0].state == SC_UNREADABLE, "unreadable: mount state %d, want SC_UNREADABLE", s_sc[0].state);
    save(1, 'b', 0);
    chmod(s_side, 0600);
    size_t len1 = 0;
    uint8_t* after = file_bytes(&len1);
    CHECK(before && after && len0 == len1 && !memcmp(before, after, len0),
          "unreadable: the sidecar changed under a save (%zu -> %zu bytes)", len0, len1);
    free(before);
    free(after);
    /* Unreadable at the save only: mounted readable, then made unreadable. */
    mount();
    chmod(s_side, 0200);
    save(2, 'c', 0);
    chmod(s_side, 0600);
    CHECK(!strcmp(tags_at(0), "a") && !strcmp(tags_at(2), ""),
          "unreadable at the save: index 0 \"%s\" index 2 \"%s\", want \"a\" and \"\"", tags_at(0), tags_at(2));
    /* Readable again: the next save merges the file's index 0 with this process's saves. */
    save(1, 'd', 0);
    CHECK(!strcmp(tags_at(0), "a"), "recovered: index 0 \"%s\", want \"a\"", tags_at(0));
    CHECK(!strcmp(tags_at(1), "d"), "recovered: index 1 \"%s\", want \"d\"", tags_at(1));
    CHECK(!strcmp(tags_at(2), "c"), "recovered: index 2 \"%s\", want \"c\"", tags_at(2));
    printf("unreadable: file unchanged by a save while unreadable; merged once readable "
           "(index 0 \"%s\", 1 \"%s\", 2 \"%s\")\n", tags_at(0), tags_at(1), tags_at(2));
}

/** @brief Block until one byte arrives on @p fd. */
static void wait_on(int fd) { char c; if (read(fd, &c, 1) != 1) _exit(3); }
/** @brief Send one byte on @p fd. */
static void signal_on(int fd) { if (write(fd, "x", 1) != 1) _exit(3); }

/** @brief Two processes on one card in sequence: A saves index 1, then B saves index 2; both then save index 3. */
static void case_sequence(void) {
    reset_files();
    mount();
    save(0, 'a', 0);
    int to_b[2], to_a[2];
    if (pipe(to_b) || pipe(to_a)) { CHECK(0, "pipe"); return; }
    const pid_t b = fork();
    if (b == 0) {
        mount();                        /* B holds index 0 only */
        signal_on(to_a[1]);
        wait_on(to_b[0]);               /* A saved index 1 */
        save(2, 'c', 1);
        signal_on(to_a[1]);
        wait_on(to_b[0]);               /* A saved index 3 */
        save(3, 'f', 1);
        _exit(0);
    }
    wait_on(to_a[0]);                   /* B mounted */
    save(1, 'b', 0);
    signal_on(to_b[1]);
    wait_on(to_a[0]);                   /* B saved index 2 */
    save(3, 'e', 0);
    signal_on(to_b[1]);
    int st = 0;
    waitpid(b, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "sequence: process B failed");
    CHECK(!strcmp(tags_at(0), "a"), "sequence: index 0 \"%s\", want \"a\"", tags_at(0));
    CHECK(!strcmp(tags_at(1), "b"), "sequence: index 1 \"%s\", want \"b\" (A's save)", tags_at(1));
    CHECK(!strcmp(tags_at(2), "c"), "sequence: index 2 \"%s\", want \"c\" (B's save)", tags_at(2));
    CHECK(!strcmp(tags_at(3), "fe"), "sequence: index 3 \"%s\", want \"fe\" (B's, then A's)", tags_at(3));
    printf("sequence: index 0 \"%s\", 1 \"%s\", 2 \"%s\", 3 \"%s\"\n", tags_at(0), tags_at(1), tags_at(2), tags_at(3));
    close(to_b[0]); close(to_b[1]); close(to_a[0]); close(to_a[1]);
}

/** @brief Two processes each save 40 snapshots to their own index at once. */
static void case_concurrent(void) {
    reset_files();
    mount();
    save(0, 'a', 0);
    int go[2];
    if (pipe(go)) { CHECK(0, "pipe"); return; }
    pid_t kid[2];
    for (int w = 0; w < 2; w++) {
        kid[w] = fork();
        if (kid[w] == 0) {
            mount();
            char c;
            if (read(go[0], &c, 1) != 1) _exit(3);
            for (int k = 0; k < 40; k++) save(4 + w, (uint8_t)('A' + k), (uint8_t)(2 + w));
            _exit(0);
        }
    }
    if (write(go[1], "xx", 2) != 2) CHECK(0, "release");
    for (int w = 0; w < 2; w++) {
        int st = 0;
        waitpid(kid[w], &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "concurrent: process %d failed", w);
    }
    /* Each index: the newest MS_CAP of its process's 40 saves ('A' + 39 first). */
    char want[MS_CAP + 1];
    for (int k = 0; k < MS_CAP; k++) want[k] = (char)('A' + 39 - k);
    want[MS_CAP] = '\0';
    CHECK(!strcmp(tags_at(4), want), "concurrent: index 4 \"%s\", want \"%s\"", tags_at(4), want);
    CHECK(!strcmp(tags_at(5), want), "concurrent: index 5 \"%s\", want \"%s\"", tags_at(5), want);
    CHECK(!strcmp(tags_at(0), "a"), "concurrent: index 0 \"%s\", want \"a\"", tags_at(0));
    printf("concurrent: index 4 \"%s\", 5 \"%s\", 0 \"%s\"\n", tags_at(4), tags_at(5), tags_at(0));
    close(go[0]); close(go[1]);
}

/** @brief Run the cases in the scratch directory argv[1]. */
int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: mod_save_sidecar_test <scratch dir>\n"); return 2; }
    if (getuid() == 0) { printf("mod_save_sidecar_test: SKIP (root reads a 0200 file)\n"); return 0; }
    mkdir(argv[1], 0755);
    snprintf(s_card, sizeof s_card, "%s/card1.mcr", argv[1]);
    snprintf(s_side, sizeof s_side, "%s.modsave", s_card);
    case_unreadable();
    case_sequence();
    case_concurrent();
    reset_files();
    printf("mod_save_sidecar_test: %s\n", s_fail ? "FAIL" : "PASS");
    return s_fail;
}
