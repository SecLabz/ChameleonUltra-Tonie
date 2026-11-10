#define _POSIX_C_SOURCE 200809L
#include "cards/slix/tag.h"
#include "common/bytes.h"
#include "tag/nfcv_emulation.h"
#include "tag/nfcv_reader.h"
#include "tag/nfcv_session.h"
#include "tag/profiles/nfcv/reply.h"
#include "platform/platform.h"
#include "core/app.h"
#include "net/http.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint8_t flash[TAG_LIBRARY_FLASH_SIZE];
static const char *path;
static bool save(void) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(flash, 1, sizeof(flash), f) == sizeof(flash);
    return fclose(f) == 0 && ok;
}

static bool read_flash(void *c, uint32_t o, void *p, size_t n) {
    (void)c;
    memcpy(p, flash + o, n);
    return true;
}

static bool write_flash(void *c, uint32_t o, const void *p, size_t n) {
    (void)c;
    const uint8_t *v = p;
    for (size_t i = 0; i < n; i++)
        if ((flash[o + i] & v[i]) != v[i])
            return false;
    for (size_t i = 0; i < n; i++)
        flash[o + i] &= v[i];
    return save();
}

static bool erase_flash(void *c, uint32_t o, size_t n) {
    (void)c;
    memset(flash + o, 255, n);
    return save();
}

flash_io_t platform_flash_io(void) {
    return (flash_io_t){NULL, read_flash, write_flash, erase_flash};
}

uint32_t platform_millis(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000u + t.tv_nsec / 1000000;
}

bool nfcv_emulation_start_from_image(const tag_image_t *im) {
    return tag_image_validate(im->bytes, im->size);
}

void nfcv_emulation_stop(void) {}

bool nfcv_emulation_field_present(void) {
    return false;
}

static tag_image_t sim_tag;

void nfcv_reader_phy_open(void) {
    memset(&sim_tag, 0, sizeof(sim_tag));
    sim_tag.size = TAG_IMAGE_HEADER_SIZE + 32u + 8u;
    memcpy(sim_tag.bytes, "NFCI", 4u);
    sim_tag.bytes[TAG_IMAGE_VERSION_OFFSET] = TAG_IMAGE_VERSION;
    sim_tag.bytes[TAG_IMAGE_PROFILE_OFFSET] = TAG_IMAGE_SLIX_L;
    sim_tag.bytes[TAG_IMAGE_UID_SIZE_OFFSET] = TAG_IMAGE_UID_BYTES;
    sim_tag.bytes[TAG_IMAGE_FLAGS_OFFSET] = TAG_IMAGE_FLAG_PRIVACY;
    const uint8_t uid[8] = {0xe0u, 0x04u, 0x03u, 0x50u, 0x11u, 0x22u, 0x33u, 0x44u};
    memcpy(sim_tag.bytes + TAG_IMAGE_UID_OFFSET, uid, 8u);
    sim_tag.bytes[TAG_IMAGE_IC_REF_OFFSET] = 0x03u;
    sim_tag.bytes[TAG_IMAGE_BLOCK_SIZE_OFFSET] = TAG_IMAGE_BLOCK_BYTES;
    write_le16(sim_tag.bytes + TAG_IMAGE_BLOCK_COUNT_OFFSET, 8u);
    write_le16(sim_tag.bytes + TAG_IMAGE_DATA_SIZE_OFFSET, 32u);
    write_le16(sim_tag.bytes + TAG_IMAGE_LOCK_SIZE_OFFSET, 8u);
    write_le32(sim_tag.bytes + TAG_IMAGE_PASSWORDS_OFFSET + 4u * SLIX_PASSWORD_TYPE_PRIVACY,
               TONIE_PRIVACY_PASSWORD);
    for (unsigned i = 0; i < 32u; i++)
        sim_tag.bytes[TAG_IMAGE_HEADER_SIZE + i] = (uint8_t)(0xa0u + i);
    nfcv_session_configure_from_image(&sim_tag);
    nfcv_session_init();
}

void nfcv_reader_phy_close(void) {}

size_t nfcv_reader_phy_exchange(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_cap) {
    if (tx_len > ISO15693_FRAME_MAX_BYTES)
        return 0;
    iso15693_frame_t frame = {.byte_count = (uint16_t)tx_len,
                              .crc_ok = iso15693_check_crc(tx, (uint16_t)tx_len),
                              .high_data_rate = true,
                              .single_subcarrier = true};
    memcpy(frame.bytes, tx, tx_len);
    nfcv_reply_t reply;
    if (!nfcv_session_handle_frame(&frame, &reply) ||
        (size_t)reply.len + ISO15693_CRC_SIZE > rx_cap)
        return 0;
    memcpy(rx, reply.bytes, reply.len);
    iso15693_append_crc(rx, reply.len);
    return (size_t)reply.len + ISO15693_CRC_SIZE;
}

void platform_show_selection(const uint32_t f[FAVORITE_SLOT_COUNT], uint32_t s) {
    (void)f;
    (void)s;
}

unsigned platform_battery_percent(void) {
    return 83;
}

bool platform_usb_power_present(void) {
    return true;
}

void platform_request_bootloader(void) {
    fprintf(stderr, "Bootloader requested (host simulation only).\n");
}

static bool send_all(int fd, const void *p, size_t n) {
    while (n) {
        ssize_t sent = send(fd, p, n, 0);
        if (sent <= 0)
            return false;
        p = (const uint8_t *)p + sent;
        n -= sent;
    }
    return true;
}

int main(int argc, char **argv) {
    path = argc > 1 ? argv[1] : "build/library.bin";
    memset(flash, 255, sizeof(flash));
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fread(flash, 1, sizeof(flash), f) != sizeof(flash)) {
            fprintf(stderr, "Invalid host flash image\n");
            return 1;
        }
        fclose(f);
    }
    if (app_init(platform_flash_io()) != RESULT_OK)
        fprintf(stderr, "Library unavailable; recovery commands remain accessible.\n");
    int sock = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(8080), .sin_addr.s_addr = htonl(0x7f000001u)};
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) || listen(sock, 4)) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "UI simulator: http://127.0.0.1:8080 (RF is simulated)\n");
    for (;;) {
        int fd = accept(sock, NULL, NULL);
        if (fd < 0)
            continue;
        struct timeval timeout = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        uint8_t request[HTTP_REQUEST_MAX_BYTES], output[HTTP_RESPONSE_MAX_BYTES];
        size_t n = 0;
        http_request_t parsed = {0};
        int complete = 0;
        while (n < sizeof(request) && complete == 0) {
            ssize_t got = recv(fd, request + n, sizeof(request) - n, 0);
            if (got <= 0)
                break;
            n += got;
            complete = http_parse_request(request, n, &parsed);
        }
        http_response_t response = http_dispatch_request(&parsed, output);
        if (send_all(fd, response.header, response.header_size))
            send_all(fd, response.body, response.size);
        close(fd);
    }
}

void platform_note_control_activity(void) {}
#ifdef NFCV_DIAGNOSTICS
size_t platform_format_diagnostics(char *out, size_t capacity) {
    const char *value = "{\"simulated\":true}";
    size_t n = strlen(value);
    if (n >= capacity)
        return 0;
    memcpy(out, value, n + 1);
    return n;
}
#endif
