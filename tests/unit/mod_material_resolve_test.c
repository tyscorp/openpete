/* mod_material_resolve_test - the material resolver (core/mods/mod_material_resolve.c)
 * against stub loader hooks.
 *
 * Each case registers materials for stub mods (id, enabled-list position,
 * manifest api pin), drives the extraction, bake and draw-time entry
 * points, and asserts the outcome the resolver produces. The cases record
 * the current rules: one material per draw outside a chain; the moby extraction walk down
 * the priority order, where a refine's decline passes the moby to the next
 * matching material and the walk's outcome is stamped into the tag; one
 * moby predicate (level, contract, identity) at extraction and draw time;
 * refine registrations and retunes refused where a refine cannot take
 * effect; refine outcomes asked once per tick and replayed on the tick's
 * re-extracts; a same-mod tie kept by the first registration below api
 * pin 13 and by the later one from it; terrain refines and declaratives in
 * one enabled-list ranking, a terrain face's resolution class (the refines
 * above its declarative winner, then that winner) shared by the retained
 * and the walked path, a terrain refine's decline passing the face to the
 * next member of its class, one terrain predicate (level and contract) on
 * both paths, terrain class resolutions computed on the sim thread and
 * read by the compose from the view, and the level read from the installed
 * view at draw time; moby chains of composable materials (pinned at 13 or
 * later, not OP_SEL_EXCLUSIVE) against a stubbed installed member set and
 * hold list: terminal, unlinkable, identity and vertex-stage members, the
 * cap, absence from the set, a set change between extraction and compose,
 * held members and the chain block; terrain chains: the class list past
 * pinned-13 declaratives, chain resolution per profile against the set,
 * the walked 0x080 | id stamp and its decode, chain pass ids and their
 * stock fallback, the terrain chain table in the view and its capacity,
 * and the terrain epoch skip for an OP_SEL_EXCLUSIVE-only retune; draw-time
 * chains on the declarative channels: composition, the resolve cache, held
 * owners, absence while a module is pending, terminal and unlinkable
 * members, and decal-contract chains on moby shadows; and the overlap
 * verdict per composition pairing. Every stub mod carries
 * its api pin, so a rule keyed on the pin changes only the cases whose mods
 * sit on the changed side of it. A material registered through reg() has
 * no link verdict, so it is unlinkable and draws alone as before
 * composition.
 *
 * Exit 0 when every check holds, 1 otherwise. */
#include "mod_material_resolve.h"
#include "openpete_native_render.h"
#include "openpete_mods.h" /* openpete_mods_site_t */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Loader and renderer stubs. */

OPENPETE_THREAD_LOCAL int openpete_mods_in_present;

/** @brief One stub mod: id, enabled-list position, manifest api pin, live toggle. */
typedef struct {
    const char* id;
    int load_index;
    int api;
    int enabled;
} stub_mod_t;

static stub_mod_t s_mods[8];
static int s_mod_n;
static int s_guard_depth, s_guard_max;
static const char* s_guard_owner;
static int s_split;

/** @brief Stub: a mod's live-toggle state; unknown ids are disabled. */
int openpete_mods_is_enabled(const char* id) {
    for (int i = 0; i < s_mod_n; i++)
        if (strcmp(s_mods[i].id, id) == 0) return s_mods[i].enabled;
    return 0;
}
/** @brief Stub: enter a guarded stretch owned by a mod, recording the owner and the nesting depth. */
void openpete_mods_guard_enter(const char* mod_id) {
    s_guard_owner = mod_id;
    if (++s_guard_depth > s_guard_max) s_guard_max = s_guard_depth;
}
/** @brief Stub: leave the guarded stretch. */
void openpete_mods_guard_leave(void) { s_guard_depth--; }
static openpete_mods_site_t s_site_now;   /* the open dispatch site */
static openpete_mods_ctx    s_refine_ctx; /* the site a refine callback ran in */
/** @brief Stub: open a dispatch site, recording its kind.
 * @param ctx the site's kind.
 * @return the enclosing site. */
openpete_mods_site_t openpete_mods_site_open(openpete_mods_ctx ctx) {
    const openpete_mods_site_t prev = s_site_now;
    s_site_now.ctx = ctx;
    return prev;
}
/** @brief Stub: restore the enclosing site.
 * @param prev the value the open returned. */
void openpete_mods_site_close(openpete_mods_site_t prev) { s_site_now = prev; }
/** @brief Stub: the mod toggle epoch, constant here. */
unsigned openpete_mods_toggle_epoch(void) { return 0; }
/** @brief Stub: record the batcher tag-split arm state. */
void openpete_native_set_tag_split(int on) { s_split = on; }
static const char* s_game = "spyro-1";
/** @brief Stub: the running game's id, set per case. */
const char* game_get_id(void) { return s_game; }

/* ------------------------------------------------------------------ */
/* Harness. */

static int s_fail, s_checks;
#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_fail = 1; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

/** @brief Start a case: empty the table, the stub mods and the census, and print its name. */
static void begin(const char* name) {
    mod_mat_reset();
    mod_mat_census_reset();
    s_mod_n = 0;
    s_guard_depth = s_guard_max = 0;
    openpete_mods_in_present = 0;
    s_game = "spyro-1";
    printf("case %s\n", name);
}
/** @brief Print one recorded outcome line of the current case. */
#define OUTCOME(...) do { printf("  "); printf(__VA_ARGS__); printf("\n"); } while (0)

/** @brief Add an enabled stub mod.
 *
 * @param id Mod id (string literal).
 * @param load Enabled-list position, later higher.
 * @param api Manifest api pin.
 * @return The stub. */
static stub_mod_t* mod(const char* id, int load, int api) {
    stub_mod_t* m = &s_mods[s_mod_n++];
    m->id = id; m->load_index = load; m->api = api; m->enabled = 1;
    return m;
}

/** @brief Register a material for a stub mod through the same validation and append the engine uses.
 *
 * @param m Owner.
 * @param sel Selector.
 * @param contract OPENPETE_MATERIAL_CONTRACT_*.
 * @param fn Refine callback, or NULL for a declarative material.
 * @return The handle, or -1 when refused. */
static int reg(const stub_mod_t* m, openpete_mod_material_selector_t sel, int contract,
               openpete_mod_material_fn fn) {
    if (!mod_mat_sel_valid(m->id, &sel, m->api)) return -1;
    if (fn && !mod_mat_refine_valid(m->id, &sel, contract)) return -1;
    return mod_mat_append(m->id, &sel, -1, 0, contract, fn, m->load_index, m->api, 1u);
}
/** @brief A selector on one channel, every other key open. */
static openpete_mod_material_selector_t sel_ch(int ch) {
    openpete_mod_material_selector_t s = openpete_mod_material_sel_any();
    s.channel = ch;
    return s;
}
/** @brief A moby selector keyed to one class (-1 = every class). */
static openpete_mod_material_selector_t sel_moby(int cls) {
    openpete_mod_material_selector_t s = sel_ch(OP_CHAN_MOBY);
    s.moby_class = cls;
    return s;
}

/** @brief A moby provenance tag: path nibble 1, class [15:0], slot [27:16]. */
static uint32_t moby_tag(int cls, int slot) {
    return (1u << 28) | ((uint32_t)slot << 16) | ((uint32_t)cls & 0xFFFFu);
}
/** @brief A terrain provenance tag: path nibble 2, env bit 27, LP bit 26, untextured bit 25, slot [24:16], Tiledef id [15:9].
 *
 * @param tex Tiledef id, or -1 for an untextured face. */
static uint32_t ter_tag(int tex, int env, int lp, int slot) {
    uint32_t t = (2u << 28) | ((uint32_t)slot << 16);
    if (env) t |= 0x08000000u;
    if (lp) t |= 0x04000000u;
    if (tex < 0) t |= 0x02000000u;
    else t |= ((uint32_t)tex & 0x7Fu) << 9;
    return t;
}
/** @brief A provenance tag of a non-moby, non-terrain channel: the channel nibble and the identity. */
static uint32_t chan_tag(int ch, int ident) { return ((uint32_t)ch << 28) | ((uint32_t)ident & 0xFFFFu); }

/** @brief Resolve a tag for a contract and return the material index, or -1 for stock. */
static int resolve(uint32_t tag, int contract) {
    int slot = 0;
    return mod_mat_resolve_index(tag, contract, &slot, NULL);
}

/** @brief Retained bake: the resolution class of a face key; @p tex -1 is an untextured face. */
static int bake(int tex, int env, int lp) {
    return openpete_mods_material_terrain_class(tex, tex < 0, env, lp);
}
/** @brief Walked path: the tag field of a face key; @p tex -1 is an untextured face. */
static int walk(int tex, int env, int lp) {
    return openpete_mods_material_terrain_walk_slot(NULL, tex, tex < 0, env, lp);
}
/** @brief Retained path: the material whose pass draws class @p c, from the published pass list and class masks, or -1 for the stock pass.
 *
 * Reads the installed view when one is set, else the live tables. Returns
 * -2 when no pass, or more than one, holds the class. */
static int retained(int c) {
    int passes[OPENPETE_MATERIAL_TER_BUCKET_MAX + 1];
    const int n = openpete_mods_material_terrain_bucket_list(passes + 1, OPENPETE_MATERIAL_TER_BUCKET_MAX);
    passes[0] = 0;
    int hit = -2, hits = 0;
    for (int i = 0; i <= n; i++) {
        uint32_t mask[8];
        (void)openpete_mods_material_terrain_pass_mask(passes[i], mask);
        if ((mask[c >> 5] >> (c & 31)) & 1u) { hit = passes[i] - 1; hits++; }
    }
    return hits == 1 ? hit : -2;
}
/** @brief Return the number of retained material passes listed for the present. */
static int retained_passes(void) {
    int passes[OPENPETE_MATERIAL_TER_BUCKET_MAX];
    return openpete_mods_material_terrain_bucket_list(passes, OPENPETE_MATERIAL_TER_BUCKET_MAX);
}
#define SCENE OPENPETE_MATERIAL_CONTRACT_SCENE
#define DECAL OPENPETE_MATERIAL_CONTRACT_DECAL
/** @brief The moby tag field of a declarative landing on material @p m. */
#define DECL(m) (OPENPETE_MATERIAL_SLOT_DECL | (m))
/** @brief The terrain tag field of a declarative landing on material @p m. */
#define TDECL(m) (OPENPETE_MATERIAL_TERRAIN_DECL | (m))
#define TSTOCK OPENPETE_MATERIAL_TERRAIN_STOCK

/* Refine callbacks. Each counts its calls; a claim writes a 16-byte block
 * derived from the key, so distinct instances intern distinct slots. */
static int s_calls;
/** @brief Refine: claim every instance with a block from its address or texture. */
static int rf_accept(const openpete_mod_material_key_t* k, void* out, uint32_t cap) {
    s_calls++;
    if (cap < 16) return -1;
    memset(out, 0, 16);
    if (k->channel == OP_CHAN_MOBY) memcpy(out, &k->u.moby.vaddr, sizeof k->u.moby.vaddr);
    else memcpy(out, &k->u.terrain.texture, sizeof k->u.terrain.texture);
    return 16;
}
/** @brief Refine: decline every instance. */
static int rf_decline(const openpete_mod_material_key_t* k, void* out, uint32_t cap) {
    (void)k; (void)out; (void)cap;
    s_calls++;
    s_refine_ctx = s_site_now.ctx;
    return -1;
}
/** @brief Refine: claim only class 7 mobys (the channel-wide refine that accepts its own instances). */
static int rf_class7(const openpete_mod_material_key_t* k, void* out, uint32_t cap) {
    s_calls++;
    if (k->channel != OP_CHAN_MOBY || k->u.moby.moby_class != 7 || cap < 16) return -1;
    memset(out, 7, 16);
    return 16;
}

/* ------------------------------------------------------------------ */
/* Cases. Each OUTCOME line is the recorded current result. */

/** @brief A later refine's decline falls through to the earlier declarative on the class. */
static void case_f1(void) {
    begin("Refine decline falls through to an earlier declarative");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(5), SCENE, rf_decline);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == DECL(ma), "slot %#x", s);
    CHECK(s_calls == 1, "calls %d", s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == ma, "declined instance resolves");
    CHECK(mod_mat_census_get(mb, MOD_MAT_CENSUS_DECLINES) == 1, "census declines");
    CHECK(mod_mat_census_get(mb, MOD_MAT_CENSUS_FALLTHROUGHS) == 1, "census fallthroughs");
    CHECK(mod_mat_census_get(ma, MOD_MAT_CENSUS_EXTRACT_WINS) == 1, "census landing");
    CHECK(s_guard_max == 1 && s_guard_depth == 0 && s_guard_owner && strcmp(s_guard_owner, "fx-b") == 0,
          "guard balance");
    CHECK(s_refine_ctx == OPENPETE_MODS_CTX_REFINE && s_site_now.ctx == OPENPETE_MODS_CTX_NONE,
          "refine ran in a refine site (ctx %d), closed after (ctx %d)", (int)s_refine_ctx,
          (int)s_site_now.ctx);
    mod_mat_census_force(0);
    OUTCOME("class 5, B declines: the walk lands on A (DECL|%d), resolve A, refine calls 1, 1 fall-through", ma);

    begin("Refine accept wins over an earlier declarative");
    a = mod("fx-a", 0, 13);
    b = mod("fx-b", 1, 13);
    reg(a, sel_moby(5), SCENE, NULL);
    const int mb2 = reg(b, sel_moby(5), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int s2 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s2 == 1, "slot %d", s2);
    CHECK(resolve(moby_tag(5, s2), SCENE) == mb2, "accepted instance");
    OUTCOME("class 5, B accepts: slot 1, resolve B");
}

/** @brief A later declarative keyed to another level does not take the moby; the earlier refine is asked and claims it. */
static void case_f1a_level(void) {
    begin("Level-keyed declarative over a refine");
    stub_mod_t* a = mod("fx-r", 0, 13);
    stub_mod_t* b = mod("fx-d", 1, 13);
    const int mr = reg(a, sel_moby(5), SCENE, rf_accept);
    openpete_mod_material_selector_t d = sel_moby(5);
    d.level = 9;
    const int md = reg(b, d, SCENE, NULL);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == 1, "slot %d", s);
    CHECK(s_calls == 1, "calls %d", s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "resolve");
    openpete_mods_set_level(9);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s9 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s9 == DECL(md) && s_calls == 0, "level 9 slot %#x calls %d", s9, s_calls);
    OUTCOME("level 3, D keyed to 9: the refine is asked and claims (slot 1); level 9: D lands, 0 calls");
}

/** @brief A DECAL-contract refine on channel -1 is refused, and a DECAL declarative on channel -1 never takes a moby. */
static void case_f1a_decal(void) {
    begin("DECAL-contract refine on channel -1");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-decal", 1, 13);
    const int ma = reg(a, sel_moby(-1), SCENE, NULL);
    const int mb = reg(b, openpete_mod_material_sel_any(), DECAL, rf_accept);
    CHECK(mb == -1, "refused, got %d", mb);
    reg(b, openpete_mod_material_sel_any(), DECAL, NULL);
    reg(b, sel_moby(999), SCENE, rf_accept);   /* a live refine, so the walk runs */
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == DECL(ma) && s_calls == 0, "slot %#x calls %d", s, s_calls);
    CHECK(resolve(moby_tag(5, s), SCENE) == ma, "scene resolve");
    OUTCOME("DECAL refine refused; with a DECAL declarative on channel -1 the moby lands on A");
}

/** @brief A later channel-wide refine accepting only class 7 passes class 5 to an earlier class-5 declarative. */
static void case_f2(void) {
    begin("Channel-wide refine falls through to an earlier class declarative");
    stub_mod_t* a = mod("fx-a", 0, 12);
    stub_mod_t* b = mod("fx-wild", 1, 11);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(-1), SCENE, rf_class7);
    openpete_mods_material_frame_reset(1);
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80010100u);
    CHECK(s5 == DECL(ma), "class 5 slot %#x", s5);
    CHECK(resolve(moby_tag(5, s5), SCENE) == ma, "class 5 resolve");
    CHECK(resolve(moby_tag(7, s7), SCENE) == mb, "class 7 resolve");
    CHECK(resolve(moby_tag(5, 0), SCENE) == ma, "class 5 untagged");
    OUTCOME("class 5: A (the wild refine's decline falls through); class 7: wild refine");
}

/** @brief A later declarative outranks an earlier terrain refine on its texture, at bake and walked; an earlier declarative does not. */
static void case_f3(void) {
    begin("Terrain refine under a later declarative");
    stub_mod_t* a = mod("fx-tr", 0, 13);
    stub_mod_t* b = mod("fx-td", 1, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 3;
    const int mr = reg(a, r, SCENE, rf_accept);
    const int md = reg(b, r, SCENE, NULL);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int c = bake(3, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(c > 0 && retained(c) == md, "retained class %d -> %d", c, retained(c));
    const int s = walk(3, 0, 0);
    CHECK(s == TDECL(md), "walked field %#x", s);
    CHECK(resolve(ter_tag(3, 0, 0, s), SCENE) == md, "walked resolve");
    CHECK(resolve(ter_tag(3, 0, 0, 0), SCENE) == md, "slotless tag");
    CHECK(s_calls == 0 && mod_mat_census_get(mr, MOD_MAT_CENSUS_REFINE_CALLS) == 0, "refine never asked");
    mod_mat_census_force(0);
    OUTCOME("texture 3: retained and walked = the later declarative; the refine below it is never asked");

    begin("Terrain refine over an earlier declarative");
    b = mod("fx-td", 0, 13);
    a = mod("fx-tr", 1, 13);
    const int md2 = reg(b, r, SCENE, NULL);
    const int mr2 = reg(a, r, SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int c2 = bake(3, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(c2) == mr2, "retained %d", retained(c2));
    const int s2 = walk(3, 0, 0);
    CHECK(s2 == 1 && resolve(ter_tag(3, 0, 0, s2), SCENE) == mr2, "walked resolve");
    (void)md2;
    OUTCOME("texture 3: retained and walked = the later refine");
}

/** @brief A terrain refine's decline passes the face to the next member of its class, per env key, on both paths. */
static void case_terrain_decline(void) {
    begin("Terrain decline under and over a declarative");
    stub_mod_t* a = mod("fx-tr", 0, 13);
    stub_mod_t* b = mod("fx-tenv", 1, 13);
    stub_mod_t* c = mod("fx-tr2", 2, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN), e = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 3;
    e.texture = 3;
    e.flags = OP_SEL_ENV_ANIM;
    reg(a, r, SCENE, rf_decline);
    const int me = reg(b, e, SCENE, NULL);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    CHECK(walk(3, 1, 0) == TDECL(me) && resolve(ter_tag(3, 1, 0, walk(3, 1, 0)), SCENE) == me,
          "env face: the later declarative");
    CHECK(walk(3, 0, 0) == TSTOCK && resolve(ter_tag(3, 0, 0, TSTOCK), SCENE) == -1,
          "plain face: the decline, nothing below");
    const int ce = bake(3, 1, 0), cp = bake(3, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(ce) == me && retained(cp) == -1, "retained env %d plain %d", retained(ce), retained(cp));
    const int mc = reg(c, r, SCENE, rf_decline);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s2 = walk(3, 1, 0);
    CHECK(s2 == TDECL(me) && resolve(ter_tag(3, 1, 0, s2), SCENE) == me,
          "env face under a later declining refine: falls through, field %#x", s2);
    CHECK(walk(3, 0, 0) == TSTOCK, "plain face: both refines decline");
    const int ce2 = bake(3, 1, 0), cp2 = bake(3, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(ce2) == me && retained(cp2) == -1, "retained env %d plain %d", retained(ce2), retained(cp2));
    CHECK(s_calls == 2, "each refine asked once in the present, calls %d", s_calls);
    CHECK(mod_mat_census_get(mc, MOD_MAT_CENSUS_FALLTHROUGHS) >= 1, "fall-through counted");
    mod_mat_census_force(0);
    OUTCOME("decline: env face draws the env declarative below the declining refines, plain face stock; "
            "retained and walked agree");
}

/** @brief OP_SEL_ENV_STATIC takes the non-env terrain faces OP_SEL_ENV_ANIM leaves, never a moby or another channel, and both bits together are refused. */
static void case_env_static(void) {
    begin("Env static: the complement of env_anim on terrain only");
    stub_mod_t* a = mod("fx-env", 0, 13);
    stub_mod_t* b = mod("fx-static", 1, 13);
    openpete_mod_material_selector_t e = sel_ch(-1), st = sel_ch(-1), both = sel_ch(OP_CHAN_TERRAIN);
    e.flags = OP_SEL_ENV_ANIM;
    st.flags = OP_SEL_ENV_STATIC;
    both.flags = OP_SEL_ENV_ANIM | OP_SEL_ENV_STATIC;
    CHECK(reg(b, both, SCENE, NULL) == -1, "both env bits refused");
    const int me = reg(a, e, SCENE, NULL);
    const int ms = reg(b, st, SCENE, NULL);
    openpete_mods_material_frame_reset(1);
    for (int tex = -1; tex < 3; tex++)
        for (int lp = 0; lp < 2; lp++) {
            CHECK(walk(tex, 1, lp) == TDECL(me) && resolve(ter_tag(tex, 1, lp, 0), SCENE) == me,
                  "env face tex %d lp %d: env_anim row", tex, lp);
            CHECK(walk(tex, 0, lp) == TDECL(ms) && resolve(ter_tag(tex, 0, lp, 0), SCENE) == ms,
                  "static face tex %d lp %d: env_static row", tex, lp);
        }
    const int ce = bake(2, 1, 0), cs = bake(2, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(ce) == me && retained(cs) == ms, "retained env %d static %d", retained(ce), retained(cs));
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == OPENPETE_MATERIAL_SLOT_STOCK &&
          resolve(moby_tag(5, 0), SCENE) == -1, "a moby matches neither env bit");
    CHECK(resolve(chan_tag(OP_CHAN_PLAYER, 0), SCENE) == -1 &&
          resolve(chan_tag(OP_CHAN_FLAME, 0), SCENE) == -1, "player and flame match neither env bit");
    OUTCOME("env faces take the env_anim row, the rest the env_static row on both paths; "
            "mobys, player and flame stay stock; both bits refused");
}

/* The level rf_band reads. */
static int s_band_level;
/** @brief Refine: claim the untextured band on an odd level, decline it on an even one. */
static int rf_band(const openpete_mod_material_key_t* k, void* out, uint32_t cap) {
    (void)k;
    s_calls++;
    if ((s_band_level & 1) == 0 || cap < 16) return -1;
    memset(out, 9, 16);
    return 16;
}
/** @brief mat-fx-terrain's pattern: an untextured-band refine above its own mod's untextured declarative, declining on even levels. */
static void case_terrain_fallthrough(void) {
    begin("Terrain decline falls through to a lower declarative (mat-fx-terrain)");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("mat-fx-terrain", 0, 13);
    openpete_mod_material_selector_t d = sel_ch(OP_CHAN_TERRAIN), u = sel_ch(OP_CHAN_TERRAIN);
    d.flags = OP_SEL_UNTEXTURED;          /* manifest row, registered first */
    u.flags = OP_SEL_UNTEXTURED;
    const int md = reg(a, d, SCENE, NULL);
    const int mu = reg(a, u, SCENE, rf_band);
    mod_mat_census_force(1);
    s_band_level = 10;
    openpete_mods_set_level(10);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int c = bake(-1, 0, 1);
    CHECK(c > 0 && mod_mat_terrain_class_count() == 1, "one class, %d", mod_mat_terrain_class_count());
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(c) == md && retained_passes() == 1, "even level, retained: %d passes %d", retained(c), retained_passes());
    CHECK(walk(-1, 0, 1) == TDECL(md) && resolve(ter_tag(-1, 0, 1, TDECL(md)), SCENE) == md, "even level, walked");
    CHECK(s_calls == 1, "one call for both paths, %d", s_calls);
    CHECK(mod_mat_census_get(mu, MOD_MAT_CENSUS_FALLTHROUGHS) == 1, "fall-through %u",
          mod_mat_census_get(mu, MOD_MAT_CENSUS_FALLTHROUGHS));
    uint32_t m0[8], m1[8];
    CHECK(openpete_mods_material_terrain_pass_mask(0, m0) >= 1 && (m0[0] & 1u) && !((m0[c >> 5] >> (c & 31)) & 1u),
          "stock pass: class 0, not the class");
    CHECK(openpete_mods_material_terrain_pass_mask(md + 1, m1) == 1, "the declarative's pass: one class");
    s_band_level = 11;
    openpete_mods_set_level(11);
    openpete_mods_material_frame_reset(1);
    const int c11 = bake(-1, 0, 1);
    openpete_mods_material_terrain_refine_present();
    const int s11 = walk(-1, 0, 1);
    CHECK(c11 == c && retained(c) == mu && s11 > 0 && s11 < TDECL(0) && resolve(ter_tag(-1, 0, 1, s11), SCENE) == mu,
          "odd level: the refine on both paths");
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset(1);
    openpete_mods_material_view_set(&v);
    CHECK(retained(c) == mu && retained_passes() == 1 && mod_mat_terrain_bucket_slot(mu) == s11,
          "captured view keeps the present's table");
    openpete_mods_material_view_set(NULL);
    CHECK(retained(c) == -1, "live table, next present before the resolution: stock pass");
    mod_mat_census_force(0);
    OUTCOME("even level: the band refine declines and both paths draw the declarative below it (1 call, 1 fall-through); "
            "odd level: the refine; 1 class, 1 material pass");
}

/** @brief The pass count is bounded by the terrain materials with baked faces; classes resolving to stock join the stock pass. */
static void case_terrain_pass_bound(void) {
    begin("Terrain classes and passes per colour slot");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    openpete_mod_material_selector_t t = sel_ch(OP_CHAN_TERRAIN), r1 = sel_ch(OP_CHAN_TERRAIN), r2;
    t.flags = OP_SEL_TEXTURED;
    r1.texture = 1;
    r2 = r1;
    r2.texture = 2;
    const int mt = reg(a, t, SCENE, NULL);
    const int ma = reg(b, r1, SCENE, rf_accept);
    const int mdc = reg(b, r2, SCENE, rf_decline);
    openpete_mod_material_selector_t e = sel_ch(OP_CHAN_TERRAIN);
    e.flags = OP_SEL_ENV_ANIM;
    const int me = reg(a, e, SCENE, NULL);   /* a's later row: over mt on env faces, under b's refines */
    openpete_mods_material_frame_reset(1);
    int cls[16], n = 0;
    for (int tex = 0; tex < 4; tex++)
        for (int env = 0; env < 2; env++) cls[n++] = bake(tex, env, 0);
    cls[n++] = bake(-1, 0, 1);
    openpete_mods_material_terrain_refine_present();
    const int nc = mod_mat_terrain_class_count();
    CHECK(nc == 6, "classes %d", nc);
    CHECK(retained(cls[2]) == ma && retained(cls[4]) == mt && retained(cls[5]) == me && retained(cls[0]) == mt,
          "tex 1 refine, tex 2 falls through to the textured declarative, env to the env declarative");
    CHECK(retained(cls[8]) == -1, "untextured LP: stock");
    CHECK(retained_passes() == 3, "passes %d (ma, mt, me)", retained_passes());
    (void)mdc;
    OUTCOME("%d classes; 3 material passes per colour slot for 4 live terrain materials (the declining refine owns none)", nc);
}
/** @brief A same-mod tie keeps the first registration, manifest row against C and C against C. */
static void case_f4(void) {
    begin("Same-mod tie, manifest row then C registration (pin 13)");
    stub_mod_t* a = mod("fx-pair", 0, 13);
    reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    const int mc = reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_PLAYER, 0), SCENE) == mc, "player");
    OUTCOME("player: the C registration (later registration) wins");

    begin("Same-mod tie, manifest row then C registration (pin 12)");
    a = mod("fx-pair12", 0, 12);
    const int mm = reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_PLAYER, 0), SCENE) == mm, "player");
    OUTCOME("player: the manifest row (first registration) wins");

    begin("Same-mod tie, C against C on flame ident 0 (pin 11)");
    a = mod("fx-gifts", 0, 11);
    openpete_mod_material_selector_t f = sel_ch(OP_CHAN_FLAME);
    f.ident = 0;
    const int m1 = reg(a, f, SCENE, NULL);
    reg(a, f, SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 0), SCENE) == m1, "flame");
    OUTCOME("flame ident 0: the first registration wins");

    begin("Same-mod tie, C against C on flame ident 0 (pin 13)");
    a = mod("fx-gifts13", 0, 13);
    reg(a, f, SCENE, NULL);
    const int m2 = reg(a, f, SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 0), SCENE) == m2, "flame");
    OUTCOME("flame ident 0: the later registration wins");
}

/** @brief OP_SEL_HP_TIER on a moby selector: the one moby predicate rejects it at extraction and at draw time. */
static void case_hp_tier_moby(void) {
    begin("OP_SEL_HP_TIER on a moby selector");
    stub_mod_t* a = mod("fx-hp", 0, 13);
    stub_mod_t* b = mod("fx-other", 1, 13);
    openpete_mod_material_selector_t s = sel_moby(-1);
    s.flags = OP_SEL_HP_TIER;
    const int ma = reg(a, s, SCENE, NULL);
    const int mb = reg(b, sel_moby(99), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int sl = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(sl == OPENPETE_MATERIAL_SLOT_STOCK, "slot %#x", sl);
    CHECK(mod_mat_census_get(ma, MOD_MAT_CENSUS_EXTRACT_WINS) == 0, "no extract win");
    CHECK(resolve(moby_tag(5, sl), SCENE) == -1, "stamped");
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "draw-time");
    (void)mb;
    OUTCOME("extraction: no candidate (STOCK); draw-time resolve of an unstamped tag: stock");
}

/** @brief A particle selector with a nonzero `sel` matches its draw on Spyro 1 and no draw on Spyro 2 or 3, where `sel` is reserved.
 *
 * The draws carry the ident the feeders write: the particle class on
 * Spyro 1, the GPU primitive code on Spyro 2 and 3 (0x2C a textured quad,
 * 0x40 a line). A channel -1 selector naming the same ident is excluded on
 * the particle channel alike; an ident -1 particle selector still matches.
 * @param game The stubbed game id. */
static void case_part_sel_reserved(const char* game) {
    begin("part_sel_reserved");
    s_game = game;
    const int s1 = strcmp(game, "spyro-1") == 0;
    const stub_mod_t* a = mod("part", 0, 13);
    openpete_mod_material_selector_t q = sel_ch(OP_CHAN_PARTICLE);
    q.ident = OP_PART_IDENT(OP_PART_QUAD, 0x2C);
    const int mq = reg(a, q, SCENE, NULL);
    openpete_mod_material_selector_t l = sel_ch(OP_CHAN_PARTICLE);
    l.ident = OP_PART_IDENT(OP_PART_LINE, 0x40);
    const int ml = reg(a, l, SCENE, NULL);
    const int rq = resolve(OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, 0x2C, 0), SCENE);
    const int rl = resolve(OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_LINE, 0x40, 0), SCENE);
    OUTCOME("%s: quad sel 0x2C -> %d, line sel 0x40 -> %d", game, rq, rl);
    CHECK(mq >= 0 && ml >= 0, "%s: registration accepted (%d, %d)", game, mq, ml);
    CHECK(rq == (s1 ? mq : -1), "%s: quad sel 0x2C resolves %d", game, rq);
    CHECK(rl == (s1 ? ml : -1), "%s: line sel 0x40 resolves %d", game, rl);

    begin("part_sel_reserved_any_channel");
    s_game = game;
    a = mod("part", 0, 13);
    openpete_mod_material_selector_t any = openpete_mod_material_sel_any();
    any.ident = OP_PART_IDENT(OP_PART_QUAD, 0x2C);
    const int ma = reg(a, any, SCENE, NULL);
    const int ra = resolve(OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, 0x2C, 0), SCENE);
    OUTCOME("%s: channel -1 ident 0x2C on a particle quad -> %d", game, ra);
    CHECK(ra == (s1 ? ma : -1), "%s: channel -1 sel 0x2C resolves %d", game, ra);

    begin("part_sel_reserved_every_ident");
    s_game = game;
    a = mod("part", 0, 13);
    const int me = reg(a, sel_ch(OP_CHAN_PARTICLE), SCENE, NULL);
    const int re = resolve(OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, 0x2C, 0), SCENE);
    OUTCOME("%s: ident -1 on a particle quad -> %d", game, re);
    CHECK(re == me && me >= 0, "%s: ident -1 resolves %d", game, re);
}

/** @brief Spyro64's OP_SEL_TEXTURED moby selector matches at extraction and at draw time. */
static void case_textured_moby(void) {
    begin("OP_SEL_TEXTURED moby selector (spyro64)");
    stub_mod_t* a = mod("fx-s64", 0, 12);
    stub_mod_t* b = mod("fx-other", 1, 13);
    openpete_mod_material_selector_t s = sel_moby(508);
    s.flags = OP_SEL_TEXTURED;
    const int ma = reg(a, s, SCENE, NULL);
    reg(b, sel_moby(99), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int sl = openpete_mods_material_moby_slot(NULL, 508, 0x80010000u);
    CHECK(sl == DECL(ma), "slot %#x", sl);
    CHECK(resolve(moby_tag(508, sl), SCENE) == ma, "draw-time");
    CHECK(resolve(moby_tag(508, 0), SCENE) == ma, "unstamped");
    OUTCOME("class 508: the walk lands on it (DECL); draw-time resolve: it");
}

/** @brief A level-keyed terrain refine draws on both paths only in its own level. */
static void case_f3a(void) {
    begin("Level-keyed terrain refine");
    stub_mod_t* a = mod("fx-tlev", 0, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 4;
    r.level = 9;
    const int mr = reg(a, r, SCENE, rf_accept);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    CHECK(bake(4, 0, 0) == 0, "bake");
    const int s = walk(4, 0, 0);
    CHECK(s == TSTOCK && s_calls == 0, "walked slot %#x calls %d", s, s_calls);
    CHECK(resolve(ter_tag(4, 0, 0, s), SCENE) == -1, "walked resolve");
    openpete_mods_set_level(9);
    openpete_mods_material_frame_reset(1);
    const int c9 = bake(4, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(c9 > 0 && retained(c9) == mr, "retained level 9");
    const int s9 = walk(4, 0, 0);
    CHECK(resolve(ter_tag(4, 0, 0, s9), SCENE) == mr, "walked level 9");
    OUTCOME("level 3: bake stock, walked stock (no call); level 9: both refine");

    begin("DECAL-contract terrain refine on the walked path");
    a = mod("fx-tdecal", 0, 13);
    r.level = -1;
    reg(a, r, DECAL, rf_accept);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    CHECK(walk(4, 0, 0) == 0 && s_calls == 0, "walked");
    CHECK(bake(4, 0, 0) == 0, "bake");
    OUTCOME("a DECAL terrain refine is asked on neither path");
}

/** @brief Refines that cannot take effect are refused at registration and at retune, before a retune is parked. */
static void case_d3a(void) {
    begin("Refines that cannot take effect are refused");
    stub_mod_t* a = mod("fx-bad", 0, 13);
    s_calls = 0;
    CHECK(reg(a, sel_ch(OP_CHAN_FLAME), SCENE, rf_accept) == -1, "flame refine");
    CHECK(reg(a, sel_ch(OP_CHAN_SHADOW), DECAL, rf_accept) == -1, "shadow DECAL refine");
    CHECK(reg(a, sel_moby(-1), DECAL, rf_accept) == -1, "moby DECAL refine");
    CHECK(reg(a, sel_ch(OP_CHAN_TERRAIN), SCENE, rf_accept) == -1, "unkeyed terrain refine");
    CHECK(mod_mat_count() == 0, "table %d", mod_mat_count());
    openpete_mod_material_selector_t t = sel_ch(OP_CHAN_TERRAIN), u = sel_ch(OP_CHAN_TERRAIN);
    t.texture = 3;
    u.flags = OP_SEL_UNTEXTURED;
    CHECK(reg(a, t, SCENE, rf_accept) >= 0, "texture-keyed terrain refine");
    CHECK(reg(a, u, SCENE, rf_accept) >= 0, "untextured-band terrain refine");
    CHECK(reg(a, sel_ch(-1), SCENE, rf_accept) >= 0, "channel -1 refine");
    CHECK(reg(a, sel_ch(OP_CHAN_FLAME), SCENE, NULL) >= 0, "flame declarative");
    const int mm = reg(a, sel_moby(5), SCENE, rf_accept);
    CHECK(mm >= 0, "moby refine");
    openpete_mod_material_selector_t f = sel_ch(OP_CHAN_FLAME);
    CHECK(mod_mat_set_selector("fx-bad", mm, &f) == -1, "retune to flame");
    openpete_mods_in_present = 1;
    openpete_mod_material_selector_t un = sel_ch(OP_CHAN_TERRAIN);
    CHECK(mod_mat_set_selector("fx-bad", mm, &un) == -1 && !mod_mat_pending(), "in-present retune not parked");
    openpete_mods_in_present = 0;
    CHECK(mod_mat_at(mm)->sel.channel == OP_CHAN_MOBY && mod_mat_at(mm)->sel.moby_class == 5, "old selector in force");
    openpete_mods_material_frame_reset(1);
    const int sl = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(sl > 0 && sl < OPENPETE_MATERIAL_SLOT_DECL, "moby refine still claims, slot %#x", sl);
    OUTCOME("refused: flame, shadow DECAL, moby DECAL, unkeyed terrain refines and both retunes; "
            "kept: keyed terrain, channel -1 and moby refines, the old selector after a refused retune");
}

/** @brief F3b: the draw-time level is read live, so a level push after the view capture changes a captured scene's resolve. */
static void case_f3b_view_level(void) {
    begin("F3b view-captured scene against a live level change");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-lev", 0, 13);
    openpete_mod_material_selector_t d = sel_moby(5);
    d.level = 3;
    const int md = reg(a, d, SCENE, NULL);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset(1);
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_view_set(&v);
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "before");
    openpete_mods_set_level(4);
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "after");
    openpete_mods_material_view_set(NULL);
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "live table after");
    OUTCOME("view captured in level 3: resolves the material before and after a push of level 4; the live table: stock");
}

/** @brief F3b: terrain refine outcomes are computed on the sim thread; the compose reads the view or this present's memo and calls nothing. */
static void case_f3b_terrain_refine(void) {
    begin("F3b terrain refine outcomes on the sim thread");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-tr", 0, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN), u = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 6;
    u.flags = OP_SEL_UNTEXTURED;
    const int mr = reg(a, r, SCENE, rf_accept);
    reg(a, u, SCENE, rf_decline);
    reg(a, sel_ch(-1), SCENE, rf_accept);   /* unkeyed: never asked on terrain */
    mod_mat_census_force(1);
    const int c6 = bake(6, 0, 0), cu = bake(-1, 0, 0);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    openpete_mods_material_terrain_refine_present();
    CHECK(s_calls == 2, "sim-thread calls %d", s_calls);
    const int sr = mod_mat_terrain_bucket_slot(mr);
    CHECK(sr == 1, "memo slot %d", sr);
    CHECK(retained(c6) == mr && retained(cu) == -1, "retained: the refine, the declined band stock");
    CHECK(walk(6, 0, 0) == sr && s_calls == 2, "walk reuses the memo");
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset(1);   /* the sim thread opens the next tick's scene */
    s_calls = 0;
    openpete_mods_material_view_set(&v);
    CHECK(mod_mat_terrain_bucket_slot(mr) == sr && retained(c6) == mr, "view slot and pass");
    CHECK(resolve(ter_tag(6, 0, 0, sr), SCENE) == mr, "walked tag through the view");
    openpete_mods_material_view_set(NULL);
    CHECK(mod_mat_terrain_bucket_slot(mr) == OPENPETE_MATERIAL_TERRAIN_STOCK, "stale memo");
    CHECK(mod_mat_census_get(mr, MOD_MAT_CENSUS_STALE_MEMO) == 1, "stale memo counted");
    CHECK(s_calls == 0, "compose calls %d", s_calls);
    CHECK(mod_mat_census_view_resolves() >= 1, "view resolves counted");
    mod_mat_census_force(0);
    OUTCOME("2 refine calls on the sim thread; the compose binds the view's outcome, a stale memo draws stock; 0 compose calls");
}

/** @brief The sim-thread refine pass asks no outranked terrain refine: one a later declarative takes on every face, or a lower refine on the same key. */
static void case_refine_present_outranked(void) {
    begin("Sim-thread refine pass skips outranked refines");
    stub_mod_t* a = mod("fx-tr", 0, 13);
    stub_mod_t* b = mod("fx-td", 1, 13);
    stub_mod_t* c = mod("fx-tr2", 2, 13);
    openpete_mod_material_selector_t r3 = sel_ch(OP_CHAN_TERRAIN), r5 = sel_ch(OP_CHAN_TERRAIN);
    r3.texture = 3;
    r5.texture = 5;
    reg(a, r3, SCENE, rf_accept);              /* outranked by b on every face of texture 3 */
    const int md = reg(b, r3, SCENE, NULL);
    reg(a, r5, SCENE, rf_accept);              /* outranked by c's refine on texture 5 */
    const int mc = reg(c, r5, SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int c3 = bake(3, 0, 0), c3e = bake(3, 1, 1), c5 = bake(5, 0, 0);
    s_calls = 0;
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(c3) == md && retained(c3e) == md && retained(c5) == mc, "texture 3 the declarative, texture 5 the later refine");
    CHECK(s_calls == 1, "calls %d", s_calls);
    CHECK(mod_mat_terrain_bucket_slot(mc) == 1, "the winning refine has an outcome");
    OUTCOME("1 call: the texture-5 winner; the refine under the later declarative and the lower texture-5 refine "
            "(reached only on its decline) get 0");
}

/** @brief Host-loop mode (no view installed): a frame reset strands an earlier present's slot, and a reused slot names another instance's block. */
static void case_host_loop(void) {
    begin("Host-loop mode, no view installed");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-tint", 0, 5);
    const int mr = reg(a, sel_moby(-1), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == 1, "slot %d", s);
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "same present");
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset(1);
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "after reset, live table");
    openpete_mods_material_view_set(&v);
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "after reset, captured view");
    openpete_mods_material_view_set(NULL);
    const int s2 = openpete_mods_material_moby_slot(NULL, 6, 0x80020000u);
    uint32_t len = 0;
    const unsigned char* bytes = (const unsigned char*)openpete_mods_material_slot_uniforms(s, &len);
    CHECK(s2 == s && bytes && memcmp(bytes, v.slots[s].bytes, 16) != 0, "slot reused with other bytes");
    CHECK(resolve(moby_tag(5, s), SCENE) == mr, "stale tag resolves");
    OUTCOME("after a reset the old tag draws stock from the live table, the material from the view;"
            " once the slot is re-minted the old tag binds the new instance's block");
}

/** @brief A level-keyed terrain declarative buckets against the previous level until the game pushes the new one. */
static void case_level_change_bake(void) {
    begin("Level-keyed terrain declarative, CACHE=1, first present after a level change");
    stub_mod_t* a = mod("fx-tdlev", 0, 13);
    openpete_mod_material_selector_t d = sel_ch(OP_CHAN_TERRAIN);
    d.level = 9;
    const int md = reg(a, d, SCENE, NULL);
    openpete_mods_set_level(3);
    const unsigned e1 = openpete_mods_material_terrain_epoch();
    CHECK(bake(5, 0, 0) == 0, "level 3");
    /* The guest has entered level 9; the bake runs before the push. */
    CHECK(bake(5, 0, 0) == 0, "before push");
    openpete_mods_set_level(9);
    CHECK(openpete_mods_material_terrain_epoch() > e1, "epoch moved");
    openpete_mods_material_frame_reset(1);
    const int c = bake(5, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(c > 0 && retained(c) == md, "after push");
    OUTCOME("bake before the push: stock; push moves the epoch; bake after: the material");
}

/** @brief Postfx collect with more passes than the cap keeps the highest-priority ones, lowest first. */
static void case_postfx_cap(void) {
    begin("Postfx collect, 20 passes, cap 15");
    mod_postfx_rank_t v[20];
    for (int i = 0; i < 20; i++) { v[i].index = 19 - i; v[i].load_index = 19 - i; v[i].order = 0; }
    const int n = mod_postfx_rank(v, 20, 15);
    CHECK(n == 15, "kept %d", n);
    int ok = 1;
    for (int i = 0; i < n; i++) ok &= v[i].load_index == i + 5;
    CHECK(ok, "kept the 15 highest load indices in ascending order");
    mod_postfx_rank_t w[3] = {{2, 1, 0}, {0, 1, 5}, {1, 1, 0}};
    CHECK(mod_postfx_rank(w, 3, 15) == 3 && w[0].index == 1 && w[1].index == 2 && w[2].index == 0,
          "ties by order then index");
    OUTCOME("kept load 5..19 in run order, dropped 0..4 (the lowest priority); same load: order, then index");
}

/** @brief Narrow-before-wide exclusions in one mod (sky-selector): the first same-mod registration wins at api 12, the later one at api 13. */
static void case_exclude(int api) {
    char name[96];
    snprintf(name, sizeof name, "narrow-before-wide exclusions in one mod (pin %d)", api);
    begin(name);
    stub_mod_t* a = mod("fx-exclude", 0, api);
    stub_mod_t* b = mod("fx-other", 1, 13);
    const int mx = reg(a, sel_moby(83), SCENE, NULL);
    const int mw = reg(a, sel_moby(-1), SCENE, NULL);
    reg(b, sel_moby(999), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int s = openpete_mods_material_moby_slot(NULL, 83, 0x80010000u);
    CHECK(s == DECL(api >= 13 ? mw : mx), "slot %#x", s);
    CHECK(resolve(moby_tag(83, s), SCENE) == (api >= 13 ? mw : mx), "excluded class");
    CHECK(resolve(moby_tag(99, 0), SCENE) == mw, "other class");
    OUTCOME("class 83: %s; class 99: the wide material",
            api >= 13 ? "the wide material (later registration)" : "the exclusion (first registration)");
}

/** @brief An accept-all channel-wide refine (moby-instance-tint) claims every class over earlier materials. */
static void case_accept_all(void) {
    begin("Accept-all channel-wide refine");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-tint", 1, 5);
    reg(a, sel_moby(5), SCENE, NULL);
    const int mt = reg(b, sel_moby(-1), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    const int s77 = openpete_mods_material_moby_slot(NULL, 77, 0x80010100u);
    CHECK(resolve(moby_tag(5, s5), SCENE) == mt && resolve(moby_tag(77, s77), SCENE) == mt, "both");
    CHECK(s5 != s77, "distinct slots per instance");
    OUTCOME("every class resolves to the refine, one slot per distinct block");
}

/** @brief A level-keyed selector retuned on a level change (sky-selector): live outside a present, parked inside one. */
static void case_level_retune(void) {
    begin("Level-keyed selector retuned on a level change");
    stub_mod_t* a = mod("fx-fog", 0, 11);
    openpete_mod_material_selector_t d = sel_moby(-1);
    d.level = 3;
    const int md = reg(a, d, SCENE, NULL);
    openpete_mods_set_level(3);
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "level 3");
    openpete_mods_set_level(4);
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "level 4 before retune");
    d.level = 4;
    CHECK(mod_mat_set_selector("fx-fog", md, &d) == 0, "retune");
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "level 4 after retune");
    openpete_mods_in_present = 1;
    d.level = 5;
    CHECK(mod_mat_set_selector("fx-fog", md, &d) == 0 && mod_mat_pending(), "parked");
    openpete_mods_in_present = 0;
    openpete_mods_set_level(5);
    CHECK(resolve(moby_tag(5, 0), SCENE) == -1, "level 5 parked");
    mod_mat_apply_pending();
    CHECK(resolve(moby_tag(5, 0), SCENE) == md, "level 5 applied");
    CHECK(mod_mat_set_selector("fx-other", md, &d) == -1, "foreign retune refused");
    OUTCOME("retune outside a present applies at once; inside one it waits for apply_pending");
}

/** @brief Cross-mod declarative order on flame, sky and player: the later-listed mod wins each shared key. */
static void case_cross_mod_decl(void) {
    begin("Cross-mod declarative order on flame, sky, player");
    stub_mod_t* a = mod("fx-broad", 0, 13);
    stub_mod_t* b = mod("fx-pair", 1, 13);
    openpete_mod_material_selector_t f0 = sel_ch(OP_CHAN_FLAME), f1 = sel_ch(OP_CHAN_FLAME);
    f0.ident = 0; f1.ident = 1;
    reg(a, f0, SCENE, NULL);
    reg(a, sel_ch(OP_CHAN_SKY), SCENE, NULL);
    reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    const int af1 = reg(a, f1, SCENE, NULL);
    const int bf0 = reg(b, f0, SCENE, NULL);
    const int bsk = reg(b, sel_ch(OP_CHAN_SKY), SCENE, NULL);
    const int bpl = reg(b, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 0), SCENE) == bf0, "flame 0");
    CHECK(resolve(chan_tag(OP_CHAN_SKY, 0), SCENE) == bsk, "sky");
    CHECK(resolve(chan_tag(OP_CHAN_PLAYER, 0), SCENE) == bpl, "player");
    CHECK(resolve(chan_tag(OP_CHAN_FLAME, 1), SCENE) == af1, "flame 1");
    OUTCOME("flame 0, sky, player: the later mod; flame 1 (only the earlier mod): the earlier mod");
}

/** @brief Imported-sector terrain faces: textured with no Tiledef id on both paths, so both take the textured declarative. */
static void case_imported_sector(void) {
    begin("Imported-sector terrain faces (texid -1)");
    stub_mod_t* a = mod("fx-untex", 0, 13);
    stub_mod_t* b = mod("fx-tex", 1, 13);
    openpete_mod_material_selector_t u = sel_ch(OP_CHAN_TERRAIN), t = sel_ch(OP_CHAN_TERRAIN);
    u.flags = OP_SEL_UNTEXTURED;
    t.flags = OP_SEL_TEXTURED;
    const int mu = reg(a, u, SCENE, rf_accept);
    const int mt = reg(b, t, SCENE, NULL);
    openpete_mods_material_frame_reset(1);
    const int c = openpete_mods_material_terrain_class(-1, 0, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(c) == mt, "retained %d", retained(c));
    const int s = openpete_mods_material_terrain_walk_slot(NULL, -1, 0, 0, 0);
    CHECK(s == TDECL(mt) && resolve(ter_tag(0, 0, 0, s), SCENE) == mt, "walked field %#x", s);
    CHECK(resolve(ter_tag(0, 0, 0, 0), SCENE) == mt, "slotless walked tag");
    (void)mu;
    OUTCOME("retained and walked: the textured declarative; the untextured-band refine keys only untextured faces");
}

/** @brief The walk runs down the priority order: each decline passes the moby on, a refine below the landing is never asked, an exhausted walk draws stock. */
static void case_walk_chain(void) {
    begin("Walk down the priority order");
    stub_mod_t* r = mod("fx-low", 0, 11);
    stub_mod_t* a = mod("fx-a", 1, 12);
    stub_mod_t* b = mod("fx-b", 2, 8);
    stub_mod_t* c = mod("fx-c", 3, 13);
    const int mr = reg(r, sel_moby(-1), SCENE, rf_accept);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(-1), SCENE, rf_decline);
    const int mc = reg(c, sel_moby(-1), SCENE, rf_class7);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s5 == DECL(ma) && s_calls == 2, "class 5 slot %#x calls %d", s5, s_calls);
    CHECK(s_guard_owner && strcmp(s_guard_owner, "fx-b") == 0 && s_guard_depth == 0, "last asked B");
    CHECK(mod_mat_census_get(mc, MOD_MAT_CENSUS_FALLTHROUGHS) == 1 &&
          mod_mat_census_get(mb, MOD_MAT_CENSUS_FALLTHROUGHS) == 1, "fall-throughs");
    CHECK(mod_mat_census_get(mr, MOD_MAT_CENSUS_REFINE_CALLS) == 0, "refine below the landing not asked");
    s_calls = 0;
    const int s6 = openpete_mods_material_moby_slot(NULL, 6, 0x80020000u);
    CHECK(s6 > 0 && s6 < OPENPETE_MATERIAL_SLOT_DECL && s_calls == 3, "class 6 slot %#x calls %d", s6, s_calls);
    CHECK(resolve(moby_tag(6, s6), SCENE) == mr, "class 6 lands on the lowest refine");
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(resolve(moby_tag(7, s7), SCENE) == mc, "class 7 claimed by the top refine");
    mod_mat_enable(mr, 0);
    openpete_mods_material_frame_reset(1);
    const int s8 = openpete_mods_material_moby_slot(NULL, 8, 0x80040000u);
    CHECK(s8 == OPENPETE_MATERIAL_SLOT_STOCK, "exhausted %#x", s8);
    CHECK(resolve(moby_tag(8, s8), SCENE) == -1, "exhausted resolves stock");
    mod_mat_census_force(0);
    OUTCOME("class 5: C, B decline, A lands (2 calls, the refine under A not asked); class 6: C, B decline, "
            "the lowest refine claims; class 7: C claims; every material declining: STOCK");
}

/** @brief A declarative landing binds its static block and stops drawing when its material, owner or hold changes before the draw. */
static void case_landing_state(void) {
    begin("Declarative landing at draw time");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const float blk[4] = {1, 0, 0, 1};
    CHECK(mod_mat_set_params(ma, blk, sizeof blk) == 0, "params");
    reg(b, sel_moby(-1), SCENE, rf_decline);
    openpete_mods_material_frame_reset(1);
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s == DECL(ma), "slot %#x", s);
    int slot = -1;
    CHECK(mod_mat_resolve_index(moby_tag(5, s), SCENE, &slot, NULL) == ma && slot == 0, "static block, slot %d", slot);
    CHECK(mod_mat_view_static_slot(ma) > 0, "static slot interned");
    CHECK(resolve(moby_tag(5, s), DECAL) == -1, "other contract");
    mod_mat_enable(ma, 0);
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "user-disabled after the stamp");
    mod_mat_enable(ma, 1);
    a->enabled = 0;
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "owner disabled after the stamp");
    a->enabled = 1;
    const char* held[1] = {"fx-a"};
    openpete_mods_hold_owners(held, 1);
    CHECK(resolve(moby_tag(5, s), SCENE) == -1, "held owner");
    openpete_mods_hold_owners(NULL, 0);
    CHECK(resolve(moby_tag(5, s), SCENE) == ma, "released");
    OUTCOME("DECL|%d binds the static block; user disable, owner disable and hold each draw stock", ma);
}

/** @brief One moby predicate: identity is the class, a level key matches its own level, OP_SEL_TEXTURED accepts, and a channel -1 refine takes mobys. */
static void case_moby_predicate(void) {
    begin("One moby predicate: identity, level, textured, channel -1");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    openpete_mod_material_selector_t id5 = sel_moby(-1), lev = sel_moby(-1);
    id5.ident = 5;
    lev.level = 4;
    lev.flags = OP_SEL_TEXTURED;
    const int mi = reg(a, id5, SCENE, NULL);
    const int ml = reg(b, lev, SCENE, NULL);
    const int mw = reg(b, sel_ch(-1), SCENE, rf_class7);
    openpete_mods_set_level(3);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == DECL(mi), "ident 5 on class 5");
    CHECK(openpete_mods_material_moby_slot(NULL, 6, 0x80020000u) == OPENPETE_MATERIAL_SLOT_STOCK, "class 6");
    CHECK(resolve(moby_tag(5, 0), SCENE) == mi && resolve(moby_tag(6, 0), SCENE) == -1, "draw-time");
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(resolve(moby_tag(7, s7), SCENE) == mw, "channel -1 refine claims class 7");
    openpete_mods_set_level(4);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 6, 0x80020000u) == DECL(ml), "level 4, textured key");
    OUTCOME("ident 5 takes class 5 only; a level-4 textured declarative takes mobys in level 4; "
            "a channel -1 refine claims on the moby channel");
}

/** @brief Refine outcomes are asked once per tick: a re-extract of the tick replays them, a new tick or a table change asks again. */
static void case_tick_memo(void) {
    begin("Refine outcomes kept for the tick");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(-1), SCENE, rf_class7);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 6;
    const int mt = reg(b, r, SCENE, rf_accept);
    CHECK(bake(6, 0, 0) > 0, "bake");
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    openpete_mods_material_terrain_refine_present();
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(s_calls == 3 && s5 == DECL(ma) && s7 == 2, "tick open: calls %d slots %#x %#x", s_calls, s5, s7);
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset(0);   /* a re-extract of the same tick */
    openpete_mods_material_terrain_refine_present();
    const int r7 = openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    const int r5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s_calls == 3, "replay calls %d", s_calls);
    CHECK(r5 == s5 && r7 == s7 && resolve(moby_tag(7, r7), SCENE) == mb, "replayed %#x %#x", r5, r7);
    CHECK(mod_mat_terrain_bucket_slot(mt) == 1 && mod_mat_census_memo_replays() == 3, "terrain replayed, %u",
          mod_mat_census_memo_replays());
    uint32_t len = 0;
    const void* blk = openpete_mods_material_slot_uniforms(r7, &len);
    CHECK(blk && len == 16 && memcmp(blk, v.slots[s7].bytes, 16) == 0, "replayed block");
    mod_mat_enable(ma, 0);                   /* an enable flip and back: a table change voids the memo */
    mod_mat_enable(ma, 1);
    openpete_mods_material_frame_reset(0);
    openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(s_calls == 4, "after a table change calls %d", s_calls);
    openpete_mods_material_frame_reset(1);
    openpete_mods_material_moby_slot(NULL, 7, 0x80030000u);
    CHECK(s_calls == 5, "new tick calls %d", s_calls);
    mod_mat_census_force(0);
    OUTCOME("tick open: 3 calls; re-extract: 0 calls, the same outcomes re-interned; "
            "a table change or a new tick asks again");
}

/** @brief A refine claim that finds the slot table full draws stock for that present and is not kept for the tick. */
static void case_slot_table_full(void) {
    begin("Slot table full");
    stub_mod_t* a = mod("fx-tint", 0, 13);
    reg(a, sel_moby(-1), SCENE, rf_accept);
    openpete_mods_material_frame_reset(1);
    int last = 0;
    for (int i = 0; i < MOD_MATERIAL_SLOTS - 1; i++)
        last = openpete_mods_material_moby_slot(NULL, 5, 0x80100000u + (uint32_t)i * 0x100u);
    CHECK(last == MOD_MATERIAL_SLOTS - 1, "last slot %d", last);
    s_calls = 0;
    const int over = openpete_mods_material_moby_slot(NULL, 5, 0x80900000u);
    CHECK(over == OPENPETE_MATERIAL_SLOT_STOCK && s_calls == 1, "full: %#x", over);
    openpete_mods_material_frame_reset(0);
    const int again = openpete_mods_material_moby_slot(NULL, 5, 0x80900000u);
    CHECK(again == 1 && s_calls == 2, "asked again on the re-extract: slot %d calls %d", again, s_calls);
    OUTCOME("the 128th distinct block draws stock; the re-extract asks again and gets slot 1");
}


/* ------------------------------------------------------------------ */
/* Composition (moby chains). Each composable material below carries its
 * own fragment shader handle; a stub installed member set lists the
 * shaders the combined module serves. */

/** @brief Register a scene-contract material with a fragment shader handle and set that shader's link and identity masks.
 *
 * @param m Owner.
 * @param sel Selector.
 * @param frag Fragment shader handle.
 * @param link Link mask (bit 0 the stock profile).
 * @param ident Identity mask.
 * @param fn Refine callback, or NULL.
 * @return The handle, or -1 when refused. */
static int regf(const stub_mod_t* m, openpete_mod_material_selector_t sel, int frag, unsigned link,
                unsigned ident, openpete_mod_material_fn fn) {
    if (!mod_mat_sel_valid(m->id, &sel, m->api)) return -1;
    const int h = mod_mat_append(m->id, &sel, -1, frag, SCENE, fn, m->load_index, m->api, 1u);
    if (h >= 0) mod_mat_set_link_of_frag(frag, link, ident);
    return h;
}

static openpete_mods_material_chain_set_t s_set_a, s_set_b;
/** @brief Fill and publish a stub member set: generation, shader handles, case id = list position in the scene profiles, none in the decal one.
 *
 * @param s The record.
 * @param gen Generation.
 * @param n Shader count.
 * @param shaders The handles. */
static void set_publish(openpete_mods_material_chain_set_t* s, unsigned gen, int n, const int* shaders) {
    memset(s, 0, sizeof *s);
    s->generation = gen;
    s->n = n;
    for (int i = 0; i < n; i++) {
        s->shader[i] = shaders[i];
        s->case_id[0][i] = s->case_id[1][i] = s->case_id[OPENPETE_MATERIAL_CHAIN_TERRAIN][i] = i;
        s->case_id[OPENPETE_MATERIAL_CHAIN_DECAL][i] = -1;
    }
    openpete_mods_material_chain_set_publish(s);
}
/** @brief The moby tag field naming chain entry @p id. */
#define CHAIN(id) (OPENPETE_MATERIAL_SLOT_CHAIN + (id))

/** @brief Resolve a chain tag: its member count, -1 for stock, 0 when the tag names no chain; fills the members. */
static int chain_of(uint32_t tag, mod_mat_chain_res_t* r) {
    const int c = mod_mat_resolve_chain(tag, SCENE, r);
    return c == 1 ? r->n : c;
}

/** @brief Two and three composable declaratives compose lowest first; a chain over the cap keeps the highest four. */
static void case_chain_compose(void) {
    begin("Chain: two, three and five composable declaratives");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    stub_mod_t* c = mod("fx-c", 2, 13);
    const int ma = regf(a, sel_moby(-1), 10, 1, 0, NULL);
    const int mb = regf(b, sel_moby(-1), 11, 1, 0, NULL);
    const float blk[4] = {1, 0, 0, 1};
    CHECK(mod_mat_set_params(mb, blk, sizeof blk) == 0, "params");
    const int sh[3] = {10, 11, 12};
    set_publish(&s_set_a, 1, 3, sh);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    const int s2 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    mod_mat_chain_res_t r;
    CHECK(s2 == CHAIN(0), "two-member field %#x", s2);
    CHECK(chain_of(moby_tag(5, s2), &r) == 2 && r.mat[0] == ma && r.mat[1] == mb, "lowest first");
    CHECK(r.slot[0] == 0 && r.slot[1] == mod_mat_view_static_slot(mb) && r.slot[1] > 0, "member slots");
    CHECK(resolve(moby_tag(5, s2), SCENE) == -1, "the one-material resolve does not take a chain tag");
    const int mc = regf(c, sel_moby(-1), 12, 1, 0, NULL);
    openpete_mods_material_frame_reset(1);
    const int s3 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, s3), &r) == 3 && r.mat[0] == ma && r.mat[2] == mc, "three members");
    stub_mod_t* d = mod("fx-d", 3, 13);
    stub_mod_t* e = mod("fx-e", 4, 13);
    const int md = regf(d, sel_moby(-1), 10, 1, 0, NULL);
    const int me = regf(e, sel_moby(-1), 11, 1, 0, NULL);
    openpete_mods_material_frame_reset(1);
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, s5), &r) == 4 && r.mat[0] == mb && r.mat[1] == mc && r.mat[2] == md &&
          r.mat[3] == me, "over the cap: the four highest");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_DRAWS_2) == 1 && mod_mat_census_chain(MOD_MAT_CHAIN_DRAWS_2 + 1) == 1 &&
          mod_mat_census_chain(MOD_MAT_CHAIN_DRAWS_2 + 2) == 1, "census by members");
    unsigned char blkout[OPENPETE_MATERIAL_CHAIN_BLOCK_BYTES];
    const unsigned char eng[4] = {9, 9, 9, 9};
    CHECK(openpete_mods_material_chain_block(0, 0, eng, 4, blkout) == 0, "block");
    uint32_t n = 0, ids[4];
    memcpy(&n, blkout, 4);
    memcpy(ids, blkout + 16, sizeof ids);
    CHECK(n == 4 && ids[0] == 1 && ids[1] == 2 && ids[2] == 0 && ids[3] == 1, "header n %u ids %u %u %u %u", n,
          ids[0], ids[1], ids[2], ids[3]);
    CHECK(memcmp(blkout + 256, blk, sizeof blk) == 0, "position 0 holds b's params");
    CHECK(memcmp(blkout + 512, eng, sizeof eng) == 0, "position 1 (c, no params) holds the engine block");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("a,b: chain [a b]; a,b,c: [a b c]; five: [b c d e], a dropped; the block header lists case ids lowest first");
}

/** @brief A terminal top draws alone; a terminal member below a composable one ends the chain; OP_SEL_EXCLUSIVE is terminal and refused below pin 13. */
static void case_chain_terminal(void) {
    begin("Chain: terminal members");
    stub_mod_t* o = mod("fx-old", 0, 12);
    stub_mod_t* a = mod("fx-a", 1, 13);
    stub_mod_t* t = mod("fx-top12", 2, 12);
    const int mo = regf(o, sel_moby(-1), 10, 1, 0, NULL);
    const int ma = regf(a, sel_moby(-1), 11, 1, 0, NULL);
    const int sh[3] = {10, 11, 12};
    set_publish(&s_set_a, 1, 3, sh);
    openpete_mods_material_frame_reset(1);
    mod_mat_chain_res_t r;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, s), &r) == 2 && r.mat[0] == mo && r.mat[1] == ma, "pin 12 below ends the chain");
    const int mt = regf(t, sel_moby(-1), 12, 1, 0, NULL);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == DECL(mt), "pin 12 on top draws alone");
    openpete_mod_material_selector_t ex = sel_moby(-1);
    ex.flags = OP_SEL_EXCLUSIVE;
    CHECK(regf(t, ex, 12, 1, 0, NULL) == -1, "OP_SEL_EXCLUSIVE refused at pin 12");
    OUTCOME("pin 12 below a composable member: the chain ends on it; pin 12 on top: alone; exclusive at pin 12: refused");

    begin("Chain: OP_SEL_EXCLUSIVE at pin 13");
    stub_mod_t* x = mod("fx-x", 0, 13);
    a = mod("fx-a", 1, 13);
    stub_mod_t* b = mod("fx-b", 2, 13);
    const int mx = regf(x, sel_moby(-1), 10, 1, 0, NULL);
    const int ma2 = regf(a, ex, 11, 1, 0, NULL);
    regf(b, sel_moby(-1), 12, 1, 0, NULL);
    set_publish(&s_set_a, 1, 3, sh);
    openpete_mods_material_frame_reset(1);
    const int s2 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, s2), &r) == 2 && r.mat[0] == ma2, "the exclusive member ends the chain under b");
    openpete_mod_material_selector_t plain = sel_moby(-1);
    CHECK(mod_mat_set_selector("fx-a", ma2, &plain) == 0, "retune drops exclusive");
    openpete_mods_material_frame_reset(1);
    const int s3 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, s3), &r) == 3 && r.mat[0] == mx, "composable again at the next walk");
    openpete_mods_material_chain_set_publish(NULL);
    OUTCOME("exclusive at pin 13: terminal, the chain ends on it; a retune that drops it composes the member below");
}

/** @brief An unlinkable member at the top draws alone; below a composable member it is skipped without being asked; an identity member is elided. */
static void case_chain_unlinkable(void) {
    begin("Chain: unlinkable members");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* u = mod("fx-unl", 1, 13);
    stub_mod_t* b = mod("fx-b", 2, 13);
    const int ma = regf(a, sel_moby(-1), 10, 1, 0, NULL);
    regf(u, sel_moby(-1), 20, 0, 0, rf_accept);         /* an unlinkable refine */
    const int mb = regf(b, sel_moby(-1), 11, 1, 0, NULL);
    const int sh[2] = {10, 11};
    set_publish(&s_set_a, 1, 2, sh);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    mod_mat_chain_res_t r;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, s), &r) == 2 && r.mat[0] == ma && r.mat[1] == mb && s_calls == 0,
          "unlinkable below b: skipped, never asked (calls %d)", s_calls);
    stub_mod_t* t = mod("fx-unl-top", 3, 13);
    const int mt = regf(t, sel_moby(-1), 21, 0, 0, NULL);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == DECL(mt), "unlinkable on top draws alone");
    openpete_mods_material_chain_set_publish(NULL);
    OUTCOME("unlinkable below a composable member: skipped, 0 refine calls; on top: alone");

    begin("Chain: identity member");
    a = mod("fx-a", 0, 13);
    stub_mod_t* i = mod("fx-id", 1, 13);
    b = mod("fx-b", 2, 13);
    const int ma2 = regf(a, sel_moby(-1), 10, 1, 0, NULL);
    regf(i, sel_moby(-1), 30, 1, 1, NULL);
    const int mb2 = regf(b, sel_moby(-1), 11, 1, 0, NULL);
    set_publish(&s_set_a, 1, 2, sh);
    openpete_mods_material_frame_reset(1);
    const int s2 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, s2), &r) == 2 && r.mat[0] == ma2 && r.mat[1] == mb2, "identity elided");
    mod_mat_enable(mb2, 0);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == DECL(ma2),
          "identity top over one member: that member alone");
    openpete_mods_material_chain_set_publish(NULL);
    OUTCOME("an identity member is left out of the chain; over one member, that member draws alone");
}

/** @brief A member with a vertex stage joins only as the chain's lowest; on top it draws alone. */
static void case_chain_vertex(void) {
    begin("Chain: vertex-stage member");
    stub_mod_t* v = mod("fx-v", 0, 13);
    stub_mod_t* a = mod("fx-a", 1, 13);
    openpete_mod_material_selector_t s = sel_moby(-1);
    const int mv = mod_mat_append(v->id, &s, 40, 10, SCENE, NULL, v->load_index, 13, 1u);
    mod_mat_set_link_of_frag(10, 1, 0);
    const int ma = regf(a, sel_moby(-1), 11, 1, 0, NULL);
    const int sh[3] = {10, 11, 12};
    set_publish(&s_set_a, 1, 3, sh);
    openpete_mods_material_frame_reset(1);
    mod_mat_chain_res_t r;
    const int f = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, f), &r) == 2 && r.mat[0] == mv && r.mat[1] == ma, "lowest: kept");
    stub_mod_t* w = mod("fx-w", 2, 13);
    const int mw = mod_mat_append(w->id, &s, 41, 12, SCENE, NULL, w->load_index, 13, 1u);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == DECL(mw), "vertex stage on top: alone");
    mod_mat_enable(mw, 0);
    stub_mod_t* x = mod("fx-x", 3, 13);
    mod_mat_append(x->id, &s, 42, 11, SCENE, NULL, x->load_index, 13, 1u);   /* a vertex member in the middle */
    stub_mod_t* y = mod("fx-y", 4, 13);
    const int my = regf(y, sel_moby(-1), 12, 1, 0, NULL);
    openpete_mods_material_frame_reset(1);
    const int g = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, g), &r) == 3 && r.mat[0] == mv && r.mat[1] == ma && r.mat[2] == my,
          "a vertex member above a kept one is dropped");
    openpete_mods_material_chain_set_publish(NULL);
    OUTCOME("a vertex-stage member joins only as the lowest; above a kept member it is dropped; on top it draws alone");
}

/** @brief Members absent from the installed set: at the top the member draws alone, below a composable member it is skipped; each counted. */
static void case_chain_absent(void) {
    begin("Chain: members absent from the installed set");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    stub_mod_t* c = mod("fx-c", 2, 13);
    const int ma = regf(a, sel_moby(-1), 10, 1, 0, NULL);
    regf(b, sel_moby(-1), 11, 1, 0, NULL);
    const int mc = regf(c, sel_moby(-1), 12, 1, 0, NULL);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == DECL(mc), "no set installed: the top draws alone");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_ABSENT) == 1, "absent top counted");
    const int sh[2] = {10, 11};
    set_publish(&s_set_a, 1, 2, sh);
    openpete_mods_material_frame_reset(1);
    CHECK(openpete_mods_material_moby_slot(NULL, 5, 0x80010000u) == DECL(mc), "top absent from the set: alone");
    const int sh2[2] = {10, 12};
    set_publish(&s_set_b, 2, 2, sh2);
    openpete_mods_material_frame_reset(1);
    mod_mat_chain_res_t r;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(chain_of(moby_tag(5, s), &r) == 2 && r.mat[0] == ma && r.mat[1] == mc, "b absent below c: skipped");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_ABSENT) == 3, "absent %u", mod_mat_census_chain(MOD_MAT_CHAIN_ABSENT));
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("absent top: alone; absent below a composable member: skipped; 3 exclusions counted");
}

/** @brief A set published between extraction and compose that lacks a stamped member sends the chain stock, counted; a re-extract under the new set walks again. */
static void case_chain_generation(void) {
    begin("Chain: set generation change between extraction and present");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = regf(a, sel_moby(-1), 10, 1, 0, NULL);
    const int mb = regf(b, sel_moby(-1), 11, 1, 0, rf_accept);
    const int sh[2] = {10, 11};
    set_publish(&s_set_a, 1, 2, sh);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    openpete_mods_material_view_capture(&v);
    mod_mat_chain_res_t r;
    openpete_mods_material_view_set(&v);
    CHECK(chain_of(moby_tag(5, s), &r) == 2 && r.set == &s_set_a, "composes under generation 1");
    const int sh2[1] = {11};
    set_publish(&s_set_b, 2, 1, sh2);   /* a's shader left the installed module */
    CHECK(chain_of(moby_tag(5, s), &r) == -1 && mod_mat_census_chain(MOD_MAT_CHAIN_LACKED) == 1,
          "the stamped chain draws stock, counted");
    openpete_mods_material_view_set(NULL);
    openpete_mods_material_frame_reset(0);    /* a re-extract of the same tick */
    const int s2 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s_calls == 2, "walked again, not replayed (calls %d)", s_calls);
    CHECK(s2 > 0 && s2 < OPENPETE_MATERIAL_SLOT_CHAIN && resolve(moby_tag(5, s2), SCENE) == mb,
          "under generation 2 the refine draws alone (%#x)", s2);
    set_publish(&s_set_a, 3, 2, sh);
    openpete_mods_material_frame_reset(0);
    const int s3 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    openpete_mods_material_frame_reset(0);
    const int s4 = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(s3 == CHAIN(0) && s4 == CHAIN(0) && s_calls == 3, "one generation: replayed (calls %d)", s_calls);
    CHECK(chain_of(moby_tag(5, s4), &r) == 2 && r.mat[0] == ma && r.slot[1] > 0, "replayed refine block re-interned");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("a set lacking a stamped member: stock, counted; the re-extract under it walks again; one set: replayed");
}

/** @brief A held member sends a stamped chain stock for the compose, and so does holding every member. */
static void case_chain_held(void) {
    begin("Chain: held members");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    regf(a, sel_moby(-1), 10, 1, 0, NULL);
    regf(b, sel_moby(-1), 11, 1, 0, NULL);
    const int sh[2] = {10, 11};
    set_publish(&s_set_a, 1, 2, sh);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    const int s = openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    mod_mat_chain_res_t r;
    const char* one[1] = {"fx-a"};
    openpete_mods_hold_owners(one, 1);
    CHECK(chain_of(moby_tag(5, s), &r) == -1, "one member held: stock");
    const char* both[2] = {"fx-a", "fx-b"};
    openpete_mods_hold_owners(both, 2);
    CHECK(chain_of(moby_tag(5, s), &r) == -1, "all held: stock");
    unsigned char blk[OPENPETE_MATERIAL_CHAIN_BLOCK_BYTES];
    CHECK(openpete_mods_material_chain_block(0, 0, NULL, 0, blk) == -1, "no block for a held chain");
    openpete_mods_hold_owners(NULL, 0);
    CHECK(chain_of(moby_tag(5, s), &r) == 2, "released");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_HELD) == 2, "held counted %u", mod_mat_census_chain(MOD_MAT_CHAIN_HELD));
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("one member held or all held: the chain draws stock for that compose, counted; released: composes");
}

/** @brief A chain whose refine block finds the slot table full draws stock for the present, counted. */
static void case_chain_table_full(void) {
    begin("Chain: slot table full");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    regf(a, sel_moby(-1), 10, 1, 0, NULL);
    regf(b, sel_moby(-1), 11, 1, 0, rf_accept);
    const int sh[2] = {10, 11};
    set_publish(&s_set_a, 1, 2, sh);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    int last = 0;
    for (int i = 0; i < MOD_MATERIAL_SLOTS - 1; i++)
        last = openpete_mods_material_moby_slot(NULL, 5, 0x80100000u + (uint32_t)i * 0x100u);
    CHECK(last == CHAIN(MOD_MATERIAL_SLOTS - 2), "127 distinct chains, last %#x", last);
    const int over = openpete_mods_material_moby_slot(NULL, 5, 0x80900000u);
    CHECK(over == OPENPETE_MATERIAL_SLOT_STOCK && mod_mat_census_chain(MOD_MAT_CHAIN_FULL) == 1,
          "slot table full: stock, counted");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("the 128th distinct refine block fills the slot table: that chain draws stock, counted");
}

/* ------------------------------------------------------------------ */
/* Terrain chains. */

/** @brief The terrain tag field naming terrain chain entry @p id. */
#define TCHAIN(id) (OPENPETE_MATERIAL_TERRAIN_CHAIN | (id))
/** @brief The retained pass id of terrain chain entry @p id. */
#define TPASS(id) (OPENPETE_MATERIAL_TER_PASS_CHAIN + (id))

/** @brief Retained path: the pass id that draws class @p c (0 the stock pass), or -2 when no pass, or more than one, holds it. */
static int retained_pass(int c) {
    int passes[OPENPETE_MATERIAL_TER_BUCKET_MAX + 1];
    const int n = openpete_mods_material_terrain_bucket_list(passes + 1, OPENPETE_MATERIAL_TER_BUCKET_MAX);
    passes[0] = 0;
    int hit = -2, hits = 0;
    for (int i = 0; i <= n; i++) {
        uint32_t mask[8];
        (void)openpete_mods_material_terrain_pass_mask(passes[i], mask);
        if ((mask[c >> 5] >> (c & 31)) & 1u) { hit = passes[i]; hits++; }
    }
    return hits == 1 ? hit : -2;
}
/** @brief A terrain selector on the textured faces. */
static openpete_mod_material_selector_t sel_ter(void) {
    openpete_mod_material_selector_t t = sel_ch(OP_CHAN_TERRAIN);
    t.flags = OP_SEL_TEXTURED;
    return t;
}

/** @brief The class list continues past pinned-13 declaratives, OP_SEL_EXCLUSIVE included, and ends at the first declarative pinned below 13. */
static void case_ter_class_list(void) {
    begin("Terrain chain: the class list past pinned-13 declaratives");
    stub_mod_t* o = mod("fx-old", 0, 12);
    stub_mod_t* a = mod("fx-a", 1, 13);
    stub_mod_t* x = mod("fx-x", 2, 13);
    stub_mod_t* b = mod("fx-b", 3, 13);
    const int mo = regf(o, sel_ter(), 20, 3, 0, NULL);
    const int ma = regf(a, sel_ter(), 21, 3, 0, NULL);
    openpete_mod_material_selector_t ex = sel_ter();
    ex.flags |= OP_SEL_EXCLUSIVE;
    const int mx = regf(x, ex, 22, 3, 0, NULL);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 3;
    const int mr = regf(b, r, 23, 3, 0, rf_accept);
    const int sh[4] = {20, 21, 22, 23};
    set_publish(&s_set_a, 1, 4, sh);
    openpete_mods_material_frame_reset(1);
    const int c = bake(3, 0, 0);
    openpete_mods_material_terrain_refine_present();
    mod_mat_chain_res_t ch;
    CHECK(chain_of(ter_tag(3, 0, 0, walk(3, 0, 0)), &ch) == 2 && ch.mat[0] == mx && ch.mat[1] == mr,
          "exclusive x ends the walk: [x r]");
    const unsigned ep = openpete_mods_material_terrain_epoch();
    openpete_mod_material_selector_t open = sel_ter();
    CHECK(mod_mat_set_selector("fx-x", mx, &open) == 0 && openpete_mods_material_terrain_epoch() == ep,
          "exclusive off: the same class, no rebake");
    openpete_mods_material_frame_reset(1);
    CHECK(bake(3, 0, 0) == c, "same class id");
    openpete_mods_material_terrain_refine_present();
    CHECK(chain_of(ter_tag(3, 0, 0, walk(3, 0, 0)), &ch) == 4 && ch.mat[0] == mo && ch.mat[1] == ma &&
          ch.mat[2] == mx && ch.mat[3] == mr, "the list held a and old12 under x: [old a x r]");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_reset();
    s_mod_n = 0;
    stub_mod_t* o2 = mod("fx-old", 0, 12);
    stub_mod_t* b2 = mod("fx-b", 1, 12);
    reg(o2, sel_ter(), SCENE, NULL);
    const int d2 = reg(b2, sel_ter(), SCENE, NULL);
    reg(b2, r, SCENE, rf_decline);
    openpete_mods_material_frame_reset(1);
    const int cp = bake(3, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(cp) == d2 && walk(3, 0, 0) == TDECL(d2), "pins below 13: the refine declines onto the declarative winner");
    OUTCOME("pin 13: one class [r x a old12]; x exclusive ends the walk at [x r], "
            "an exclusive-only retune keeps the class and composes [old12 a x r]; pins below 13: P4's list");
}

/** @brief Two composable terrain declaratives chain on both paths: the walked stamp, the chain pass, the view, the decode and the block. */
static void case_ter_chain_resolve(void) {
    begin("Terrain chain: two composable declaratives on both paths");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = regf(a, sel_ter(), 20, 3, 0, NULL);
    const int mb = regf(b, sel_ter(), 21, 3, 0, NULL);
    const float blk[4] = {0.5f, 1, 1, 0};
    CHECK(mod_mat_set_params(mb, blk, sizeof blk) == 0, "params");
    const int sh[2] = {20, 21};
    set_publish(&s_set_a, 1, 2, sh);
    s_set_a.case_id[OPENPETE_MATERIAL_CHAIN_TERRAIN][0] = 5;   /* the terrain module numbers its cases its own way */
    s_set_a.case_id[OPENPETE_MATERIAL_CHAIN_TERRAIN][1] = 6;
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    const int c = bake(2, 0, 0);
    openpete_mods_material_terrain_refine_present();
    const int f = walk(2, 0, 0);
    CHECK(f == TCHAIN(0), "walked stamp %#x", f);
    CHECK(retained_pass(c) == TPASS(0) && retained_passes() == 1, "retained: the chain pass %#x", retained_pass(c));
    CHECK(mod_mat_terrain_class_resolved(c) == MOD_MAT_TER_OUT_CHAIN && mod_mat_terrain_class_retained(c) == MOD_MAT_TER_OUT_CHAIN,
          "one outcome on both paths");
    mod_mat_chain_res_t r;
    CHECK(chain_of(ter_tag(2, 0, 0, f), &r) == 2 && r.mat[0] == ma && r.mat[1] == mb &&
          r.entry == OPENPETE_MATERIAL_CHAINS, "walked decode: lowest first, entry %d", r.entry);
    CHECK(resolve(ter_tag(2, 0, 0, f), SCENE) == -1, "the one-material resolve does not take a chain field");
    CHECK(mod_mat_resolve_terrain_chain(TPASS(0), &r) == 1 && r.n == 2, "retained decode");
    CHECK(mod_mat_resolve_terrain_chain(1 + ma, &r) == 0, "a material pass names no chain");
    unsigned char out[OPENPETE_MATERIAL_CHAIN_BLOCK_BYTES];
    uint32_t n = 0, ids[4];
    CHECK(openpete_mods_material_chain_block(OPENPETE_MATERIAL_CHAINS, OPENPETE_MATERIAL_CHAIN_TERRAIN, NULL, 0, out) == 0,
          "terrain block");
    memcpy(&n, out, 4);
    memcpy(ids, out + 16, sizeof ids);
    CHECK(n == 2 && ids[0] == 5 && ids[1] == 6, "terrain case ids %u %u", ids[0], ids[1]);
    CHECK(memcmp(out + 512, blk, sizeof blk) == 0, "position 1 holds b's params");
    CHECK(openpete_mods_material_chain_block(OPENPETE_MATERIAL_CHAINS, 0, NULL, 0, out) == 0 &&
          (memcpy(ids, out + 16, sizeof ids), ids[0] == 0 && ids[1] == 1), "stock block: stock case ids");
    CHECK(openpete_mods_material_chain_block(0, OPENPETE_MATERIAL_CHAIN_TERRAIN, NULL, 0, out) == -1,
          "a moby entry has no terrain block");
    CHECK(openpete_mods_material_chain_ubo_block(OPENPETE_MATERIAL_CHAINS, 2) == 1 + 2 * OPENPETE_MATERIAL_CHAINS + 2 &&
          openpete_mods_material_chain_ubo_block(1, 1) == 4 && openpete_mods_material_chain_ubo_block(0, 2) == -1,
          "chain uniform blocks");
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_frame_reset(1);
    CHECK(chain_of(ter_tag(2, 0, 0, f), &r) == -1 && mod_mat_census_chain(MOD_MAT_CHAIN_STALE) == 1,
          "live table after the reset: stale, counted");
    openpete_mods_material_view_set(&v);
    CHECK(v.ter_chain_n == 1 && v.ter_class_pass[c] == TPASS(0), "the view holds the chain and its pass id");
    CHECK(retained_pass(c) == TPASS(0) && chain_of(ter_tag(2, 0, 0, f), &r) == 2, "composed through the view");
    openpete_mods_material_view_set(NULL);
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_TER_WALKS) == 1 && mod_mat_census_chain(MOD_MAT_CHAIN_TER_PASSES) == 1,
          "census: %u chains formed, %u passes", mod_mat_census_chain(MOD_MAT_CHAIN_TER_WALKS),
          mod_mat_census_chain(MOD_MAT_CHAIN_TER_PASSES));
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("a,b on terrain: walked field 0x080, retained pass 0x100, entry 256 lowest first; terrain block with the terrain case ids");
}

/** @brief A terrain refine over a composable declarative chains while it accepts and leaves the declarative alone when it declines; an exclusive top or no installed set draws alone. */
static void case_ter_chain_refine_terminal(void) {
    begin("Terrain chain: refine over a declarative, exclusive and absent tops");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int md = regf(a, sel_ter(), 20, 3, 0, NULL);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 1;
    const int mr = regf(b, r, 21, 3, 0, rf_accept);
    const int sh[2] = {20, 21};
    set_publish(&s_set_a, 1, 2, sh);
    openpete_mods_material_frame_reset(1);
    s_calls = 0;
    const int c = bake(1, 0, 0);
    openpete_mods_material_terrain_refine_present();
    mod_mat_chain_res_t ch;
    const int f = walk(1, 0, 0);
    CHECK(f == TCHAIN(0) && chain_of(ter_tag(1, 0, 0, f), &ch) == 2 && ch.mat[0] == md && ch.mat[1] == mr &&
          ch.slot[1] > 0, "refine over declarative: chain with the refine's slot");
    CHECK(s_calls == 1 && retained_pass(c) == TPASS(0), "one call for both paths, %d", s_calls);
    openpete_mods_material_chain_set_publish(NULL);
    openpete_mods_material_frame_reset(1);
    openpete_mods_material_terrain_refine_present();
    const int f2 = walk(1, 0, 0);
    CHECK(f2 > 0 && f2 < TCHAIN(0) && resolve(ter_tag(1, 0, 0, f2), SCENE) == mr && retained(c) == mr,
          "no installed set: the refine draws alone (%#x)", f2);
    set_publish(&s_set_a, 2, 2, sh);
    openpete_mod_material_selector_t rx = r;
    rx.flags |= OP_SEL_EXCLUSIVE;
    CHECK(mod_mat_set_selector("fx-b", mr, &rx) == 0, "retune exclusive");
    openpete_mods_material_frame_reset(1);
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(c) == mr && resolve(ter_tag(1, 0, 0, walk(1, 0, 0)), SCENE) == mr, "exclusive top draws alone");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_reset();
    s_mod_n = 0;
    a = mod("fx-a", 0, 13);
    b = mod("fx-b", 1, 13);
    const int md2 = regf(a, sel_ter(), 20, 3, 0, NULL);
    const int mr2 = regf(b, r, 21, 3, 0, rf_decline);
    set_publish(&s_set_a, 3, 2, sh);
    openpete_mods_material_frame_reset(1);
    const int c2 = bake(1, 0, 0);
    openpete_mods_material_terrain_refine_present();
    CHECK(retained(c2) == md2 && walk(1, 0, 0) == TDECL(md2), "declining refine: the declarative alone");
    (void)mr2;
    openpete_mods_material_chain_set_publish(NULL);
    OUTCOME("refine accepts: chain [decl refine] on both paths; no set: the refine alone; exclusive: alone; declines: decl alone");
}

/** @brief A member without a linkable terrain profile reduces the retained outcome while the walked stamp names the full chain. */
static void case_ter_chain_profiles(void) {
    begin("Terrain chain: per-profile linkability");
    stub_mod_t* c = mod("fx-c", 0, 13);
    stub_mod_t* a = mod("fx-a", 1, 13);
    stub_mod_t* b = mod("fx-b", 2, 13);
    const int ma = regf(a, sel_ter(), 20, 3, 0, NULL);
    const int mb = regf(b, sel_ter(), 21, 3, 0, NULL);
    const int mc = regf(c, sel_ter(), 22, 1, 0, NULL);   /* stock profile only */
    const int sh[3] = {20, 21, 22};
    set_publish(&s_set_a, 1, 3, sh);
    s_set_a.case_id[OPENPETE_MATERIAL_CHAIN_TERRAIN][2] = -1;
    openpete_mods_material_frame_reset(1);
    const int k = bake(2, 0, 0);
    openpete_mods_material_terrain_refine_present();
    mod_mat_chain_res_t r, t;
    const int f = walk(2, 0, 0);
    CHECK(chain_of(ter_tag(2, 0, 0, f), &r) == 3 && r.mat[0] == mc, "walked: the full chain [c a b]");
    const int p = retained_pass(k);
    CHECK(p == TPASS(1) && mod_mat_resolve_terrain_chain(p, &t) == 1 && t.n == 2 && t.mat[0] == ma && t.mat[1] == mb,
          "retained: c, without a terrain profile, is skipped below the top - pass %#x", p);
    CHECK(mod_mat_terrain_class_resolved(k) == MOD_MAT_TER_OUT_CHAIN && mod_mat_terrain_class_retained(k) ==
          MOD_MAT_TER_OUT_CHAIN + 1, "two outcomes, two entries");
    openpete_mods_material_chain_set_publish(NULL);
    OUTCOME("c (lowest) lacks a terrain profile: walked chain [c a b], retained chain [a b]");
}

/** @brief A retained chain pass draws stock for a held member, a member the installed terrain module lacks and a stale entry. */
static void case_ter_chain_fallback(void) {
    begin("Terrain chain: pass fallback to stock");
    static openpete_mods_material_view_t v;
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    regf(a, sel_ter(), 20, 3, 0, NULL);
    regf(b, sel_ter(), 21, 3, 0, NULL);
    const int sh[2] = {20, 21};
    set_publish(&s_set_a, 1, 2, sh);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    const int c = bake(2, 0, 0);
    openpete_mods_material_terrain_refine_present();
    openpete_mods_material_view_capture(&v);
    openpete_mods_material_view_set(&v);
    mod_mat_chain_res_t r;
    CHECK(mod_mat_resolve_terrain_chain(retained_pass(c), &r) == 1, "resolves");
    const char* one[1] = {"fx-b"};
    openpete_mods_hold_owners(one, 1);
    CHECK(mod_mat_resolve_terrain_chain(TPASS(0), &r) == -1 && mod_mat_census_chain(MOD_MAT_CHAIN_HELD) == 1,
          "held member: stock, counted");
    openpete_mods_hold_owners(NULL, 0);
    s_set_b = s_set_a;
    s_set_b.generation = 2;
    s_set_b.case_id[OPENPETE_MATERIAL_CHAIN_TERRAIN][1] = -1;
    openpete_mods_material_chain_set_publish(&s_set_b);
    CHECK(mod_mat_resolve_terrain_chain(TPASS(0), &r) == -1 && mod_mat_census_chain(MOD_MAT_CHAIN_LACKED) == 1,
          "the installed terrain module lacks b: stock, counted");
    openpete_mods_material_chain_set_publish(&s_set_a);
    CHECK(mod_mat_resolve_terrain_chain(TPASS(1), &r) == -1 && mod_mat_census_chain(MOD_MAT_CHAIN_STALE) == 1,
          "an entry past the view: stock, counted");
    openpete_mods_material_view_set(NULL);
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("held, lacked by the terrain module, stale: the chain pass draws stock, each counted");
}

/** @brief A full terrain chain table sends the next class's chain to stock, counted. */
static void case_ter_chain_table_full(void) {
    begin("Terrain chain: table full");
    stub_mod_t* d = mod("fx-d", 0, 13);
    stub_mod_t* km = mod("fx-k", 1, 13);
    openpete_mod_material_selector_t all = sel_ch(OP_CHAN_TERRAIN);
    regf(d, all, 20, 3, 0, NULL);
    int sh[2] = {20, 21};
    for (int t = 0; t < 128; t++) {   /* one keyed declarative per Tiledef id over the channel-wide one */
        openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN);
        r.texture = t;
        regf(km, r, 21, 3, 0, NULL);
    }
    openpete_mod_material_selector_t u = sel_ch(OP_CHAN_TERRAIN);
    u.flags = OP_SEL_UNTEXTURED;
    regf(km, u, 21, 3, 0, NULL);
    set_publish(&s_set_a, 1, 2, sh);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    int last = 0;
    for (int t = 0; t < 128; t++) last = walk(t, 0, 0);
    CHECK(last == TCHAIN(127) && openpete_mods_material_terrain_chain_count() == 128, "128 chains, last %#x", last);
    CHECK(walk(-1, 0, 0) == TSTOCK && mod_mat_census_chain(MOD_MAT_CHAIN_FULL) == 1, "the 129th: stock, counted");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("128 distinct terrain chains fill the table; the 129th class draws stock, counted");
}

/** @brief An OP_SEL_EXCLUSIVE-only retune leaves the terrain epoch; any other change moves it. */
static void case_ter_epoch_skip(void) {
    begin("Terrain epoch: OP_SEL_EXCLUSIVE masked");
    openpete_mod_material_selector_t x = sel_ter(), y = sel_ter();
    y.flags |= OP_SEL_EXCLUSIVE;
    CHECK(mod_mat_sel_same_but_exclusive(&x, &y) && mod_mat_sel_same_but_exclusive(&y, &x) &&
          mod_mat_sel_same_but_exclusive(&x, &x), "differ only in the bit: same");
    openpete_mod_material_selector_t z = y;
    z.texture = 4;
    CHECK(!mod_mat_sel_same_but_exclusive(&x, &z), "a key differs: not same");
    z = x;
    z.flags |= OP_SEL_ENV_ANIM;
    CHECK(!mod_mat_sel_same_but_exclusive(&x, &z), "another flag differs: not same");
    stub_mod_t* a = mod("fx-a", 0, 13);
    const int m = reg(a, x, SCENE, NULL);
    const unsigned e0 = openpete_mods_material_terrain_epoch();
    CHECK(mod_mat_set_selector("fx-a", m, &y) == 0 && openpete_mods_material_terrain_epoch() == e0,
          "exclusive on: no bump");
    CHECK(mod_mat_set_selector("fx-a", m, &x) == 0 && openpete_mods_material_terrain_epoch() == e0,
          "exclusive off: no bump");
    CHECK(mod_mat_set_selector("fx-a", m, &x) == 0 && openpete_mods_material_terrain_epoch() == e0,
          "same bytes: no bump");
    z = x;
    z.texture = 4;
    CHECK(mod_mat_set_selector("fx-a", m, &z) == 0 && openpete_mods_material_terrain_epoch() == e0 + 1, "a key: bump");
    openpete_mods_in_present = 1;
    openpete_mod_material_selector_t zx = z;
    zx.flags |= OP_SEL_EXCLUSIVE;
    CHECK(mod_mat_set_selector("fx-a", m, &zx) == 0, "parked");
    openpete_mods_in_present = 0;
    mod_mat_apply_pending();
    CHECK(openpete_mods_material_terrain_epoch() == e0 + 1, "parked exclusive-only retune: no bump");
    OUTCOME("an exclusive-only retune at either store site keeps the epoch; a key change bumps it");
}

/** @brief An enable to the state already set leaves the terrain epoch and the table generation; a flip moves both. */
static void case_ter_epoch_enable(void) {
    begin("Terrain epoch: no-op enable");
    stub_mod_t* a = mod("fx-a", 0, 13);
    const int m = reg(a, sel_ter(), SCENE, NULL);
    CHECK(mod_mat_enable(m, 1) == 0, "armed");
    const unsigned e0 = openpete_mods_material_terrain_epoch(), g0 = mod_mat_table_gen();
    CHECK(mod_mat_enable(m, 1) == 0 && openpete_mods_material_terrain_epoch() == e0 &&
          mod_mat_table_gen() == g0, "same state: no bump");
    CHECK(mod_mat_enable(m, 0) == 0 && openpete_mods_material_terrain_epoch() == e0 + 1 &&
          mod_mat_table_gen() != g0, "off: bump");
    const unsigned e1 = openpete_mods_material_terrain_epoch(), g1 = mod_mat_table_gen();
    CHECK(mod_mat_enable(m, 0) == 0 && openpete_mods_material_terrain_epoch() == e1 &&
          mod_mat_table_gen() == g1, "off again: no bump");
    CHECK(mod_mat_enable(m, 1) == 0 && openpete_mods_material_terrain_epoch() == e1 + 1, "on: bump");
    OUTCOME("a no-op enable keeps the terrain epoch and the table generation; an enable flip bumps both");
}

/* ------------------------------------------------------------------ */
/* Draw-time chains. */

/** @brief Register a material of a contract with a fragment handle and its link verdicts. */
static int regc(const stub_mod_t* m, openpete_mod_material_selector_t sel, int frag, unsigned link, int contract) {
    if (!mod_mat_sel_valid(m->id, &sel, m->api)) return -1;
    const int h = mod_mat_append(m->id, &sel, -1, frag, contract, NULL, m->load_index, m->api, 1u);
    if (h >= 0) mod_mat_set_link_of_frag(frag, link, 0);
    return h;
}
/** @brief Fill and publish a stub member set whose shaders have a case in one profile only: case id = list position.
 *
 * @param s The record.
 * @param gen Generation.
 * @param n Shader count.
 * @param shaders The handles.
 * @param profile The profile with cases. */
static void set_publish_profile(openpete_mods_material_chain_set_t* s, unsigned gen, int n, const int* shaders,
                                int profile) {
    memset(s, 0, sizeof *s);
    s->generation = gen;
    s->n = n;
    for (int i = 0; i < n; i++) {
        s->shader[i] = shaders[i];
        for (int p = 0; p < OPENPETE_MATERIAL_CHAIN_PROFILES; p++) s->case_id[p][i] = p == profile ? i : -1;
    }
    openpete_mods_material_chain_set_publish(s);
}
/** @brief Draw-time resolve of a tag: the member count of a chain (filling @p r), 1 for one material, 0 for stock. */
static int dt_of(uint32_t tag, int contract, mod_mat_chain_res_t* r, int* mat) {
    int slot = 0;
    const int m = mod_mat_resolve_index(tag, contract, &slot, r);
    if (mat) *mat = m;
    return m == MOD_MAT_RESOLVE_CHAIN ? r->n : m >= 0 ? 1 : 0;
}

/** @brief Two composable declaratives on the player channel compose at draw time; the outcome is cached per tag and contract; over the cap the highest four stay. */
static void case_dt_chain(void) {
    begin("Draw-time chain: player channel");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = regf(a, sel_ch(OP_CHAN_PLAYER), 10, 1, 0, NULL);
    const int mb = regf(b, sel_ch(OP_CHAN_PLAYER), 11, 1, 0, NULL);
    const float blk[4] = {0, 1, 0, 1};
    CHECK(mod_mat_set_params(mb, blk, sizeof blk) == 0, "params");
    const int sh[5] = {10, 11, 12, 13, 14};
    set_publish(&s_set_a, 1, 5, sh);
    mod_mat_census_force(1);
    openpete_mods_material_frame_reset(1);
    mod_mat_chain_res_t r;
    CHECK(dt_of(chan_tag(OP_CHAN_PLAYER, 0) | (7u << 16), SCENE, &r, NULL) == 2 && r.mat[0] == ma &&
          r.mat[1] == mb && r.entry == OPENPETE_MATERIAL_DT_CHAIN_BASE, "a,b: chain [a b] at the first draw-time entry");
    CHECK(r.slot[1] == mod_mat_view_static_slot(mb) && r.slot[0] == 0, "member slots: static blocks");
    CHECK(dt_of(chan_tag(OP_CHAN_PLAYER, 0) | (9u << 16), SCENE, &r, NULL) == 2 &&
          mod_mat_census_chain(MOD_MAT_CHAIN_DT_WALKS) == 1, "another model record: the cached chain, one walk");
    unsigned char out[OPENPETE_MATERIAL_CHAIN_BLOCK_BYTES];
    CHECK(openpete_mods_material_chain_block(r.entry, 0, NULL, 0, out) == 0, "stock block");
    CHECK(openpete_mods_material_chain_block(r.entry, OPENPETE_MATERIAL_CHAIN_DECAL, NULL, 0, out) == -1,
          "no decal block for a scene chain");
    CHECK(memcmp(out + 512, blk, sizeof blk) == 0, "position 1 holds b's params");
    CHECK(openpete_mods_material_chain_ubo_block(r.entry, 0) ==
              1 + 2 * OPENPETE_MATERIAL_CHAINS + 3 * OPENPETE_MATERIAL_TERRAIN_CHAINS &&
          openpete_mods_material_chain_ubo_block(r.entry, OPENPETE_MATERIAL_CHAIN_TERRAIN) == -1,
          "draw-time blocks follow the terrain entries");
    CHECK(dt_of(chan_tag(OP_CHAN_FLAME, 0), SCENE, &r, NULL) == 0, "flame: no candidate, stock");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_DT_STOCK) == 0, "stock with no candidate is not counted");
    stub_mod_t* c = mod("fx-c", 2, 13);
    stub_mod_t* d = mod("fx-d", 3, 13);
    stub_mod_t* e = mod("fx-e", 4, 13);
    regf(c, sel_ch(OP_CHAN_PLAYER), 12, 1, 0, NULL);
    regf(d, openpete_mod_material_sel_any(), 13, 1, 0, NULL);
    const int me = regf(e, sel_ch(OP_CHAN_PLAYER), 14, 1, 0, NULL);
    CHECK(dt_of(chan_tag(OP_CHAN_PLAYER, 0), SCENE, &r, NULL) == 4 && r.mat[0] == mb && r.mat[3] == me,
          "five members over the cap: the four highest, a channel -1 member among them");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_DRAWS_2 + 2) == 1, "a draw of 4 members");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("player a,b: chain [a b] in the draw-time range, cached across model records; five: [b c d e]");
}

/** @brief A held owner is left out of the draw-time walk, so the draw keeps the outcome it had before the owner entered; matched stock is counted. */
static void case_dt_held(void) {
    begin("Draw-time chain: held owners");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = regf(a, sel_ch(OP_CHAN_PLAYER), 10, 1, 0, NULL);
    regf(b, sel_ch(OP_CHAN_PLAYER), 11, 1, 0, NULL);
    const int sh[2] = {10, 11};
    set_publish(&s_set_a, 1, 2, sh);
    mod_mat_census_force(1);
    mod_mat_chain_res_t r;
    int m = -1;
    const char* top[1] = {"fx-b"};
    openpete_mods_hold_owners(top, 1);
    CHECK(dt_of(chan_tag(OP_CHAN_PLAYER, 0), SCENE, &r, &m) == 1 && m == ma, "b held: a alone, as before b entered");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_DT_HELD) == 1, "held exclusion counted");
    const char* both[2] = {"fx-a", "fx-b"};
    openpete_mods_hold_owners(both, 2);
    CHECK(dt_of(chan_tag(OP_CHAN_PLAYER, 0), SCENE, &r, NULL) == 0, "both held: stock");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_DT_STOCK) == 1, "matched stock counted");
    openpete_mods_hold_owners(NULL, 0);
    CHECK(dt_of(chan_tag(OP_CHAN_PLAYER, 0), SCENE, &r, NULL) == 2, "released: composes");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_DT_HELD) == 3, "held %u", mod_mat_census_chain(MOD_MAT_CHAIN_DT_HELD));
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("top held: the member below alone; all held: stock (counted); released: chain");
}

/** @brief A composable top an installed set lacks is left out while a newer module is pending, else draws alone; below a composable member it is skipped. */
static void case_dt_pending(void) {
    begin("Draw-time chain: a member absent while its module is pending");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = regf(a, sel_ch(OP_CHAN_FLAME), 10, 1, 0, NULL);
    const int mb = regf(b, sel_ch(OP_CHAN_FLAME), 11, 1, 0, NULL);
    const int sh[1] = {10};
    set_publish(&s_set_a, 1, 1, sh);
    mod_mat_census_force(1);
    mod_mat_chain_res_t r;
    int m = -1;
    mod_mat_chain_pending_set(1);
    openpete_mods_material_chain_set_publish(NULL);
    CHECK(dt_of(chan_tag(OP_CHAN_FLAME, 1), SCENE, &r, &m) == 1 && m == mb, "no set installed: the top alone");
    set_publish(&s_set_a, 1, 1, sh);
    CHECK(dt_of(chan_tag(OP_CHAN_FLAME, 1), SCENE, &r, &m) == 1 && m == ma, "pending: b left out, a alone");
    CHECK(mod_mat_census_chain(MOD_MAT_CHAIN_DT_PENDING) == 1, "pending exclusion counted");
    mod_mat_chain_pending_set(0);
    CHECK(dt_of(chan_tag(OP_CHAN_FLAME, 1), SCENE, &r, &m) == 1 && m == mb, "nothing pending: absent b draws alone");
    const int sh2[2] = {10, 11};
    set_publish(&s_set_b, 2, 2, sh2);
    CHECK(dt_of(chan_tag(OP_CHAN_FLAME, 1), SCENE, &r, NULL) == 2, "installed: chain");
    const int sh3[1] = {11};
    set_publish(&s_set_a, 3, 1, sh3);
    CHECK(dt_of(chan_tag(OP_CHAN_FLAME, 1), SCENE, &r, &m) == 1 && m == mb, "a absent below b: skipped, b alone");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("no set: top alone; absent top while pending: left out; not pending: alone; installed: chain; absent below: skipped");
}

/** @brief Terminal, pinned-below-13 and unlinkable members follow the extraction walk's rules at draw time; a retune or enable change voids the cache. */
static void case_dt_rules(void) {
    begin("Draw-time chain: terminal and unlinkable members, cache inputs");
    stub_mod_t* o = mod("fx-old", 0, 12);
    stub_mod_t* a = mod("fx-a", 1, 13);
    stub_mod_t* u = mod("fx-unl", 2, 13);
    const int mo = regf(o, sel_ch(OP_CHAN_SKY), 10, 1, 0, NULL);
    const int ma = regf(a, sel_ch(OP_CHAN_SKY), 11, 1, 0, NULL);
    const int sh[2] = {10, 11};
    set_publish(&s_set_a, 1, 2, sh);
    mod_mat_chain_res_t r;
    int m = -1;
    CHECK(dt_of(chan_tag(OP_CHAN_SKY, 0), SCENE, &r, NULL) == 2 && r.mat[0] == mo, "pin 12 below a: the chain ends on it");
    const int mu = regf(u, sel_ch(OP_CHAN_SKY), 20, 0, 0, NULL);
    CHECK(dt_of(chan_tag(OP_CHAN_SKY, 0), SCENE, &r, &m) == 1 && m == mu, "unlinkable on top: alone");
    mod_mat_enable(mu, 0);
    CHECK(dt_of(chan_tag(OP_CHAN_SKY, 0), SCENE, &r, NULL) == 2, "disabled: the cache follows");
    openpete_mod_material_selector_t ex = sel_ch(OP_CHAN_SKY);
    ex.flags = OP_SEL_EXCLUSIVE;
    CHECK(mod_mat_set_selector("fx-a", ma, &ex) == 0, "retune to exclusive");
    CHECK(dt_of(chan_tag(OP_CHAN_SKY, 0), SCENE, &r, &m) == 1 && m == ma, "exclusive top: alone");
    openpete_mods_material_chain_set_publish(NULL);
    OUTCOME("pin 12 below: ends the chain; unlinkable top: alone; enable and retune void the cached outcome");
}

/** @brief Decal-contract composable declaratives on moby shadows (shadow ident 0) compose in the decal module; scene sites never take them. */
static void case_dt_decal(void) {
    begin("Draw-time chain: decal contract on shadow ident 0");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    openpete_mod_material_selector_t s0 = sel_ch(OP_CHAN_SHADOW);
    s0.ident = 0;
    const int ma = regc(a, s0, 30, 1, DECAL);
    const int mb = regc(b, s0, 31, 1, DECAL);
    const int sh[2] = {30, 31};
    set_publish_profile(&s_set_a, 1, 2, sh, OPENPETE_MATERIAL_CHAIN_DECAL);
    mod_mat_census_force(1);
    mod_mat_chain_res_t r;
    CHECK(dt_of(chan_tag(OP_CHAN_SHADOW, 0), DECAL, &r, NULL) == 2 && r.mat[0] == ma && r.mat[1] == mb,
          "decal a,b: chain");
    unsigned char out[OPENPETE_MATERIAL_CHAIN_BLOCK_BYTES];
    CHECK(openpete_mods_material_chain_block(r.entry, OPENPETE_MATERIAL_CHAIN_DECAL, NULL, 0, out) == 0,
          "decal block");
    CHECK(openpete_mods_material_chain_block(r.entry, 0, NULL, 0, out) == -1, "no stock block for a decal chain");
    CHECK(openpete_mods_material_chain_ubo_block(r.entry, OPENPETE_MATERIAL_CHAIN_DECAL) ==
              openpete_mods_material_chain_ubo_block(r.entry, 0), "the decal profile takes the stock block");
    uint32_t n = 0, ids[2];
    memcpy(&n, out, 4);
    memcpy(ids, out + 16, sizeof ids);
    CHECK(n == 2 && ids[0] == 0 && ids[1] == 1, "decal case ids");
    CHECK(dt_of(chan_tag(OP_CHAN_SHADOW, 0), SCENE, &r, NULL) == 0, "a scene site takes no decal material");
    CHECK(dt_of(chan_tag(OP_CHAN_SHADOW, 1), DECAL, &r, NULL) == 0, "the player shadow (ident 1): none");
    const int sh1[2] = {30, 31};
    set_publish(&s_set_b, 2, 2, sh1);   /* cases in the scene profiles only */
    int m = -1;
    CHECK(dt_of(chan_tag(OP_CHAN_SHADOW, 0), DECAL, &r, &m) == 1 && m == mb, "a set without decal cases: the top alone");
    openpete_mods_material_chain_set_publish(NULL);
    mod_mat_census_force(0);
    OUTCOME("shadow ident 0, decal a,b: chain in the decal profile; scene sites and ident 1 take none");
}

/* ------------------------------------------------------------------ */
/* Overlap diagnostic. */

/** @brief Recompute the overlap pairs when their inputs moved, as the tick boundary does, without the registry lock. */
static void ovl_refresh(void) {
    if (!mod_mat_overlap_due()) return;
    mod_mat_overlap_snapshot();
    mod_mat_overlap_compute();
    mod_mat_overlap_publish();
}
/** @brief Return the published shadow verdict of material @p hi over @p lo after a refresh. */
static int ovl(int hi, int lo) {
    ovl_refresh();
    return mod_mat_overlap_verdict(hi, lo);
}
#define FULL MOD_MAT_SHADOW_FULL
#define PART MOD_MAT_SHADOW_PARTIAL
#define NONE MOD_MAT_SHADOW_NONE

/** @brief The declarative fixtures: mat-fx-pair over mat-fx-broad on flame 0, sky and player; mat-fx-pair's C player row over its manifest row. */
static void case_ovl_fixture_decl(void) {
    begin("Overlap: mat-fx-pair over mat-fx-broad");
    stub_mod_t* a = mod("mat-fx-broad", 0, 13);
    stub_mod_t* b = mod("mat-fx-pair", 1, 13);
    openpete_mod_material_selector_t f0 = sel_ch(OP_CHAN_FLAME);
    f0.ident = 0;
    const int am = reg(a, sel_ch(OP_CHAN_MOBY), SCENE, NULL);
    const int af = reg(a, f0, SCENE, NULL);
    const int as = reg(a, sel_ch(OP_CHAN_SKY), SCENE, NULL);
    const int ap = reg(a, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    const int bp = reg(b, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    const int bf = reg(b, f0, SCENE, NULL);
    const int bs = reg(b, sel_ch(OP_CHAN_SKY), SCENE, NULL);
    const int bc = reg(b, sel_ch(OP_CHAN_PLAYER), SCENE, NULL);
    CHECK(ovl(bf, af) == FULL && ovl(bs, as) == FULL && ovl(bp, ap) == FULL && ovl(bc, ap) == FULL,
          "cross-mod full: flame %d sky %d player %d", ovl(bf, af), ovl(bs, as), ovl(bp, ap));
    CHECK(ovl(bc, bp) == FULL && ovl(bp, bc) == NONE, "same-mod C over manifest row at pin 13");
    CHECK(ovl(bf, am) == NONE && ovl(bs, af) == NONE, "disjoint channels");
    CHECK(mod_mat_overlap_total() == 5, "pairs %d", mod_mat_overlap_total());
    OUTCOME("pair over broad: flame 0, sky, player full; pair C player over its manifest row full; 5 pairs");
}

/** @brief The wild-refine fixtures: a later channel-wide refine over an earlier one is partial; a declining refine stays partial. */
static void case_ovl_fixture_wild(void) {
    begin("Overlap: mat-fx-wildrefine-b over mat-fx-wildrefine-a and mat-fx-classrefine");
    stub_mod_t* c = mod("mat-fx-classrefine", 0, 13);
    stub_mod_t* a = mod("mat-fx-wildrefine-a", 1, 13);
    stub_mod_t* b = mod("mat-fx-wildrefine-b", 2, 13);
    const int mc = reg(c, sel_moby(7), SCENE, rf_accept);
    const int ma = reg(a, sel_moby(-1), SCENE, rf_class7);
    const int mb = reg(b, sel_moby(-1), SCENE, rf_class7);
    CHECK(ovl(mb, ma) == PART && ovl(mb, mc) == PART && ovl(ma, mc) == PART, "before any call");
    openpete_mods_material_frame_reset(1);
    (void)openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);   /* b and a decline class 5 */
    CHECK(mod_mat_overlap_due(), "first decline moves the inputs");
    CHECK(ovl(mb, ma) == PART && ovl(mb, mc) == PART, "after declines");
    OUTCOME("wild-b over wild-a, wild-b over classrefine, wild-a over classrefine: partial before and after calls");
}

/** @brief mat-fx-levelkey: a level-keyed declarative and a later same-mod refine on its class; the pair exists only in the keyed level. */
static void case_ovl_fixture_levelkey(void) {
    begin("Overlap: mat-fx-levelkey");
    stub_mod_t* a = mod("mat-fx-levelkey", 0, 13);
    openpete_mod_material_selector_t d = sel_moby(5);
    d.level = 4;
    const int md = reg(a, d, SCENE, NULL);
    const int mr = reg(a, sel_moby(5), SCENE, rf_accept);
    openpete_mods_set_level(3);
    CHECK(ovl(mr, md) == NONE, "level 3: the declarative matches nothing");
    openpete_mods_set_level(4);
    CHECK(ovl(mr, md) == PART, "level 4, refine not yet asked");
    openpete_mods_material_frame_reset(1);
    (void)openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(ovl(mr, md) == FULL, "level 4, refine asked and accepted");
    OUTCOME("level 3 none; level 4 partial before the refine is asked, full (accept-all) after");
}

/** @brief The terrain fixtures: a later env declarative over a texture refine is partial; a refine over a declarative on its key is full. */
static void case_ovl_fixture_terrain(void) {
    begin("Overlap: mat-fx-terrain-env over mat-fx-terrain");
    stub_mod_t* a = mod("mat-fx-terrain", 0, 13);
    stub_mod_t* b = mod("mat-fx-terrain-env", 1, 13);
    openpete_mod_material_selector_t r = sel_ch(OP_CHAN_TERRAIN), u = sel_ch(OP_CHAN_TERRAIN);
    r.texture = 1;
    u.flags = OP_SEL_UNTEXTURED;
    const int mr = reg(a, r, SCENE, rf_accept);
    const int mu = reg(a, u, SCENE, rf_decline);
    openpete_mod_material_selector_t e = r;
    e.flags = OP_SEL_ENV_ANIM;
    const int me = reg(b, e, SCENE, NULL);
    CHECK(ovl(me, mr) == PART, "env declarative over the texture refine");
    CHECK(ovl(me, mu) == NONE && ovl(mu, mr) == NONE && ovl(mr, mu) == NONE, "band refine shares no key");
    stub_mod_t* c = mod("fx-tr-late", 2, 13);
    const int ml = reg(c, r, SCENE, rf_decline);
    CHECK(ovl(ml, me) == PART && ovl(ml, mr) == PART, "a terrain refine over its key, not yet asked: partial");
    openpete_mods_material_frame_reset(1);
    (void)walk(1, 0, 0);                       /* ml declines texture 1 */
    CHECK(ovl(ml, me) == PART && ovl(ml, mr) == PART, "after its decline: partial");
    OUTCOME("env decl over texture refine partial; band refine none; a later declining texture-1 refine partial over both");

    begin("Overlap: an accept-all terrain refine over a declarative on its key");
    stub_mod_t* d = mod("fx-tdecl", 0, 13);
    stub_mod_t* t = mod("fx-tacc", 1, 13);
    const int md = reg(d, r, SCENE, NULL);
    const int mt = reg(t, r, SCENE, rf_accept);
    CHECK(ovl(mt, md) == PART, "not yet asked");
    openpete_mods_material_frame_reset(1);
    (void)walk(1, 0, 0);
    CHECK(ovl(mt, md) == FULL, "asked and accepted: full (accept-all)");
    OUTCOME("a terrain refine over a declarative on its key: partial until asked, full while it has accepted every call");
}

/** @brief Census pattern: sky-selector's narrow exclusions before its wide fog, at pin 12 and pin 13. */
static void case_ovl_exclude(int api) {
    char name[96];
    snprintf(name, sizeof name, "Overlap: exclusions before the wide material (pin %d)", api);
    begin(name);
    stub_mod_t* a = mod("fx-exclude", 0, api);
    int mx[5];
    for (int i = 0; i < 5; i++) mx[i] = reg(a, sel_moby(83 + i), SCENE, NULL);
    const int mw = reg(a, sel_moby(-1), SCENE, NULL);
    int ok = 1;
    for (int i = 0; i < 5; i++)
        ok &= api >= 13 ? (ovl(mw, mx[i]) == FULL && ovl(mx[i], mw) == NONE)
                        : (ovl(mx[i], mw) == PART && ovl(mw, mx[i]) == NONE);
    CHECK(ok, "pin %d verdicts", api);
    CHECK(mod_mat_overlap_total() == 5, "pairs %d", mod_mat_overlap_total());
    OUTCOME("%s", api >= 13 ? "wide over each exclusion: full (the exclusions never draw)"
                            : "each exclusion over the wide material: partial (one class each)");
}

/** @brief Census pattern: Japanese Extras (a later channel-wide refine accepting its own eggs) over a Portals class-keyed refine. */
static void case_ovl_je_portals(void) {
    begin("Overlap: Japanese Extras over Portals");
    stub_mod_t* p = mod("spyro-portals", 0, 12);
    stub_mod_t* j = mod("spyro-japanese-extras", 1, 11);
    const int mp = reg(p, sel_moby(5), SCENE, rf_accept);
    const int mj = reg(j, sel_moby(-1), SCENE, rf_class7);
    CHECK(ovl(mj, mp) == PART, "before a call");
    openpete_mods_material_frame_reset(1);
    const int s7 = openpete_mods_material_moby_slot(NULL, 7, 0x80010000u);
    CHECK(s7 == 1 && ovl(mj, mp) == FULL, "accepted every call so far: full");
    const int s5 = openpete_mods_material_moby_slot(NULL, 5, 0x80010100u);
    CHECK(s5 != 1 && ovl(mj, mp) == PART, "after its first decline: partial");
    OUTCOME("JE over Portals: partial; full only while JE has accepted every call it was asked; partial from its first decline");
}

/** @brief Census pattern: an accept-all refine (moby-instance-tint) over earlier moby materials is reported full, a declarative on another channel too partial. */
static void case_ovl_accept_all(void) {
    begin("Overlap: accept-all refine");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* t = mod("moby-instance-tint", 1, 5);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mw = reg(a, openpete_mod_material_sel_any(), SCENE, NULL);
    const int mt = reg(t, sel_moby(-1), SCENE, rf_accept);
    CHECK(ovl(mt, ma) == PART, "not yet asked");
    openpete_mods_material_frame_reset(1);
    (void)openpete_mods_material_moby_slot(NULL, 5, 0x80010000u);
    CHECK(ovl(mt, ma) == FULL, "accept-all: full");
    const mod_mat_overlap_t* p = NULL;
    for (int i = 0; i < mod_mat_overlap_total(); i++)
        if (mod_mat_overlap_at(i)->hi == mt && mod_mat_overlap_at(i)->lo == ma) p = mod_mat_overlap_at(i);
    CHECK(p && p->accept_all && p->chans == (1u << OP_CHAN_MOBY), "listed with the accept-all note");
    CHECK(ovl(mt, mw) == PART, "channel -1 declinative keeps its other channels");
    CHECK(!mod_mat_overlap_due(), "a further accepted call moves nothing");
    (void)openpete_mods_material_moby_slot(NULL, 6, 0x80010200u);
    CHECK(!mod_mat_overlap_due(), "second call");
    OUTCOME("tint over the class-5 declarative: partial until asked, then full (accept-all); over a channel -1 declarative partial");
}

/** @brief The inputs: an unchanged retune or enable moves nothing; a changed retune, an enable change, a level change and an owner toggle do. */
static void case_ovl_inputs(void) {
    begin("Overlap: recompute inputs");
    stub_mod_t* a = mod("fx-a", 0, 13);
    stub_mod_t* b = mod("fx-b", 1, 13);
    const int ma = reg(a, sel_moby(5), SCENE, NULL);
    const int mb = reg(b, sel_moby(5), SCENE, NULL);
    CHECK(ovl(mb, ma) == FULL, "full");
    openpete_mod_material_selector_t s5 = sel_moby(5), s6 = sel_moby(6);
    CHECK(mod_mat_set_selector("fx-b", mb, &s5) == 0 && !mod_mat_overlap_due(), "same retune");
    CHECK(mod_mat_enable(mb, 1) == 0 && !mod_mat_overlap_due(), "same enable");
    CHECK(mod_mat_set_selector("fx-b", mb, &s6) == 0 && mod_mat_overlap_due(), "retune");
    CHECK(ovl(mb, ma) == NONE, "retuned away");
    CHECK(mod_mat_set_selector("fx-b", mb, &s5) == 0 && ovl(mb, ma) == FULL, "retuned back");
    CHECK(mod_mat_enable(mb, 0) == 0 && ovl(mb, ma) == NONE, "disabled");
    CHECK(mod_mat_enable(mb, 1) == 0 && ovl(mb, ma) == FULL, "enabled");
    openpete_mods_set_level(2);
    CHECK(mod_mat_overlap_due(), "level change");
    ovl_refresh();
    openpete_mods_in_present = 1;
    CHECK(mod_mat_set_selector("fx-b", mb, &s6) == 0 && !mod_mat_overlap_due(), "parked retune waits");
    openpete_mods_in_present = 0;
    mod_mat_apply_pending();
    CHECK(ovl(mb, ma) == NONE, "applied");
    OUTCOME("same retune or enable: no recompute; retune, enable change, level change, applied parked retune: recompute");
}

/** @brief Composition pairings of the overlap verdict: which higher material shadows a lower one once materials compose. */
static void case_ovl_compose(void) {
    begin("Overlap: composition pairings");
    stub_mod_t* l = mod("fx-lo", 0, 13);
    stub_mod_t* h = mod("fx-hi", 1, 13);
    const int lo = regf(l, sel_moby(5), 10, 1, 0, NULL);                 /* linkable, composable */
    const int hc = regf(h, sel_moby(5), 11, 1, 0, NULL);                 /* linkable, composable */
    CHECK(ovl(hc, lo) == NONE, "composable linkable over linkable: composes, no shadow");
    OUTCOME("composable over linkable: none");

    begin("Overlap: a linkable terminal declarative");
    l = mod("fx-lo", 0, 13);
    stub_mod_t* t = mod("fx-t", 1, 12);
    stub_mod_t* c = mod("fx-c", 2, 13);
    const int lo2 = regf(l, sel_moby(5), 10, 1, 0, NULL);
    const int ht = regf(t, sel_moby(5), 11, 1, 0, NULL);
    CHECK(ovl(ht, lo2) == FULL, "pin 12 linkable over lo: full");
    regf(c, sel_moby(5), 12, 1, 0, NULL);
    CHECK(ovl(ht, lo2) == FULL, "a composable member above it: still full (it joins and ends the chain)");
    OUTCOME("linkable terminal over lo: full, with or without a composable member above");

    begin("Overlap: an unlinkable declarative draws alone only on top");
    l = mod("fx-lo", 0, 13);
    stub_mod_t* u = mod("fx-u", 1, 13);
    c = mod("fx-c", 2, 13);
    const int lo3 = regf(l, sel_moby(5), 10, 1, 0, NULL);
    const int hu = regf(u, sel_moby(5), 20, 0, 0, NULL);
    CHECK(ovl(hu, lo3) == FULL, "unlinkable on top over lo: full");
    const int mc = regf(c, sel_moby(5), 12, 1, 0, NULL);
    CHECK(ovl(hu, lo3) == PART, "a composable linkable member above it: skipped there, partial");
    CHECK(ovl(mc, hu) == FULL && ovl(mc, lo3) == NONE, "composable over unlinkable: full; over linkable lo: none");
    OUTCOME("unlinkable over lo: full alone, partial under a composable member; composable over unlinkable: full");

    begin("Overlap: a vertex-stage declarative");
    l = mod("fx-lo", 0, 13);
    stub_mod_t* v = mod("fx-v", 1, 13);
    c = mod("fx-c", 2, 13);
    const int lo4 = regf(l, sel_moby(5), 10, 1, 0, NULL);
    openpete_mod_material_selector_t s5 = sel_moby(5);
    const int hv = mod_mat_append(v->id, &s5, 40, 11, SCENE, NULL, v->load_index, 13, 1u);
    mod_mat_set_link_of_frag(11, 1, 0);
    CHECK(ovl(hv, lo4) == FULL, "vertex stage on top over lo: full");
    regf(c, sel_moby(5), 12, 1, 0, NULL);
    CHECK(ovl(hv, lo4) == PART, "under a composable member it is dropped above lo: partial");
    OUTCOME("vertex-stage over lo: full on top, partial under a composable member");

    begin("Overlap: refines among composable members");
    l = mod("fx-lo", 0, 13);
    stub_mod_t* r = mod("fx-r", 1, 13);
    stub_mod_t* rt = mod("fx-rt", 2, 12);
    const int lo5 = regf(l, sel_moby(5), 20, 0, 0, NULL);               /* unlinkable */
    const int hr = regf(r, sel_moby(5), 11, 1, 0, rf_accept);          /* composable linkable refine */
    CHECK(ovl(hr, lo5) == PART, "composable refine over unlinkable lo, not yet asked: partial");
    const int lo6 = regf(l, sel_moby(6), 10, 1, 0, NULL);
    const int ht2 = regf(rt, sel_moby(6), 12, 1, 0, rf_decline);       /* terminal refine */
    CHECK(ovl(ht2, lo6) == PART, "terminal refine over lo: partial");
    CHECK(ovl(hr, lo6) == NONE, "disjoint keys");
    OUTCOME("composable refine over unlinkable lo: partial; terminal refine over lo: partial");
}

int main(void) {
    case_f1();
    case_f1a_level();
    case_f1a_decal();
    case_f2();
    case_f3();
    case_terrain_decline();
    case_env_static();
    case_terrain_fallthrough();
    case_terrain_pass_bound();
    case_f4();
    case_hp_tier_moby();
    case_textured_moby();
    case_part_sel_reserved("spyro-1");
    case_part_sel_reserved("spyro-2");
    case_part_sel_reserved("spyro-3");
    case_f3a();
    case_d3a();
    case_f3b_view_level();
    case_f3b_terrain_refine();
    case_refine_present_outranked();
    case_host_loop();
    case_level_change_bake();
    case_postfx_cap();
    case_exclude(12);
    case_exclude(13);
    case_accept_all();
    case_level_retune();
    case_cross_mod_decl();
    case_imported_sector();
    case_walk_chain();
    case_landing_state();
    case_moby_predicate();
    case_tick_memo();
    case_slot_table_full();
    case_chain_compose();
    case_chain_terminal();
    case_chain_unlinkable();
    case_chain_vertex();
    case_chain_absent();
    case_chain_generation();
    case_chain_held();
    case_chain_table_full();
    case_ter_class_list();
    case_ter_chain_resolve();
    case_ter_chain_refine_terminal();
    case_ter_chain_profiles();
    case_ter_chain_fallback();
    case_ter_chain_table_full();
    case_ter_epoch_skip();
    case_ter_epoch_enable();
    case_dt_chain();
    case_dt_held();
    case_dt_pending();
    case_dt_rules();
    case_dt_decal();
    case_ovl_fixture_decl();
    case_ovl_fixture_wild();
    case_ovl_fixture_levelkey();
    case_ovl_fixture_terrain();
    case_ovl_exclude(12);
    case_ovl_exclude(13);
    case_ovl_je_portals();
    case_ovl_accept_all();
    case_ovl_inputs();
    case_ovl_compose();
    CHECK(s_split == 1, "tag split armed while materials are live");
    printf("%s: %d checks\n", s_fail ? "FAIL" : "PASS", s_checks);
    return s_fail;
}
