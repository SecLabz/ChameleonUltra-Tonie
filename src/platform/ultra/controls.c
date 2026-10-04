#include "platform/platform.h"
#include "platform/ultra/board.h"
#include "platform/ultra/internal.h"
#include "core/app.h"
#include "core/button.h"
#include "nrf.h"
#include "nrf_gpio.h"

static uint32_t battery_until;
static const uint8_t leds[FAVORITE_SLOT_COUNT] = {20, 17, 15, 13, 12, 41, 8, 6};
/* Ultra RGB cathodes are shared and active low (upstream common/hw_connect.c).
 * Slot order: orange, blue, green, yellow, magenta, cyan, white, red.
 * PWM0 mixes orange without CPU interrupts or changes to the RF timers. */
static const uint8_t slot_colors[FAVORITE_SLOT_COUNT] = {1, 4, 2, 3, 5, 6, 7, 1};
static uint16_t orange_green = 25;
static void show_slot_color(unsigned slot) {
    if (NRF_PWM0->ENABLE) {
        NRF_PWM0->EVENTS_STOPPED = 0;
        NRF_PWM0->TASKS_STOP = 1;
        while (!NRF_PWM0->EVENTS_STOPPED)
            platform_watchdog_feed();
        NRF_PWM0->ENABLE = 0;
        NRF_PWM0->PSEL.OUT[0] = 0xffffffffu;
    }
    const unsigned pins[3] = {RGB_RED, RGB_GREEN, RGB_BLUE};
    for (unsigned i = 0; i < 3; i++)
        nrf_gpio_pin_write(pins[i], !(slot_colors[slot] & (1u << i)));
    if (slot == 0) {
        /* Rising-edge polarity: green cathode stays low for 25% of each period. */
        NRF_PWM0->PSEL.OUT[0] = RGB_GREEN;
        for (unsigned i = 1; i < 4; i++)
            NRF_PWM0->PSEL.OUT[i] = 0xffffffffu;
        NRF_PWM0->MODE = 0;
        NRF_PWM0->PRESCALER = 4;
        NRF_PWM0->COUNTERTOP = 100;
        NRF_PWM0->DECODER = 0;
        NRF_PWM0->LOOP = 1;
        for (unsigned i = 0; i < 2; i++) {
            NRF_PWM0->SEQ[i].PTR = (uint32_t)&orange_green;
            NRF_PWM0->SEQ[i].CNT = 1;
            NRF_PWM0->SEQ[i].REFRESH = 0;
            NRF_PWM0->SEQ[i].ENDDELAY = 0;
        }
        NRF_PWM0->INTENCLR = 0xffffffffu;
        NRF_PWM0->SHORTS = PWM_SHORTS_LOOPSDONE_SEQSTART0_Msk;
        NRF_PWM0->ENABLE = 1;
        NRF_PWM0->TASKS_SEQSTART[0] = 1;
    }
}
/* Ultra slot LEDs are active high. Selection remains visible while paused. */
void platform_show_selection(const uint32_t f[FAVORITE_SLOT_COUNT], uint32_t id) {
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
        nrf_gpio_pin_clear(leds[i]);
    unsigned slot = 0;
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
        if (id && f[i] == id) {
            slot = i;
            break;
        }
    show_slot_color(slot);
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
        nrf_gpio_pin_write(leds[i], id && f[i] == id);
}

void ultra_leds_init(void) {
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++) {
        nrf_gpio_cfg_output(leds[i]);
        nrf_gpio_pin_clear(leds[i]);
    }
}

void ultra_leds_off(void) {
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
        nrf_gpio_pin_clear(leds[i]);
    show_slot_color(7);
    nrf_gpio_pin_clear(RGB_RED);
    nrf_gpio_pin_clear(RGB_GREEN);
    nrf_gpio_pin_clear(RGB_BLUE);
}

void ultra_controls_poll(uint32_t now) {
    static button_state_t buttons[2];
    const unsigned pins[] = {BUTTON_A, BUTTON_B};
    for (unsigned i = 0; i < 2; i++) {
        button_event_t event = button_poll(&buttons[i], nrf_gpio_pin_read(pins[i]), now);
        /* Do not enter System OFF with a held button already asserting wake. */
        if (buttons[i].pressed)
            app_note_activity();
        if (event == BUTTON_EVENT_SHORT)
            app_select_next_favorite(i ? 1 : -1);
        else if (event == BUTTON_EVENT_HOLD) {
            if (!i) {
                battery_until = now + 2000;
                for (unsigned j = 0; j < FAVORITE_SLOT_COUNT; j++)
                    nrf_gpio_pin_clear(leds[j]);
                show_slot_color(2);
                for (unsigned j = 0; j < FAVORITE_SLOT_COUNT; j++)
                    nrf_gpio_pin_write(leds[j], j < (platform_battery_percent() * 8 + 99) / 100);
            }
            app_note_activity();
        }
    }
    if (battery_until && (int32_t)(now - battery_until) >= 0) {
        battery_until = 0;
        platform_show_selection(app_state.library.favorites, app_state.selected);
    }
}
