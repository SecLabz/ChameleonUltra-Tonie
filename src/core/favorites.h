#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "core/limits.h"

/* Duplicate shortcuts must not trap navigation on the current tag. */
static inline uint32_t favorite_next_tag_id(const uint32_t favorites[FAVORITE_SLOT_COUNT],
                                            uint32_t selected, int direction) {
    if (direction != 1 && direction != -1)
        return 0;
    int start = direction > 0 ? (int)FAVORITE_SLOT_COUNT - 1 : 0;
    bool current = false;
    for (unsigned i = 0; selected && i < FAVORITE_SLOT_COUNT; i++)
        if (favorites[i] == selected) {
            start = (int)i;
            current = true;
            break;
        }
    for (int i = 1; i <= (int)FAVORITE_SLOT_COUNT; i++) {
        unsigned slot =
            (unsigned)(start + direction * i + 2 * (int)FAVORITE_SLOT_COUNT) % FAVORITE_SLOT_COUNT;
        if (favorites[slot] && favorites[slot] != selected) {
            unsigned first = 0;
            while (first < slot && favorites[first] != favorites[slot])
                first++;
            if (first == slot)
                return favorites[slot];
        }
    }
    return current ? selected : 0;
}
