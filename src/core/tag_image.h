#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { TAG_IMAGE_SLIX = 1, TAG_IMAGE_SLIX_L = 2 };
#include "core/limits.h"
#include "core/image_format.h"
/* NFCI v1 is explicitly serialized; no native struct is written to flash. */
typedef struct {
    uint8_t bytes[TAG_IMAGE_MAX_BYTES];
    uint16_t size;
} tag_image_t;
bool tag_image_validate(const uint8_t *data, size_t size);
void tag_image_prepare_emulation(tag_image_t *image);
bool tag_name_validate(const char *name);
