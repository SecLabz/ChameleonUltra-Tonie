#pragma once
#include "platform/flash.h"
#include "core/limits.h"
void platform_init(void);
void platform_usb_init(void);
uint32_t platform_millis(void);
void platform_watchdog_feed(void);
flash_io_t platform_flash_io(void);
void platform_poll(void);
void platform_request_bootloader(void);
void platform_idle(bool emulating);
void platform_show_selection(const uint32_t favorites[FAVORITE_SLOT_COUNT], uint32_t selected);
unsigned platform_battery_percent(void);
bool platform_usb_power_present(void);
void platform_note_control_activity(void);
#ifdef NFCV_DIAGNOSTICS
size_t platform_format_diagnostics(char *out, size_t capacity);
bool platform_test_idle_wake(void);
#endif
