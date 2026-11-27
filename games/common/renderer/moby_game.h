/* renderer/moby_game.h - the per-game half of the shared moby feeder.
 *
 * The walk in renderer/moby.c owns everything the games share: the cull-record
 * iteration, the model->view projection and its near fixup, the OT/depth
 * policy, the keyframe blend, the faceblock-patch apply/restore, the tri/quad
 * fan-out, the face-decode cache shell, the emit and the sub-tick shape and
 * rotation interpolation. Every engine address it reads is a game_layout.moby
 * field.
 *
 * What it cannot share is the bit layout of the two streams the models are
 * encoded in and the shape of one cull record, so those three decoders are
 * function slots here, plus a pair for the simple models a game draws apart
 * (a static geometry list drawn by its own pass, canonically and on a sub-tick
 * replay) and one for the object identity the sub-tick interp ring keys on. The remaining slots are per-game
 * data with no engine address behind it - the HUD/cutscene classifications and
 * the one-shot instance table - and the addresses of scratch windows and
 * material assets a game keeps outside the descriptor. A NULL function slot or
 * a zero address makes the walk skip the path that would read it; a game with
 * no moby feeder names no table and binds the all-NULL one. */
#ifndef OPENPETE_COMMON_RENDERER_MOBY_GAME_H
#define OPENPETE_COMMON_RENDERER_MOBY_GAME_H

#include <stdint.h>
#include <string.h>
#include "psx_runtime_internal.h"   /* CPUState: the decoders read guest memory */
#include "renderer/moby_rank.h"      /* the paint-key rows the providers name */
#include "openpete_thread_roles.h"   /* OP_SLOT */

#ifdef __cplusplus
extern "C" {
#endif

/* One model's vertex stream, mid-decode. The walk owns the loop and the
 * per-vertex projection; the game owns what one step of the stream means.
 *
 * VX/VY/VZ is the current model-space vertex. `s5` is the per-model shift the
 * header carries. `base`/`base_va`/`len` are a bounded host alias of the
 * stream's mapping, so a stream inside the aliased window costs no guest
 * access per vertex; `flag` and the four scratch words are the decoder's own
 * state (which form the next item takes, cursors into a second stream, a
 * per-model static vertex base). */
typedef struct moby_vstream {
    uint32_t       litp, shortp;
    const uint8_t* base;
    uint32_t       base_va, len;
    int            VX, VY, VZ, flag, s5;
    uint32_t       scratch_u[4];
    int            scratch_i[4];
} moby_vstream_t;

/* Bounded reads through a vstream's host alias: inside [base_va, base_va+len)
 * the load goes straight to host memory, anything else takes the guest path. */
/** @brief Read one guest word through a vstream's host alias.
 *
 * @param cpu The guest CPU state, read only.
 * @param s The stream, whose alias bounds the direct read.
 * @param va The guest address.
 * @return The word. */
static inline uint32_t moby_vs_read_word(CPUState* cpu, const moby_vstream_t* s, uint32_t va) {
    if (s->base && va >= s->base_va && va - s->base_va + 4u <= s->len) {
        uint32_t w; memcpy(&w, s->base + (va - s->base_va), 4); return w;
    }
    return cpu->read_word(va);
}
/** @brief Read one guest halfword through a vstream's host alias.
 *
 * Same window test as moby_vs_read_word; the value is zero-extended.
 * @param cpu The guest CPU state, read only.
 * @param s The stream, whose alias bounds the direct read.
 * @param va The guest address.
 * @return The unsigned halfword. */
static inline uint32_t moby_vs_read_half(CPUState* cpu, const moby_vstream_t* s, uint32_t va) {
    if (s->base && va >= s->base_va && va - s->base_va + 2u <= s->len) {
        uint16_t h; memcpy(&h, s->base + (va - s->base_va), 2); return h;
    }
    return cpu->read_half(va);
}

/* One decoded face, the walk's unit of topology and material. i0..i3 are
 * stream vertex indices, c0..c3 byte offsets into the model's colour block;
 * `fresh` marks a face that carries its own three (or four) indices against a
 * strip face that advances one, `bit0` the winding flag the NCLIP test reads,
 * `hl` the per-face special-material flag, `blendbit` the GPU
 * semi-transparency enable, `abr` its blend mode and `otbias` the signed
 * per-face ordering-table bias field the draw applies to the face's depth key
 * (each game's walk defines the arithmetic).
 * `untested` is the face record's sign bit on its own: a draw that emits every
 * face textured still carries that bit, and reads it as the face's exemption
 * from the back-face test rather than as a texture flag. tpx/tpy/clx/cly/bpp and
 * u[]/v[] are the face's texture page, CLUT and texel corners.
 *
 * n1/n2/n3 is the signed face normal a special-material face carries in the
 * same word the colour indices occupy, for the games whose special treatment
 * reinterprets that word; 0 on every other face. */
typedef struct moby_wface {
    uint16_t i0, i1, i2, i3;
    uint16_t c0, c1, c2, c3;
    uint8_t  fresh, bit0, textured, hl;
    uint8_t  untested;
    uint8_t  blendbit, abr, bpp;
    int8_t   otbias;
    int8_t   n1, n2, n3;
    int16_t  tpx, tpy, clx, cly;
    uint8_t  u[4], v[4];
} moby_wface_t;

/* The cull record a special face belongs to, as the walk holds it: the
 * record's guest address (0 for a record the walk built itself, which has
 * none), its flags word, its AnimationHeader, the Z of its view-space
 * translation as the cull stores it, and that Z at full precision, the
 * centre the record projects through at this present (the stored Z where the
 * record has no such centre). */
typedef struct moby_face_rec {
    uint32_t addr;
    uint32_t w0;
    uint32_t header;
    int32_t  tr_z;
    float    tr_zf;
} moby_face_rec_t;

/* A texture page and CLUT one face draws from instead of its own, in the same
 * decoded form as the face's fields. `set` 0 leaves the face's texture alone;
 * a treatment that sets it also names the colour depth and blend mode of the
 * substitute page. */
typedef struct moby_face_tex {
    int16_t tpx, tpy, clx, cly;
    uint8_t bpp, abr, set;
    /* Texture coordinates the treatment generates for the face's four corners,
     * in place of the ones the face record carries, in texels, unrounded.
     * `uv_set` 0 keeps the face's own. */
    float   u[4], v[4];
    uint8_t uv_set;
} moby_face_tex_t;

/* The water-plane split one record's draw runs (water_plane.h). Each model
 * vertex V's signed distance is plane - (row . V) / 4096 * unit: `row` is a
 * row of the moby's model-to-world rotation (Q12) and `unit` the model unit in
 * the plane's units. The negative side is depth-cued toward `fc` (the three
 * GTE far-colour control words) at `ir0`, or dropped when `drop_below`. */
typedef struct moby_wplane {
    double  plane;
    double  row[3];
    double  unit;
    int32_t fc[3];
    int32_t ir0;
    int     drop_below;
} moby_wplane_t;

/* What a face-stream decode reports about the stream as a whole: the total
 * records it walked, how many it skipped without emitting, and whether the
 * cursor ended on the bytecount (0), on the generous end bound (1), or
 * somewhere else, which means the per-record advance desynced (2). */
typedef struct moby_face_stats {
    int raw, sprites, skips;
    int align;
} moby_face_stats_t;

/* The fields of one cull record the walk consumes. `tier` is the game's own
 * per-record LOD verdict (negative = the low-poly band); `material` is the
 * per-moby material word a game may carry there, 0 where it has none. */
typedef struct moby_record {
    uint32_t header;        /* AnimationHeader* */
    uint32_t header_b;      /* keyframe B's own AnimationHeader* (== header where
                             * the game blends within one animation) */
    uint32_t frame_a;       /* keyframe A */
    uint32_t frame_b;       /* keyframe B (0 = no blend) */
    uint32_t w0;            /* the record's flags word */
    uint32_t moby;          /* the Moby* the record was built from */
    uint32_t material;      /* per-moby material/colour word */
    int      TR_raw[3];     /* view-space translation, as stored */
    int      M[3][3];       /* model->view rotation, 1.0 = 4096 */
    int      tier;          /* LOD/colour-fade verdict */
} moby_record_t;

/* The two model kinds a reveal can build a record for, one per draw pass. A
 * game with only one kind is offered candidates for that kind alone. */
typedef enum moby_reveal_kind {
    MOBY_REVEAL_ANIMATED = 0,   /* the keyframe-animated models, hook_draw_regular */
    MOBY_REVEAL_SIMPLE   = 1,   /* the second, static geometry kind, hook_draw_simple */
} moby_reveal_kind_t;

/* Why a moby the cull's distance box rejected produced no record. The
 * candidate walk tallies these per pass on its census line, so a class of
 * moby that stays absent under extended LOD names the gate that dropped it. */
typedef enum moby_reveal_reason {
    MOBY_REVEAL_OK = 0,         /* a record was appended */
    MOBY_REVEAL_NO_MODEL,       /* the moby's class names no model-table entry */
    MOBY_REVEAL_OTHER_KIND,     /* the model is the other kind, drawn by the other pass */
    MOBY_REVEAL_DUPLICATE,      /* the cull already queued a record for this moby */
    MOBY_REVEAL_NO_POSE,        /* the model's animation state names no keyframe */
    MOBY_REVEAL_OTHER_FORM,     /* the cull composes this moby's record a way the builder does not */
    MOBY_REVEAL_QUEUE_FULL,
    MOBY_REVEAL_REASONS
} moby_reveal_reason_t;

/* The widened-view verdict on one animated moby the cull left without a
 * record (drawn-verdict byte 0, or 1 with no record): the first admission
 * test it fails, re-evaluated natively with the lateral arms widened to the
 * presentation's view, or ADMIT when the widened view shows it and the
 * canonical 4:3 view does not. The candidate walk tallies these on its census
 * line. */
typedef enum moby_wide_verdict {
    MOBY_WIDE_OUTSIDE = 0,      /* outside the cull's distance box */
    MOBY_WIDE_SECTOR,           /* the moby's sector byte names a dormant sector */
    MOBY_WIDE_OTHER_KIND,       /* no model, or the simple kind */
    MOBY_WIDE_RANGE,            /* view depth at or past the box threshold */
    MOBY_WIDE_NEAR,             /* behind the near bound */
    MOBY_WIDE_LATERAL,          /* outside the widened horizontal cone */
    MOBY_WIDE_VERTICAL,         /* outside the vertical cone */
    MOBY_WIDE_SPHERE2,          /* the cull's second sphere fails under the widened view */
    MOBY_WIDE_CANONICAL,        /* passes the 4:3 cone too: the cull rejected it
                                 * for a reason these arms do not express */
    MOBY_WIDE_ADMIT,
    MOBY_WIDE_VERDICTS
} moby_wide_verdict_t;

typedef struct moby_game {
    /* ---- stream decoders --------------------------------------------- */
    /* Open one keyframe's vertex stream. `header` is the AnimationHeader, so
     * a decoder whose stream is relative to a per-model static vertex array
     * can find it; `s5` is the per-model shift the walk already read. */
    void (*vstream_init)(CPUState* cpu, moby_vstream_t* s,
                         uint32_t header, uint32_t frame, int s5);
    /* Advance to the next vertex; `first` opens the stream. A codec whose first
     * item is always a literal reads it; one whose keyframe word names the
     * first item's form ignores it. Leaves the vertex in s->VX/VY/VZ. */
    void (*vstream_next)(CPUState* cpu, moby_vstream_t* s, int first);
    /* How many keyframes `header`'s keyframe array holds, so a consumer that
     * needs the whole animation rather than the pose being drawn can open
     * every stream (moby_keyframe_ref indexes them). NULL where the game's
     * header layout does not name the count; a consumer then has no
     * whole-model measurement and falls back to the header's own
     * bounding-radius byte. */
    int  (*anim_frame_count)(CPUState* cpu, uint32_t header);
    /* Decode a whole faceblock into out[0..cap) and fill `st`. `bytecount` is
     * the block's leading length word, already read and range-checked.
     * Returns the emittable face count. */
    int  (*decode_faces)(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                         moby_wface_t* out, int cap, moby_face_stats_t* st);
    /* Read one cull record. Returns 0 when the record is not a live animated
     * record (the walk then stops or skips it). */
    int  (*read_record)(CPUState* cpu, uint32_t rec, moby_record_t* out);

    /* The faceblock patch list this record's keyframe carries, or 0 when it
     * carries none. Entries are ((count << 24) | byte offset into the model's
     * high-poly faceblock) followed by `count` replacement words, ending with
     * the entry whose header word is not positive; the walk applies them in
     * place around the decode and restores the originals inside the same hook
     * call. NULL where the game has no patch mechanism.
     * `model` is the moby's model, passed only for a keyframe the record does
     * not name (the previous pose a sub-tick present holds across a clip
     * switch between meshes of different vertex counts); it is 0 whenever the
     * keyframe is the record's own, and a game whose keyframe carries its own
     * list ignores it. */
    uint32_t (*frame_patch_list)(CPUState* cpu, uint32_t rec, uint32_t model,
                                 uint32_t frame_a);
    /* Nonzero where the game's patch copy loop runs its body before testing
     * the entry's count, so an entry with count 0 copies one word and
     * consumes one list word (moby_patch_list.h); 0 where it tests first. */
    int frame_patch_body_first;

    /* The record's keyframe-blend byte (record word 0 bits 8-15, the numerator
     * the draw scales by 16) for a given value of the moby's animation-state
     * byte at anim_state_off + 4. A game whose cull stores that byte straight
     * through leaves this NULL; a game whose cull maps it through a table
     * answers with the mapped byte, so a pose rebuilt from a gamestate
     * snapshot blends exactly as the cull's own record does. */
    uint32_t (*anim_blend_byte)(CPUState* cpu, uint32_t anim_state_byte4);

    /* Which of the two model kinds a model-table entry is. The walk decodes
     * an animated model itself; a game that flags a simple model by the
     * entry's own sign, by a count field or any other rule answers here. */
    int  (*model_is_animated)(CPUState* cpu, uint32_t model);

    /* ---- simple models ---------------------------------------------- */
    /* A game whose simple models are drawn by their own
     * pass (a static geometry list) feeds it here, on that pass's own draw
     * hook (game_layout.moby.hook_draw_simple). The pass owns the guest
     * dispatch and the decode, and hands the walk each record's transform and
     * model-space vertices through moby_simple_record and
     * moby_simple_project_record, then its triangles through moby_simple_tri
     * from the per-record job moby_simple_fanout runs; it returns the triangle
     * count. `simple_replay` re-runs the decode alone for a sub-tick present,
     * from the queue the canonical call captured, so the simple mobys survive
     * between ticks. NULL where the game has no such kind, and the walk feeds
     * the animated records alone. */
    int  (*simple_pass)(CPUState* cpu) OP_SLOT(sim);
    int  (*simple_replay)(CPUState* cpu) OP_SLOT(sim);
    /* The mobys of the simple models' queue copy taken in canonical epoch
     * `epoch`, written to out (at most max); 0 when that epoch captured no
     * queue, -1 when `epoch` is not the one before the last committed. A held camera cut replays that copy (moby_native_prev_queue_replay)
     * and the cut guard checks each of these can show that tick. NULL where
     * the game keeps no previous copy. */
    int  (*simple_prev_mobys)(uint32_t epoch, uint32_t* out, int max);
    /* The mobys of the simple models' queue copy taken in the current
     * canonical epoch, reveal records included, written to out (at most
     * max); 0 when this epoch captured no queue. NULL where the game draws no
     * simple pass. */
    int  (*simple_mobys)(uint32_t* out, int max) OP_SLOT(sim);

    /* ---- per-face special material -----------------------------------
     * A face the stream marks with a special-material flag (moby_wface_t.hl)
     * whose treatment the game selects from the record's material word, not
     * from the face. The hook rewrites the face's four vertex colours in place,
     * may set or clear `textured`, and may fill `tex` to draw the face from a
     * substitute texture page and CLUT; it returns 0 to leave the face alone,
     * or 1 + the treatment index it applied, which the walk counts. `material`
     * is the record's material word, `M` its model-to-view matrix at unit
     * scale, `moby` the
     * Moby the record was built from, `rec` the cull record the face belongs
     * to, and
     * `mv` the four corners' model-space vertices after the keyframe blend,
     * which a coordinate-generating treatment reads. NULL
     * where the game's special faces are handled by the walk's own branch. */
    int  (*special_face)(CPUState* cpu, uint32_t material, const float M[3][3],
                         uint32_t moby, const moby_face_rec_t* rec, const struct moby_wface* F,
                         uint32_t c[4], const float mv[4][3], int* textured,
                         moby_face_tex_t* tex);

    /* ---- mirrored record ----------------------------------------------
     * The sign the draw multiplies into every face test of a record with
     * this material word: -1 when the word selects a mirrored draw, whose
     * matrix reverses screen winding, so the draw negates both NCLIPs to
     * keep the faces that face the camera; +1 otherwise. NULL: every record
     * keeps the plain test. */
    int  (*record_face_sign)(uint32_t material);

    /* ---- water-plane split --------------------------------------------
     * Fills `wp` and returns 1 when the record's material word selects the
     * draw's water-plane split, which replaces the per-face special material
     * for the whole record; returns 0 otherwise. `moby` is the Moby the
     * record was built from and `header` its AnimationHeader. NULL where the
     * game's draw has no split. */
    int  (*water_plane)(CPUState* cpu, uint32_t material, uint32_t moby,
                        uint32_t header, moby_wplane_t* wp);

    /* ---- whole-table material tint ------------------------------------
     * A material word that recolours the record's whole colour table after
     * the LOD fade: every colour is depth-cued (GTE DPCS) toward one 12-bit
     * far colour at one IR0, and every face draws from the result. Returns 1
     * and fills `fc` (12-bit R, G, B) and `ir0` when `material` selects the
     * tint, else 0. NULL where the game has no such material. */
    int  (*material_tint)(uint32_t material, int fc[3], int* ir0);

    /* 1 = the walk may rebuild cull records from gamestate for the mobys the
     * canonical cull never queued (the widescreen and extended-LOD reveal).
     * The rebuild writes the record fields in this game's own encoding, so a
     * game whose encoding it does not implement leaves it 0 and feeds only
     * what the cull queued. */
    int  host_reveal;

    /* The canonical cull's own distance-box threshold for one moby, with
     * `drawn_bits` standing in for the moby's drawn-verdict byte so the caller
     * can substitute the reveal's own hysteresis for it. 0 or negative = the
     * box is degenerate and admits nothing, the dormant-slot sentinel.
     * Together with `reveal_record` this is what a game supplies instead of
     * `host_reveal` when its record encoding is its own: the shared candidate
     * walk moby_reveal_candidates() applies the cull's admission and the view
     * window, and the game appends the record. NULL leaves the walk feeding
     * exactly what the cull queued. */
    int  (*cull_box_threshold)(CPUState* cpu, uint32_t moby, unsigned drawn_bits);
    /* Append one record for a moby the cull's distance box rejected, built
     * from that moby's gamestate in the game's own encoding, to the queue the
     * named model kind's pass decodes. Returns 1 when a record was appended, 0
     * otherwise, and writes the reason for a decline to `why`, which the
     * candidate walk tallies per pass. Reads gamestate; writes no guest
     * memory, so the cull's own verdict byte and the canonical queue are
     * untouched. */
    int  (*reveal_record)(CPUState* cpu, uint32_t moby, moby_reveal_kind_t kind,
                          moby_reveal_reason_t* why);
    /* The canonical cull's admission of one animated moby, re-evaluated under a
     * view `widen` times wider and `vwiden` times taller than 4:3: the
     * distance box with `drawn_bits` as the verdict byte the cull read, the
     * sector byte, the model kind, then the range, near, lateral and vertical
     * arms and any second sphere the cull tests, the lateral slopes scaled by
     * `widen` and the vertical ones by `vwiden`. ADMIT only when a widened arm
     * passes where the 4:3 one fails. `open_sector` skips the sector-byte
     * test, so a moby dropped on that byte alone takes the later tests' verdict.
     * Reads gamestate only. NULL: the walk re-admits nothing inside the box. */
    moby_wide_verdict_t (*cull_view_admits)(CPUState* cpu, uint32_t moby,
                                            unsigned drawn_bits, double widen,
                                            double vwiden, int open_sector);

    /* ---- paint order inside one moby ----------------------------------
     * The key arithmetic of each walk is a row of moby_rank.h's table; the
     * game decodes the fields the row reads.
     *
     * `otkey_record` names the row of one animated record and fills `a`, the
     * animation-header byte the row scales by, and `tr`, the integer
     * translation the draw loads into GTE TRX/TRY/TRZ for the record's RTPS;
     * MOBY_OTK_NONE leaves the record's faces unranked (true depth alone
     * orders them). NULL: no animated record is ranked.
     *
     * `specular_otkey_row` replaces the row `otkey_record` names for the
     * records the specular pass draws (hook_draw_specular), whose walk decodes
     * the same record fields; MOBY_OTK_NONE keeps `otkey_record`'s row.
     *
     * `shaded_otkey_row` is the row of the second material pass (the shaded
     * list the walk feeds from `shaded_list`), MOBY_OTK_NONE for none, and
     * `shaded_otkey_face` decodes one of its faces' two words into the bias,
     * back-face and near-keep fields of `f`. */
    moby_otk_row_t (*otkey_record)(CPUState* cpu, uint32_t header, uint32_t header_b,
                                   uint32_t w0, const int TR_raw[3], int tr[3], int* a);
    moby_otk_row_t specular_otkey_row;
    moby_otk_row_t shaded_otkey_row;
    void (*shaded_otkey_face)(uint32_t w0, uint32_t w1, moby_otk_face_t* f);
    /* `otkey_splice` reads the world ordering-table splice of one animated
     * cull record: the world bucket its local bucket array is spliced from
     * (`wbase`) and the shift `n` that sets 0x100 >> n array bytes per world
     * bucket (moby_otk_splice_bucket). Returns 0 when the record carries
     * none. NULL: the game's moby faces keep their per-triangle contest key. */
    int (*otkey_splice)(CPUState* cpu, uint32_t rec, int* wbase, int* n);

    /* ---- identity ----------------------------------------------------- */
    /* The key the sub-tick interp ring stores a moby's prev/curr snapshots
     * under. Slot-stable across pool churn where the game reuses records. */
    uint32_t (*interp_key)(uint32_t moby);

    /* ---- per-game classifications (NULL = the path is skipped) -------- */
    /* A moby drawn by a cutscene renderer that owns the draw for the frames
     * it runs: its triangles go to the front pass, and the host-side reveal
     * adds nothing while that renderer owns the draw. */
    int  (*cutscene_actor)(CPUState* cpu, uint32_t moby);
    int  (*cutscene_owns_draw)(CPUState* cpu);
    /* The game's own verdict on one step of a moby its cutscene path
     * sampler drives, from the moby's tick N-1 and tick N records: 1 when the
     * sampler moved it to a new place (a teleport), 0 when the step is part of
     * a continuous track, interpolated by the sampler or placed one segment
     * at a time, -1 when the moby is not such an actor or the game has no
     * such rule (NULL = -1). */
    int  (*cutpath_jump)(uint32_t moby, const uint8_t* prev_rec, const uint8_t* curr_rec);
    /* A moby the sub-tick view holds at its previous tick on this present:
     * every byte of its record the feeders read is tick N-1's, so no channel
     * of its record may take tick N's value. */
    int  (*moby_held)(uint32_t moby);
    /* An instance whose first update does a one-shot job and retires it: the
     * walk emits nothing for it and the reveal never admits it. */
    int  (*run_once_hidden)(CPUState* cpu, uint32_t moby, unsigned cls);
    /* The game is in a state that draws the HUD under the player model. */
    int  (*hud_under_player)(CPUState* cpu);
    /* Per-row factors the game's draw multiplies into a cull record's
     * matrix after the cull built it and before the record draws: 1 with
     * scale[3] filled (row 0..2) when the draw scales the record at `rec`,
     * 0 when it does not. On a sub-tick present the factors are lerped
     * between the tick pair; at alpha 1 they are the canonical present's.
     * NULL = the game scales no record. */
    int  (*record_row_scale)(CPUState* cpu, uint32_t rec, double scale[3]);

    /* ---- addresses outside the descriptor (0 = the path is skipped) --- */
    /* The render scratch block the draw passes build their transient data in,
     * and the windows inside it the feeder reads back: the per-vertex
     * depth-cued colours the draw wrote, the simple models' Moby* list
     * and its per-vertex lit colours. `spec_window_off`/`_len` is the
     * sub-range of the block a second material pass rebuilds, which the
     * sub-tick re-extract snapshots and replays. */
    uint32_t scratch;
    uint32_t colour_scratch;
    uint32_t shaded_list;
    uint32_t shaded_colour;
    uint32_t spec_window_off, spec_window_len;
    /* The far-geometry suppression flag the low-poly colour band consults. */
    uint32_t skip_lowpoly;
    /* The player's world position block, read by the reveal-sizing census. */
    uint32_t player_pos;
    /* The specular material block: the env-reflection matrix at +0x18, the
     * light direction at +0x0C, and the static Tiledef of the reflection
     * texture. 0 on a game with no specular material, and the walk's
     * per-face special-material branch never arms. */
    uint32_t spec_light;
    uint32_t metal_tiledef;
    /* Offsets into one Moby that only this game's materials and culls read:
     * the spawn-parameter pointer, the LOD/fade byte, the specular colour
     * word, the render radius byte, the update-distance byte and the
     * substate byte. */
    uint32_t props_off, renderer_off, spec_colour_off,
             render_radius_off, update_dist_off, substate_off;
    /* The bits of the LOD/fade byte the cull shifts into the tier. */
    uint32_t renderer_mask;
    /* The bit of the same byte that selects the specular renderer for this
     * moby. The list builder sorts every moby into one of three renderer
     * lists on this bit alone, so it is the per-moby predicate the specular
     * pass applies before any distance or visibility test. 0 = the game has
     * no such bit, and the host reveal then claims no specular draw. */
    uint32_t renderer_shiny_mask;
    /* The camera's fine heading word, which the env reflection folds into its
     * azimuth so the reflection stays world-anchored under a pan; 0 takes
     * game_layout.moby.env_bias_word. `cam_heading_period` is the period the
     * map reads the halfword with, the circle a sub-tick blend wraps on
     * (0 = 4096). */
    uint32_t cam_heading;
    uint16_t cam_heading_period;
    /* The simple models' base-colour tables, flat and metal. */
    uint32_t shade_coltab, metal_coltab;
} moby_game_t;

/* The running game's table: game_runtime_install (game_host_desc.h) copies
 * the registered descriptor's table here before any feeder runs. */
extern moby_game_t moby_game;

/** @brief Read one word of a cull record, from the copy taken at the cull hook when it holds the address.
 *
 * Where the game's cull is hooked the walk holds a copy of the record list
 * taken at that hook, and an aligned address inside the copy resolves against
 * it: the draw pass that follows the cull reuses the scratch the list sits
 * in, so a record read back from memory afterwards may hold that draw's
 * output instead of the cull's fields. Addresses outside the copy, and every
 * game whose cull is not hooked, read guest memory. A record reader uses this
 * in place of cpu->read_word, and moby_rec_matrix in place of
 * ngte_read_matrix, for every field it takes out of a record.
 * @param cpu The guest CPU state, read only.
 * @param addr The guest address of the word.
 * @return The word. */
uint32_t moby_rec_word(CPUState* cpu, uint32_t addr);
/** @brief Whether the walk running is the verify probe's reference walk.
 *
 * The probe re-walks a pass to score it against the prims the game drew; a
 * treatment that generates texture coordinates then emits the game's own
 * integer coordinates, the reference the probe scores, in place of the drawn
 * ones.
 * @return 1 inside the reference walk, else 0. */
int      moby_env_reference(void);
/** @brief The camera heading the environment map reads, at the present the walk draws.
 *
 * The halfword at moby_game.cam_heading (or game_layout.moby.env_bias_word),
 * blended at a sub-tick present along the shorter way round
 * moby_game.cam_heading_period; loaded once per walk before any face.
 * @return The heading in the halfword's units. */
float    moby_env_heading(void);
/** @brief Read a cull record's packed rotation matrix, each word through moby_rec_word.
 *
 * Unpacks five words at addr..addr+16 with ngte_unpack_matrix.
 * @param cpu The guest CPU state, read only.
 * @param addr The guest address of the matrix inside the record.
 * @param M Receives the matrix, Q12. */
void     moby_rec_matrix(CPUState* cpu, uint32_t addr, int M[3][3]);

/* The part of an animated cull record that carries no game-specific encoding,
 * rebuilt from one moby's gamestate for a reveal: the keyframe pair the cull
 * would have read out of the moby's animation state, the blend byte that pair
 * carries, the model->view rotation composed from the moby's angle bytes, and
 * the view-space centre in quarter-world units before the game's own
 * translation rescale. A game's builder turns these into its record's bit
 * layout. */
typedef struct moby_anim_fields {
    uint32_t model;                 /* the model-table entry the pose came from */
    uint32_t header, header_b;      /* keyframe A's and B's AnimationHeader */
    uint32_t frame_a, frame_b;      /* the keyframe references, in the record's form */
    uint32_t blend;                 /* the record's keyframe-blend byte, 0 = frame A alone */
    int      centre[3];             /* camera matrix applied to (moby - cam) >> 2 */
    int      M[3][3];               /* model->view rotation, 1.0 = 4096 */
    int      cam[3][3];             /* the camera matrix M was composed against */
} moby_anim_fields_t;

/** @brief The cull's quarter-world camera deltas for one moby and their view-space centre through a camera matrix.
 *
 * The deltas in the cull's own form (Spyro 3 mobydraw.s:79-96): X is
 * (moby - cam) >> 2, Y and Z are (cam - moby) >> 2, each subtraction
 * wrapping at 32 bits. The centre is the cull's MVMVA (mobydraw.s:125-130):
 * IR1 the Y delta, IR2 the Z, IR3 the X, MAC = (M . IR) >> 12, accumulated
 * at 64 bits. With `ir16` each delta enters as the 16-bit IR register the
 * cull loads it into; without it at full width, for a centre past the
 * registers' range. A NULL `M` computes the deltas alone.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param cam The camera position the cull reads at G_CAMERA+0x28..0x30.
 * @param M The camera matrix at G_CAMERA+0x00, 1.0 = 4096, or NULL.
 * @param ir16 Nonzero to truncate each delta to the cull's 16-bit IR register.
 * @param d Receives the X, Y and Z deltas.
 * @param mac Receives the view-space centre x, y, z; unused when `M` is NULL. */
void moby_cull_centre(CPUState* cpu, uint32_t moby, const int32_t cam[3], const int M[3][3],
                      int ir16, int32_t d[3], int64_t mac[3]);

/** @brief Fill the encoding-free fields of an animated cull record for one moby from gamestate.
 *
 * The keyframe pair and blend byte come from the moby's animation state bytes
 * by the cull's rule; the rotation composes the byte-exact euler table lookup
 * with the camera matrix of the view the current pass projects through, which
 * `cam` keeps for a game whose cull composes another form; the centre is the
 * cull's MVMVA over the quarter-world camera delta, accumulated
 * at 64 bits and clamped to int32. Reads gamestate only.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param f Receives the fields.
 * @return 0 when the moby's class names no animated model or its animation
 *         state names no keyframe header, else 1. */
int moby_reveal_anim_fields(CPUState* cpu, uint32_t moby, moby_anim_fields_t* f);

/** @brief Reserve one record slot past the cull's own queue in the host copy taken at the cull hook.
 *
 * For a reveal that composes an animated record in the game's own encoding.
 * The copy's terminator moves past the new slot, so the walk reads the
 * appended records after the queued ones, and every record reader resolves
 * them through moby_rec_word. Only a copy that reached the cull's terminator
 * can be extended: a copy that ran out of room ends on a live record whose
 * successors read guest memory. Writes no guest memory.
 * @param addr Receives the guest address the slot shadows, or NULL.
 * @return A zeroed array of rec_stride/4 words to compose into; NULL when
 *         there is no copy, the copy did not reach the terminator, or it is
 *         full. */
uint32_t* moby_rec_append(uint32_t* addr);

/** @brief Whether the host copy already holds a record naming this moby, queued by the cull or appended by a reveal.
 *
 * A cull admits on more than its distance box, so a moby the box rejected can
 * still carry a record, and a second one would draw it twice. The scan stops
 * at the first record moby_game.read_record rejects.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @return 1 when a record names the moby; 0 otherwise, and always 0 with no
 *         copy or no read_record slot. */
int moby_rec_holds_moby(CPUState* cpu, uint32_t moby);

/** @brief Whether the current VBL (`openpete_frame`) lies inside the OPENPETE_MOBY_VERIFY reporting window.
 *
 * Every probe line a game's moby provider prints under that variable gates on
 * this, so the VBLs the window names are the VBLs reported. The variable
 * carries the window as a first VBL or a first-last VBL range; the value `1`
 * selects the default window, 13 VBLs from OPENPETE_MOBY_VERIFY_FRAME
 * (default 2000), plus every later VBL that is a multiple of 200. Always 0
 * while the variable is unset or `0`.
 * @return 1 inside the window, else 0. */
int moby_verify_vbl(void);

/** @brief Print one `[moby-kind]` census line for a moby pass when OPENPETE_MOBY_KIND_CENSUS is on, read once.
 *
 * Each pass that decodes moby records, and each extended-LOD reveal walk,
 * calls it once per present.
 * @param leg `canon` for the canonical pass, `sub` for a sub-tick replay.
 * @param kind The model kind, or `<kind>-reveal` for a reveal walk.
 * @param recs The records the pass offered (a reveal walk: appended).
 * @param occl The records the occlusion test culled.
 * @param verts The vertices the decode produced.
 * @param tris The triangles the decode produced. */
void moby_kind_census(const char* leg, const char* kind, long recs, long occl, long verts, long tris);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_MOBY_GAME_H */
