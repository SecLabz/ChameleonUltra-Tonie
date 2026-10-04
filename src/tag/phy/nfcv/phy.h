#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "diagnostics.h"

/* Ultra backend: bounded polling with physical HF_ANT_SEL transmission. */
bool nfcv_phy_start(void);
/* Stop atomically and leave the antenna switch at its inactive level. */
void nfcv_phy_stop(void);
void nfcv_phy_poll(void);
bool nfcv_phy_field_present(void);
const uint32_t *nfcv_phy_get_stats(void);

#ifdef NFCV_DIAGNOSTICS
size_t nfcv_phy_format_diagnostics(char *out, size_t capacity);
bool nfcv_phy_test_response_delay(uint32_t cycles);
size_t nfcv_phy_copy_rx_edges(uint8_t *out, size_t capacity);
#endif
