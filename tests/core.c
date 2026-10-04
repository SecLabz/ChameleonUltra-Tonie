#include "common/crc32.h"
#include "common/bytes.h"
#include "core/tag_library.h"
#include "tag/nfcv_session.h"
#include "tag/protocol/nfcv/commands.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[TAG_LIBRARY_FLASH_SIZE], saved[TAG_LIBRARY_FLASH_SIZE];
static int fail = -1;
static bool rd(void *c, uint32_t o, void *p, size_t n) {
    (void)c;
    assert(o + n <= sizeof(flash));
    memcpy(p, flash + o, n);
    return true;
}

static bool wr(void *c, uint32_t o, const void *p, size_t n) {
    (void)c;
    assert(o + n <= sizeof(flash));
    const uint8_t *v = p;
    for (size_t i = 0; i < n; i++) {
        if (fail == 0)
            return false;
        if (fail > 0)
            fail--;
        assert((flash[o + i] & v[i]) == v[i]);
        flash[o + i] &= v[i];
    }
    return true;
}

static bool erase(void *c, uint32_t o, size_t n) {
    (void)c;
    assert(o + n <= sizeof(flash));
    for (size_t i = 0; i < n; i++) {
        if (fail == 0)
            return false;
        if (fail > 0)
            fail--;
        flash[o + i] = 255;
    }
    return true;
}

static flash_io_t io = {NULL, rd, wr, erase};
static tag_image_t image(void) {
    tag_image_t im = {.size = 128 + 32 + 8};
    memcpy(im.bytes, "NFCI", 4);
    im.bytes[4] = 1;
    im.bytes[5] = TAG_IMAGE_SLIX_L;
    im.bytes[6] = 8;
    im.bytes[8] = 0xe0;
    im.bytes[9] = 4;
    im.bytes[19] = 4;
    write_le16(im.bytes + 20, 8);
    write_le16(im.bytes + 22, 32);
    write_le16(im.bytes + 24, 8);
    for (unsigned i = 0; i < 32; i++)
        im.bytes[128 + i] = i;
    assert(tag_image_validate(im.bytes, im.size));
    return im;
}

static void incompatible_store(void) {
    memset(flash, 255, sizeof(flash));
    uint8_t h[20] = {0};
    write_le32(h, 0x31424e46u);
    write_le32(h + 4, 1);
    write_le32(h + 8, 1);
    write_le32(h + 12, crc32_compute(h, 12));
    write_le32(h + 16, 0xf00dcafeu);
    memcpy(flash, h, sizeof(h));
    memcpy(saved, flash, sizeof(flash));
    tag_library_t s;
    assert(tag_library_open(&s, io) == RESULT_STORAGE_ERROR);
    assert(!memcmp(flash, saved, sizeof(flash)));
    puts("Storage: incompatible bank rejected without mutation");
}

static void storage(void) {
    tag_library_t s;
    tag_image_t im = image(), out;
    uint32_t id, id2;
    memset(flash, 255, sizeof(flash));
    assert(tag_library_open(&s, io) == RESULT_OK);
    assert(tag_library_write_image(&s, 0, "Synthetic", &im, &id) == RESULT_OK);
    uint32_t favorites[FAVORITE_SLOT_COUNT] = {id, 0, id, id, 0, id, 0, id};
    assert(tag_library_write_settings(&s, favorites, id) == RESULT_OK);
    assert(tag_library_open(&s, io) == RESULT_OK &&
           !memcmp(s.favorites, favorites, sizeof(favorites)) && s.selected == id);
    memcpy(saved, flash, sizeof(flash));
    for (int cut = 0; cut < 200; cut++) {
        memcpy(flash, saved, sizeof(flash));
        fail = -1;
        assert(tag_library_open(&s, io) == RESULT_OK);
        fail = cut;
        tag_library_write_image(&s, id, "Changed", &im, NULL);
        fail = -1;
        assert(tag_library_open(&s, io) == RESULT_OK);
        assert(tag_library_read_image(&s, id, &out) == RESULT_OK &&
               !memcmp(out.bytes, im.bytes, im.size));
        assert(!strcmp(s.tags[0].name, "Synthetic") || !strcmp(s.tags[0].name, "Changed"));
        assert(!memcmp(s.favorites, favorites, sizeof(favorites)) && s.selected == id);
        assert(tag_library_write_image(&s, 0, "Other", &im, &id2) == RESULT_OK);
    }
    memcpy(flash, saved, sizeof(flash));
    assert(tag_library_open(&s, io) == RESULT_OK);
    assert(tag_library_write_image(&s, 0, "Deleted", &im, &id2) == RESULT_OK);
    assert(tag_library_delete(&s, id2) == RESULT_OK);
    for (unsigned i = 0; i < 700; i++)
        assert(tag_library_write_image(&s, id, "Synthetic", &im, NULL) == RESULT_OK);
    assert(s.generation > 1);
    uint32_t fresh;
    assert(tag_library_write_image(&s, 0, "New", &im, &fresh) == RESULT_OK && fresh > id2);
    /* Force compaction and tear every important boundary. */
    s.dirty = true;
    memcpy(saved, flash, sizeof(flash));
    const int cuts[] = {0, 1, 4095, 4096, 98303, 98304, 98316, 98324, 98400, 99000, 100000};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(*cuts); i++) {
        memcpy(flash, saved, sizeof(flash));
        fail = -1;
        assert(tag_library_open(&s, io) == RESULT_OK);
        s.dirty = true;
        fail = cuts[i];
        tag_library_write_image(&s, id, "Compacted", &im, NULL);
        fail = -1;
        assert(tag_library_open(&s, io) == RESULT_OK);
        assert(tag_library_read_image(&s, id, &out) == RESULT_OK);
        assert(!memcmp(s.favorites, favorites, sizeof(favorites)) && s.selected == id);
    }
    memset(flash, 255, sizeof(flash));
    assert(tag_library_open(&s, io) == RESULT_OK);
    for (unsigned i = 0; i < 64; i++) {
        char name[20];
        snprintf(name, sizeof(name), "Tag %u", i);
        assert(tag_library_write_image(&s, 0, name, &im, NULL) == RESULT_OK);
    }
    assert(tag_library_write_image(&s, 0, "Full", &im, NULL) == RESULT_LIBRARY_FULL);
    assert(s.count == 64);
    memset(flash, 255, sizeof(flash));
    assert(tag_library_open(&s, io) == RESULT_OK);
    flash[12] ^= 1;
    memcpy(saved, flash, sizeof(flash));
    assert(tag_library_open(&s, io) == RESULT_STORAGE_ERROR &&
           !memcmp(saved, flash, sizeof(flash)));
    assert(tag_name_validate("Tonie é 🦊") && !tag_name_validate("bad\xc0\xaf") &&
           !tag_name_validate("bad\xed\xa0\x80") && !tag_name_validate("bad\xf4\x90\x80\x80") &&
           !tag_name_validate("bad\xe2") && !tag_name_validate("bad/filename"));
    im.bytes[26] = 1;
    assert(!tag_image_validate(im.bytes, im.size));
    puts("Storage: 200 append cuts, compaction cuts, recovery, IDs and 64-tag limit passed");
}

static nfcv_reply_t vresponse;
static void frame_crc(void) {
    uint8_t frame[] = "123456789\0\0";
    assert(iso15693_crc16(frame, 9) == 0x906eu);
    iso15693_append_crc(frame, 9);
    assert(frame[9] == 0x6e && frame[10] == 0x90);
    assert(iso15693_check_crc(frame, 11));
    frame[0] ^= 1;
    assert(!iso15693_check_crc(frame, 11));
    assert(!iso15693_check_crc(NULL, 11));
    assert(!iso15693_check_crc(frame, 0) && !iso15693_check_crc(frame, 1));
    iso15693_append_crc(frame, 0);
    assert(iso15693_check_crc(frame, 2));
}

static bool vcommand(const uint8_t *bytes, unsigned n) {
    iso15693_frame_t f = {
        .byte_count = n + 2, .crc_ok = true, .high_data_rate = true, .single_subcarrier = true};
    memcpy(f.bytes, bytes, n);
    iso15693_append_crc(f.bytes, n);
    memset(&vresponse, 0, sizeof(vresponse));
    return nfcv_session_handle_frame(&f, &vresponse);
}

static void slix(void) {
    tag_image_t im = image();
    im.bytes[15] = 3;
    tag_image_t original = im;
    assert(nfcv_session_configure_from_image(&im));
    nfcv_session_init();
    uint8_t cmd[16] = {0x26, 1, 0};
    assert(vcommand(cmd, 3) && vresponse.len == 10 && vresponse.bytes[2] == 3);
    cmd[0] = 6;
    assert(!vcommand(cmd, 3) && nfcv_session_inventory_pending());
    assert(!nfcv_session_advance_inventory_slot(&vresponse));
    assert(!nfcv_session_advance_inventory_slot(&vresponse));
    assert(nfcv_session_advance_inventory_slot(&vresponse) && vresponse.bytes[2] == 3 &&
           !nfcv_session_inventory_pending());
    cmd[0] = 2;
    cmd[1] = 0x21;
    cmd[2] = 1;
    memcpy(cmd + 3, "TEST", 4);
    assert(vcommand(cmd, 7) && vresponse.bytes[0] == 0);
    cmd[1] = 0x20;
    assert(vcommand(cmd, 3) && vresponse.len == 5 && !memcmp(vresponse.bytes + 1, "TEST", 4));
    nfcv_session_reset_on_field_loss();
    assert(vcommand(cmd, 3) && !memcmp(vresponse.bytes + 1, "TEST", 4));
    assert(!memcmp(im.bytes + 132, "\x04\x05\x06\x07", 4));
    cmd[1] = 0x22;
    assert(vcommand(cmd, 3));
    assert(!memcmp(im.bytes, original.bytes, im.size));
    cmd[1] = 0x21;
    assert(vcommand(cmd, 7) && vresponse.bytes[0] == 1 &&
           vresponse.bytes[1] == ISO15693_ERR_BLOCK_LOCKED);
    /* Reload restores original bytes and lock state, not the reader's writes. */
    nfcv_session_init();
    cmd[1] = ISO15693_CMD_READ_SINGLE;
    assert(vcommand(cmd, 3) && !memcmp(vresponse.bytes + 1, original.bytes + 132, 4));
    cmd[1] = ISO15693_CMD_WRITE_SINGLE;
    assert(vcommand(cmd, 7) && vresponse.bytes[0] == 0);
    /* A rejected configuration must leave the previous original available. */
    tag_image_t invalid = original;
    invalid.bytes[TAG_IMAGE_VERSION_OFFSET]++;
    assert(!nfcv_session_configure_from_image(&invalid));
    nfcv_session_init();
    cmd[1] = ISO15693_CMD_READ_SINGLE;
    assert(vcommand(cmd, 3) && !memcmp(vresponse.bytes + 1, original.bytes + 132, 4));
    cmd[1] = 0x23;
    cmd[2] = 7;
    cmd[3] = 1;
    assert(vcommand(cmd, 4) && vresponse.bytes[0] == 1);
    im.bytes[7] = 1;
    write_le32(im.bytes + 76, 0x01020304);
    assert(nfcv_session_configure_from_image(&im));
    nfcv_session_init();
    cmd[0] = 0x26;
    cmd[1] = 1;
    cmd[2] = 0;
    assert(!vcommand(cmd, 3));
    cmd[0] = 2;
    cmd[1] = ISO15693_CMD_NXP_GET_RANDOM_NUMBER;
    cmd[2] = 4;
    assert(vcommand(cmd, 3) && vresponse.len == 3);
    uint8_t mask[2] = {vresponse.bytes[1], vresponse.bytes[2]};
    cmd[1] = ISO15693_CMD_NXP_SET_PASSWORD;
    cmd[3] = SLIX_PASSWORD_PRIVACY;
    for (unsigned i = 0; i < 4; i++)
        cmd[4 + i] = (i + 1) ^ mask[i % 2];
    assert(vcommand(cmd, 8) && vresponse.len == 1);
    cmd[0] = 0x26;
    cmd[1] = 1;
    cmd[2] = 0;
    assert(vcommand(cmd, 3));
    nfcv_session_init();
    assert(!vcommand(cmd, 3));
    puts("SLIX-L session: inventory slots, privacy/password, locks, bounds and volatile writes "
         "passed");
}

static bool unlock(uint8_t id, uint32_t password) {
    uint8_t cmd[8] = {2, ISO15693_CMD_NXP_GET_RANDOM_NUMBER, 4};
    assert(vcommand(cmd, 3) && vresponse.len == 3);
    uint8_t mask[2] = {vresponse.bytes[1], vresponse.bytes[2]};
    cmd[1] = ISO15693_CMD_NXP_SET_PASSWORD;
    cmd[3] = id;
    for (unsigned i = 0; i < 4; i++)
        cmd[4 + i] = (uint8_t)(password >> (24 - 8 * i)) ^ mask[i % 2];
    return vcommand(cmd, sizeof(cmd)) && vresponse.len == 1;
}

static void inventory_slots(void) {
    /* Cover every slot, including a mask whose slot bits cross a byte boundary. */
    const uint8_t masks[] = {0, 5};
    for (unsigned m = 0; m < sizeof(masks); m++) {
        for (unsigned target = 0; target < 16; target++) {
            tag_image_t im = image();
            uint8_t wire[ISO15693_UID_SIZE];
            iso15693_uid_to_wire(wire, im.bytes + TAG_IMAGE_UID_OFFSET);
            unsigned mask = masks[m];
            for (unsigned bit = 0; bit < 4; bit++) {
                unsigned at = mask + bit;
                wire[at / 8] = (wire[at / 8] & ~(1u << (at % 8))) |
                               (((target >> bit) & 1u) << (at % 8));
            }
            iso15693_uid_to_wire(im.bytes + TAG_IMAGE_UID_OFFSET, wire);
            assert(nfcv_session_configure_from_image(&im));
            nfcv_session_init();
            uint8_t cmd[] = {ISO15693_REQ_HIGH_RATE | ISO15693_REQ_INVENTORY,
                             ISO15693_CMD_INVENTORY, mask, wire[0] & 0x1fu};
            unsigned size = mask ? 4 : 3;
            assert(vcommand(cmd, size) == (target == 0));
            for (unsigned slot = 1; slot <= target; slot++) {
                assert(nfcv_session_inventory_pending());
                assert(nfcv_session_advance_inventory_slot(&vresponse) == (slot == target));
            }
            assert(vresponse.len == 10 && !memcmp(vresponse.bytes + 2, wire, sizeof(wire)));
            assert(!nfcv_session_inventory_pending());
            assert(!nfcv_session_advance_inventory_slot(&vresponse));
            cmd[0] |= ISO15693_REQ_SLOT1;
            assert(vcommand(cmd, size) && !nfcv_session_inventory_pending());
            if (mask) {
                cmd[3] ^= 1u;
                cmd[0] &= ~ISO15693_REQ_SLOT1;
                assert(!vcommand(cmd, size) && !nfcv_session_inventory_pending());
            }
        }
    }
    puts("NFC-V inventory: all 16 slots, single-slot responses and masked UIDs passed");
}

static void slix_controls(void) {
    tag_image_t im = image();
    assert(nfcv_session_configure_from_image(&im));
    nfcv_session_init();
    uint8_t cmd[8] = {2, ISO15693_CMD_NXP_RESET_EAS, 4};
    assert(vcommand(cmd, 3) && vresponse.len == 1);
    cmd[1] = ISO15693_CMD_NXP_EAS_ALARM;
    assert(vcommand(cmd, 3) && vresponse.len == 33);
    for (unsigned i = 0; i < 32; i++)
        assert(vresponse.bytes[i + 1] == 0);
    cmd[1] = ISO15693_CMD_NXP_SET_EAS;
    assert(vcommand(cmd, 3));
    cmd[1] = ISO15693_CMD_NXP_LOCK_EAS;
    assert(vcommand(cmd, 3));
    cmd[1] = ISO15693_CMD_NXP_RESET_EAS;
    assert(vcommand(cmd, 3));
    cmd[1] = ISO15693_CMD_NXP_EAS_ALARM;
    assert(vcommand(cmd, 3));
    for (unsigned i = 0; i < 32; i++)
        assert(vresponse.bytes[i + 1] == (uint8_t)(0xa5u ^ i));

    cmd[1] = ISO15693_CMD_NXP_PASSWORD_PROTECT_EAS_AFI;
    assert(!vcommand(cmd, 3));
    assert(unlock(SLIX_PASSWORD_EAS_AFI, 0));
    assert(vcommand(cmd, 3));
    nfcv_session_reset_on_field_loss();
    assert(!vcommand(cmd, 3));

    cmd[1] = ISO15693_CMD_NXP_WRITE_PASSWORD;
    cmd[3] = SLIX_PASSWORD_READ;
    memcpy(cmd + 4, "\x01\x02\x03\x04", 4);
    assert(!vcommand(cmd, sizeof(cmd)));
    assert(unlock(SLIX_PASSWORD_READ, 0));
    assert(vcommand(cmd, sizeof(cmd)));
    assert(!vcommand(cmd, sizeof(cmd)));
    assert(!unlock(SLIX_PASSWORD_READ, 0));
    assert(unlock(SLIX_PASSWORD_READ, 0x01020304));
    puts("SLIX controls: EAS state/locks, authentication reset and protected password updates "
         "passed");
}

static void bounds(void) {
    tag_image_t im = image();
    im.bytes[5] = TAG_IMAGE_SLIX;
    im.size = TAG_IMAGE_HEADER_SIZE + 160u * 5u;
    write_le16(im.bytes + 20, 160);
    write_le16(im.bytes + 22, 640);
    write_le16(im.bytes + 24, 160);
    for (unsigned i = 0; i < 640; i++)
        im.bytes[TAG_IMAGE_HEADER_SIZE + i] = (uint8_t)i;
    memset(im.bytes + TAG_IMAGE_HEADER_SIZE + 640, 0, 160);
    assert(nfcv_session_configure_from_image(&im));
    nfcv_session_init();

    /* Every legal count, with and without the per-block security byte. */
    for (unsigned option = 0; option < 2; option++) {
        for (unsigned count = 1; count <= 160; count++) {
            uint8_t cmd[] = {2u | (option ? ISO15693_REQ_OPTION : 0u), ISO15693_CMD_READ_MULTIPLE,
                             0, (uint8_t)(count - 1u)};
            assert(vcommand(cmd, sizeof(cmd)));
            unsigned size = 1u + count * (4u + option);
            if (size + ISO15693_CRC_SIZE <= sizeof(vresponse.bytes)) {
                assert(vresponse.len == size && vresponse.bytes[0] == ISO15693_RES_OK);
                assert(vresponse.bytes[size - 1u] == (uint8_t)(count * 4u - 1u));
            } else {
                assert(vresponse.len == 2 && vresponse.bytes[0] == ISO15693_RES_ERROR);
                assert(vresponse.bytes[1] == ISO15693_ERR_GENERIC);
            }
        }
    }
    uint8_t cmd[] = {2, ISO15693_CMD_GET_BLOCK_SECURITY, 0, 159};
    assert(vcommand(cmd, sizeof(cmd)) && vresponse.len == 161);
    cmd[3] = 255;
    assert(vcommand(cmd, sizeof(cmd)) && vresponse.bytes[0] == ISO15693_RES_ERROR);
    cmd[1] = ISO15693_CMD_READ_MULTIPLE;
    assert(vcommand(cmd, sizeof(cmd)) && vresponse.bytes[0] == ISO15693_RES_ERROR);

    iso15693_frame_t oversized = {
        .byte_count = ISO15693_FRAME_MAX_BYTES + 1u, .crc_ok = true, .single_subcarrier = true};
    assert(!nfcv_session_handle_frame(&oversized, &vresponse));
    puts("NFC-V: 320 multi-block response boundaries, count overflow and frame bounds passed");
}

int main(void) {
    storage();
    incompatible_store();
    frame_crc();
    slix();
    inventory_slots();
    slix_controls();
    bounds();
    puts("Core tests passed under address/undefined-behavior sanitizers");
}
