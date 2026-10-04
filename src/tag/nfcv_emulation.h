#pragma once
#include "core/tag_image.h"

/* Reload a stored original into volatile profile RAM and start the PHY. */
bool nfcv_emulation_start_from_image(const tag_image_t *image);
void nfcv_emulation_stop(void);
void nfcv_emulation_poll(void);
bool nfcv_emulation_field_present(void);
