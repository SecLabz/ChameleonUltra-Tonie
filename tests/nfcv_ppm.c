#include "tag/phy/nfcv/ppm.h"
#include <assert.h>
#include <stdio.h>
#include "tag/phy/nfcv/waveform.h"
#include "tag/protocol/nfcv/frame.h"

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

static void waveform(const uint8_t *bytes, unsigned length, unsigned rate) {
    uint8_t expected[ISO15693_RESPONSE_MAX_BYTES * 16u + 8u];
    unsigned count = 0;
    expected[count++] = 24 * rate;
    expected[count++] = 128 | (24 * rate);
    expected[count++] = 8 * rate;
    expected[count++] = 128 | (8 * rate);
    for (unsigned i = 0; i < length; i++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            bool one = (bytes[i] >> bit) & 1u;
            expected[count++] = (8 * rate) | (one ? 0 : 128);
            expected[count++] = (8 * rate) | (one ? 128 : 0);
        }
    }
    expected[count++] = 128 | (8 * rate);
    expected[count++] = 8 * rate;
    expected[count++] = 128 | (24 * rate);
    expected[count++] = 24 * rate;
    assert(count == length * NFCV_WAVEFORM_SEGMENTS_PER_BYTE + NFCV_WAVEFORM_FRAMING_SEGMENTS);
    for (unsigned segment = 0; segment < count; segment++)
        assert(nfcv_waveform_segment(bytes, length, segment, rate) == expected[segment]);
}

static void waveforms(void) {
    uint8_t bytes[ISO15693_RESPONSE_MAX_BYTES];
    for (unsigned rate = 1; rate <= 4; rate += 3) {
        for (unsigned first = 0; first < 256; first++) {
            bytes[0] = first;
            waveform(bytes, 1, rate);
            for (unsigned second = 0; second < 256; second++) {
                bytes[1] = second;
                waveform(bytes, 2, rate);
            }
        }
        for (unsigned pattern = 0; pattern < 4; pattern++) {
            for (unsigned i = 0; i < sizeof(bytes); i++)
                bytes[i] = pattern < 2 ? (pattern ? 0xff : 0) : pattern == 2 ? i : (i & 1 ? 0xaa : 0x55);
            waveform(bytes, sizeof(bytes), rate);
        }
    }
}

int main(void) {
    waveforms();
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
