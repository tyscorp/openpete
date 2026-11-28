// core/runtime/host_control.c - the host-action reads of the consumer
// sites. Each read samples the keymap's last poll; the gate masks only the
// key term, so a site's own non-key work runs with the menu open.

#include "host_control.h"

#include "openpete_menu.h"

/** @brief Report whether the site's gate masks the key term now. */
static int hc_masked(hc_gate_t g)
{
    return g == HC_GATE_MENU && openpete_menu_is_open();
}

/* Documented at the declaration in host_control.h. */
int hc_edge(openpete_action_t a, hc_gate_t g)
{
    return hc_masked(g) ? 0 : openpete_key_pressed(a);
}

/* Documented at the declaration in host_control.h. */
int hc_held(openpete_action_t a, hc_gate_t g)
{
    return hc_masked(g) ? 0 : openpete_key_down(a);
}

/* Documented at the declaration in host_control.h. */
int hc_count(openpete_action_t a, hc_gate_t g)
{
    return hc_masked(g) ? 0 : openpete_key_edge(a) + openpete_key_pulses(a);
}
