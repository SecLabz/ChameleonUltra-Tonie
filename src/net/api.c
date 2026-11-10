#include "tag/phy/nfcv/phy.h"
#include "tag/nfcv_emulation.h"
#ifdef NFCV_SCAN_DIAGNOSTICS
#include "tag/nfcv_reader.h"
#endif
#include "api.h"
#include "firmware_version.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef struct {
    char *p;
    size_t cap, n;
    bool ok;
} json_writer_t;
static void json_append_format(json_writer_t *j, const char *fmt, ...) {
    if (!j->ok)
        return;
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(j->p + j->n, j->cap - j->n, fmt, a);
    va_end(a);
    if (n < 0 || (size_t)n >= j->cap - j->n)
        j->ok = false;
    else
        j->n += (size_t)n;
}

static void json_append_string(json_writer_t *j, const char *s) {
    json_append_format(j, "\"");
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\')
            json_append_format(j, "\\%c", c);
        else if (c < 32)
            json_append_format(j, "\\u%04x", c);
        else
            json_append_format(j, "%c", c);
    }
    json_append_format(j, "\"");
}

static void json_append_image_uid(json_writer_t *j, const uint8_t *p) {
    json_append_format(j, "\"");
    for (unsigned i = 0; i < p[TAG_IMAGE_UID_SIZE_OFFSET]; i++)
        json_append_format(j, "%02X", p[TAG_IMAGE_UID_OFFSET + i]);
    json_append_format(j, "\"");
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static bool query_parameter(const char *uri, const char *key, char *out, size_t cap) {
    const char *p = strchr(uri, '?');
    if (!p)
        return false;
    p++;
    bool found = false;
    size_t keylen = strlen(key);
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end)
            end = p + strlen(p);
        const char *eq = memchr(p, '=', end - p);
        if (eq && (size_t)(eq - p) == keylen && !memcmp(p, key, keylen)) {
            if (found)
                return false;
            found = true;
            size_t n = 0;
            for (const char *v = eq + 1; v < end; v++) {
                int c = (uint8_t)*v;
                if (c == '%') {
                    if (v + 2 >= end || hex_digit_value(v[1]) < 0 || hex_digit_value(v[2]) < 0)
                        return false;
                    c = hex_digit_value(v[1]) * 16 + hex_digit_value(v[2]);
                    v += 2;
                } else if (c == '+')
                    c = ' ';
                if (!c || n + 1 >= cap)
                    return false;
                out[n++] = (char)c;
            }
            out[n] = 0;
        }
        p = *end ? end + 1 : end;
    }
    return found;
}

static bool query_uint32(const char *uri, const char *key, uint32_t *out) {
    char p[16]; /* Ten decimal digits, terminator and spare capacity. */
    if (!query_parameter(uri, key, p, sizeof(p)) || !p[0])
        return false;
    uint32_t n = 0;
    for (char *s = p; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT32_MAX - (*s - '0')) / 10)
            return false;
        n = n * 10 + *s - '0';
    }
    *out = n;
    return true;
}

static api_response_t api_result_response(operation_result_t r, json_writer_t *j) {
    j->n = 0;
    j->ok = true;
    json_append_format(j, "{\"ok\":%s,\"error\":\"%s\"}", r == RESULT_OK ? "true" : "false",
                       operation_result_name(r));
    return (api_response_t){r == RESULT_OK              ? 200
                            : r == RESULT_NOT_FOUND     ? 404
                            : r == RESULT_NO_TAG        ? 404
                            : r == RESULT_RF_BUSY       ? 409
                            : r == RESULT_STORAGE_ERROR ? 503
                                                        : 400,
                            "application/json", j->ok ? j->n : 0};
}

static api_response_t api_json_response(const json_writer_t *json) {
    if (!json->ok)
        return (api_response_t){500, "application/json", 0};
    return (api_response_t){200, "application/json", json->n};
}

static api_response_t api_get_state(json_writer_t *json) {
    const tag_library_entry_t *e = tag_library_find(&app_state.library, app_state.selected);
    json_append_format(
        json,
        "{\"ready\":%s,\"active\":%s,\"selected\":%lu,\"usb\":%s,\"battery\":%u,\"count\":%u,"
        "\"limit\":%u,\"name\":",
        app_state.ready ? "true" : "false", app_state.emulating ? "true" : "false",
        (unsigned long)app_state.selected, platform_usb_power_present() ? "true" : "false",
        platform_battery_percent(), app_state.library.count, TAG_LIBRARY_CAPACITY);
    json_append_string(json, e ? e->name : "");
    json_append_format(
        json, ",\"profile\":%u,\"uid\":",
        app_state.selected ? app_state.loaded_image.bytes[TAG_IMAGE_PROFILE_OFFSET] : 0);
    if (app_state.selected)
        json_append_image_uid(json, app_state.loaded_image.bytes);
    else
        json_append_format(json, "\"\"");
    json_append_format(json, ",\"favorites\":[");
    for (unsigned i = 0; i < FAVORITE_SLOT_COUNT; i++)
        json_append_format(json, "%s%lu", i ? "," : "",
                           (unsigned long)app_state.library.favorites[i]);
    json_append_format(
        json,
        "],\"firmware\":\"%s\",\"storageUsed\":%lu,\"storageCapacity\":%u,\"uptimeMs\":%"
        "lu,\"bootloader\":\"%s\"}",
        FIRMWARE_VERSION, (unsigned long)(app_state.library.tail - app_state.library.bank),
        TAG_LIBRARY_BANK_SIZE, (unsigned long)platform_millis(), "dfu");
    return api_json_response(json);
}

static api_response_t api_get_tags(json_writer_t *json) {
    if (!app_state.ready)
        return api_result_response(RESULT_STORAGE_ERROR, json);
    json_append_format(json, "{\"tags\":[");
    tag_image_t im;
    for (unsigned i = 0; i < app_state.library.count; i++) {
        const tag_library_entry_t *e = &app_state.library.tags[i];
        if (tag_library_read_image(&app_state.library, e->id, &im) != RESULT_OK)
            return api_result_response(RESULT_STORAGE_ERROR, json);
        json_append_format(json, "%s{\"id\":%lu,\"name\":", i ? "," : "", (unsigned long)e->id);
        json_append_string(json, e->name);
        json_append_format(json, ",\"profile\":%u,\"uid\":", im.bytes[TAG_IMAGE_PROFILE_OFFSET]);
        json_append_image_uid(json, im.bytes);
        json_append_format(json, "}");
    }
    json_append_format(json, "]}");
    return api_json_response(json);
}

static api_response_t api_get_tag(uint32_t id, bool emulation, uint8_t *out, size_t capacity,
                                  json_writer_t *json) {
    operation_result_t r;
    tag_image_t im;
    if (!app_state.ready)
        return api_result_response(RESULT_STORAGE_ERROR, json);
    r = tag_library_read_image(&app_state.library, id, &im);
    if (r != RESULT_OK)
        return api_result_response(r, json);
    if (im.size > capacity)
        return api_result_response(RESULT_INVALID_DATA, json);
    if (emulation)
        tag_image_prepare_emulation(&im);
    memcpy(out, im.bytes, im.size);
    return (api_response_t){200, "application/octet-stream", im.size};
}

static api_response_t api_mutate(const char *path, const char *uri, const uint8_t *body,
                                 size_t size, uint8_t *out, size_t capacity,
                                 json_writer_t *json) {
    uint32_t id = 0, slot = 0;
    char name[TAG_NAME_MAX_BYTES + 1u];
    operation_result_t r = RESULT_INVALID_DATA;
    app_note_activity();
    if (!strcmp(path, "/api/v1/library-reset") && !size)
        return api_result_response(app_reset_library(platform_flash_io()), json);
    if (!strcmp(path, "/api/v1/scan") && !size) {
#ifdef NFCV_SCAN_DIAGNOSTICS
        uint32_t extended, auth;
        nfcv_reader_trace_configure(query_uint32(uri, "extended", &extended) && extended == 1u,
                                    query_uint32(uri, "auth", &auth) && auth == 1u);
#endif
        tag_image_t im;
        r = app_scan_tag(&im);
        if (r != RESULT_OK)
            return api_result_response(r, json);
        if (im.size > capacity)
            return api_result_response(RESULT_INVALID_DATA, json);
        memcpy(out, im.bytes, im.size);
        return (api_response_t){200, "application/octet-stream", im.size};
    }
    if (!strcmp(path, "/api/v1/import") && query_parameter(uri, "name", name, sizeof(name)) &&
        size <= TAG_IMAGE_MAX_BYTES) {
        tag_image_t im;
        im.size = size;
        if (size)
            memcpy(im.bytes, body, size);
        r = app_import_tag(name, &im, &id);
        if (r == RESULT_OK) {
            json_append_format(json, "{\"ok\":true,\"id\":%lu}", (unsigned long)id);
            return api_json_response(json);
        }
    } else if (size)
        r = RESULT_INVALID_DATA;
    else if (!strcmp(path, "/api/v1/rename") && query_uint32(uri, "id", &id) &&
             query_parameter(uri, "name", name, sizeof(name)))
        r = app_rename_tag(id, name);
    else if (!strcmp(path, "/api/v1/delete") && query_uint32(uri, "id", &id))
        r = app_delete_tag(id);
    else if (!strcmp(path, "/api/v1/favorite") && query_uint32(uri, "slot", &slot) && slot >= 1 &&
             slot <= FAVORITE_SLOT_COUNT && query_uint32(uri, "id", &id))
        r = app_assign_favorite(slot - 1, id);
    else if (!strcmp(path, "/api/v1/emulate") && query_uint32(uri, "id", &id))
        r = app_start_emulation(id);
    else if (!strcmp(path, "/api/v1/stop")) {
        app_pause_emulation();
        r = RESULT_OK;
    } else if (!strcmp(path, "/api/v1/bootloader")) {
        app_pause_emulation();
        platform_request_bootloader();
        r = RESULT_OK;
    }
    return api_result_response(r, json);
}

#ifdef NFCV_DIAGNOSTICS
static bool api_diagnostic_request(const char *path, const char *uri, bool get, bool post,
                                   size_t size, uint8_t *out, size_t capacity, json_writer_t *json,
                                   api_response_t *response) {
    if (!strcmp(path, "/api/v1/test-idle") && post) {
        if (size)
            *response = api_result_response(RESULT_INVALID_DATA, json);
        else if (nfcv_emulation_field_present())
            *response = api_result_response(RESULT_RF_BUSY, json);
        else
            *response = api_result_response(
                platform_test_idle_wake() ? RESULT_OK : RESULT_STORAGE_ERROR, json);
    } else if (!strcmp(path, "/api/v1/test-rf-edges") && get) {
        size_t length = nfcv_phy_copy_rx_edges(out, capacity);
        *response = (api_response_t){200, "application/octet-stream", length};
    } else if (!strcmp(path, "/api/v1/test-rf-delay") && post) {
        uint32_t cycles;
        if (!query_uint32(uri, "cycles", &cycles) || size)
            *response = api_result_response(RESULT_INVALID_DATA, json);
        else if (nfcv_emulation_field_present())
            *response = api_result_response(RESULT_RF_BUSY, json);
        else
            *response = api_result_response(
                nfcv_phy_test_response_delay(cycles) ? RESULT_OK : RESULT_INVALID_DATA, json);
    } else if (!strcmp(path, "/api/v1/test-platform") && get) {
        size_t length = platform_format_diagnostics((char *)out, capacity);
        *response = (api_response_t){length ? 200 : 500, "application/json", length};
    } else if (!strcmp(path, "/api/v1/test-storage") && get) {
        /* Read-only recovery snapshot; deliberately absent from production. */
        uint32_t offset, length;
        if (!query_uint32(uri, "offset", &offset) || !query_uint32(uri, "length", &length) ||
            !length || length > TAG_IMAGE_MAX_BYTES || length > capacity ||
            offset > TAG_LIBRARY_FLASH_SIZE || length > TAG_LIBRARY_FLASH_SIZE - offset) {
            *response = api_result_response(RESULT_INVALID_DATA, json);
        } else {
            flash_io_t flash = platform_flash_io();
            if (!flash.read(flash.ctx, offset, out, length))
                *response = api_result_response(RESULT_STORAGE_ERROR, json);
            else
                *response = (api_response_t){200, "application/octet-stream", length};
        }
    } else {
        return false;
    }
    return true;
}
#endif

api_response_t api_dispatch_request(const char *method, const char *uri, const uint8_t *body,
                                    size_t size, uint8_t *out, size_t capacity) {
    platform_note_control_activity();
    json_writer_t json = {(char *)out, capacity, 0, true};
    char path[64];
    size_t path_length = strcspn(uri, "?");
    if (path_length >= sizeof(path))
        return api_result_response(RESULT_INVALID_DATA, &json);
    memcpy(path, uri, path_length);
    path[path_length] = 0;
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
#ifdef NFCV_DIAGNOSTICS
    api_response_t diagnostic;
    if (api_diagnostic_request(path, uri, get, post, size, out, capacity, &json, &diagnostic))
        return diagnostic;
#endif
    if (get) {
#ifdef NFCV_SCAN_DIAGNOSTICS
        if (!strcmp(path, "/api/v1/test-scan")) {
            size_t count;
            const nfcv_scan_trace_t *trace = nfcv_reader_trace(&count);
            json_append_format(&json, "[");
            for (size_t i = 0; i < count; i++)
                json_append_format(&json, "%s[%u,%u,%u,%u,%u]", i ? "," : "", trace[i].command,
                                   trace[i].response_bytes, trace[i].response_error, trace[i].offset,
                                   trace[i].capture_us);
            json_append_format(&json, "]");
            return api_json_response(&json);
        }
#endif
        if (!strcmp(path, "/api/v1/state"))
            return api_get_state(&json);
        if (!strcmp(path, "/api/v1/tags"))
            return api_get_tags(&json);
        uint32_t id, emulation;
        if (!strcmp(path, "/api/v1/tag") && query_uint32(uri, "id", &id))
            return api_get_tag(id, query_uint32(uri, "emulation", &emulation) && emulation == 1u,
                               out, capacity, &json);
    } else if (post) {
        return api_mutate(path, uri, body, size, out, capacity, &json);
    }
    return (api_response_t){404, "application/json", 0};
}
