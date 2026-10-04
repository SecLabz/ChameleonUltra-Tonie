#pragma once
#include <stdint.h>

/* Private Ultra services used by the platform orchestrator. */
void ultra_clock_init(void);
void ultra_start_hf_clock(void);
void ultra_sample_battery(void);
void ultra_leds_init(void);
void ultra_leds_off(void);
void ultra_controls_poll(uint32_t now);
#ifdef NFCV_DIAGNOSTICS
typedef struct {
    int millivolts, first_adc, average_adc;
    unsigned timeouts;
} ultra_battery_diagnostics_t;
const ultra_battery_diagnostics_t *ultra_battery_diagnostics(void);
uint32_t ultra_idle_wake_count(void);
#endif
