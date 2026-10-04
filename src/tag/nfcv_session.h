#pragma once
#include "tag/protocol/nfcv/frame.h"
#include "tag/protocol/nfcv/reply.h"
#include "core/tag_image.h"

/* Configure successfully before session_init, and keep the original alive and unchanged.
 * Field-loss resets retain mutable RF memory and do not reread the original. */
bool nfcv_session_configure_from_image(const tag_image_t *image);
void nfcv_session_init(void);
void nfcv_session_reset_on_field_loss(void);
bool nfcv_session_inventory_pending(void);
bool nfcv_session_high_response_rate(void);
bool nfcv_session_advance_inventory_slot(nfcv_reply_t *response);
bool nfcv_session_handle_frame(const iso15693_frame_t *frame, nfcv_reply_t *response);
