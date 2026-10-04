/* Frame-to-frame NFC-V coordination: response rate and inventory slots. */
#include "tag/nfcv_session.h"
#include "common/bytes.h"
#include "cards/slix/tag.h"
#include "tag/profiles/nfcv/reply.h"

static const uint8_t *original;
static struct {
    nfcv_mem_t memory;
    nfcv_std_t standard;
    slix_t slix;
} product;

bool nfcv_session_configure_from_image(const tag_image_t *image) {
    if (!tag_image_validate(image->bytes, image->size))
        return false;
    original = image->bytes;
    return true;
}

/* Decode the validated original and copy its data into mutable RF state. */
static void load_original(void) {
    const uint8_t *data = original + TAG_IMAGE_HEADER_SIZE;
    uint8_t flags = original[TAG_IMAGE_FLAGS_OFFSET];
    uint32_t passwords[SLIX_PASSWORD_TYPE_COUNT];
    for (unsigned i = 0; i < SLIX_PASSWORD_TYPE_COUNT; i++)
        passwords[i] = read_le32(original + TAG_IMAGE_PASSWORDS_OFFSET + 4 * i);
    nfcv_mem_init(&product.memory, read_le16(original + TAG_IMAGE_BLOCK_COUNT_OFFSET), data,
                  data + read_le16(original + TAG_IMAGE_DATA_SIZE_OFFSET));
    nfcv_std_init(&product.standard, &product.memory, original + TAG_IMAGE_UID_OFFSET,
                  original[TAG_IMAGE_DSFID_OFFSET], original[TAG_IMAGE_AFI_OFFSET],
                  original[TAG_IMAGE_IC_REF_OFFSET], flags & TAG_IMAGE_FLAG_DSFID_LOCKED,
                  flags & TAG_IMAGE_FLAG_AFI_LOCKED);
    slix_init(&product.slix, &product.standard, flags & TAG_IMAGE_FLAG_EAS_LOCKED,
              flags & TAG_IMAGE_FLAG_PRIVACY, passwords);
}

static bool pending, high_rate;
static uint8_t slot_index, slot_target;
static nfcv_reply_t inventory;
void nfcv_session_init(void) {
    load_original();
    pending = false;
    high_rate = true;
}

void nfcv_session_reset_on_field_loss(void) {
    nfcv_std_reset_session(&product.standard);
    slix_reset_session(&product.slix);
    pending = false;
}

bool nfcv_session_inventory_pending(void) {
    return pending;
}

bool nfcv_session_high_response_rate(void) {
    return high_rate;
}

bool nfcv_session_advance_inventory_slot(nfcv_reply_t *response) {
    if (!pending)
        return false;
    if (++slot_index >= 16) {
        pending = false;
        return false;
    }
    if (slot_index != slot_target)
        return false;
    *response = inventory;
    pending = false;
    return true;
}

bool nfcv_session_handle_frame(const iso15693_frame_t *frame, nfcv_reply_t *response) {
    pending = false;
    response->len = 0;
    if (!frame->crc_ok || frame->byte_count < 4 || !frame->single_subcarrier)
        return false;
    high_rate = frame->high_data_rate;
    bool slots =
        (frame->bytes[0] & ISO15693_REQ_INVENTORY) && !(frame->bytes[0] & ISO15693_REQ_SLOT1);
    nfcv_request_t request;
    if (!nfcv_std_prepare(&product.standard, frame, &request) ||
        !slix_allows_command(&product.slix, request.command))
        return false;
    nfcv_std_result_t result = nfcv_std_handle(&product.standard, &request, response);
    if (result == NFCV_STANDARD_UNHANDLED)
        slix_handle(&product.slix, &request, response);
    if (slots && response->len && request.command == ISO15693_CMD_INVENTORY) {
        unsigned mask = request.params[0];
        uint8_t wire[8];
        iso15693_uid_to_wire(wire, product.standard.uid);
        slot_target = 0;
        for (unsigned b = 0; b < 4; b++)
            if (mask + b < 64)
                slot_target |= ((wire[(mask + b) / 8] >> ((mask + b) % 8)) & 1u) << b;
        slot_index = 0;
        inventory = *response;
        pending = slot_target != 0;
        if (pending)
            response->len = 0;
    }
    return response->len != 0;
}
