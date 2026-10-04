#include "platform/platform.h"
#include "platform/ultra/board.h"
#include "platform/ultra/internal.h"
#include "tag/nfcv_emulation.h"
#include "nrf.h"
#include "nrf_gpio.h"

static unsigned battery;
#ifdef NFCV_DIAGNOSTICS
static ultra_battery_diagnostics_t battery_diagnostics;
const ultra_battery_diagnostics_t *ultra_battery_diagnostics(void) {
    return &battery_diagnostics;
}
#endif

/* Each hardware wait is bounded so battery sensing cannot strand USB control. */
static bool adc_wait(volatile uint32_t *event) {
    uint32_t begin = DWT->CYCCNT;
    while (!*event)
        if (DWT->CYCCNT - begin >= 128000u)
            return false;
    return true;
}

void ultra_sample_battery(void) {
    if (nfcv_emulation_field_present())
        return;
    static int16_t samples[9];
    static bool calibrated;
    nrf_gpio_cfg_default(BATTERY_SENSE); /* Ultra battery divider: P0.04 / AIN2. */
    NRF_SAADC->ENABLE = 0;
    NRF_SAADC->INTENCLR = 0xffffffffu;
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_14bit;
    NRF_SAADC->OVERSAMPLE = 0;
    NRF_SAADC->SAMPLERATE = 0;
    for (unsigned i = 0; i < 8; i++) {
        NRF_SAADC->CH[i].PSELP = 0;
        NRF_SAADC->CH[i].PSELN = 0;
    }
    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_AnalogInput2;
    NRF_SAADC->CH[0].CONFIG = (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
                              (SAADC_CH_CONFIG_TACQ_40us << SAADC_CH_CONFIG_TACQ_Pos);
    NRF_SAADC->ENABLE = 1;
    bool ok = true;
    if (!calibrated) {
        NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
        NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
        ok = adc_wait(&NRF_SAADC->EVENTS_CALIBRATEDONE);
        calibrated = ok;
    }
    if (ok) {
        NRF_SAADC->RESULT.PTR = (uint32_t)samples;
        NRF_SAADC->RESULT.MAXCNT = 9;
        NRF_SAADC->EVENTS_STARTED = 0;
        NRF_SAADC->EVENTS_END = 0;
        NRF_SAADC->TASKS_START = 1;
        ok = adc_wait(&NRF_SAADC->EVENTS_STARTED);
        for (unsigned i = 0; ok && i < 9; i++) {
            NRF_SAADC->EVENTS_DONE = 0;
            NRF_SAADC->TASKS_SAMPLE = 1;
            ok = adc_wait(&NRF_SAADC->EVENTS_DONE);
        }
        if (ok)
            ok = adc_wait(&NRF_SAADC->EVENTS_END);
    }
    NRF_SAADC->EVENTS_STOPPED = 0;
    NRF_SAADC->TASKS_STOP = 1;
    bool stopped = adc_wait(&NRF_SAADC->EVENTS_STOPPED);
    NRF_SAADC->ENABLE = 0;
    if (!ok || !stopped) {
#ifdef NFCV_DIAGNOSTICS
        battery_diagnostics.timeouts++;
#endif
        return;
    }
    /* Discard initial acquisition, then average eight fully settled samples. */
    int sum = 0;
    for (unsigned i = 1; i < 9; i++)
        sum += samples[i];
    int average = (sum + 4) / 8;
    int mv = average * 7200 / 16383 + 100;
#ifdef NFCV_DIAGNOSTICS
    battery_diagnostics.millivolts = mv;
    battery_diagnostics.first_adc = samples[0];
    battery_diagnostics.average_adc = average;
#endif
    static const unsigned volts[] = {3400, 3644, 3754, 3824, 3904, 4034, 4200},
                          percent[] = {0, 5, 20, 40, 60, 80, 100};
    battery = 0;
    for (unsigned i = 1; i < 7; i++)
        if (mv >= (int)volts[i])
            battery = percent[i];
        else if (mv > (int)volts[i - 1]) {
            battery = percent[i - 1] + (mv - volts[i - 1]) * (percent[i] - percent[i - 1]) /
                                           (volts[i] - volts[i - 1]);
            break;
        }
}

unsigned platform_battery_percent(void) {
    return battery;
}
