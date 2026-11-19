/**
 * @file nvn_slots.c
 * @brief See nvn_slots.h.
 */
#include "nvn_slots.h"

#include <stdlib.h>

bool nvn_slots_init(nvn_slots *s, uint32_t nslots, uint32_t nids)
{
    *s = (nvn_slots){ .nslots = nslots, .nids = nids };
    s->id_slot = calloc(nids, sizeof *s->id_slot);
    s->slot_id = calloc(nslots, sizeof *s->slot_id);
    s->free_stack = calloc(nslots, sizeof *s->free_stack);
    return s->id_slot && s->slot_id && s->free_stack && nslots;
}

uint32_t nvn_slots_take(nvn_slots *s, uint32_t id, uint32_t *evicted)
{
    *evicted = 0;
    if (id >= s->nids)
        return UINT32_MAX;
    if (s->id_slot[id])
        return s->id_slot[id] - 1;
    uint32_t slot;
    if (s->nfree) {
        slot = s->free_stack[--s->nfree];
    } else {
        slot = s->next;
        s->next = (s->next + 1) % s->nslots;
    }
    if (s->slot_id[slot]) {
        *evicted = s->slot_id[slot];
        s->id_slot[s->slot_id[slot] - 1] = 0;
    }
    s->id_slot[id] = slot + 1;
    s->slot_id[slot] = id + 1;
    return slot;
}

void nvn_slots_free(nvn_slots *s, uint32_t slot)
{
    if (slot >= s->nslots || !s->slot_id[slot])
        return;
    s->id_slot[s->slot_id[slot] - 1] = 0;
    s->slot_id[slot] = 0;
    s->free_stack[s->nfree++] = slot;
}
