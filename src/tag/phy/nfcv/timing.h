/* Extracted from the inherited GPIO NFC-V implementation,
 * GPL-3.0. See docs/nfcv.md for the inherited mechanism. */
#pragma once
/* Established 64 MHz CPU-cycle timings. Preserve integer arithmetic, strict
 * acceptance bounds and polling order. Detector thresholds are inherited;
 * their tolerances are not replacements for nominal ISO15693 timings.
 * See docs/nfcv.md. */
#define NFCV_RAM_FUNCTION __attribute__((section(".ramcode"), noinline, aligned(4)))
#define NFCV_ACTIVATE_TIMEOUT_CYCLES 64000u
#define NFCV_CLOCK_TIMEOUT_CYCLES 640000u
#define NFCV_FIELD_LOSS_GUARD_CYCLES 128000u
#define NFCV_RX_SOF_TIMEOUT_CYCLES 640000u
#define NFCV_RX_FRAME_TIMEOUT_CYCLES 23040000u
#define NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES 6400u
#define NFCV_RX_1_OF_4_GAP_TIMEOUT_CYCLES 19200u
#define NFCV_RX_1_OF_256_GAP_TIMEOUT_CYCLES 704000u

/* A PPM tick is 604.13 CPU cycles. Half-denominator rounding is intentional. */
#define NFCV_PPM_TICK_CYCLES_NUMERATOR 60413u
#define NFCV_PPM_TICK_CYCLES_DENOMINATOR 100u
#define NFCV_PPM_TICK_ROUNDING 30206u
#define NFCV_RX_TICK_TOLERANCE_CYCLES 160u
#define NFCV_RX_1_OF_4_SOF_MIN_CYCLES 2830u
#define NFCV_RX_1_OF_4_SOF_MAX_CYCLES 3220u
#define NFCV_RX_1_OF_256_SOF_MIN_CYCLES 4037u
#define NFCV_RX_1_OF_256_SOF_MAX_CYCLES 4420u
#define NFCV_RX_SOF_CODING_SPLIT_CYCLES 3500u
#define NFCV_RX_MERGED_PULSE_MIN_CYCLES 1993u
#define NFCV_RX_MERGED_PULSE_MAX_CYCLES 2780u

#define NFCV_PPM_1_OF_4_SYMBOL_TICKS 8u
#define NFCV_PPM_1_OF_256_SOF_TICKS 8u
#define NFCV_PPM_1_OF_256_SYMBOL_TICKS 512u
#define NFCV_PPM_1_OF_4_LAST_POSITION 7u
#define NFCV_PPM_1_OF_256_LAST_POSITION 511u
#define NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE 4u
#define NFCV_PPM_EOF_POSITION 2u
#define NFCV_RX_MIN_FRAME_BYTES 4u

/* 64 MHz / (2 * 423.75 kHz): rational 75/76-cycle half periods. */
#define NFCV_TX_HALF_PERIOD_BASE_CYCLES 75u
#define NFCV_TX_HALF_PERIOD_FRACTION 437500u
#define NFCV_TX_HALF_PERIOD_DENOMINATOR 847500u
#define NFCV_TX_LONG_SEGMENT_CYCLES 24u
#define NFCV_TX_BIT_HALF_CYCLES 8u
#define NFCV_TX_LOW_RATE_MULTIPLIER 4u
#define NFCV_WAVEFORM_CYCLE_MASK 127u
#define NFCV_WAVEFORM_CARRIER_FLAG 128u
#define NFCV_WAVEFORM_SEGMENTS_PER_BYTE 16u
#define NFCV_WAVEFORM_FRAMING_CAPACITY 9u

/* Retain both inherited response-delay terms and the measured correction.
 * These schedule waveform start; the first segment is inactive. */
#define NFCV_TX_EOF_REFERENCE_OFFSET_CYCLES 604u
#define NFCV_TX_RESPONSE_WAIT_CYCLES 20540u
#define NFCV_TX_UNADJUSTED_DELAY_CYCLES 21144
#define NFCV_TX_DEFAULT_ADJUSTMENT_CYCLES (-32)
#define NFCV_TX_TEST_DELAY_MIN_CYCLES 21016u
#define NFCV_TX_TEST_DELAY_MAX_CYCLES 21272u
