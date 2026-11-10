#pragma once
#include "tag_library.h"

typedef struct {
    tag_library_t library;
    /* Prepared image for reloads. Reader writes stay in profile RAM. */
    tag_image_t loaded_image;
    uint32_t selected, last_activity;
    bool emulating, ready;
} app_state_t;
extern app_state_t app_state;
operation_result_t app_init(flash_io_t flash);
operation_result_t app_start_emulation(uint32_t id);
void app_pause_emulation(void);
operation_result_t app_select_next_favorite(int direction);
operation_result_t app_import_tag(const char *name, const tag_image_t *image, uint32_t *id);
operation_result_t app_scan_tag(tag_image_t *out);
operation_result_t app_delete_tag(uint32_t id);
operation_result_t app_rename_tag(uint32_t id, const char *name);
operation_result_t app_assign_favorite(unsigned slot, uint32_t id);
void app_note_activity(void);

/* Explicit erase only; reader writes are never saved by this operation. */
operation_result_t app_reset_library(flash_io_t flash);
