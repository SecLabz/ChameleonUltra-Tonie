#pragma once
#include "tag_image.h"
#include "result.h"
#include "platform/flash.h"
#define TAG_LIBRARY_BANK_SIZE (96u * 1024u)
#define TAG_LIBRARY_FLASH_SIZE (2u * TAG_LIBRARY_BANK_SIZE)
typedef struct {
    uint32_t id, offset;
    uint16_t size;
    char name[TAG_NAME_MAX_BYTES + 1];
} tag_library_entry_t;
typedef struct {
    flash_io_t flash;
    uint32_t bank, generation, tail, next_id;
    /* An invalid append tail requires compaction before the next write. */
    bool dirty;
    unsigned count;
    tag_library_entry_t tags[TAG_LIBRARY_CAPACITY];
    uint32_t favorites[FAVORITE_SLOT_COUNT], selected;
    /* Header + maximum name/image + alignment, with retained spare capacity. */
    uint8_t scratch[1120];
} tag_library_t;
operation_result_t tag_library_open(tag_library_t *library, flash_io_t flash);
const tag_library_entry_t *tag_library_find(const tag_library_t *library, uint32_t id);
operation_result_t tag_library_read_image(tag_library_t *library, uint32_t id, tag_image_t *image);
/* ID zero inserts; an existing ID appends its replacement original. */
operation_result_t tag_library_write_image(tag_library_t *library, uint32_t id, const char *name,
                                           const tag_image_t *image, uint32_t *written_id);
operation_result_t tag_library_delete(tag_library_t *library, uint32_t id);
operation_result_t tag_library_write_settings(tag_library_t *library,
                                              const uint32_t favorites[FAVORITE_SLOT_COUNT],
                                              uint32_t selected);
