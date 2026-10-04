#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tag/protocol/nfcv/frame.h"

typedef struct {
    uint8_t bytes[ISO15693_RESPONSE_MAX_BYTES];
    uint16_t len;
} nfcv_reply_t;
