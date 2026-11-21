/**
 * @file nvn_slots.h
 * @brief The map from the texture ids a handle names to the slots the host
 * texture set has room for.
 */
#ifndef NVN_SLOTS_H
#define NVN_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief nslots slots for ids below nids, the first pinned of them taken by the ids below pinned, which always
 * hold the slot of their own number (a shader that reads a handle out of memory names the id, which the host cannot
 * rewrite). The ids from pinned on share the other nslots - pinned slots: id_slot and slot_id hold the other side
 * plus one (0: none), indexed from pinned; freed slots are reused before the next in turn is taken.
 */
typedef struct nvn_slots {
    uint32_t nslots, nids, nfree, next, pinned;
    uint32_t *id_slot, *slot_id, *free_stack;
} nvn_slots;

/** @brief Makes the table; false when out of memory or when no slot is left past pinned. */
bool nvn_slots_init(nvn_slots *s, uint32_t nslots, uint32_t nids, uint32_t pinned);

/**
 * @brief The slot of id: the one it holds, or a free one, or the next in turn
 * (*evicted is then the id that held it plus one, now holding nothing, or 0
 * when the slot was free); an id below pinned is its own slot and evicts nothing. UINT32_MAX for an id past nids.
 */
uint32_t nvn_slots_take(nvn_slots *s, uint32_t id, uint32_t *evicted);

/** @brief Gives slot back: no id holds it and the next take uses it. */
void nvn_slots_free(nvn_slots *s, uint32_t slot);

#endif
