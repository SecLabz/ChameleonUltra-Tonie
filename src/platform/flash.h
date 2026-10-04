#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    bool (*read)(void *context, uint32_t offset, void *data, size_t size);
    bool (*write)(void *context, uint32_t offset, const void *data, size_t size);
    bool (*erase)(void *context, uint32_t offset, size_t size);
} flash_io_t;
