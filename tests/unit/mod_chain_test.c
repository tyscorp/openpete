/* tests/unit/mod_chain_test.c - the override frame stack of
 * core/mods/mod_chain.c against stub fixture links. A link that removes its
 * own owner's links, or another owner's links ahead of it, while its frame
 * is live still reaches every remaining link below it once through base(),
 * including a link removed at index 0 (the frame's index -1) and a removal
 * followed by a re-insert at the front; base() and crash attribution under
 * a dispatch site opened over a live frame see no frame of theirs; a bus
 * frame is refused by base(), named by crash attribution and left alone by
 * the re-anchor. The module is included whole, so a case can push a bus
 * frame through the same bracket the walkers use; the host services it
 * calls are stubs, and the original body is a counter. */

#include "../../core/mods/mod_chain.c"
#include "psx_runtime.h" /* CPUState */

#define ADDR_X 0x80010000u

static game_mod_desc_t s_game; /* window [0, 0): no overlay filter */
const game_mod_desc_t* openpete_mods_game_desc(void) { return &s_game; }
uint32_t (*feature_registry_overlay_probe(void))(void) { return NULL; }
static const char* s_hook_owner;
const char* openpete_imgui_guard_hook_owner(void) { return s_hook_owner; }
void openpete_imgui_guard_set_hook_owner(const char* id) { s_hook_owner = id; }
void openpete_mods_link_registered(const char* owner_id, uint32_t addr, uint32_t overlay) {
    (void)owner_id; (void)addr; (void)overlay;
}
op_time_t op_now(void) { return 0; }
SDL_ThreadID SDL_GetCurrentThreadID(void) { return 1; }

static int s_orig;   /* recompiled-original runs */
static int s_runs_b, s_runs_c;

/** @brief The dispatch layer: the callee's prologue reaches openpete_mods_dispatch, and the body runs when it returns 0. */
void call_by_address(CPUState* cpu, uint32_t addr) {
    if (!openpete_mods_dispatch(cpu, addr)) s_orig++;
}

static int s_fail;

#define CHECK(cond, msg) do {                                             \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); s_fail = 1; } \
} while (0)

/** @brief Drop every chain, owner state and frame, so each case starts empty. */
static void reset(void) {
    for (int i = 0; i < chain_count(); i++) mod_fvec_truncate(&chain_at(i)->links, 0);
    mod_fvec_truncate(&s_owner_state, 0);
    s_live_links = 0;
    s_frame_depth = 0;
    s_site.ctx = OPENPETE_MODS_CTX_GUEST;
    s_site.depth = 0;
    s_orig = s_runs_b = s_runs_c = 0;
}

/** @brief Link of owner B: counts and continues. */
static void link_b(CPUState* cpu) { s_runs_b++; openpete_mods_base(cpu); }
/** @brief Link of owner C: counts and continues. */
static void link_c(CPUState* cpu) { s_runs_c++; openpete_mods_base(cpu); }
/** @brief A link that never runs (its owner is toggled off). */
static void link_d(CPUState* cpu) { (void)cpu; CHECK(0, "a disabled link ran"); }

static const char* s_remove; /* the owner link_a removes before its base() */
static int s_reinsert;       /* nonzero: link_a re-adds a D link at the front */

/** @brief Owner A: removes s_remove's links (and re-adds one D link) while its frame is live, then continues. */
static void link_a(CPUState* cpu) {
    if (s_remove) openpete_mods_chain_remove_owner(s_remove);
    if (s_reinsert) (void)openpete_mods_chain_add(ADDR_X, link_d, "D");
    openpete_mods_base(cpu);
}

/** @brief Dispatch ADDR_X once and check every live link below A ran once and the original once. */
static void run_and_check(const char* what, int want_b, int want_c) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    CHECK(openpete_mods_dispatch(&cpu, ADDR_X) == 1, what);
    if (s_runs_b != want_b || s_runs_c != want_c || s_orig != 1) {
        fprintf(stderr, "FAIL %s: B ran %d (want %d), C ran %d (want %d), original %d (want 1)\n",
                what, s_runs_b, want_b, s_runs_c, want_c, s_orig);
        s_fail = 1;
    }
    CHECK(s_frame_depth == 0, "frame stack unwound");
}

/** @brief A removes its own link at index 0: the frame holds -1 and base() runs B, the new index 0. */
static void case_own_link_head(void) {
    reset();
    (void)openpete_mods_chain_add(ADDR_X, link_b, "B");
    (void)openpete_mods_chain_add(ADDR_X, link_a, "A");
    s_remove = "A"; s_reinsert = 0;
    run_and_check("own link at index 0 removed", 1, 0);
}

/** @brief A's own link at index 0 is removed and a D link inserted at the front: base() still runs B. */
static void case_own_link_head_reinsert(void) {
    reset();
    (void)openpete_mods_chain_add(ADDR_X, link_b, "B");
    (void)openpete_mods_chain_add(ADDR_X, link_a, "A");
    openpete_mods_chain_set_owner_enabled("D", 0);
    s_remove = "A"; s_reinsert = 1;
    run_and_check("own link removed, a link re-added at the front", 1, 0);
}

/** @brief A toggled-off D link ahead of A is removed while A's frame is live: base() runs C. */
static void case_passed_link(void) {
    reset();
    (void)openpete_mods_chain_add(ADDR_X, link_c, "C");
    (void)openpete_mods_chain_add(ADDR_X, link_a, "A");
    (void)openpete_mods_chain_add(ADDR_X, link_d, "D");
    openpete_mods_chain_set_owner_enabled("D", 0);
    s_remove = "D"; s_reinsert = 0;
    run_and_check("passed link removed", 0, 1);
}

/** @brief Two D links ahead of A are removed and one re-added (a reload): base() runs C once. */
static void case_two_passed_links_reload(void) {
    reset();
    (void)openpete_mods_chain_add(ADDR_X, link_c, "C");
    (void)openpete_mods_chain_add(ADDR_X, link_a, "A");
    (void)openpete_mods_chain_add(ADDR_X, link_d, "D");
    (void)openpete_mods_chain_add(ADDR_X, link_d, "D");
    openpete_mods_chain_set_owner_enabled("D", 0);
    s_remove = "D"; s_reinsert = 1;
    run_and_check("two passed links removed, one re-added", 0, 1);
}

/** @brief A link behind A is removed: A's frame does not move and base() runs the original. */
static void case_link_behind(void) {
    reset();
    (void)openpete_mods_chain_add(ADDR_X, link_c, "C");
    (void)openpete_mods_chain_add(ADDR_X, link_a, "A");
    s_remove = "C"; s_reinsert = 0;
    run_and_check("link behind the frame removed", 0, 0);
}

static int s_site_checked;

/** @brief Owner S: opens a toggle site over its own live frame, as the tick boundary does over a parked fiber, and checks base() and attribution there. */
static void link_site(CPUState* cpu) {
    openpete_mods_attribution_t at;
    uint32_t addr = 0;
    CHECK(openpete_mods_frame_owner_live("S", &addr) && addr == ADDR_X, "S frame live");
    CHECK(!openpete_mods_frame_owner_live("C", NULL), "C frame not live yet");
    const openpete_mods_site_t prev = openpete_mods_site_open(OPENPETE_MODS_CTX_TOGGLE);
    CHECK(openpete_mods_site_now().tick, "a toggle hook under GUEST is tick class");
    openpete_mods_base(cpu);
    CHECK(s_runs_c == 0 && s_orig == 0, "base() over a frame below the site ran the chain");
    CHECK(!openpete_mods_crash_attribution(&at), "attribution named a frame below the site");
    const openpete_mods_site_t inner = openpete_mods_site_open(OPENPETE_MODS_CTX_SHUTDOWN);
    CHECK(!openpete_mods_site_now().tick, "a shutdown hook is not tick class");
    openpete_mods_site_close(inner);
    openpete_mods_site_close(prev);
    CHECK(openpete_mods_crash_attribution(&at) && at.kind == OPENPETE_MODS_FRAME_OVERRIDE &&
          at.addr == ADDR_X && strcmp(at.owner, "S") == 0, "attribution names S's override");
    openpete_mods_base(cpu);
    s_site_checked = 1;
}

/** @brief base() and attribution under a site opened over a live frame see none of it; after the site closes they see it again. */
static void case_site_over_frame(void) {
    reset();
    (void)openpete_mods_chain_add(ADDR_X, link_c, "C");
    (void)openpete_mods_chain_add(ADDR_X, link_site, "S");
    s_site_checked = 0;
    run_and_check("site over a live frame", 0, 1);
    CHECK(s_site_checked, "link_site ran");
}

/** @brief A bus frame on top refuses base(), is named with its bus, and is not moved by a removal on a chain. */
static void case_bus_frame(void) {
    reset();
    (void)openpete_mods_chain_add(ADDR_X, link_c, "C");
    mod_bracket_t b;
    mod_frame_t* f = frame_enter(&b, OPENPETE_MODS_FRAME_BUS, "H");
    CHECK(f != NULL, "bus frame pushed");
    if (!f) return;
    f->bus = "status";
    f->link = 5;
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    openpete_mods_base(&cpu);
    CHECK(s_runs_c == 0 && s_orig == 0, "base() from a bus frame ran a chain");
    openpete_mods_attribution_t at;
    CHECK(openpete_mods_crash_attribution(&at) && at.kind == OPENPETE_MODS_FRAME_BUS &&
          strcmp(at.bus, "status") == 0 && strcmp(at.owner, "H") == 0 && at.addr == 0,
          "attribution names the bus and its handler's owner");
    uint32_t addr = 1;
    CHECK(openpete_mods_frame_owner_live("H", &addr) && addr == 0, "bus frame owner live");
    openpete_mods_chain_remove_owner("C");
    CHECK(f->link == 5, "the re-anchor moved a bus frame");
    frame_leave(&b);
    CHECK(s_frame_depth == 0, "bus frame popped");
}

/** @brief Run every case; exit 1 on the first failed check of any. */
int main(void) {
    case_own_link_head();
    case_own_link_head_reinsert();
    case_passed_link();
    case_two_passed_links_reload();
    case_link_behind();
    case_site_over_frame();
    case_bus_frame();
    if (s_fail) return 1;
    printf("mod_chain_test: all cases pass\n");
    return 0;
}
