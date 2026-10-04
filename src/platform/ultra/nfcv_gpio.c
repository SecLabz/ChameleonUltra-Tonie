/* GPIO PHY adapted from the inherited NFC-V implementation, GPL-3.0.
 * RF decoding and waveform timing retained; no internal-register TX backend.
 */
#include "tag/protocol/nfcv/commands.h"
#include "tag/phy/nfcv/diagnostics.h"
#include "platform/ultra/board.h"
#include "tag/phy/nfcv/phy.h"
#include "nrf.h"
#include "nrf_clock.h"
#include "nrf_gpio.h"
#include "nrfx_nfct.h"
#include "platform/platform.h"
#include "tag/nfcv_session.h"
#include <string.h>
#ifdef NFCV_DIAGNOSTICS
#include <stdio.h>
#endif
#include "tag/phy/nfcv/timing.h"
#include "tag/phy/nfcv/ppm.h"
static bool enabled;
#ifdef NFCV_DIAGNOSTICS
#define RF_PHASE(n) (NRF_POWER->GPREGRET2 = (n))
#else
#define RF_PHASE(n) ((void)0)
#endif
static uint32_t stats[NFCV_STAT_CAPACITY], last_field_cycles, sense_started_cycles;
static iso15693_frame_t frame;
static nfcv_reply_t response;
static uint32_t tx_deadline_cycles, tx_fraction, tx_missed_half_periods, tx_max_lateness_cycles;
static uint32_t gpio_mask;
static uint32_t observed_eof_cycles;
/* PM3 A/B/A/B: -0.5 us yielded 40/40 single reads and 10/10 multiple reads. */
static int32_t tx_timing_adjustment_cycles = NFCV_TX_DEFAULT_ADJUSTMENT_CYCLES;
/* Record last complete physical RX/TX, outside any USB work. */
static uint8_t last_tx[ISO15693_RESPONSE_MAX_BYTES], last_rx[ISO15693_FRAME_MAX_BYTES];
static unsigned last_tx_len, last_rx_len;
static uint32_t edges[NFCV_RX_EDGE_CAPACITY], saved_edges[NFCV_RX_EDGE_CAPACITY];
static unsigned edge_count, saved_edge_count;
static uint8_t waveform[ISO15693_RESPONSE_MAX_BYTES * NFCV_WAVEFORM_SEGMENTS_PER_BYTE +
                        NFCV_WAVEFORM_FRAMING_CAPACITY];
static unsigned waveform_len;
static void sense(void) {
    NRF_NFCT->EVENTS_FIELDDETECTED = 0;
    NRF_NFCT->EVENTS_FIELDLOST = 0;
    NRF_NFCT->TASKS_SENSE = 1;
    sense_started_cycles = DWT->CYCCNT;
}

static void activate(void) {
    NRF_NFCT->EVENTS_READY = 0;
    NRF_NFCT->TASKS_ACTIVATE = 1;
    stats[NFCV_STAT_ACTIVATE_COUNT]++;
    uint32_t begin = DWT->CYCCNT;
    while (!NRF_NFCT->EVENTS_READY && DWT->CYCCNT - begin < NFCV_ACTIVATE_TIMEOUT_CYCLES) {
    }
#ifdef NFCV_DIAGNOSTICS
    stats[NFCV_STAT_ACTIVATE_CYCLES] = DWT->CYCCNT - begin;
    if (stats[NFCV_STAT_ACTIVATE_CYCLES] > stats[NFCV_STAT_ACTIVATE_MAX_CYCLES])
        stats[NFCV_STAT_ACTIVATE_MAX_CYCLES] = stats[NFCV_STAT_ACTIVATE_CYCLES];
#endif
    if (!NRF_NFCT->EVENTS_READY)
        stats[NFCV_STAT_ACTIVATE_TIMEOUT]++;
    NRF_NFCT->EVENTS_FIELDLOST = 0;
}
/* Full subcarrier cycles; rational 75/76 CPU-cycle half periods. Each symbol
   uses an integer number of cycles, so no final half-cycle gets cut short. */
NFCV_RAM_FUNCTION static bool transmit_waveform(void) {
    uint32_t next = tx_deadline_cycles, phase = tx_fraction, missed = 0, worst = 0;
    for (unsigned s = 0; s < waveform_len; s++) {
        unsigned count = (waveform[s] & NFCV_WAVEFORM_CYCLE_MASK) * 2u;
        bool on = (waveform[s] & NFCV_WAVEFORM_CARRIER_FLAG) != 0;
        if (!on) {
            /* Every carrier cycle ends high. Silence needs no repeated writes;
               advance the same rational schedule and prepare the next edge early. */
            uint32_t accumulated = phase + count * NFCV_TX_HALF_PERIOD_FRACTION;
            next += count * NFCV_TX_HALF_PERIOD_BASE_CYCLES +
                    accumulated / NFCV_TX_HALF_PERIOD_DENOMINATOR;
            phase = accumulated % NFCV_TX_HALF_PERIOD_DENOMINATOR;
            continue;
        }
        for (unsigned h = 0; h < count; h++) {
            volatile uint32_t *out = !(h & 1u) ? &NRF_P1->OUTCLR : &NRF_P1->OUTSET;
            while ((int32_t)(DWT->CYCCNT - next) < 0) {
            }
            *out = gpio_mask;
            /* Measure after switching; bookkeeping must not delay this edge. */
            uint32_t behind = DWT->CYCCNT - next;
            if (behind > NFCV_TX_HALF_PERIOD_BASE_CYCLES)
                missed++;
            if (behind > worst)
                worst = behind;
            next += NFCV_TX_HALF_PERIOD_BASE_CYCLES;
            phase += NFCV_TX_HALF_PERIOD_FRACTION;
            if (phase >= NFCV_TX_HALF_PERIOD_DENOMINATOR) {
                phase -= NFCV_TX_HALF_PERIOD_DENOMINATOR;
                next++;
            }
        }
    }
    tx_deadline_cycles = next;
    tx_fraction = phase;
    tx_missed_half_periods = missed;
    tx_max_lateness_cycles = worst;
    return true;
}
/* Keep waveform preparation in the RAM caller: flash veneers per segment
   otherwise push multi-block replies beyond the existing response deadline. */
static inline __attribute__((always_inline)) void add_segment(unsigned cycles, bool on) {
    waveform[waveform_len++] = (uint8_t)cycles | (on ? NFCV_WAVEFORM_CARRIER_FLAG : 0u);
}
NFCV_RAM_FUNCTION static void send_response(void) {
    if (!response.len || response.len > sizeof(last_tx) - ISO15693_CRC_SIZE)
        return;
    memcpy(last_tx, response.bytes, response.len);
    iso15693_append_crc(last_tx, response.len);
    last_tx_len = response.len + ISO15693_CRC_SIZE;
    unsigned rate = nfcv_session_high_response_rate() ? 1u : NFCV_TX_LOW_RATE_MULTIPLIER;
    waveform_len = 0;
    add_segment(NFCV_TX_LONG_SEGMENT_CYCLES * rate, false);
    add_segment(NFCV_TX_LONG_SEGMENT_CYCLES * rate, true);
    add_segment(NFCV_TX_BIT_HALF_CYCLES * rate, false);
    add_segment(NFCV_TX_BIT_HALF_CYCLES * rate, true);
    for (unsigned i = 0; i < last_tx_len; i++)
        for (unsigned b = 0; b < 8; b++) {
            bool one = (last_tx[i] >> b) & 1u;
            add_segment(NFCV_TX_BIT_HALF_CYCLES * rate, !one);
            add_segment(NFCV_TX_BIT_HALF_CYCLES * rate, one);
        }
    add_segment(NFCV_TX_BIT_HALF_CYCLES * rate, true);
    add_segment(NFCV_TX_BIT_HALF_CYCLES * rate, false);
    add_segment(NFCV_TX_LONG_SEGMENT_CYCLES * rate, true);
    add_segment(NFCV_TX_LONG_SEGMENT_CYCLES * rate, false);
    NRF_NFCT->TASKS_DISABLE = 1;
    __DSB();
    uint32_t when = observed_eof_cycles + NFCV_TX_EOF_REFERENCE_OFFSET_CYCLES +
                    NFCV_TX_RESPONSE_WAIT_CYCLES + (uint32_t)tx_timing_adjustment_cycles;
    if ((int32_t)(DWT->CYCCNT - when) > 0)
        stats[NFCV_STAT_TX_LATE_START]++;
    while ((int32_t)(DWT->CYCCNT - when) < 0) {
    }
    tx_deadline_cycles = DWT->CYCCNT;
    tx_fraction = 0;
    tx_missed_half_periods = tx_max_lateness_cycles = 0;
    if (!transmit_waveform()) {
        return;
    }
    while ((int32_t)(DWT->CYCCNT - tx_deadline_cycles) < 0) {
    }
    NRF_P1->OUTSET = gpio_mask;
    __DSB();
    stats[NFCV_STAT_TX_COUNT]++;
    stats[NFCV_STAT_TX_MISSED_HALF_PERIODS] += tx_missed_half_periods;
    if (tx_max_lateness_cycles > stats[NFCV_STAT_TX_MAX_LATE_CYCLES])
        stats[NFCV_STAT_TX_MAX_LATE_CYCLES] = tx_max_lateness_cycles;
#ifdef NFCV_DIAGNOSTICS
    stats[NFCV_STAT_TX_END_CYCLES] = DWT->CYCCNT;
    stats[NFCV_STAT_TX_END_TAG_STATE] = NRF_NFCT->NFCTAGSTATE;
    stats[NFCV_STAT_TX_END_FIELD] = NRF_NFCT->FIELDPRESENT;
#endif
    activate();
#ifdef NFCV_DIAGNOSTICS
    stats[NFCV_STAT_TX_RETURN_CYCLES] = DWT->CYCCNT - stats[NFCV_STAT_TX_END_CYCLES];
    stats[NFCV_STAT_TX_RETURN_FIELD] = NRF_NFCT->FIELDPRESENT;
#endif
    last_field_cycles = DWT->CYCCNT;
}
/* Physical PPM reconstruction: no command templates or phase search. */
NFCV_RAM_FUNCTION static bool receive_frame_or_slot(void) {
    uint32_t receive_started_cycles = DWT->CYCCNT, last_lock_fall_cycles = 0,
             sof_reference_cycles = 0, decoded_symbol_count = 0;
    bool previous_lock = (NRF_NFCT->FIELDPRESENT & NFCT_FIELDPRESENT_LOCKDETECT_Msk) != 0,
         sof_acquired = false, coding_1_of_256 = false;
    edge_count = 0;
    bool edge_overflow = false;
    memset(&frame, 0, sizeof(frame));
    /* Poll and timestamp field indicators before any decoding work. */
    while (DWT->CYCCNT - receive_started_cycles <
           (sof_acquired ? NFCV_RX_FRAME_TIMEOUT_CYCLES : NFCV_RX_SOF_TIMEOUT_CYCLES)) {
        uint32_t now_cycles = DWT->CYCCNT, field_indicators = NRF_NFCT->FIELDPRESENT;
        bool current_lock = (field_indicators & NFCT_FIELDPRESENT_LOCKDETECT_Msk) != 0;
        if (!(field_indicators & NFCT_FIELDPRESENT_FIELDPRESENT_Msk))
            return false;
        last_field_cycles = now_cycles;
        if (previous_lock != current_lock) {
            if (edge_count < NFCV_RX_EDGE_CAPACITY)
                edges[edge_count++] = (now_cycles - receive_started_cycles) |
                                      (current_lock ? NFCV_RX_EDGE_LOCK_FLAG : 0u);
            else
                edge_overflow = true;
        }
        /* LOCK can merge adjacent PPM pulses separated by two ticks. The
           measured low interval is then four ticks instead of two. Only
           pos7 -> pos1 (or pos511 -> pos1) permits this within legal PPM;
           infer the second zero symbol from the measured interval, not CRC
           or a command template. CRC still validates the reconstructed frame. */
        if (!previous_lock && current_lock && sof_acquired && last_lock_fall_cycles) {
            uint32_t lock_low_width_cycles = now_cycles - last_lock_fall_cycles;
            uint32_t last_fall_tick =
                NFCV_PPM_TICK_FROM_CYCLES(last_lock_fall_cycles - sof_reference_cycles);
            unsigned last_fall_position = NFCV_PPM_PULSE_POSITION(last_fall_tick, coding_1_of_256);
            if (lock_low_width_cycles > NFCV_RX_MERGED_PULSE_MIN_CYCLES &&
                lock_low_width_cycles < NFCV_RX_MERGED_PULSE_MAX_CYCLES &&
                last_fall_position == (coding_1_of_256 ? NFCV_PPM_1_OF_256_LAST_POSITION
                                                       : NFCV_PPM_1_OF_4_LAST_POSITION) &&
                decoded_symbol_count < NFCV_PPM_MAX_SYMBOLS(coding_1_of_256)) {
                decoded_symbol_count++;
                stats[NFCV_STAT_RX_MERGED_PULSE_COUNT]++;
            }
        }
        if (previous_lock && !current_lock) {
            stats[NFCV_STAT_RX_LOCK_FALL_COUNT]++;
            uint32_t fall_gap_cycles = now_cycles - last_lock_fall_cycles;
            /* Acquire SOF, then decode one PPM symbol per falling transition. */
            if (!sof_acquired) {
                if (last_lock_fall_cycles &&
                    ((fall_gap_cycles > NFCV_RX_1_OF_4_SOF_MIN_CYCLES &&
                      fall_gap_cycles < NFCV_RX_1_OF_4_SOF_MAX_CYCLES) ||
                     (fall_gap_cycles > NFCV_RX_1_OF_256_SOF_MIN_CYCLES &&
                      fall_gap_cycles < NFCV_RX_1_OF_256_SOF_MAX_CYCLES))) {
                    coding_1_of_256 = fall_gap_cycles > NFCV_RX_SOF_CODING_SPLIT_CYCLES;
                    sof_reference_cycles = last_lock_fall_cycles;
                    decoded_symbol_count = 0;
                    sof_acquired = true;
                    memset(frame.bytes, 0, sizeof(frame.bytes));
                    stats[NFCV_STAT_RX_SOF]++;
                }
            } else {
                uint32_t elapsed_cycles = now_cycles - sof_reference_cycles;
                uint32_t pulse_tick = NFCV_PPM_TICK_FROM_CYCLES(elapsed_cycles);
                uint32_t expected_pulse_cycles = NFCV_PPM_CYCLES_FROM_TICK(pulse_tick);
                uint32_t tick_error_cycles = elapsed_cycles > expected_pulse_cycles
                                                 ? elapsed_cycles - expected_pulse_cycles
                                                 : expected_pulse_cycles - elapsed_cycles;
                unsigned symbol_index = NFCV_PPM_SYMBOL_INDEX(pulse_tick, coding_1_of_256);
                unsigned pulse_position = NFCV_PPM_PULSE_POSITION(pulse_tick, coding_1_of_256);
                if (tick_error_cycles <= NFCV_RX_TICK_TOLERANCE_CYCLES &&
                    symbol_index == decoded_symbol_count + 1u && (pulse_position & 1u) &&
                    decoded_symbol_count < NFCV_PPM_MAX_SYMBOLS(coding_1_of_256)) {
                    if (coding_1_of_256)
                        frame.bytes[decoded_symbol_count] = (pulse_position - 1u) / 2u;
                    else
                        frame.bytes[decoded_symbol_count / NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE] |=
                            ((pulse_position - 1u) / 2u)
                            << ((decoded_symbol_count % NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE) * 2u);
                    decoded_symbol_count++;
                } else if (tick_error_cycles <= NFCV_RX_TICK_TOLERANCE_CYCLES &&
                           symbol_index == decoded_symbol_count + 1u &&
                           pulse_position == NFCV_PPM_EOF_POSITION &&
                           decoded_symbol_count >= NFCV_PPM_MIN_FRAME_SYMBOLS(coding_1_of_256) &&
                           (coding_1_of_256 ||
                            !(decoded_symbol_count % NFCV_PPM_1_OF_4_SYMBOLS_PER_BYTE))) {
                    /* Complete bytes and CRC are required before command dispatch. */
                    frame.byte_count = NFCV_PPM_FRAME_BYTES(decoded_symbol_count, coding_1_of_256);
                    frame.crc_ok = iso15693_check_crc(frame.bytes, frame.byte_count);
                    frame.high_data_rate = (frame.bytes[0] & ISO15693_REQ_HIGH_RATE) != 0;
                    frame.single_subcarrier = (frame.bytes[0] & ISO15693_REQ_DUAL_SUBCARRIER) == 0;
                    stats[frame.crc_ok ? NFCV_STAT_RX_OK : NFCV_STAT_RX_CRC_ERROR]++;
                    stats[NFCV_STAT_RX_LAST_BYTES] = frame.byte_count;
                    stats[NFCV_STAT_RX_LAST_COMMAND] = frame.bytes[1];
                    stats[NFCV_STAT_RX_LAST_CODING] = coding_1_of_256 ? 256u : 4u;
                    last_rx_len = frame.byte_count;
                    memcpy(last_rx, frame.bytes, last_rx_len);
                    saved_edge_count = edge_count;
                    memcpy(saved_edges, edges, edge_count * sizeof(uint32_t));
                    stats[NFCV_STAT_RX_LAST_CRC_OK] = frame.crc_ok;
                    stats[NFCV_STAT_RX_EDGE_OVERFLOW] = edge_overflow;
                    stats[NFCV_STAT_RX_SOF_OFFSET] = sof_reference_cycles - receive_started_cycles;
                    observed_eof_cycles = now_cycles;
                    return frame.crc_ok;
                } else {
                    stats[NFCV_STAT_RX_MALFORMED]++;
                    stats[NFCV_STAT_RX_ERROR_ELAPSED] = elapsed_cycles;
                    stats[NFCV_STAT_RX_ERROR_TICK] = pulse_tick;
                    stats[NFCV_STAT_RX_ERROR_SYMBOL_COUNT] = decoded_symbol_count;
                    stats[NFCV_STAT_RX_ERROR_DISTANCE] = tick_error_cycles;
                    stats[NFCV_STAT_RX_ERROR_SYMBOL_INDEX] = symbol_index;
                    stats[NFCV_STAT_RX_ERROR_POSITION] = pulse_position;
                    last_rx_len = NFCV_PPM_FRAME_BYTES(decoded_symbol_count, coding_1_of_256);
                    memcpy(last_rx, frame.bytes, last_rx_len);
                    saved_edge_count = edge_count;
                    memcpy(saved_edges, edges, edge_count * sizeof(uint32_t));
                    sof_acquired = false;
                    last_lock_fall_cycles = 0;
                }
            }
            last_lock_fall_cycles = now_cycles;
        }
        /* A single EOF-only pulse advances a pending 16-slot Inventory. Wait
           beyond both SOF gaps so a new command never masquerades as a slot. */
        if (!sof_acquired && last_lock_fall_cycles && nfcv_session_inventory_pending() &&
            now_cycles - last_lock_fall_cycles > NFCV_RX_SLOT_ADVANCE_GUARD_CYCLES) {
            observed_eof_cycles = last_lock_fall_cycles;
            frame.byte_count = 0;
            stats[NFCV_STAT_RX_SLOT_ADVANCE]++;
            return true;
        }
        if (sof_acquired && now_cycles - last_lock_fall_cycles >
                                (coding_1_of_256 ? NFCV_RX_1_OF_256_GAP_TIMEOUT_CYCLES
                                                 : NFCV_RX_1_OF_4_GAP_TIMEOUT_CYCLES)) {
            stats[NFCV_STAT_RX_MALFORMED]++;
#ifdef NFCV_DIAGNOSTICS
            stats[NFCV_STAT_RX_ERROR_ELAPSED] = now_cycles - sof_reference_cycles;
            stats[NFCV_STAT_RX_ERROR_TICK] =
                NFCV_PPM_TICK_FROM_CYCLES(now_cycles - sof_reference_cycles);
            stats[NFCV_STAT_RX_ERROR_SYMBOL_COUNT] = decoded_symbol_count;
            stats[NFCV_STAT_RX_ERROR_DISTANCE] = now_cycles - last_lock_fall_cycles;
            stats[NFCV_STAT_RX_ERROR_SYMBOL_INDEX] = stats[NFCV_STAT_RX_ERROR_POSITION] = 0;
            stats[NFCV_STAT_RX_ERROR_SINCE_TX] = now_cycles - stats[NFCV_STAT_TX_END_CYCLES];
            stats[NFCV_STAT_RX_ERROR_SOF_SINCE_TX] =
                sof_reference_cycles - stats[NFCV_STAT_TX_END_CYCLES];
            stats[NFCV_STAT_RX_ERROR_BEGIN_SINCE_TX] =
                receive_started_cycles - stats[NFCV_STAT_TX_END_CYCLES];
            last_rx_len = NFCV_PPM_FRAME_BYTES(decoded_symbol_count, coding_1_of_256);
            memcpy(last_rx, frame.bytes, last_rx_len);
            saved_edge_count = edge_count;
            memcpy(saved_edges, edges, edge_count * sizeof(uint32_t));
#endif
            sof_acquired = false;
            last_lock_fall_cycles = 0;
        }
        previous_lock = current_lock;
    }
    return false;
}

bool nfcv_phy_start(void) {
    RF_PHASE(NFCV_PHASE_SESSION_INIT);
    enabled = true;
    nfcv_session_init();
    RF_PHASE(NFCV_PHASE_DRIVER_INIT);
    nrfx_nfct_config_t config = {.rxtx_int_mask = 0, .cb = NULL};
    if (nrfx_nfct_init(&config) != NRFX_SUCCESS) {
        enabled = false;
        return false;
    }
    RF_PHASE(NFCV_PHASE_DRIVER_RELEASE);
    nrfx_nfct_uninit();
    RF_PHASE(NFCV_PHASE_GPIO_INIT);
    nrf_gpio_cfg_output(READER_POWER);
    nrf_gpio_pin_clear(READER_POWER);
    nrf_gpio_cfg_output(LF_ANT_DRIVER);
    nrf_gpio_pin_clear(LF_ANT_DRIVER);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint32_t clock_begin = DWT->CYCCNT;
    while (!nrf_clock_hf_is_running(NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
        if (DWT->CYCCNT - clock_begin > NFCV_CLOCK_TIMEOUT_CYCLES) {
            stats[NFCV_STAT_CLOCK_TIMEOUT]++;
            enabled = false;
            return false;
        }
    }
    nrf_gpio_cfg(HF_ANT_SEL, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
                 NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
    nrf_gpio_pin_set(HF_ANT_SEL);
    gpio_mask = 1u << (HF_ANT_SEL & 31u);
    NRF_NFCT->INTENCLR = 0xffffffffu;
    NRF_NFCT->AUTOCOLRESCONFIG |= 1u;
    NRF_NFCT->SHORTS = 0;
    last_field_cycles = DWT->CYCCNT;
    sense();
    RF_PHASE(NFCV_PHASE_SENSING);
    return true;
}

void nfcv_phy_poll(void) {
    if (!enabled)
        return;
    RF_PHASE(NFCV_PHASE_POLL);
    stats[NFCV_STAT_POLLS]++;
    uint32_t now = DWT->CYCCNT;
    if (NRF_NFCT->NFCTAGSTATE < 3u) {
        if (now - sense_started_cycles >= NFCV_ACTIVATE_TIMEOUT_CYCLES)
            activate();
        else
            return;
    }
    if (!(NRF_NFCT->FIELDPRESENT & NFCT_FIELDPRESENT_FIELDPRESENT_Msk)) {
        if (now - last_field_cycles > NFCV_FIELD_LOSS_GUARD_CYCLES) {
            nfcv_session_reset_on_field_loss();
            sense();
            last_field_cycles = now;
        }
        return;
    }
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    RF_PHASE(NFCV_PHASE_RECEIVE);
    bool complete = receive_frame_or_slot();
    RF_PHASE(NFCV_PHASE_DISPATCH);
    if (complete) {
        bool reply = frame.byte_count ? nfcv_session_handle_frame(&frame, &response)
                                      : nfcv_session_advance_inventory_slot(&response);
        if (reply)
            send_response();
    }
    __set_PRIMASK(primask);
}

void nfcv_phy_stop(void) {
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    enabled = false;
    NRF_NFCT->TASKS_DISABLE = 1;
    nrf_gpio_pin_set(HF_ANT_SEL);
    __set_PRIMASK(irq);
}

bool nfcv_phy_field_present(void) {
    return enabled && (NRF_NFCT->FIELDPRESENT & NFCT_FIELDPRESENT_FIELDPRESENT_Msk);
}

const uint32_t *nfcv_phy_get_stats(void) {
    return stats;
}
#ifdef NFCV_DIAGNOSTICS
size_t nfcv_phy_copy_rx_edges(uint8_t *out, size_t capacity) {
    size_t length = saved_edge_count * sizeof(uint32_t);
    if (length > capacity)
        return 0;
    memcpy(out, saved_edges, length);
    return length;
}

bool nfcv_phy_test_response_delay(uint32_t cycles) {
    if (!enabled || nfcv_phy_field_present() || cycles < NFCV_TX_TEST_DELAY_MIN_CYCLES ||
        cycles > NFCV_TX_TEST_DELAY_MAX_CYCLES)
        return false;
    tx_timing_adjustment_cycles = (int32_t)cycles - NFCV_TX_UNADJUSTED_DELAY_CYCLES;
    return true;
}
size_t nfcv_phy_format_diagnostics(char *out, size_t capacity) {
    int n = snprintf(out, capacity,
                     "\"txLateStart\":%lu,\"lateHalfPeriods\":%lu,\"maxLateCycles\":%lu,"
                     "\"rxMalformed\":%lu,\"lastCommand\":%lu,\"lastRx\":\"",
                     (unsigned long)stats[NFCV_STAT_TX_LATE_START],
                     (unsigned long)stats[NFCV_STAT_TX_MISSED_HALF_PERIODS],
                     (unsigned long)stats[NFCV_STAT_TX_MAX_LATE_CYCLES],
                     (unsigned long)stats[NFCV_STAT_RX_MALFORMED],
                     (unsigned long)stats[NFCV_STAT_RX_LAST_COMMAND]);
    if (n < 0 || (size_t)n >= capacity)
        return 0;
    size_t used = (size_t)n;
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < last_rx_len; i++) {
        if (used + 2 >= capacity)
            return 0;
        out[used++] = hex[last_rx[i] >> 4];
        out[used++] = hex[last_rx[i] & 15u];
    }
    const char middle[] = "\",\"lastTx\":\"";
    if (used + sizeof(middle) >= capacity)
        return 0;
    memcpy(out + used, middle, sizeof(middle) - 1);
    used += sizeof(middle) - 1;
    for (unsigned i = 0; i < last_tx_len; i++) {
        if (used + 2 >= capacity)
            return 0;
        out[used++] = hex[last_tx[i] >> 4];
        out[used++] = hex[last_tx[i] & 15u];
    }
    if (used + 2 >= capacity)
        return 0;
    out[used++] = '"';
    n = snprintf(
        out + used, capacity - used,
        ",\"rxErrorElapsed\":%lu,\"rxErrorTick\":%lu,\"rxErrorSymbols\":%lu,\"rxErrorDistance\":%"
        "lu,\"rxErrorSymbol\":%lu,\"rxErrorPosition\":%lu,\"replyDelayCycles\":%ld",
        (unsigned long)stats[NFCV_STAT_RX_ERROR_ELAPSED],
        (unsigned long)stats[NFCV_STAT_RX_ERROR_TICK],
        (unsigned long)stats[NFCV_STAT_RX_ERROR_SYMBOL_COUNT],
        (unsigned long)stats[NFCV_STAT_RX_ERROR_DISTANCE],
        (unsigned long)stats[NFCV_STAT_RX_ERROR_SYMBOL_INDEX],
        (unsigned long)stats[NFCV_STAT_RX_ERROR_POSITION],
        (long)(NFCV_TX_UNADJUSTED_DELAY_CYCLES + tx_timing_adjustment_cycles));
    if (n < 0 || (size_t)n >= capacity - used)
        return 0;
    used += (size_t)n;
    n = snprintf(out + used, capacity - used,
                 ",\"activateCycles\":%lu,\"maxActivateCycles\":%lu,\"rxErrorSinceTx\":%lu,"
                 "\"rxErrorSofSinceTx\":%lu,\"rxErrorBeginSinceTx\":%lu,\"txEndState\":%lu,"
                 "\"txEndField\":%lu,\"txReturnCycles\":%lu,\"txReturnField\":%lu",
                 (unsigned long)stats[NFCV_STAT_ACTIVATE_CYCLES],
                 (unsigned long)stats[NFCV_STAT_ACTIVATE_MAX_CYCLES],
                 (unsigned long)stats[NFCV_STAT_RX_ERROR_SINCE_TX],
                 (unsigned long)stats[NFCV_STAT_RX_ERROR_SOF_SINCE_TX],
                 (unsigned long)stats[NFCV_STAT_RX_ERROR_BEGIN_SINCE_TX],
                 (unsigned long)stats[NFCV_STAT_TX_END_TAG_STATE],
                 (unsigned long)stats[NFCV_STAT_TX_END_FIELD],
                 (unsigned long)stats[NFCV_STAT_TX_RETURN_CYCLES],
                 (unsigned long)stats[NFCV_STAT_TX_RETURN_FIELD]);
    if (n < 0 || (size_t)n >= capacity - used)
        return 0;
    return used + (size_t)n;
}
#endif
