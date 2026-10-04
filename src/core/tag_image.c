#include "common/bytes.h"
#include "tag_image.h"
#include <string.h>

bool tag_name_validate(const char *s) {
    if (!s || !s[0])
        return false;
    size_t n = 0;
    while (s[n]) {
        uint8_t c = (uint8_t)s[n++];
        if (c < 32 || c == 127 || c == '/' || c == '\\' || n > TAG_NAME_MAX_BYTES)
            return false;
        if (c < 128)
            continue;
        unsigned more;
        uint32_t cp, min;
        if (c >= 0xc2 && c <= 0xdf) {
            more = 1;
            cp = c & 31;
            min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            more = 2;
            cp = c & 15;
            min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            more = 3;
            cp = c & 7;
            min = 0x10000;
        } else
            return false;
        while (more--) {
            c = (uint8_t)s[n];
            if ((c & 0xc0) != 0x80 || ++n > TAG_NAME_MAX_BYTES)
                return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}

bool tag_image_validate(const uint8_t *p, size_t n) {
    if (!p || n < TAG_IMAGE_HEADER_SIZE || n > TAG_IMAGE_MAX_BYTES || memcmp(p, "NFCI", 4) ||
        p[TAG_IMAGE_VERSION_OFFSET] != TAG_IMAGE_VERSION ||
        p[TAG_IMAGE_FLAGS_OFFSET] & TAG_IMAGE_FLAGS_RESERVED ||
        p[TAG_IMAGE_RESERVED_GEOMETRY_OFFSET] || p[TAG_IMAGE_RESERVED_GEOMETRY_OFFSET + 1u])
        return false;
    unsigned block_count = read_le16(p + TAG_IMAGE_BLOCK_COUNT_OFFSET),
             data_size = read_le16(p + TAG_IMAGE_DATA_SIZE_OFFSET),
             lock_count = read_le16(p + TAG_IMAGE_LOCK_SIZE_OFFSET);
    if (n != TAG_IMAGE_HEADER_SIZE + data_size + lock_count)
        return false;
    if (p[TAG_IMAGE_PROFILE_OFFSET] == TAG_IMAGE_SLIX ||
        p[TAG_IMAGE_PROFILE_OFFSET] == TAG_IMAGE_SLIX_L) {
        if (p[TAG_IMAGE_UID_SIZE_OFFSET] != TAG_IMAGE_UID_BYTES ||
            p[TAG_IMAGE_UID_OFFSET] != 0xe0 ||
            p[TAG_IMAGE_BLOCK_SIZE_OFFSET] != TAG_IMAGE_BLOCK_BYTES || !block_count ||
            block_count > TAG_IMAGE_MAX_BLOCKS ||
            data_size != block_count * TAG_IMAGE_BLOCK_BYTES || lock_count != block_count)
            return false;
        if (p[TAG_IMAGE_PROFILE_OFFSET] == TAG_IMAGE_SLIX_L &&
            block_count != TAG_IMAGE_SLIX_L_BLOCKS)
            return false;
        for (unsigned i = 0; i < lock_count; i++)
            if (p[TAG_IMAGE_HEADER_SIZE + data_size + i] > 1)
                return false;
    } else
        return false;
    for (unsigned i = TAG_IMAGE_RESERVED_TAIL_OFFSET; i < TAG_IMAGE_HEADER_SIZE; i++)
        if (p[i])
            return false;
    return true;
}
