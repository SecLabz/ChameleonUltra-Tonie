#include "tag/nfcv_emulation.h"
#include "common/bytes.h"
/* Application failure-state checks for SLIX-L. */
#include "core/app.h"
#include "platform/platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[TAG_LIBRARY_FLASH_SIZE];
static bool rf, field, erase_fails, write_fails;
static uint32_t led_selected;
static unsigned erases;
static unsigned failures, loads, stops;
static bool read_flash(void *c, uint32_t o, void *p, size_t n) {
    (void)c;
    assert(o + n <= sizeof(flash));
    memcpy(p, flash + o, n);
    return true;
}

static bool write_flash(void *c, uint32_t o, const void *p, size_t n) {
    (void)c;
    assert(!rf && o + n <= sizeof(flash));
    if (write_fails)
        return false;
    const uint8_t *v = p;
    for (size_t i = 0; i < n; i++) {
        assert((flash[o + i] & v[i]) == v[i]);
        flash[o + i] &= v[i];
    }
    return true;
}

static bool erase_flash(void *c, uint32_t o, size_t n) {
    (void)c;
    assert(!rf && o + n <= sizeof(flash));
    erases++;
    if (erase_fails)
        return false;
    memset(flash + o, 255, n);
    return true;
}

uint32_t platform_millis(void) {
    return 1234;
}

void nfcv_emulation_stop(void) {
    rf = false;
    stops++;
}

bool nfcv_emulation_start_from_image(const tag_image_t *im) {
    assert(tag_image_validate(im->bytes, im->size));
    loads++;
    if (failures) {
        failures--;
        rf = true;
        return false;
    }
    rf = true;
    return true;
}

bool nfcv_emulation_field_present(void) {
    return field;
}

void platform_show_selection(const uint32_t f[FAVORITE_SLOT_COUNT], uint32_t id) {
    (void)f;
    led_selected = id;
}

static tag_image_t image(void) {
    tag_image_t im = {.size = TAG_IMAGE_HEADER_SIZE + 40};
    uint8_t *p = im.bytes;
    memcpy(p, "NFCI", 4);
    p[4] = 1;
    p[5] = TAG_IMAGE_SLIX_L;
    p[6] = 8;
    p[8] = 0xe0;
    p[19] = 4;
    write_le16(p + 20, 8);
    write_le16(p + 22, 32);
    write_le16(p + 24, 8);
    assert(tag_image_validate(p, im.size));
    return im;
}

int main(void) {
    memset(flash, 255, sizeof(flash));
    flash_io_t io = {NULL, read_flash, write_flash, erase_flash};
    assert(app_init(io) == RESULT_OK);
    tag_image_t im = image(), stored;
    uint32_t id;
    assert(app_import_tag("Synthetic", &im, &id) == RESULT_OK);
    assert(app_start_emulation(id) == RESULT_OK);
    assert(loads == 1);
    unsigned before = loads;
    field = true;
    assert(app_start_emulation(id) == RESULT_RF_BUSY);
    assert(app_state.emulating && rf && loads == before);
    field = false;
    assert(tag_library_read_image(&app_state.library, id, &stored) == RESULT_OK &&
           !memcmp(stored.bytes, im.bytes, im.size));
    failures = 1;
    assert(app_start_emulation(id) == RESULT_INVALID_DATA);
    assert(!app_state.emulating && !rf && led_selected == id);
    assert(app_start_emulation(id) == RESULT_OK);
    assert(!memcmp(app_state.loaded_original.bytes, im.bytes, im.size));
    uint32_t id2;
    tag_image_t second = im;
    second.bytes[TAG_IMAGE_HEADER_SIZE] = 0x42;
    assert(app_import_tag("Second", &second, &id2) == RESULT_OK);
    assert(app_start_emulation(id) == RESULT_OK);
    before = loads;
    failures = 1;
    assert(app_start_emulation(id2) == RESULT_INVALID_DATA);
    assert(loads == before + 1 && app_state.library.selected == id2);
    assert(app_state.selected == id2 && !app_state.emulating && !rf && led_selected == id2);
    assert(app_start_emulation(id2) == RESULT_OK && app_state.emulating && rf);
    assert(!memcmp(app_state.loaded_original.bytes, second.bytes, second.size));
    write_fails = true;
    before = loads;
    assert(app_start_emulation(id) == RESULT_STORAGE_ERROR);
    assert(loads == before + 1 && app_state.library.selected == id2);
    assert(app_state.selected == id && app_state.emulating && rf && led_selected == id);
    assert(!memcmp(app_state.loaded_original.bytes, im.bytes, im.size));
    write_fails = false;
    before = loads;
    app_pause_emulation();
    assert(!app_state.emulating && !rf && led_selected == id && loads == before);
    assert(app_start_emulation(id) == RESULT_OK && rf && led_selected == id);
    assert(app_start_emulation(id2) == RESULT_OK);
    /* Reset owns its RF guard and never erases during an active reader field. */
    field = true;
    unsigned erases_before = erases, stops_before = stops;
    assert(app_reset_library(io) == RESULT_RF_BUSY);
    assert(erases == erases_before && stops == stops_before);
    assert(app_state.ready && app_state.emulating && app_state.library.count == 2);
    field = false;
    erase_fails = true;
    assert(app_reset_library(io) == RESULT_STORAGE_ERROR);
    assert(!app_state.ready && !app_state.emulating && !rf);
    assert(tag_library_read_image(&app_state.library, id, &stored) == RESULT_OK);
    assert(!memcmp(stored.bytes, im.bytes, im.size));
    erase_fails = false;
    assert(app_reset_library(io) == RESULT_OK);
    assert(app_state.ready && !app_state.emulating && !rf);
    assert(!app_state.library.count && !app_state.selected);
    for (unsigned slot = 0; slot < FAVORITE_SLOT_COUNT; slot++)
        assert(!app_state.library.favorites[slot]);
    puts("Application reset: busy-field guard, erase failure and explicit recovery passed");
    puts("Application: busy field, RF failure state, selection LEDs, preserved originals, paused "
         "flash and reload passed");
}
