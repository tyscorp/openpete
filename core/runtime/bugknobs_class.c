/* core/runtime/bugknobs_class.c - the bug-report replay class of a knob
 * registry row. The recorder writes it into knobs.txt for the reader; the
 * replay classifies this build's own row with the same function, so a
 * bundle's class text never decides what the replay applies. See
 * openpete_bugreport.h. */
#include "openpete_bugreport.h"

#include "openpete_knobs.h"

int openpete_bugknobs_is_env(const struct knob_desc* k)
{
    return k->env && !k->cli && k->type == KNOB_STRING;
}

const char* openpete_bugknobs_class(const struct knob_desc* k)
{
    if (k->type == KNOB_ACTION) return "action";
    if (k->mut != KNOB_LIVE) return "latched";
    if (openpete_bugknobs_is_env(k)) return "env";
    switch ((knob_replay_t)k->replay) {
    case KNOB_REPLAY_HOST:   return "host";
    case KNOB_REPLAY_SIM:    return "sim";
    case KNOB_REPLAY_FOLLOW: return "follow";
    case KNOB_REPLAY_RENDER: return "render";
    case KNOB_REPLAY_UNSET:  break;
    }
    /* openpete_knob_add and the built-in table check refuse such a row; a
     * class the replay never applies keeps an unclassified value out of it. */
    return "unset";
}
