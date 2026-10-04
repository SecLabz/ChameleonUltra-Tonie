#include "common/crc32.h"
#include "common/bytes.h"
#include "tag_library.h"
#include <string.h>
#include "journal_format.h"
static unsigned align_journal_record(unsigned size) {
    return (size + 3u) & ~3u;
}

const tag_library_entry_t *tag_library_find(const tag_library_t *library, uint32_t id) {
    for (unsigned i = 0; i < library->count; i++)
        if (library->tags[i].id == id)
            return &library->tags[i];
    return NULL;
}

static bool read_flash_range(tag_library_t *library, uint32_t offset, void *bytes, size_t size) {
    return offset <= TAG_LIBRARY_FLASH_SIZE && size <= TAG_LIBRARY_FLASH_SIZE - offset &&
           library->flash.read(library->flash.ctx, offset, bytes, size);
}

static bool write_flash_range(tag_library_t *library, uint32_t offset, const void *bytes,
                              size_t size) {
    return offset <= TAG_LIBRARY_FLASH_SIZE && size <= TAG_LIBRARY_FLASH_SIZE - offset &&
           library->flash.write(library->flash.ctx, offset, bytes, size);
}

static bool journal_bank_valid(tag_library_t *library, uint32_t base, uint32_t *generation) {
    uint8_t header[JOURNAL_BANK_HEADER_BYTES];
    if (!read_flash_range(library, base, header, JOURNAL_BANK_HEADER_BYTES) ||
        read_le32(header) != JOURNAL_BANK_MAGIC ||
        read_le32(header + JOURNAL_COMMIT_OFFSET) != JOURNAL_COMMIT_WORD ||
        crc32_compute(header, JOURNAL_CRC_OFFSET) != read_le32(header + JOURNAL_CRC_OFFSET))
        return false;
    *generation = read_le32(header + JOURNAL_BANK_GENERATION_OFFSET);
    return true;
}

static bool journal_begin_bank(tag_library_t *library, uint32_t bank_offset, uint32_t generation) {
    uint8_t header[JOURNAL_COMMIT_OFFSET] = {0};
    write_le32(header, JOURNAL_BANK_MAGIC);
    write_le32(header + JOURNAL_BANK_GENERATION_OFFSET, generation);
    write_le32(header + JOURNAL_BANK_NEXT_ID_OFFSET, library->next_id ? library->next_id : 1);
    write_le32(header + JOURNAL_CRC_OFFSET, crc32_compute(header, JOURNAL_CRC_OFFSET));
    return library->flash.erase(library->flash.ctx, bank_offset, TAG_LIBRARY_BANK_SIZE) &&
           write_flash_range(library, bank_offset, header, JOURNAL_COMMIT_OFFSET);
}

static bool journal_commit_bank(tag_library_t *library, uint32_t bank_offset) {
    uint8_t commit_word[4];
    write_le32(commit_word, JOURNAL_COMMIT_WORD);
    return write_flash_range(library, bank_offset + JOURNAL_COMMIT_OFFSET, commit_word, 4);
}

static void library_remove_index_entry(tag_library_t *library, uint32_t id) {
    for (unsigned i = 0; i < library->count; i++)
        if (library->tags[i].id == id) {
            library->tags[i] = library->tags[--library->count];
            return;
        }
}

/* CRC covers the pre-commit representation. Normalize scratch only, never flash. */
static bool journal_record_crc_valid(uint8_t *record_bytes, unsigned length) {
    uint32_t expected_crc = read_le32(record_bytes + JOURNAL_CRC_OFFSET);
    write_le32(record_bytes + JOURNAL_CRC_OFFSET, 0);
    write_le32(record_bytes + JOURNAL_COMMIT_OFFSET, JOURNAL_ERASED_WORD);
    return crc32_compute(record_bytes, length) == expected_crc;
}

/* Invalid payloads mark a torn/corrupt tail; exhausted index capacity fails the open
 * operation. */
static bool apply_journal_record(tag_library_t *library, uint32_t offset, const uint8_t *bytes,
                                 unsigned name_size, unsigned image_size, uint32_t id) {
    if (bytes[JOURNAL_RECORD_KIND_OFFSET] == JOURNAL_RECORD_TAG) {
        if (!name_size || memchr(bytes + JOURNAL_RECORD_HEADER_BYTES, 0, name_size) ||
            !tag_image_validate(bytes + JOURNAL_RECORD_HEADER_BYTES + name_size, image_size)) {
            library->dirty = true;
            return true;
        }
        library_remove_index_entry(library, id);
        if (library->count >= TAG_LIBRARY_CAPACITY)
            return false;
        tag_library_entry_t *entry = &library->tags[library->count++];
        entry->id = id;
        entry->offset = offset + JOURNAL_RECORD_HEADER_BYTES + name_size;
        entry->size = image_size;
        memcpy(entry->name, bytes + JOURNAL_RECORD_HEADER_BYTES, name_size);
        entry->name[name_size] = 0;
    } else if (bytes[JOURNAL_RECORD_KIND_OFFSET] == JOURNAL_RECORD_DELETE) {
        if (name_size || image_size) {
            library->dirty = true;
            return true;
        }
        library_remove_index_entry(library, id);
    } else if (bytes[JOURNAL_RECORD_KIND_OFFSET] == JOURNAL_RECORD_SETTINGS) {
        if (name_size || image_size != JOURNAL_SETTINGS_BYTES) {
            library->dirty = true;
            return true;
        }
        for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
            library->favorites[i] = read_le32(bytes + JOURNAL_RECORD_HEADER_BYTES + 4 * i);
        library->selected =
            read_le32(bytes + JOURNAL_RECORD_HEADER_BYTES + JOURNAL_SETTINGS_SELECTED_OFFSET);
    } else {
        library->dirty = true;
        return true;
    }
    if ((bytes[JOURNAL_RECORD_KIND_OFFSET] == JOURNAL_RECORD_TAG ||
         bytes[JOURNAL_RECORD_KIND_OFFSET] == JOURNAL_RECORD_DELETE) &&
        id >= library->next_id)
        library->next_id = id + 1;
    return true;
}

static bool journal_scan_bank(tag_library_t *library) {
    library->count = 0;
    library->selected = 0;
    uint8_t highwater[4];
    if (!read_flash_range(library, library->bank + JOURNAL_BANK_NEXT_ID_OFFSET, highwater, 4))
        return false;
    library->next_id = read_le32(highwater);
    memset(library->favorites, 0, sizeof(library->favorites));
    library->dirty = false;
    uint32_t offset = library->bank + JOURNAL_BANK_PREFIX_BYTES;
    while (offset + JOURNAL_RECORD_HEADER_BYTES <= library->bank + TAG_LIBRARY_BANK_SIZE) {
        uint8_t *bytes = library->scratch;
        if (!read_flash_range(library, offset, bytes, JOURNAL_RECORD_HEADER_BYTES))
            return false;
        if (read_le32(bytes) == JOURNAL_ERASED_WORD)
            break;
        unsigned name_size = bytes[JOURNAL_RECORD_NAME_SIZE_OFFSET],
                 image_size = read_le16(bytes + JOURNAL_RECORD_IMAGE_SIZE_OFFSET),
                 record_size =
                     align_journal_record(JOURNAL_RECORD_HEADER_BYTES + name_size + image_size);
        uint32_t id = read_le32(bytes + JOURNAL_RECORD_ID_OFFSET);
        if (read_le32(bytes) != JOURNAL_RECORD_MAGIC ||
            read_le32(bytes + JOURNAL_COMMIT_OFFSET) != JOURNAL_COMMIT_WORD ||
            name_size > TAG_NAME_MAX_BYTES || record_size > sizeof(library->scratch) ||
            offset + record_size > library->bank + TAG_LIBRARY_BANK_SIZE) {
            library->dirty = true;
            break;
        }
        if (!read_flash_range(library, offset, bytes, record_size))
            return false;
        if (!journal_record_crc_valid(bytes, record_size)) {
            library->dirty = true;
            break;
        }
        if (!apply_journal_record(library, offset, bytes, name_size, image_size, id))
            return false;
        if (library->dirty)
            break;
        offset += record_size;
    }
    library->tail = offset;
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
        if (!tag_library_find(library, library->favorites[i]))
            library->favorites[i] = 0;
    if (!tag_library_find(library, library->selected))
        library->selected = 0;
    return true;
}

static bool journal_append_record(tag_library_t *library, uint32_t *tail, uint8_t kind, uint32_t id,
                                  const char *name, const uint8_t *data, unsigned size) {
    unsigned name_size = name ? (unsigned)strlen(name) : 0,
             record_size = align_journal_record(JOURNAL_RECORD_HEADER_BYTES + name_size + size);
    uint8_t *bytes = library->scratch;
    if (record_size > sizeof(library->scratch))
        return false;
    /* memmove permits callers to use the scratch image while renaming. */
    if (size)
        memmove(bytes + JOURNAL_RECORD_HEADER_BYTES + name_size, data, size);
    if (name_size)
        memcpy(bytes + JOURNAL_RECORD_HEADER_BYTES, name, name_size);
    memset(bytes, 0, JOURNAL_RECORD_HEADER_BYTES);
    memset(bytes + JOURNAL_RECORD_HEADER_BYTES + name_size + size, 0xff,
           record_size - JOURNAL_RECORD_HEADER_BYTES - name_size - size);
    write_le32(bytes, JOURNAL_RECORD_MAGIC);
    bytes[JOURNAL_RECORD_KIND_OFFSET] = kind;
    bytes[JOURNAL_RECORD_NAME_SIZE_OFFSET] = name_size;
    write_le16(bytes + JOURNAL_RECORD_IMAGE_SIZE_OFFSET, size);
    write_le32(bytes + JOURNAL_RECORD_ID_OFFSET, id);
    write_le32(bytes + JOURNAL_COMMIT_OFFSET, JOURNAL_ERASED_WORD);
    write_le32(bytes + JOURNAL_CRC_OFFSET, crc32_compute(bytes, record_size));
    if (!write_flash_range(library, *tail, bytes, record_size))
        return false;
    uint8_t commit_word[4];
    write_le32(commit_word, JOURNAL_COMMIT_WORD);
    if (!write_flash_range(library, *tail + JOURNAL_COMMIT_OFFSET, commit_word, 4))
        return false;
    *tail += record_size;
    return true;
}

operation_result_t tag_library_read_image(tag_library_t *library, uint32_t id, tag_image_t *image) {
    const tag_library_entry_t *entry = tag_library_find(library, id);
    if (!entry)
        return RESULT_NOT_FOUND;
    image->size = entry->size;
    return read_flash_range(library, entry->offset, image->bytes, image->size)
               ? RESULT_OK
               : RESULT_STORAGE_ERROR;
}

static void encode_settings(uint8_t bytes[JOURNAL_SETTINGS_BYTES],
                            const uint32_t favorites[FAVORITE_SLOT_COUNT], uint32_t selected) {
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
        write_le32(bytes + 4 * i, favorites[i]);
    write_le32(bytes + JOURNAL_SETTINGS_SELECTED_OFFSET, selected);
}

static operation_result_t journal_compact(tag_library_t *library) {
    uint32_t destination_bank = library->bank ? 0 : TAG_LIBRARY_BANK_SIZE;
    uint32_t destination_tail = destination_bank + JOURNAL_BANK_PREFIX_BYTES;
    tag_image_t image;
    if (!journal_begin_bank(library, destination_bank, library->generation + 1))
        return RESULT_STORAGE_ERROR;
    for (unsigned i = 0; i < library->count; i++) {
        tag_library_entry_t entry = library->tags[i];
        if (tag_library_read_image(library, entry.id, &image) != RESULT_OK ||
            !journal_append_record(library, &destination_tail, JOURNAL_RECORD_TAG, entry.id,
                                   entry.name, image.bytes, image.size))
            return RESULT_STORAGE_ERROR;
    }
    uint8_t settings[JOURNAL_SETTINGS_BYTES];
    encode_settings(settings, library->favorites, library->selected);
    if (!journal_append_record(library, &destination_tail, JOURNAL_RECORD_SETTINGS, 0, NULL,
                               settings, JOURNAL_SETTINGS_BYTES) ||
        !journal_commit_bank(library, destination_bank))
        return RESULT_STORAGE_ERROR;
    library->bank = destination_bank;
    library->generation++;
    return journal_scan_bank(library) ? RESULT_OK : RESULT_STORAGE_ERROR;
}

static operation_result_t journal_append(tag_library_t *library, uint8_t kind, uint32_t id,
                                         const char *name, const uint8_t *data, unsigned size) {
    unsigned need =
        align_journal_record(JOURNAL_RECORD_HEADER_BYTES + (name ? strlen(name) : 0) + size);
    if (library->dirty || library->tail + need > library->bank + TAG_LIBRARY_BANK_SIZE) {
        operation_result_t result = journal_compact(library);
        if (result != RESULT_OK)
            return result;
    }
    if (library->tail + need > library->bank + TAG_LIBRARY_BANK_SIZE)
        return RESULT_LIBRARY_FULL;
    if (!journal_append_record(library, &library->tail, kind, id, name, data, size)) {
        library->dirty = true;
        return RESULT_STORAGE_ERROR;
    }
    return journal_scan_bank(library) ? RESULT_OK : RESULT_STORAGE_ERROR;
}

operation_result_t tag_library_open(tag_library_t *library, flash_io_t flash) {
    memset(library, 0, sizeof(*library));
    library->flash = flash;
    uint32_t generation_a = 0, generation_b = 0;
    bool bank_a_valid = journal_bank_valid(library, 0, &generation_a),
         bank_b_valid = journal_bank_valid(library, TAG_LIBRARY_BANK_SIZE, &generation_b);
    if (!bank_a_valid && !bank_b_valid) {
        uint8_t header_a[JOURNAL_BANK_HEADER_BYTES], header_b[JOURNAL_BANK_HEADER_BYTES];
        if (!read_flash_range(library, 0, header_a, JOURNAL_BANK_HEADER_BYTES) ||
            !read_flash_range(library, TAG_LIBRARY_BANK_SIZE, header_b, JOURNAL_BANK_HEADER_BYTES))
            return RESULT_STORAGE_ERROR;
        if ((read_le32(header_a) == JOURNAL_BANK_MAGIC &&
             read_le32(header_a + JOURNAL_COMMIT_OFFSET) == JOURNAL_COMMIT_WORD) ||
            (read_le32(header_b) == JOURNAL_BANK_MAGIC &&
             read_le32(header_b + JOURNAL_COMMIT_OFFSET) == JOURNAL_COMMIT_WORD))
            return RESULT_STORAGE_ERROR;
        if (read_le32(header_a) != JOURNAL_ERASED_WORD && read_le32(header_a) != JOURNAL_BANK_MAGIC)
            return RESULT_STORAGE_ERROR;
        if (!journal_begin_bank(library, 0, 1) || !journal_commit_bank(library, 0))
            return RESULT_STORAGE_ERROR;
        library->bank = 0;
        library->generation = 1;
    } else {
        library->bank =
            bank_b_valid && (!bank_a_valid || (int32_t)(generation_b - generation_a) > 0)
                ? TAG_LIBRARY_BANK_SIZE
                : 0;
        library->generation = library->bank ? generation_b : generation_a;
    }
    return journal_scan_bank(library) ? RESULT_OK : RESULT_STORAGE_ERROR;
}

operation_result_t tag_library_write_image(tag_library_t *library, uint32_t id, const char *name,
                                           const tag_image_t *image, uint32_t *out) {
    if (!tag_name_validate(name) || !tag_image_validate(image->bytes, image->size))
        return RESULT_INVALID_DATA;
    if (id && !tag_library_find(library, id))
        return RESULT_NOT_FOUND;
    if (!id) {
        if (library->count >= TAG_LIBRARY_CAPACITY)
            return RESULT_LIBRARY_FULL;
        id = library->next_id;
        if (!id || id == UINT32_MAX)
            return RESULT_LIBRARY_FULL;
    }
    operation_result_t result =
        journal_append(library, JOURNAL_RECORD_TAG, id, name, image->bytes, image->size);
    if (result == RESULT_OK && out)
        *out = id;
    return result;
}

operation_result_t tag_library_delete(tag_library_t *library, uint32_t id) {
    return tag_library_find(library, id)
               ? journal_append(library, JOURNAL_RECORD_DELETE, id, NULL, NULL, 0)
               : RESULT_NOT_FOUND;
}

operation_result_t tag_library_write_settings(tag_library_t *library,
                                              const uint32_t favorites[FAVORITE_SLOT_COUNT],
                                              uint32_t selected) {
    uint8_t bytes[JOURNAL_SETTINGS_BYTES];
    if (selected && !tag_library_find(library, selected))
        return RESULT_NOT_FOUND;
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++) {
        if (favorites[i] && !tag_library_find(library, favorites[i]))
            return RESULT_NOT_FOUND;
    }
    encode_settings(bytes, favorites, selected);
    return journal_append(library, JOURNAL_RECORD_SETTINGS, 0, NULL, bytes, JOURNAL_SETTINGS_BYTES);
}
