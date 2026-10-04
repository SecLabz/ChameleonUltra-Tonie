#pragma once
/* Only the inherited NFCT delayed-close callback uses this adapter. */
#include <stdint.h>
#include <assert.h>
#include "nrfx.h"
typedef struct {
    void (*callback)(void *);
    uint32_t deadline;
    void *context;
    bool active;
} nfct_close_timer_t;
#define APP_TIMER_DEF(name)                                                                        \
    static nfct_close_timer_t name##_object;                                                       \
    static nfct_close_timer_t *name = &name##_object
#define APP_TIMER_MODE_SINGLE_SHOT 0
#define APP_TIMER_TICKS(ms) (ms)
#define APP_ERROR_CHECK(code) assert((code) == NRFX_SUCCESS)
uint32_t platform_millis(void);
extern nfct_close_timer_t *nfct_close_timer;
static inline unsigned app_timer_create(nfct_close_timer_t **t, unsigned mode, void (*cb)(void *)) {
    (void)mode;
    (*t)->callback = cb;
    nfct_close_timer = *t;
    return NRFX_SUCCESS;
}
static inline unsigned app_timer_start(nfct_close_timer_t *t, uint32_t ms, void *ctx) {
    t->deadline = platform_millis() + ms;
    t->context = ctx;
    t->active = true;
    return NRFX_SUCCESS;
}
static inline unsigned app_timer_stop(nfct_close_timer_t *t) {
    t->active = false;
    return NRFX_SUCCESS;
}
