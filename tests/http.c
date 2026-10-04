/* Exercise framing and request guards without opening sockets or executing mutations. */
#include "net/http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned dispatches;
static const uint8_t *expected_body;
static size_t expected_size;

api_response_t api_dispatch_request(const char *method, const char *uri, const uint8_t *body,
                                    size_t size, uint8_t *out, size_t capacity) {
    (void)method;
    (void)uri;
    (void)out;
    (void)capacity;
    dispatches++;
    if (expected_body) {
        assert(size == expected_size);
        assert(!memcmp(body, expected_body, size));
    }
    return (api_response_t){200, "application/json", 0};
}

static void check(const char *method, const char *headers, const char *body, int complete,
                  unsigned status, unsigned calls) {
    uint8_t bytes[HTTP_REQUEST_MAX_BYTES + 1], output[HTTP_RESPONSE_MAX_BYTES];
    int n = snprintf((char *)bytes, sizeof(bytes), "%s /api/v1/stop HTTP/1.1\r\n%s\r\n%s", method,
                     headers, body);
    assert(n >= 0 && (size_t)n < sizeof(bytes));
    http_request_t request;
    dispatches = 0;
    assert(http_parse_request(bytes, n, &request) == complete);
    assert(http_dispatch_request(&request, output).status == status);
    assert(dispatches == calls);
}

static void fragmentation(void) {
    const uint8_t bytes[] = "POST /api/v1/import?name=Test HTTP/1.1\r\n"
                            "Host: 192.168.7.1\r\nContent-Type: application/octet-stream\r\n"
                            "Content-Length: 4\r\n\r\n\x00\x01\xff\x02";
    http_request_t request;
    uint8_t output[HTTP_RESPONSE_MAX_BYTES];
    for (size_t size = 0; size < sizeof(bytes) - 1; size++) {
        assert(http_parse_request(bytes, size, &request) == 0);
        dispatches = 0;
        assert(http_dispatch_request(&request, output).status == 400 && !dispatches);
    }
    expected_body = bytes + sizeof(bytes) - 5;
    expected_size = 4;
    assert(http_parse_request(bytes, sizeof(bytes) - 1, &request) == 1);
    dispatches = 0;
    assert(http_dispatch_request(&request, output).status == 200 && dispatches == 1);
    expected_body = NULL;
    /* Extra bytes cannot become a second request or an oversized body. */
    assert(http_parse_request(bytes, sizeof(bytes), &request) == -1);
    dispatches = 0;
    assert(http_dispatch_request(&request, output).status == 400 && !dispatches);
    uint8_t full[HTTP_REQUEST_MAX_BYTES + 1];
    memset(full, 'X', sizeof(full));
    assert(http_parse_request(full, sizeof(full) - 2, &request) == 0);
    assert(http_parse_request(full, sizeof(full) - 1, &request) == -1);
    assert(http_parse_request(full, sizeof(full), &request) == -1);
}

int main(void) {
    fragmentation();
    check("POST",
          "hOsT:\t192.168.7.1 \t\r\ncontent-length:\t0 \r\n"
          "Origin: http://192.168.7.1\r\nSec-Fetch-Site: same-origin\r\n",
          "", 1, 200, 1);
    check("POST", "Host: 192.168.7.1\r\nSec-Fetch-Site: none\r\n", "", 1, 200, 1);
    check("POST", "Host: 192.168.7.1\r\nX-Test: one\r\nX-Test: two\r\n", "", 1, 200, 1);
    check("POST", "Host: 192.168.7.1\r\nContent-Length: 0\r\ncontent-length: 0\r\n", "", -1, 400,
          0);
    check("POST", "Host: 192.168.7.1\r\nTransfer-Encoding: chunked\r\n", "", -1, 400, 0);
    check("POST", "Host: 192.168.7.1\r\nTransfer-Encoding:\r\n", "", -1, 400, 0);
    const char *invalid_lengths[] = {"", "-1", "+0", "1 0", "1025", "99999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid_lengths) / sizeof(*invalid_lengths); i++) {
        char headers[128];
        snprintf(headers, sizeof(headers), "Host: 192.168.7.1\r\nContent-Length: %s\r\n",
                 invalid_lengths[i]);
        check("POST", headers, "", -1, 400, 0);
    }
    check("POST", "Host: 192.168.7.1\r\nContent-Length : 0\r\n", "", -1, 400, 0);
    check("POST", "Host: 192.168.7.1\r\n X-Folded: value\r\n", "", -1, 400, 0);
    check("POST", "Host: 192.168.7.1\r\nX-Test: bad\x01value\r\n", "", -1, 400, 0);
    check("POST", "Host: 192.168.7.1\r\nX-Test: bad\x7fvalue\r\n", "", -1, 400, 0);
    check("POST", "", "", 1, 403, 0);
    check("POST", "Host: evil.example\r\n", "", 1, 403, 0);
    check("POST", "Host: 192.168.7.1\r\nhost: 192.168.7.1\r\n", "", 1, 403, 0);
    check("POST", "Host: 192.168.7.1\r\nOrigin: https://evil.example\r\n", "", 1, 403, 0);
    check("POST", "Host: 192.168.7.1\r\nOrigin: http://192.168.7.1\r\nOrigin:\r\n", "", 1, 400, 0);
    check("POST", "Host: 192.168.7.1\r\nSec-Fetch-Site: cross-site\r\n", "", 1, 403, 0);
    check("POST", "Host: 192.168.7.1\r\nSec-Fetch-Site: none\r\nSec-Fetch-Site: none\r\n", "", 1,
          400, 0);
    check("POST", "Host: 192.168.7.1\r\nContent-Length: 1\r\n", "X", 1, 415, 0);
    check("POST", "Host: 192.168.7.1\r\nContent-Length: 1\r\nContent-Type: text/plain\r\n", "X", 1,
          415, 0);
    check("POST",
          "Host: 192.168.7.1\r\nContent-Length: 1\r\n"
          "Content-Type: application/octet-stream\r\nContent-Type: application/octet-stream\r\n",
          "X", 1, 415, 0);
    /* Origin/content-type guards apply to POST as before, not read-only GET. */
    check("GET", "Host: 192.168.7.1\r\nOrigin: https://evil.example\r\n", "", 1, 200, 1);
    puts("HTTP parser: every fragmentation boundary, binary body, size limits and mutation guards "
         "passed");
}
