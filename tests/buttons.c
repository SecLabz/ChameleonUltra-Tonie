#include "core/button.h"
#include "core/favorites.h"
#include <assert.h>
#include <stdio.h>

static void favorite_cycles(void) {
    /* Every eight-slot arrangement of empty slots and three distinct tags. */
    for (uint32_t arrangement = 0; arrangement < 65536u; arrangement++) {
        uint32_t f[8], encoded = arrangement, present = 0;
        for (unsigned i = 0; i < 8; i++) {
            f[i] = encoded & 3u;
            encoded >>= 2;
            if (f[i])
                present |= 1u << f[i];
        }
        for (int dir = -1; dir <= 1; dir += 2) {
            uint32_t first = favorite_next_tag_id(f, 0, dir), selected = first, seen = 0;
            if (!present) {
                assert(!first);
                continue;
            }
            do {
                assert(selected >= 1 && selected <= 3);
                assert(!(seen & (1u << selected)));
                seen |= 1u << selected;
                selected = favorite_next_tag_id(f, selected, dir);
            } while (selected != first);
            assert(seen == present);
        }
    }
}

int main(void) {
    button_state_t b = {0};
    /* A brief input spike after a long idle must produce no action. */
    assert(button_poll(&b, true, 10000) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 10004) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 10100) == BUTTON_EVENT_NONE);
    /* Press and release bounce cannot advance multiple favorites. */
    assert(button_poll(&b, true, 10200) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 10210) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 10220) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 10249) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 10250) == BUTTON_EVENT_PRESS);
    assert(button_poll(&b, false, 10300) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 10310) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 10320) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 10350) == BUTTON_EVENT_SHORT);
    assert(button_poll(&b, false, 10400) == BUTTON_EVENT_NONE);
    /* One hold action, no repeat and no short action on release. */
    b = (button_state_t){0};
    assert(button_poll(&b, true, 0) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 30) == BUTTON_EVENT_PRESS);
    assert(button_poll(&b, true, 829) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 830) == BUTTON_EVENT_HOLD);
    assert(button_poll(&b, true, 2000) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 2010) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 2040) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 2050) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 2080) == BUTTON_EVENT_PRESS);
    assert(button_poll(&b, false, 2100) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 2130) == BUTTON_EVENT_SHORT);
    /* Release before the hold threshold remains short during debounce. */
    b = (button_state_t){0};
    button_poll(&b, true, 0);
    button_poll(&b, true, 30);
    assert(button_poll(&b, false, 820) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 830) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 850) == BUTTON_EVENT_SHORT);
    /* Debounce and hold deadlines survive the unsigned timer rollover. */
    b = (button_state_t){0};
    assert(button_poll(&b, true, UINT32_MAX - 15u) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, true, 14) == BUTTON_EVENT_PRESS);
    assert(button_poll(&b, true, 814) == BUTTON_EVENT_HOLD);
    b = (button_state_t){.candidate = true, .pressed = true, .held = true};
    assert(button_poll(&b, false, UINT32_MAX - 10u) == BUTTON_EVENT_NONE);
    assert(button_poll(&b, false, 19) == BUTTON_EVENT_NONE);
    assert(!b.pressed);
    puts("Buttons: stable debounce, bounce rejection, single hold, release boundary and timer "
         "rollover passed");
    const uint32_t empty[8] = {0}, sparse[8] = {11, 0, 0, 22, 0, 0, 0, 33},
                   duplicates[8] = {11, 11, 22, 0, 11, 33, 0, 0}, single[8] = {0, 11, 0, 11};
    assert(!favorite_next_tag_id(empty, 0, 1) && !favorite_next_tag_id(empty, 99, -1));
    assert(favorite_next_tag_id(sparse, 0, 1) == 11 && favorite_next_tag_id(sparse, 0, -1) == 33);
    assert(favorite_next_tag_id(sparse, 99, 1) == 11 && favorite_next_tag_id(sparse, 99, -1) == 33);
    assert(favorite_next_tag_id(sparse, 11, 1) == 22 && favorite_next_tag_id(sparse, 22, -1) == 11);
    assert(favorite_next_tag_id(sparse, 33, 1) == 11 && favorite_next_tag_id(sparse, 11, -1) == 33);
    assert(favorite_next_tag_id(duplicates, 11, 1) == 22 &&
           favorite_next_tag_id(duplicates, 11, -1) == 33);
    assert(favorite_next_tag_id(duplicates, 22, 1) == 33 &&
           favorite_next_tag_id(duplicates, 33, 1) == 11);
    assert(favorite_next_tag_id(duplicates, 33, -1) == 22 &&
           favorite_next_tag_id(duplicates, 22, -1) == 11);
    assert(favorite_next_tag_id(single, 11, 1) == 11 && favorite_next_tag_id(single, 11, -1) == 11);
    assert(!favorite_next_tag_id(sparse, 11, 0) && !favorite_next_tag_id(sparse, 11, INT32_MIN));
    favorite_cycles();
    puts("Favorites: empty/sparse lists, outside selection, wrapping, duplicate references and "
         "single-tag reload passed");
}
