#include "crc32.h"

uint32_t crc32_compute(const void *data, size_t size) {
    const uint8_t *p = data;
    uint32_t c = ~0u;
    while (size--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
    }
    return ~c;
}
