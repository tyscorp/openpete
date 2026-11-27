// tests/unit/mod_modset_test.c - the modset tags (core/mods/mod_modset.c):
// the set form byte-identical to the order-free legacy tag, the ordered
// form and its order hash (entry order, deferred entry, hot reload, late
// links, id boundaries), measuring and truncation, and the savestate
// verdict matrix of a stored tag against the running modset.

#include "openpete_mods.h"

#include <stdio.h>
#include <string.h>

static int s_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        s_fail++; \
    } \
} while (0)

static char s_set[256], s_ord[256];

#define EV(k, id) {OPENPETE_MODSET_EV_##k, id, 0, 0}

/** @brief Format both tags of @p n members, the ordered one over the hash of @p ne events. */
static void tags(const openpete_modset_member_t* m, int n,
                 const openpete_modset_event_t* ev, int ne) {
    openpete_modset_format_set(s_set, sizeof s_set, m, n);
    openpete_modset_format_order(s_ord, sizeof s_ord, m, n,
                                 openpete_modset_order_hash(ev, ne));
}

/** @brief Format the ordered tag of members a@1 and b@2 over @p ev into @p out. */
static void ab_tag(char* out, size_t cap, const openpete_modset_event_t* ev,
                   int ne) {
    const openpete_modset_member_t m[] = {{"a", "1"}, {"b", "2"}};
    openpete_modset_format_order(out, cap, m, 2,
                                 openpete_modset_order_hash(ev, ne));
}

/** @brief Vanilla is the empty string in both forms and matches only vanilla. */
static void test_vanilla(void) {
    tags(NULL, 0, NULL, 0);
    CHECK(strcmp(s_set, "") == 0);
    CHECK(strcmp(s_ord, "") == 0);
    CHECK(openpete_modset_compare("", "", "") == OPENPETE_MODSET_SET_MATCH);
    CHECK(openpete_modset_compare(NULL, NULL, NULL) == OPENPETE_MODSET_SET_MATCH);
    CHECK(openpete_modset_compare("", "v2:a@1#0000000000000001", "a@1") ==
          OPENPETE_MODSET_SET_DIFFERS);
    CHECK(openpete_modset_compare("v2:a@1#0000000000000001", "", "") ==
          OPENPETE_MODSET_SET_DIFFERS);
}

/** @brief The set form sorts by id ("a" before "a-b"), as the legacy tag did, and the ordered form wraps it with the prefix and the hash. */
static void test_forms(void) {
    const openpete_modset_member_t m[] = {
        {"b", "2.0"}, {"a-b", "1"}, {"a", "1.0"}};
    tags(m, 3, NULL, 0);
    CHECK(strcmp(s_set, "a@1.0+a-b@1+b@2.0") == 0);
    /* FNV-1a 64 offset basis: the hash of no events. */
    CHECK(strcmp(s_ord, "v2:a@1.0+a-b@1+b@2.0#cbf29ce484222325") == 0);
}

/** @brief The same set entered in another order, or with a deferred entry, a hot reload or a late link, gives the same set tag and another ordered tag. */
static void test_order(void) {
    const openpete_modset_event_t ab[] = {EV(ENTRY, "a"), EV(ENTRY, "b")};
    const openpete_modset_event_t ba[] = {EV(ENTRY, "b"), EV(ENTRY, "a")};
    /* b deferred: a at boot, b entered later, after a's late link. */
    const openpete_modset_event_t late[] = {
        EV(ENTRY, "a"), {OPENPETE_MODSET_EV_LINK, "a", 0x80012340u, 0},
        EV(ENTRY, "b")};
    const openpete_modset_event_t late2[] = {
        EV(ENTRY, "a"), {OPENPETE_MODSET_EV_LINK, "a", 0x80012344u, 0},
        EV(ENTRY, "b")};
    const openpete_modset_event_t late3[] = {
        EV(ENTRY, "a"), {OPENPETE_MODSET_EV_LINK, "a", 0x80012340u, 3},
        EV(ENTRY, "b")};
    /* a hot-reloaded after b's boot entry. */
    const openpete_modset_event_t reload[] = {
        EV(ENTRY, "a"), EV(ENTRY, "b"), EV(STRIP, "a"), EV(ENTRY, "a")};
    /* Id boundaries: "ab"+"c" and "a"+"bc" differ. */
    const openpete_modset_event_t idb1[] = {EV(ENTRY, "ab"), EV(ENTRY, "c")};
    const openpete_modset_event_t idb2[] = {EV(ENTRY, "a"), EV(ENTRY, "bc")};
    const openpete_modset_event_t* all[] = {ab, ba, late, late2, late3, reload};
    const int alln[] = {2, 2, 3, 3, 3, 4};
    char t[6][256];
    for (int i = 0; i < 6; i++) ab_tag(t[i], sizeof t[i], all[i], alln[i]);
    for (int i = 0; i < 6; i++)
        for (int k = i + 1; k < 6; k++) CHECK(strcmp(t[i], t[k]) != 0);
    for (int i = 0; i < 6; i++) {
        CHECK(strncmp(t[i], "v2:a@1+b@2#", 11) == 0);
        CHECK(strlen(t[i]) == 11 + 16);
    }
    /* Deterministic: the same events give the same tag. */
    char again[256];
    ab_tag(again, sizeof again, late, 3);
    CHECK(strcmp(again, t[2]) == 0);
    CHECK(openpete_modset_order_hash(idb1, 2) !=
          openpete_modset_order_hash(idb2, 2));
    /* Every pair is the same set in another order. */
    CHECK(openpete_modset_compare(t[1], t[0], "a@1+b@2") ==
          OPENPETE_MODSET_ORDER_DIFFERS);
    CHECK(openpete_modset_compare(t[5], t[2], "a@1+b@2") ==
          OPENPETE_MODSET_ORDER_DIFFERS);
}

/** @brief A short buffer is truncated and NUL-terminated, and the full length is returned. */
static void test_measure(void) {
    const openpete_modset_member_t m[] = {{"abc", "1.0"}};
    char small[6];
    CHECK(openpete_modset_format_order(NULL, 0, m, 1, 0) ==
          strlen("v2:abc@1.0#0000000000000000"));
    CHECK(openpete_modset_format_set(NULL, 0, m, 1) == strlen("abc@1.0"));
    CHECK(openpete_modset_format_order(small, sizeof small, m, 1, 0) == 27);
    CHECK(strcmp(small, "v2:ab") == 0);
}

/** @brief The savestate matrix: an ordered tag loads only when equal and names set or order; a set-form tag compares as a set. */
static void test_compare(void) {
    const char* now_ord = "v2:a@1+b@2#00000000000000aa";
    const char* now_set = "a@1+b@2";
    CHECK(openpete_modset_compare("v2:a@1+b@2#00000000000000aa", now_ord,
                                  now_set) == OPENPETE_MODSET_MATCH);
    CHECK(openpete_modset_compare("v2:a@1+b@2#00000000000000bb", now_ord,
                                  now_set) == OPENPETE_MODSET_ORDER_DIFFERS);
    CHECK(openpete_modset_compare("v2:a@1+b@3#00000000000000aa", now_ord,
                                  now_set) == OPENPETE_MODSET_SET_DIFFERS);
    CHECK(openpete_modset_compare("v2:a@1#00000000000000aa", now_ord,
                                  now_set) == OPENPETE_MODSET_SET_DIFFERS);
    CHECK(openpete_modset_compare("v2:a@1+b@2+c@3#00000000000000aa", now_ord,
                                  now_set) == OPENPETE_MODSET_SET_DIFFERS);
    /* Set-form (pre-order) files: the set decides, under any order. */
    CHECK(openpete_modset_compare("a@1+b@2", now_ord, now_set) ==
          OPENPETE_MODSET_SET_MATCH);
    CHECK(openpete_modset_compare("a@1+b@2", "v2:a@1+b@2#00000000000000bb",
                                  now_set) == OPENPETE_MODSET_SET_MATCH);
    CHECK(openpete_modset_compare("a@1", now_ord, now_set) ==
          OPENPETE_MODSET_SET_DIFFERS);
    /* Malformed ordered tags never match. */
    CHECK(openpete_modset_compare("v2:a@1+b@2", now_ord, now_set) ==
          OPENPETE_MODSET_SET_DIFFERS);
    CHECK(openpete_modset_compare("v2:#00000000000000aa", now_ord, now_set) ==
          OPENPETE_MODSET_SET_DIFFERS);
    CHECK(openpete_modset_compare("v2:", now_ord, now_set) ==
          OPENPETE_MODSET_SET_DIFFERS);
}

int main(void) {
    test_vanilla();
    test_forms();
    test_order();
    test_measure();
    test_compare();
    if (s_fail) {
        printf("mod_modset_test: %d failure(s)\n", s_fail);
        return 1;
    }
    printf("mod_modset_test: PASS\n");
    return 0;
}
