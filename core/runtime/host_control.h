// core/runtime/host_control.h - the reads every consumer site of a
// row-driving host action goes through: an action's press edge, held level
// or press count, with the site's settings-menu gate applied inside.
#pragma once

#include "openpete_keymap.h"

/* Whether a site's key term is masked while the settings menu is open. The
 * menu owns the keyboard and the pad while it is open; most sites drop a
 * key made under it. Turbo and quit read their key with the menu open. */
typedef enum {
    HC_GATE_NONE = 0,   /* the key reads with the menu open */
    HC_GATE_MENU        /* the key reads 0 while the menu is open */
} hc_gate_t;

/** @brief Return whether a host action was pressed at the last keymap poll, under the site's menu gate.
 *
 * The press is openpete_key_pressed's: a released-to-held edge or at least
 * one wheel pulse.
 * @param a the action.
 * @param g the site's menu gate.
 * @return 1 when pressed and not masked, else 0. */
int hc_edge(openpete_action_t a, hc_gate_t g);

/** @brief Return whether a host action was held at the last keymap poll, under the site's menu gate.
 *
 * @param a the action.
 * @param g the site's menu gate.
 * @return 1 when held and not masked, else 0. */
int hc_held(openpete_action_t a, hc_gate_t g);

/** @brief Return the presses of a host action at the last keymap poll, under the site's menu gate: its edge plus its wheel notches.
 *
 * A site that steps once per press reads this, so several wheel notches in
 * one poll step several times.
 * @param a the action.
 * @param g the site's menu gate.
 * @return the press count, 0 when masked. */
int hc_count(openpete_action_t a, hc_gate_t g);
