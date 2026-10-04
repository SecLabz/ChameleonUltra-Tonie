#include "tag/phy/nfcv/diagnostics.h"
#include "platform/ultra/board.h"
#include "platform/ultra/internal.h"
#include "tag/phy/nfcv/phy.h"
#include "tag/nfcv_emulation.h"
#include "platform/platform.h"
#include "core/app.h"
#include "config/flash_layout.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "tusb.h"
#include "app_timer.h"
#include <stdio.h>

nfct_close_timer_t *nfct_close_timer;
static uint32_t boot_deadline, battery_at;
#ifdef NFCV_DIAGNOSTICS
static uint32_t remote_deadline;
static uint32_t previous_rf_phase;
static volatile struct {
    uint32_t magic, line, pc;
} fault_record __attribute__((section(".noinit")));
#endif
void platform_note_control_activity(void) {
#ifdef NFCV_DIAGNOSTICS
    remote_deadline = platform_millis() + 90000;
#endif
}

#ifdef NFCV_DIAGNOSTICS
void __assert_func(const char *file, int line, const char *function, const char *expression) {
    (void)file;
    (void)function;
    (void)expression;
    fault_record.line = line;
    fault_record.pc = (uint32_t)__builtin_return_address(0);
    fault_record.magic = 0x46554e44;
    __DSB();
    NRF_POWER->GPREGRET2 = 0x40u | (NRF_POWER->GPREGRET2 & 0x3fu);
    NRF_POWER->GPREGRET = 0xb1;
    NVIC_SystemReset();
    for (;;) {
    }
}

void HardFault_Handler(void) {
    NRF_POWER->GPREGRET2 = 0xc0u | (NRF_POWER->GPREGRET2 & 0x3fu);
    NRF_POWER->GPREGRET = 0xb1;
    NVIC_SystemReset();
    for (;;) {
    }
}
size_t platform_format_diagnostics(char *out, size_t capacity) {
    const ultra_battery_diagnostics_t *battery = ultra_battery_diagnostics();
    bool recorded = fault_record.magic == 0x46554e44;
    const uint32_t *s = nfcv_phy_get_stats();
    int n = snprintf(
        out, capacity,
        "{\"rtc\":%lu,\"lfClock\":%lu,\"lfSource\":%lu,\"hfClock\":%lu,\"cycles\":%lu,"
        "\"resetReason\":%lu,\"rfPhase\":%lu,\"previousRfPhase\":%lu,\"assertLine\":%lu,"
        "\"assertPc\":%lu,\"controlClock\":\"RTC\",\"field\":%lu,\"tagState\":%lu,\"polls\":%lu,"
        "\"rxOk\":%lu,\"rxCrcError\":%lu,\"tx\":%lu,\"activateTimeout\":%lu}",
        (unsigned long)NRF_RTC1->COUNTER, (unsigned long)NRF_CLOCK->LFCLKSTAT,
        (unsigned long)NRF_CLOCK->LFCLKSRC, (unsigned long)NRF_CLOCK->HFCLKSTAT,
        (unsigned long)DWT->CYCCNT, (unsigned long)NRF_POWER->RESETREAS,
        (unsigned long)NRF_POWER->GPREGRET2, (unsigned long)previous_rf_phase,
        (unsigned long)(recorded ? fault_record.line : 0),
        (unsigned long)(recorded ? fault_record.pc : 0), (unsigned long)NRF_NFCT->FIELDPRESENT,
        (unsigned long)NRF_NFCT->NFCTAGSTATE, (unsigned long)s[NFCV_STAT_POLLS],
        (unsigned long)s[NFCV_STAT_RX_OK], (unsigned long)s[NFCV_STAT_RX_CRC_ERROR],
        (unsigned long)s[NFCV_STAT_TX_COUNT], (unsigned long)s[NFCV_STAT_ACTIVATE_TIMEOUT]);
    if (n <= 0 || (size_t)n >= capacity)
        return 0;
    size_t used = (size_t)n - 1;
    n = snprintf(out + used, capacity - used,
                 ",\"batteryMv\":%d,\"batteryFirstAdc\":%d,\"batteryAverageAdc\":%d,"
                 "\"batteryTimeouts\":%u,\"idleWakeups\":%lu,",
                 battery->millivolts, battery->first_adc, battery->average_adc, battery->timeouts,
                 (unsigned long)ultra_idle_wake_count());
    if (n < 0 || (size_t)n >= capacity - used)
        return 0;
    used += (size_t)n;
    size_t extra = nfcv_phy_format_diagnostics(out + used, capacity - used);
    if (!extra || used + extra + 2 > capacity)
        return 0;
    used += extra;
    out[used++] = '}';
    out[used] = 0;
    return used;
}
#endif
void platform_watchdog_feed(void) {
    if (NRF_WDT->RUNSTATUS)
        NRF_WDT->RR[0] = 0x6e524635;
}

bool platform_usb_power_present(void) {
    return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}
void platform_request_bootloader(void) {
    boot_deadline = platform_millis() + 300;
}

void platform_init(void) {
#ifdef NFCV_DIAGNOSTICS
    previous_rf_phase = NRF_POWER->GPREGRET2;
#endif
    SCB->VTOR = APP_FLASH_START;
    NRF_POWER->GPREGRET = 0xb1;
    NRF_WDT->CONFIG = 1;
    NRF_WDT->CRV = 32768 * 10;
    NRF_WDT->RREN = 1;
    NRF_WDT->TASKS_START = 1;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    ultra_clock_init();
    ultra_leds_init();
    const unsigned pins[] = {RGB_RED,    RGB_GREEN,    RGB_BLUE,      FIELD_LED,
                             HF_ANT_SEL, READER_POWER, LF_ANT_DRIVER, BOARD_AUX_OUTPUT};
    for (unsigned i = 0; i < 8; i++)
        nrf_gpio_cfg_output(pins[i]);
    nrf_gpio_pin_set(RGB_RED);
    nrf_gpio_pin_clear(RGB_GREEN);
    nrf_gpio_pin_clear(RGB_BLUE);
    nrf_gpio_pin_set(FIELD_LED);
    nrf_gpio_pin_set(HF_ANT_SEL);
    nrf_gpio_pin_clear(READER_POWER);
    nrf_gpio_pin_clear(LF_ANT_DRIVER);
    nrf_gpio_pin_clear(BOARD_AUX_OUTPUT);
    nrf_gpio_cfg_input(BUTTON_A, NRF_GPIO_PIN_PULLDOWN);
    nrf_gpio_cfg_input(BUTTON_B, NRF_GPIO_PIN_PULLDOWN);
    platform_usb_init();
    ultra_sample_battery();
    platform_note_control_activity();
}

void platform_poll(void) {
    uint32_t now = platform_millis();
    platform_watchdog_feed();
    if (!platform_usb_power_present())
        ultra_start_hf_clock();
#ifdef NFCV_DIAGNOSTICS
    if (remote_deadline && (int32_t)(now - remote_deadline) >= 0) {
        NRF_POWER->GPREGRET = 0xb1;
        NVIC_SystemReset();
    }
#endif
    if (boot_deadline && (int32_t)(now - boot_deadline) >= 0) {
        NRF_POWER->GPREGRET = 0xb1;
        NVIC_SystemReset();
    }
    if (nfct_close_timer && nfct_close_timer->active &&
        (int32_t)(now - nfct_close_timer->deadline) >= 0) {
        nfct_close_timer->active = false;
        nfct_close_timer->callback(nfct_close_timer->context);
    }
    ultra_controls_poll(now);
    if (now - battery_at >= 30000) {
        battery_at = now;
        ultra_sample_battery();
    }
    if (nfcv_emulation_field_present())
        app_note_activity();
    nrf_gpio_pin_write(FIELD_LED, !nfcv_emulation_field_present());
}
