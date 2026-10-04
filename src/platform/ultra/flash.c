#include "platform/platform.h"
#include "config/flash_layout.h"
#include "common/bytes.h"
#include "nrf.h"
#include <string.h>

static bool read_flash(void *ctx, uint32_t o, void *p, size_t n) {
    (void)ctx;
    memcpy(p, (void *)(LIBRARY_FLASH_START + o), n);
    return true;
}

static void wait_flash(void) {
    while (!NRF_NVMC->READY)
        platform_watchdog_feed();
}

static bool write_flash(void *ctx, uint32_t o, const void *p, size_t n) {
    (void)ctx;
    if (o % 4 || n % 4)
        return false;
    uint32_t *dst = (uint32_t *)(LIBRARY_FLASH_START + o);
    const uint8_t *v = p;
    for (size_t i = 0; i < n; i += 4)
        if ((dst[i / 4] & read_le32(v + i)) != read_le32(v + i))
            return false;
    NRF_NVMC->CONFIG = 1;
    wait_flash();
    for (size_t i = 0; i < n; i += 4) {
        dst[i / 4] = read_le32(v + i);
        wait_flash();
    }
    NRF_NVMC->CONFIG = 0;
    wait_flash();
    return true;
}

static bool erase_flash(void *ctx, uint32_t o, size_t n) {
    (void)ctx;
    if (o % 4096 || n % 4096)
        return false;
    NRF_NVMC->CONFIG = 2;
    wait_flash();
    for (size_t i = 0; i < n; i += 4096) {
        NRF_NVMC->ERASEPAGE = LIBRARY_FLASH_START + o + i;
        wait_flash();
    }
    NRF_NVMC->CONFIG = 0;
    wait_flash();
    return true;
}

flash_io_t platform_flash_io(void) {
    return (flash_io_t){NULL, read_flash, write_flash, erase_flash};
}
