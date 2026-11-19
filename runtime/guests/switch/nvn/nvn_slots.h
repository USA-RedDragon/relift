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
 * @brief nslots slots for ids below nids. id_slot and slot_id hold the
 * other side plus one (0: none); freed slots are reused before the next in
 * turn is taken.
 */
typedef struct nvn_slots {
    uint32_t nslots, nids, nfree, next;
    uint32_t *id_slot, *slot_id, *free_stack;
} nvn_slots;

/** @brief Makes the table; false when out of memory. */
bool nvn_slots_init(nvn_slots *s, uint32_t nslots, uint32_t nids);

/**
 * @brief The slot of id: the one it holds, or a free one, or the next in turn
 * (*evicted is then the id that held it plus one, now holding nothing, or 0
 * when the slot was free). UINT32_MAX for an id past nids.
 */
uint32_t nvn_slots_take(nvn_slots *s, uint32_t id, uint32_t *evicted);

/** @brief Gives slot back: no id holds it and the next take uses it. */
void nvn_slots_free(nvn_slots *s, uint32_t slot);

#endif
