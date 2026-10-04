#include "platform/platform.h"
#include "platform/ultra/board.h"
#include "platform/ultra/internal.h"
#include "core/app.h"
#include "tag/nfcv_emulation.h"
#include "nrf.h"
#include "nrf_gpio.h"

#ifdef NFCV_DIAGNOSTICS
static volatile uint32_t idle_wakeups;
uint32_t ultra_idle_wake_count(void) {
    return idle_wakeups;
}
#endif

void RTC1_IRQHandler(void) {
    if (NRF_RTC1->EVENTS_COMPARE[0]) {
        NRF_RTC1->EVENTS_COMPARE[0] = 0;
        NRF_RTC1->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
        NRF_RTC1->EVTENCLR = RTC_EVTENCLR_COMPARE0_Msk;
#ifdef NFCV_DIAGNOSTICS
        idle_wakeups++;
#endif
    }
}

static void idle_event(void) {
    /* Wake within 8 ms to poll buttons and advance the standby deadline. */
    if (!(NRF_RTC1->INTENSET & RTC_INTENSET_COMPARE0_Msk)) {
        NRF_RTC1->EVENTS_COMPARE[0] = 0;
        NRF_RTC1->CC[0] = (NRF_RTC1->COUNTER + 262u) & 0xffffffu;
        NVIC_ClearPendingIRQ(RTC1_IRQn);
        NVIC_SetPriority(RTC1_IRQn, 7);
        NVIC_EnableIRQ(RTC1_IRQn);
        NRF_RTC1->EVTENSET = RTC_EVTENSET_COMPARE0_Msk;
        NRF_RTC1->INTENSET = RTC_INTENSET_COMPARE0_Msk;
    }
    /* Other IRQs may wake early; keep the deadline instead of postponing it. */
    __DSB();
    __WFE();
}
#ifdef NFCV_DIAGNOSTICS
bool platform_test_idle_wake(void) {
    uint32_t before = idle_wakeups, begin = platform_millis();
    while (idle_wakeups == before && platform_millis() - begin < 100u) {
        platform_watchdog_feed();
        idle_event();
    }
    return idle_wakeups != before;
}
#endif

void platform_idle(bool emulating) {
    if (platform_usb_power_present())
        return;
    if (platform_millis() - app_state.last_activity < 60000) {
        if (!emulating)
            idle_event();
        return;
    }
    /* SYSTEMOFF retains flash only; all reader writes disappear. */
    nfcv_emulation_stop();
    ultra_leds_off();
    nrf_gpio_cfg_sense_input(BUTTON_A, NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
    nrf_gpio_cfg_sense_input(BUTTON_B, NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);
    NRF_POWER->GPREGRET = 0;
    NRF_NFCT->INTENCLR = 0xffffffff;
    NRF_NFCT->TASKS_SENSE = 1;
    __DSB();
    NRF_POWER->SYSTEMOFF = 1;
    __DSB();
    for (;;)
        __WFE();
}
