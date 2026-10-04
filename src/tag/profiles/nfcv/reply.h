#pragma once
#include "tag/protocol/nfcv/commands.h"

#include <stdbool.h>
#include <stdint.h>

#include "tag/protocol/nfcv/reply.h"

/*
 * Response helpers for NFC-V tags.
 */
static inline void nfcv_reply_silent(nfcv_reply_t *response) {
    response->len = 0u;
}

static inline void nfcv_reply_ok(nfcv_reply_t *response) {
    response->bytes[0] = ISO15693_RES_OK;
    response->len = 1u;
}

static inline void nfcv_reply_error(nfcv_reply_t *response, uint8_t error_code) {
    response->bytes[0] = ISO15693_RES_ERROR;
    response->bytes[1] = error_code;
    response->len = 2u;
}
