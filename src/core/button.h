#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BUTTON_DEBOUNCE_MS 30u
#define BUTTON_HOLD_MS 800u

typedef struct {
    bool candidate, pressed, held;
    uint32_t changed, begin;
} button_state_t;
typedef enum {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_PRESS,
    BUTTON_EVENT_SHORT,
    BUTTON_EVENT_HOLD
} button_event_t;

/* Poll with a monotonic millisecond clock. A bounce restarts the stable interval. */
static inline button_event_t button_poll(button_state_t *b, bool down, uint32_t now) {
    if (down != b->candidate) {
        b->candidate = down;
        b->changed = now;
    }
    if (b->candidate != b->pressed && now - b->changed >= BUTTON_DEBOUNCE_MS) {
        b->pressed = b->candidate;
        if (b->pressed) {
            b->begin = now;
            b->held = false;
            return BUTTON_EVENT_PRESS;
        }
        if (!b->held)
            return BUTTON_EVENT_SHORT;
    }
    /* A pending release must not turn a short press into a hold. */
    if (b->candidate && b->pressed && !b->held && now - b->begin >= BUTTON_HOLD_MS) {
        b->held = true;
        return BUTTON_EVENT_HOLD;
    }
    return BUTTON_EVENT_NONE;
}
