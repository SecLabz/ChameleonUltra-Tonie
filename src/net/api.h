#pragma once
#include "core/app.h"

typedef struct {
    unsigned status;
    const char *type;
    size_t size;
} api_response_t;

api_response_t api_dispatch_request(const char *method, const char *uri, const uint8_t *body,
                                    size_t size, uint8_t *out, size_t capacity);
