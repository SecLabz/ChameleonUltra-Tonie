#include "tag/nfcv_emulation.h"
#include "app.h"
#include "favorites.h"
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>

#define IMPORT_NAME_PREFIX_MAX_BYTES 54u

app_state_t app_state;
void app_note_activity(void) {
    app_state.last_activity = platform_millis();
}

static void show_selection(void) {
    platform_show_selection(app_state.library.favorites, app_state.selected);
}

void app_pause_emulation(void) {
    nfcv_emulation_stop();
    app_state.emulating = false;
    app_note_activity();
    show_selection();
}

operation_result_t app_init(flash_io_t flash) {
    memset(&app_state, 0, sizeof(app_state));
    operation_result_t result = tag_library_open(&app_state.library, flash);
    if (result != RESULT_OK)
        return result;
    app_state.ready = true;
    uint32_t id = app_state.library.selected;
    if (!id)
        for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
            if (app_state.library.favorites[i]) {
                id = app_state.library.favorites[i];
                break;
            }
    app_note_activity();
#ifdef NFCV_DIAGNOSTICS
    /* Keep USB recovery reachable while RF startup is being validated. */
    (void)id;
    return RESULT_OK;
#else
    return id ? app_start_emulation(id) : RESULT_OK;
#endif
}

operation_result_t app_start_emulation(uint32_t id) {
    if (!app_state.ready)
        return RESULT_STORAGE_ERROR;
    if (nfcv_emulation_field_present())
        return RESULT_RF_BUSY;
    tag_image_t image;
    operation_result_t result = tag_library_read_image(&app_state.library, id, &image);
    if (result != RESULT_OK)
        return result;
    nfcv_emulation_stop();
    app_state.emulating = false;
    if (!tag_image_validate(image.bytes, image.size)) {
        show_selection();
        return RESULT_INVALID_DATA;
    }
    app_state.loaded_original = image;
    app_state.selected = id;
    /* Persist selection only, never the live RF image. */
    if (app_state.library.selected != id)
        result = tag_library_write_settings(&app_state.library, app_state.library.favorites, id);
    app_state.emulating = nfcv_emulation_start_from_image(&app_state.loaded_original);
    if (!app_state.emulating) {
        nfcv_emulation_stop();
        result = RESULT_INVALID_DATA;
    }
    app_note_activity();
    show_selection();
    return result;
}

operation_result_t app_select_next_favorite(int direction) {
    uint32_t id = favorite_next_tag_id(app_state.library.favorites, app_state.selected, direction);
    return id ? app_start_emulation(id) : RESULT_NOT_FOUND;
}

static bool name_used(const char *name) {
    for (unsigned i = 0; i < app_state.library.count; i++)
        if (!strcmp(name, app_state.library.tags[i].name))
            return true;
    return false;
}

operation_result_t app_import_tag(const char *name, const tag_image_t *image, uint32_t *id) {
    if (!app_state.ready)
        return RESULT_STORAGE_ERROR;
    if (!tag_name_validate(name))
        return RESULT_INVALID_DATA;
    app_pause_emulation();
    char unique_name[TAG_NAME_MAX_BYTES + 1u];
    snprintf(unique_name, sizeof(unique_name), "%s", name);
    for (unsigned i = 2; name_used(unique_name); i++) {
        if (i > TAG_LIBRARY_CAPACITY + 1)
            return RESULT_LIBRARY_FULL;
        size_t cut = strlen(name);
        /* Keep the established UTF-8 truncation and suffix allowance. */
        if (cut > IMPORT_NAME_PREFIX_MAX_BYTES) {
            cut = IMPORT_NAME_PREFIX_MAX_BYTES;
            while (cut && ((unsigned char)name[cut] & 0xc0) == 0x80)
                cut--;
        }
        snprintf(unique_name, sizeof(unique_name), "%.*s (%u)", (int)cut, name, i);
    }
    return tag_library_write_image(&app_state.library, 0, unique_name, image, id);
}

operation_result_t app_delete_tag(uint32_t id) {
    if (!app_state.ready)
        return RESULT_STORAGE_ERROR;
    app_pause_emulation();
    operation_result_t result = tag_library_delete(&app_state.library, id);
    if (result == RESULT_OK && app_state.selected == id) {
        app_state.selected = 0;
    }
    show_selection();
    return result;
}

operation_result_t app_rename_tag(uint32_t id, const char *name) {
    if (!app_state.ready)
        return RESULT_STORAGE_ERROR;
    tag_image_t image;
    operation_result_t result = tag_library_read_image(&app_state.library, id, &image);
    if (result != RESULT_OK)
        return result;
    app_pause_emulation();
    return tag_library_write_image(&app_state.library, id, name, &image, NULL);
}

operation_result_t app_assign_favorite(unsigned slot, uint32_t id) {
    if (!app_state.ready)
        return RESULT_STORAGE_ERROR;
    if (slot >= FAVORITE_SLOT_COUNT)
        return RESULT_INVALID_DATA;
    uint32_t favorites[FAVORITE_SLOT_COUNT];
    memcpy(favorites, app_state.library.favorites, sizeof(favorites));
    favorites[slot] = id;
    app_pause_emulation();
    operation_result_t result =
        tag_library_write_settings(&app_state.library, favorites, app_state.selected);
    show_selection();
    return result;
}

operation_result_t app_reset_library(flash_io_t flash) {
    if (nfcv_emulation_field_present())
        return RESULT_RF_BUSY;
    app_pause_emulation();
    app_state.ready = false;
    if (!flash.erase(flash.ctx, 0, TAG_LIBRARY_FLASH_SIZE))
        return RESULT_STORAGE_ERROR;
    return app_init(flash);
}
