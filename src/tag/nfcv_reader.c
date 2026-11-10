#include "tag/nfcv_reader.h"
#include "cards/slix/tag.h"
#include "common/bytes.h"
#include "tag/protocol/nfcv/commands.h"
#include "tag/protocol/nfcv/frame.h"
#include "tag/protocol/nfcv/reader_codec.h"
#include <string.h>

static size_t exchange_cmd(const uint8_t *payload, size_t len, uint8_t *rx, size_t rx_cap,
                           unsigned attempts) {
    if (len + ISO15693_CRC_SIZE > NFCV_READER_MAX_FRAME)
        return 0;
    uint8_t tx[NFCV_READER_MAX_FRAME];
    memcpy(tx, payload, len);
    iso15693_append_crc(tx, (uint16_t)len);
    for (unsigned i = 0; i < attempts; i++) {
        size_t got = nfcv_reader_phy_exchange(tx, len + ISO15693_CRC_SIZE, rx, rx_cap);
        if (got >= 3 && iso15693_check_crc(rx, (uint16_t)got))
            return got;
    }
    return 0;
}

static bool inventory(uint8_t *rx, unsigned attempts) {
    const uint8_t cmd[] = {ISO15693_REQ_HIGH_RATE | ISO15693_REQ_INVENTORY | ISO15693_REQ_SLOT1,
                           ISO15693_CMD_INVENTORY, 0x00u};
    return exchange_cmd(cmd, sizeof(cmd), rx, NFCV_READER_MAX_FRAME, attempts) == 12u &&
           rx[0] == ISO15693_RES_OK;
}

static bool unlock_tonie_privacy(const uint8_t rnd[2]) {
    uint8_t set_pw[8] = {
        ISO15693_REQ_HIGH_RATE,
        ISO15693_CMD_NXP_SET_PASSWORD,
        0x04u,
        SLIX_PASSWORD_PRIVACY,
    };
    write_le32(set_pw + 4u, __builtin_bswap32(TONIE_PRIVACY_PASSWORD));
    for (unsigned i = 0; i < 4u; i++)
        set_pw[4u + i] ^= rnd[i % 2u];
    uint8_t rx[NFCV_READER_MAX_FRAME];
    return exchange_cmd(set_pw, sizeof(set_pw), rx, sizeof(rx), 2u) == 3u &&
           rx[0] == ISO15693_RES_OK;
}

static operation_result_t read_tag(tag_image_t *out) {
    uint8_t inv_rx[NFCV_READER_MAX_FRAME];
    bool privacy = !inventory(inv_rx, 2u);
    bool authenticate = privacy;
#ifdef NFCV_SCAN_DIAGNOSTICS
    authenticate |= nfcv_reader_trace_verify_auth();
#endif
    if (authenticate) {
        const uint8_t get_rnd[] = {ISO15693_REQ_HIGH_RATE, ISO15693_CMD_NXP_GET_RANDOM_NUMBER,
                                   0x04u};
        uint8_t rnd_rx[NFCV_READER_MAX_FRAME];
        size_t rnd_len = exchange_cmd(get_rnd, sizeof(get_rnd), rnd_rx, sizeof(rnd_rx), 2u);
        if (rnd_len != 5u || rnd_rx[0] != ISO15693_RES_OK)
            return RESULT_NO_TAG;
        if (!unlock_tonie_privacy(rnd_rx + 1u) || !inventory(inv_rx, 3u))
            return RESULT_INVALID_DATA;
    }

    uint8_t dsfid = inv_rx[1];
    uint8_t wire_uid[ISO15693_UID_SIZE];
    memcpy(wire_uid, inv_rx + 2u, ISO15693_UID_SIZE);
    uint8_t uid[ISO15693_UID_SIZE];
    iso15693_uid_to_wire(uid, wire_uid);
    if (uid[0] != 0xe0u)
        return RESULT_INVALID_DATA;

    bool slix_l = uid[1] == 0x04u && uid[2] == 0x03u;
    uint8_t afi = 0x00u;
    uint8_t ic_ref = slix_l ? 0x03u : 0x01u;
    unsigned block_count = slix_l ? TAG_IMAGE_SLIX_L_BLOCKS : 28u;

    uint8_t sys_cmd[2u + ISO15693_UID_SIZE] = {ISO15693_REQ_HIGH_RATE | ISO15693_REQ_ADDRESS,
                                               ISO15693_CMD_GET_SYSTEM_INFO};
    memcpy(sys_cmd + 2u, wire_uid, ISO15693_UID_SIZE);
    uint8_t sys_rx[NFCV_READER_MAX_FRAME];
    size_t sys_len = exchange_cmd(sys_cmd, sizeof(sys_cmd), sys_rx, sizeof(sys_rx), 3u);
    if (sys_len >= 12u && sys_rx[0] == ISO15693_RES_OK &&
        !memcmp(sys_rx + 2u, wire_uid, ISO15693_UID_SIZE)) {
        uint8_t info_flags = sys_rx[1];
        size_t pos = 10u, payload_end = sys_len - ISO15693_CRC_SIZE;
        if ((info_flags & 0x01u) && pos < payload_end)
            dsfid = sys_rx[pos++];
        if ((info_flags & 0x02u) && pos < payload_end)
            afi = sys_rx[pos++];
        if ((info_flags & 0x04u) && pos + 1u < payload_end) {
            unsigned bc = (unsigned)sys_rx[pos++] + 1u;
            unsigned bs = (unsigned)(sys_rx[pos++] & 0x1fu) + 1u;
            if (bs != TAG_IMAGE_BLOCK_BYTES || bc > TAG_IMAGE_MAX_BLOCKS)
                return RESULT_INVALID_DATA;
            block_count = bc;
        }
        if ((info_flags & 0x08u) && pos < payload_end)
            ic_ref = sys_rx[pos++];
    }

    uint16_t data_size = (uint16_t)(block_count * TAG_IMAGE_BLOCK_BYTES);
    uint16_t lock_size = (uint16_t)block_count;
    out->size = (uint16_t)(TAG_IMAGE_HEADER_SIZE + data_size + lock_size);
    memset(out->bytes, 0, out->size);
    uint8_t *blocks = out->bytes + TAG_IMAGE_HEADER_SIZE;
    uint8_t *locks = blocks + data_size;

    uint8_t cmd[4u + ISO15693_UID_SIZE] = {0}, rx[NFCV_READER_MAX_FRAME];
    memcpy(cmd + 2u, wire_uid, ISO15693_UID_SIZE);
    for (unsigned b = 0; b < block_count; b++) {
        cmd[0] = ISO15693_REQ_HIGH_RATE | ISO15693_REQ_ADDRESS | ISO15693_REQ_OPTION;
        cmd[1] = ISO15693_CMD_READ_SINGLE;
        cmd[2u + ISO15693_UID_SIZE] = (uint8_t)b;
        size_t got = exchange_cmd(cmd, sizeof(cmd) - 1u, rx, sizeof(rx), 3u);
        if (got == 8u && rx[0] == ISO15693_RES_OK) {
            locks[b] = rx[1] ? 1u : 0u;
            memcpy(blocks + b * TAG_IMAGE_BLOCK_BYTES, rx + 2u, TAG_IMAGE_BLOCK_BYTES);
            continue;
        }
        cmd[0] &= ~ISO15693_REQ_OPTION;
        got = exchange_cmd(cmd, sizeof(cmd) - 1u, rx, sizeof(rx), 3u);
        if (got != 7u || rx[0] != ISO15693_RES_OK)
            return RESULT_NO_TAG;
        memcpy(blocks + b * TAG_IMAGE_BLOCK_BYTES, rx + 1u, TAG_IMAGE_BLOCK_BYTES);
        cmd[1] = ISO15693_CMD_GET_BLOCK_SECURITY;
        /* The final byte requests one block (count minus one). */
        got = exchange_cmd(cmd, sizeof(cmd), rx, sizeof(rx), 2u);
        locks[b] = (got == 4u && rx[0] == ISO15693_RES_OK && rx[1] != 0u) ? 1u : 0u;
    }

    memcpy(out->bytes, "NFCI", 4u);
    out->bytes[TAG_IMAGE_VERSION_OFFSET] = TAG_IMAGE_VERSION;
    out->bytes[TAG_IMAGE_PROFILE_OFFSET] =
        (block_count == TAG_IMAGE_SLIX_L_BLOCKS) ? TAG_IMAGE_SLIX_L : TAG_IMAGE_SLIX;
    out->bytes[TAG_IMAGE_UID_SIZE_OFFSET] = TAG_IMAGE_UID_BYTES;
    out->bytes[TAG_IMAGE_FLAGS_OFFSET] = privacy ? TAG_IMAGE_FLAG_PRIVACY : 0u;
    memcpy(out->bytes + TAG_IMAGE_UID_OFFSET, uid, TAG_IMAGE_UID_BYTES);
    out->bytes[TAG_IMAGE_DSFID_OFFSET] = dsfid;
    out->bytes[TAG_IMAGE_AFI_OFFSET] = afi;
    out->bytes[TAG_IMAGE_IC_REF_OFFSET] = ic_ref;
    out->bytes[TAG_IMAGE_BLOCK_SIZE_OFFSET] = TAG_IMAGE_BLOCK_BYTES;
    write_le16(out->bytes + TAG_IMAGE_BLOCK_COUNT_OFFSET, (uint16_t)block_count);
    write_le16(out->bytes + TAG_IMAGE_DATA_SIZE_OFFSET, data_size);
    write_le16(out->bytes + TAG_IMAGE_LOCK_SIZE_OFFSET, lock_size);
    if (privacy)
        write_le32(out->bytes + TAG_IMAGE_PASSWORDS_OFFSET + 4u * SLIX_PASSWORD_TYPE_PRIVACY,
                   TONIE_PRIVACY_PASSWORD);
    return tag_image_validate(out->bytes, out->size) ? RESULT_OK : RESULT_INVALID_DATA;
}

operation_result_t nfcv_reader_scan(tag_image_t *out) {
    if (!out)
        return RESULT_INVALID_DATA;
    nfcv_reader_phy_open();
    operation_result_t result = read_tag(out);
    nfcv_reader_phy_close();
    return result;
}
