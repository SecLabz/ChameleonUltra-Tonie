#include "platform/platform.h"
#include "platform/ultra/internal.h"
#include "nrf.h"

static uint64_t ticks;
static uint32_t previous_rtc;

void ultra_start_hf_clock(void) {
    const uint32_t mask = CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Msk;
    if ((NRF_CLOCK->HFCLKSTAT & mask) == mask)
        return;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    while ((NRF_CLOCK->HFCLKSTAT & mask) != mask) {
    }
}

static void rtc_settle(void) {
    uint32_t begin = DWT->CYCCNT;
    while (DWT->CYCCNT - begin < 64000u)
        platform_watchdog_feed();
}

static void wait_lf_clock(bool running) {
    uint32_t begin = DWT->CYCCNT;
    while (!!(NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk) != running) {
        if (DWT->CYCCNT - begin >= 6400000u) {
#ifdef NFCV_DIAGNOSTICS
            NRF_POWER->GPREGRET2 = running ? 0x71 : 0x70;
            return;
#else
            NRF_POWER->GPREGRET = 0xb1;
            NVIC_SystemReset();
#endif
        }
    }
}
uint32_t platform_millis(void) {
    uint32_t irq = __get_PRIMASK();
    __disable_irq();
    uint32_t rtc = NRF_RTC1->COUNTER;
    ticks += (rtc - previous_rtc) & 0xffffff;
    previous_rtc = rtc;
    uint32_t value = ticks * 1000 / 32768;
    __set_PRIMASK(irq);
    return value;
}

void ultra_clock_init(void) {
    ultra_start_hf_clock();
    /* Stop inherited clock IRQs; retain an already running 32 kHz source. */
    NRF_CLOCK->INTENCLR = 0xffffffffu;
    if (!(NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk))
        NRF_CLOCK->LFCLKSRC = CLOCK_LFCLKSRC_SRC_RC;
    /* A bootloader can leave status latched while its LF start request retires. */
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_LFCLKSTART = 1;
    wait_lf_clock(true);
    /* RTC tasks cross the 32 kHz clock domain; allow STOP and CLEAR to settle. */
    NVIC_DisableIRQ(RTC1_IRQn);
    NVIC_ClearPendingIRQ(RTC1_IRQn);
    NRF_RTC1->TASKS_STOP = 1;
    rtc_settle();
    NRF_RTC1->INTENCLR = 0xffffffffu;
    NRF_RTC1->EVTENCLR = 0xffffffffu;
    NRF_RTC1->TASKS_CLEAR = 1;
    NRF_RTC1->PRESCALER = 0;
    rtc_settle();
    NRF_RTC1->TASKS_START = 1;
    rtc_settle();
}
