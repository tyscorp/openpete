/* tracers.h : the tracer feeder's export. */
#pragma once
#include <stdint.h>
#include "psx_runtime_internal.h"
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/** @brief Re-feeds the tick's captured ribbon and mesh primitives on a sub-tick present, rebased onto the pose it shows.
 *
 * The primitives are the guest's own output and are gone once the polybuf
 * is reused, so the replay re-feeds the held set at the depths the
 * canonical tick paired, each corner moved by the screen displacement of
 * its source point between the tick's camera and records and the staged
 * camera and interpolated records. At alpha 1, and on a present outside a
 * bracket, it emits the captured corners verbatim and reproduces the
 * canonical present. A held camera cut shows tick N-1's set
 * (cut_held_sets_enter) verbatim, since the staged camera is then tick
 * N-1's. A no-op until hook_reextract_ready_held(RP_TRACER, ...).
 * @param cpu The guest CPU state.
 * @param bracketed Nonzero on a present inside a reextract bracket.
 * @param alpha The present's position in the tick interval, 0..1.
 * @param hold Nonzero on a held camera cut. */
void tracers_reextract(CPUState* cpu, int bracketed, double alpha, int hold) OP_REQUIRES(sim);

/* The per-object store key of a mesh vertex: the tag over the vertex's
 * offset in the 8 MiB KSEG0 window. The tag lies outside KSEG0 (0x8xxxxxxx,
 * every record keyed by its guest address) and the credits and tally role
 * keys (0x6xxxxxxx, 0x70xxxxxx, 0x71xxxxxx), so no vertex capture shares an
 * entry with a moby or any other record. */
#define TRACERS_MESH_KEY_TAG  0x50000000u
#define TRACERS_MESH_KEY_MASK 0x007FFFFFu

/** @brief Returns the per-object store key of the mesh vertex at @p va.
 *
 * @param va The vertex's KSEG0 guest address.
 * @return TRACERS_MESH_KEY_TAG over the address's offset in KSEG0. */
uint32_t tracers_mesh_vertex_key(uint32_t va);

/** @brief Copies out the mesh vertex lists the most recent VBL with a mesh draw read.
 *
 * The per-object capture keys each vertex by tracers_mesh_vertex_key, so the
 * sub-tick replay has a previous position to rebase every mesh corner onto.
 * The set is empty once a canonical render after the one that read it has
 * closed with no mesh call, so a list the game no longer draws (one a level
 * load freed) is never captured.
 * @param addr Receives each list's guest address.
 * @param nverts Receives each list's vertex count.
 * @param max The capacity of addr and nverts.
 * @return The count of lists written. */
int tracers_mesh_lists(uint32_t* addr, uint32_t* nverts, int max) OP_REQUIRES(sim);
