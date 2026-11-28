/* core/mods/mod_bus.h - the bus table, its membership snapshot and the key
 * table (mod_bus.c), and the loader's per-mod reads they rely on
 * (mod_loader.c). Engine-internal: the mod-facing entries are
 * openpete_mod_api_t's bus_* members, which the loader's gates route here. */
#ifndef OPENPETE_MOD_BUS_H
#define OPENPETE_MOD_BUS_H

#include <stdint.h>
#include "openpete_mod_api.h"

/* ---- read by mod_bus.c, defined by the loader ----------------------- */

/** @brief Return 1 while a mod is enabled; an atomic read, legal from any thread.
 *
 * @param m the mod.
 * @return 1 or 0. */
int mod_owner_enabled(const openpete_mod_t* m);
/** @brief Return 1 once the exit pass has closed a mod's api or the whole api.
 *
 * @param m the mod.
 * @return 1 or 0. */
int mod_owner_closed(const openpete_mod_t* m);
/** @brief Return a mod's retirement count: the number of times its registrations were stripped.
 *
 * An atomic read. A snapshot row stores the count it was built under, so a
 * walker tells a handler whose object was retired since from a live one.
 * @param m the mod.
 * @return the count. */
uint32_t mod_owner_retired(const openpete_mod_t* m);
/** @brief Return a mod's key, or 0 for a keyless mod.
 *
 * @param m the mod.
 * @return the key. */
uint32_t mod_owner_key(const openpete_mod_t* m);
/** @brief Return 1 for a mod whose manifest declares sim_mutative = false.
 *
 * @param m the mod.
 * @return 1 or 0. */
int mod_owner_sim_neutral(const openpete_mod_t* m);
/** @brief Return a mod's log tag, "mod:<id>".
 *
 * @param m the mod.
 * @return the tag. */
const char* mod_owner_tag(const openpete_mod_t* m);

/* ---- the key table --------------------------------------------------- */

/** @brief Add one discovered mod's key and id to the key table, before it is sealed.
 *
 * The id is copied and interned for the process.
 * @param id the manifest id.
 * @param key its key, nonzero and unique among the added rows.
 * @return the interned id, or NULL on an allocation failure or after the
 *         seal. */
const char* openpete_mods_keys_add(const char* id, uint32_t key);
/** @brief Seal the key table: from here it is immutable and readable from any thread. */
void openpete_mods_keys_seal(void);
/** @brief Return the interned id of a key, or NULL for an unknown key or before the seal.
 *
 * @param key the key.
 * @return the id, or NULL. */
const char* openpete_mods_key_id(uint32_t key);

/* ---- the bus table (sim thread, or the main thread at boot) --------- */

/** @brief Remove every handler a mod attached; the caller publishes afterwards.
 * @param m the mod. */
void openpete_mods_bus_remove_owner(const openpete_mod_t* m);
/** @brief Build and publish a new membership snapshot from the bus table and each owner's enabled state.
 *
 * The previous snapshot is kept for the process: a walker or an any-thread
 * reader may hold it. */
void openpete_mods_bus_publish(void);
/** @brief Log the walker's handler-run counters at exit, when any handler ran. */
void openpete_mods_bus_report(void);
/** @brief Count a tick boundary reached while a bus frame is live on the sim thread, and record its tick.
 * @param tick the canonical tick of the boundary. */
void openpete_mods_bus_note_boundary(unsigned tick);

/* ---- the api entries (loader gates call these) ----------------------- */

/** @brief Attach the caller's handler to a bus (bus_attach).
 *
 * @param self the caller.
 * @param bus the bus name.
 * @param fn the handler.
 * @param user its user pointer.
 * @return 0, or -1 when refused. */
int openpete_mods_bus_attach(openpete_mod_t* self, const char* bus,
                             openpete_mod_bus_fn fn, void* user);
/** @brief Walk a bus's enabled handlers in walk order (bus_call).
 *
 * @param self the caller.
 * @param cpu its CPU state.
 * @param bus the bus name.
 * @param msg_addr the message address.
 * @param size the message length.
 * @return the walk's result. */
int openpete_mods_bus_call(openpete_mod_t* self, CPUState* cpu, const char* bus,
                           uint32_t msg_addr, uint32_t size);
/** @brief Run one mod's handler on a bus (bus_call_to).
 *
 * @param self the caller.
 * @param cpu its CPU state.
 * @param bus the bus name.
 * @param key the target's key.
 * @param msg_addr the message address.
 * @param size the message length.
 * @return the handler's result. */
int openpete_mods_bus_call_to(openpete_mod_t* self, CPUState* cpu,
                              const char* bus, uint32_t key, uint32_t msg_addr,
                              uint32_t size);
/** @brief List a bus's enabled members from the published snapshot (bus_members).
 *
 * @param self the caller.
 * @param bus the bus name.
 * @param out the records.
 * @param cap their count.
 * @return the member count, or -1. */
int openpete_mods_bus_members(openpete_mod_t* self, const char* bus,
                              openpete_mod_bus_sender_t* out, int cap);
/** @brief Return the caller's key (mod_key).
 *
 * @param self the caller.
 * @return its key, or 0 for a keyless mod. */
uint32_t openpete_mods_bus_mod_key(openpete_mod_t* self);
/** @brief Fill the sender record of a key's mod from the key table (mod_info).
 *
 * @param self the caller.
 * @param key the key.
 * @param out the record.
 * @return 0, or -1. */
int openpete_mods_bus_mod_info(openpete_mod_t* self, uint32_t key,
                               openpete_mod_bus_sender_t* out);

#endif /* OPENPETE_MOD_BUS_H */
