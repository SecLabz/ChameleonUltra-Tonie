#include "tag/phy/nfcv/ppm.h"
#include <assert.h>
#include <stdio.h>

static void coding_positions(bool coding_1_of_256, unsigned symbol_count, unsigned positions) {
    for (unsigned symbol = 1; symbol <= symbol_count; symbol++) {
        for (unsigned value = 0; value < positions; value++) {
            /* Independent PPM geometry: one odd pulse position per symbol. */
            uint32_t tick = coding_1_of_256 ? 8u + (symbol - 1u) * 512u + value * 2u + 1u
                                            : symbol * 8u + value * 2u + 1u;
            uint32_t cycles = tick * 60413u / 100u;
            assert(NFCV_PPM_TICK_FROM_CYCLES(cycles) == tick);
            assert(NFCV_PPM_CYCLES_FROM_TICK(tick) == cycles);
            assert(NFCV_PPM_SYMBOL_INDEX(tick, coding_1_of_256) == symbol);
            assert(NFCV_PPM_PULSE_POSITION(tick, coding_1_of_256) == value * 2u + 1u);
        }
    }
}

int main(void) {
    coding_positions(false, 256, 4);
    coding_positions(true, 64, 256);
    assert(NFCV_PPM_MAX_SYMBOLS(false) == 256 && NFCV_PPM_MAX_SYMBOLS(true) == 64);
    assert(NFCV_PPM_MIN_FRAME_SYMBOLS(false) == 16 && NFCV_PPM_MIN_FRAME_SYMBOLS(true) == 4);
    for (unsigned count = 0; count <= 256; count++)
        assert(NFCV_PPM_FRAME_BYTES(count, false) == count / 4);
    assert(NFCV_PPM_FRAME_BYTES(64, true) == 64);
    /* The slow-coding SOF prefix still uses the short-symbol position rule. */
    assert(NFCV_PPM_SYMBOL_INDEX(7, true) == 0 && NFCV_PPM_PULSE_POSITION(7, true) == 7);
    /* Elapsed cycle subtraction stays unsigned across CYCCNT rollover. */
    uint32_t start = UINT32_MAX - 999u, elapsed = 5437u;
    uint32_t now = start + elapsed;
    assert(NFCV_PPM_TICK_FROM_CYCLES(now - start) == 9);
    puts("NFC-V PPM: both codings, all frame positions, byte bounds and cycle rollover passed");
}
