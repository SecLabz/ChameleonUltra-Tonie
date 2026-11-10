#pragma once
#include "core/result.h"
#include "core/tag_image.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TONIE_PRIVACY_PASSWORD 0x5b6efd7fu

void nfcv_reader_phy_open(void);
void nfcv_reader_phy_close(void);
size_t nfcv_reader_phy_exchange(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_cap);

operation_result_t nfcv_reader_scan(tag_image_t *out);

#ifdef NFCV_SCAN_DIAGNOSTICS
typedef struct {
    unsigned command, response_bytes, response_error, offset, capture_us;
} nfcv_scan_trace_t;

void nfcv_reader_trace_configure(bool extended_auth, bool verify_auth);
bool nfcv_reader_trace_verify_auth(void);
const nfcv_scan_trace_t *nfcv_reader_trace(size_t *count);
#endif
