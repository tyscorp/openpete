/* tests/unit/mod_save_block_test.c - the key's block a savestate carries
 * (core/mods/mod_save.c) against crafted game-extra sections: a block whose
 * legacy lengths, key or key slot and index lie outside their ranges is
 * refused at parse (key unknown, nothing copied, the game's payload still
 * split off), and a well-formed block restores every field and saves back
 * to the same bytes. The module is included whole so the cases read its
 * block layout; the host services it calls are stubs. */

#include "../../core/mods/mod_save.c"

#include <stddef.h>

int      openpete_speculative = 0;
unsigned openpete_frame = 0u;

int op_atomic_write_file(const char* path, const void* data, size_t n) { (void)path; (void)data; (void)n; return -1; }
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
/* No save descriptor: the game writes synchronously, so a section has no pending trailer. */
static game_mod_desc_t s_game;
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

#define CHECK(cond, msg) do {                                             \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); s_fail = 1; } \
} while (0)

/** @brief A game-extra section: 16 payload bytes then @p blk with no rows and its trailer.
 * @param out Receives the section; holds 16 + sizeof(ms_block_t) + 8 bytes.
 * @param blk The block header written into it.
 * @return The section length. */
static uint32_t section(uint8_t* out, const ms_block_t* blk) {
    const uint32_t len = (uint32_t)sizeof *blk, magic = MS_BLOCK_MAGIC;
    memset(out, 0xA5, 16);
    memcpy(out + 16, blk, sizeof *blk);
    memcpy(out + 16 + len, &len, 4);
    memcpy(out + 16 + len + 4, &magic, 4);
    return 16u + len + 8u;
}

/** @brief A well-formed block: key set at card slot 1 save 3, a 5-byte legacy copy, a full pending legacy copy. */
static ms_block_t valid_block(void) {
    ms_block_t b;
    memset(&b, 0, sizeof b);
    b.version = MS_BLOCK_VER;
    b.key = KEY_SET;
    b.key_slot = 1;
    b.key_idx = MS_MAX_IDX - 1;
    b.pload = 1;
    for (int i = 0; i < 16; i++) b.lineage[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 32; i++) b.hash[i] = (uint8_t)(0x40 + i);
    b.gen = 7;
    b.epoch = 9;
    b.legacy_len = 5;
    memcpy(b.legacy, "\x01\x02\x03\x04\x05", 5);
    b.pload_cands = 0x30u;
    b.pload_legacy_len = OPENPETE_MODSAVE_LEGACY_MAX;
    memset(b.pload_legacy, 0x77, sizeof b.pload_legacy);
    b.dedupe_epoch = 10;
    return b;
}

/** @brief Load a section holding @p blk and require it refused: key unknown, legacy copy empty, the payload split off, the mod status in range. */
static void expect_refused(const ms_block_t* blk, const char* what) {
    static uint8_t sec[16 + sizeof(ms_block_t) + 8];
    s_blk = valid_block();
    const uint32_t n = section(sec, blk);
    const uint32_t own = openpete_modsave_block_load(sec, n);
    if (own != 16u) { fprintf(stderr, "FAIL %s: payload length %u, want 16\n", what, own); s_fail = 1; }
    if (s_blk.key != KEY_UNKNOWN || s_blk.legacy_len != 0 || s_blk.pload != 0) {
        fprintf(stderr, "FAIL %s: block accepted (key %u legacy_len %u pload %u)\n", what,
                s_blk.key, s_blk.legacy_len, s_blk.pload);
        s_fail = 1;
    }
    ms_reg_t g;
    memset(&g, 0, sizeof g);
    openpete_mod_save_read_t r;
    status_for(&g, &r);
    if (r.size > OPENPETE_MODSAVE_LEGACY_MAX) {
        fprintf(stderr, "FAIL %s: status hands a mod %u legacy bytes\n", what, r.size);
        s_fail = 1;
    }
}

int main(void) {
    /* A well-formed block restores every field and saves back byte for byte. */
    {
        static uint8_t sec[16 + sizeof(ms_block_t) + 8];
        const ms_block_t in = valid_block();
        const uint32_t n = section(sec, &in);
        memset(&s_blk, 0, sizeof s_blk);
        CHECK(openpete_modsave_block_load(sec, n) == 16u, "valid block: payload length");
        CHECK(!memcmp(&s_blk, &in, sizeof in), "valid block: restored fields differ");
        CHECK(openpete_modsave_block_size() == sizeof in + 8u, "valid block: saved size");
        static uint8_t out[sizeof(ms_block_t) + 8];
        openpete_modsave_block_save(out);
        CHECK(!memcmp(out, sec + 16, sizeof out), "valid block: save differs from the loaded bytes");
        ms_reg_t g;
        memset(&g, 0, sizeof g);
        openpete_mod_save_read_t r;
        status_for(&g, &r);
        CHECK(r.status == OP_MOD_SAVE_LEGACY_PAD && r.size == 5u && r.save_index == MS_MAX_IDX - 1u,
              "valid block: legacy status");
    }
    {
        ms_block_t b = valid_block();
        b.legacy_len = 0xFFFFFFF0u;
        expect_refused(&b, "legacy_len 0xFFFFFFF0");
        b.legacy_len = OPENPETE_MODSAVE_LEGACY_MAX + 1u;
        expect_refused(&b, "legacy_len max + 1");
    }
    {
        ms_block_t b = valid_block();
        b.pload_legacy_len = 0xFFFFFFF0u;
        expect_refused(&b, "pload_legacy_len 0xFFFFFFF0");
    }
    {
        ms_block_t b = valid_block();
        b.key = KEY_SET + 1;
        expect_refused(&b, "key past KEY_SET");
    }
    {
        ms_block_t b = valid_block();
        b.key_slot = MS_SLOTS;
        expect_refused(&b, "key_slot out of range");
    }
    {
        ms_block_t b = valid_block();
        b.key_idx = MS_MAX_IDX;
        expect_refused(&b, "key_idx out of range");
    }
    {
        ms_block_t b = valid_block();
        b.pload = 2;
        expect_refused(&b, "pload not 0 or 1");
    }
    if (s_fail) return 1;
    printf("PASS mod_save_block_test\n");
    return 0;
}
