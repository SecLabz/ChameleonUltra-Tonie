/* Exercise the actual DHCP server and lwIP pbufs; only UDP transport is mocked. */
#include "net/dhcp.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static struct udp_pcb pcb;
static udp_recv_fn callback;
static void *callback_arg;
static uint8_t reply[600];
static unsigned replies;
static u16_t reply_size;
struct udp_pcb *udp_new_ip_type(u8_t type) {
    assert(type == IPADDR_TYPE_V4);
    return &pcb;
}
err_t udp_bind(struct udp_pcb *p, const ip_addr_t *ip, u16_t port) {
    (void)ip;
    assert(p == &pcb && port == 67);
    return ERR_OK;
}

void udp_remove(struct udp_pcb *p) {
    assert(p == &pcb);
}

void udp_recv(struct udp_pcb *p, udp_recv_fn fn, void *arg) {
    assert(p == &pcb);
    callback = fn;
    callback_arg = arg;
}
err_t udp_sendto_if(struct udp_pcb *p, struct pbuf *b, const ip_addr_t *ip, u16_t port,
                    struct netif *netif) {
    assert(p == &pcb && port == 68 && netif && ip4_addr_get_u32(ip_2_ip4(ip)) == 0xffffffffu);
    assert(b->tot_len <= sizeof(reply));
    reply_size = b->tot_len;
    assert(pbuf_copy_partial(b, reply, b->tot_len, 0) == b->tot_len);
    replies++;
    return ERR_OK;
}

static uint8_t request[300];
static unsigned length;
static void start(uint8_t type, uint8_t mac) {
    memset(request, 0, sizeof(request));
    request[0] = 1;
    request[1] = 1;
    request[2] = 6;
    request[4] = 0x12;
    request[5] = 0x34;
    request[28] = mac;
    memcpy(request + 236, (uint8_t[]){99, 130, 83, 99}, 4);
    request[240] = 53;
    request[241] = 1;
    request[242] = type;
    length = 243;
}

static void address(unsigned option, unsigned a, unsigned b, unsigned c, unsigned d) {
    request[length++] = (uint8_t)option;
    request[length++] = 4;
    request[length++] = a;
    request[length++] = b;
    request[length++] = c;
    request[length++] = d;
}

static void send(bool split) {
    request[length++] = 255;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)(split ? 239 : length), PBUF_RAM);
    assert(p);
    assert(pbuf_take(p, request, p->len) == ERR_OK);
    if (split) {
        struct pbuf *tail = pbuf_alloc(PBUF_RAW, (u16_t)(length - 239), PBUF_RAM);
        assert(tail);
        assert(pbuf_take(tail, request + 239, tail->len) == ERR_OK);
        pbuf_cat(p, tail);
    }
    callback(callback_arg, &pcb, p, IP_ANY_TYPE, 68);
}

static void expect(unsigned previous, uint8_t type, uint8_t last) {
    assert(replies == previous + 1 && reply_size >= 300 && reply[0] == 2 && reply[1] == 1 &&
           reply[2] == 6);
    assert(reply[4] == 0x12 && reply[5] == 0x34);
    assert(reply[240] == 53 && reply[241] == 1 && reply[242] == type);
    assert(reply[243] == 54 && reply[244] == 4);
    assert(!memcmp(reply + 245, (uint8_t[]){192, 168, 7, 1}, 4));
    assert(reply[19] == last);
}

int main(void) {
    mem_init();
    memp_init();
    struct netif netif = {0};
    ip4_addr_t server, mask, lease;
    IP4_ADDR(&server, 192, 168, 7, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&lease, 192, 168, 7, 2);
    assert(web_dhcp_init(&netif, &server, &mask, &lease) == ERR_OK);
    start(1, 1);
    send(false);
    expect(0, 2, 2);
    start(3, 1);
    address(50, 192, 168, 7, 2);
    address(54, 192, 168, 7, 1);
    send(true);
    expect(1, 5, 2);
    assert(reply[28] == 1);
    start(3, 1);
    memcpy(request + 12, (uint8_t[]){192, 168, 7, 2}, 4);
    send(true);
    expect(2, 5, 2);
    start(1, 2);
    address(50, 192, 168, 7, 3);
    send(true);
    expect(3, 2, 3);
    assert(reply[28] == 2);
    start(3, 2);
    address(50, 192, 168, 7, 3);
    send(false);
    expect(4, 5, 3);
    start(3, 2);
    address(54, 192, 168, 7, 99);
    address(50, 192, 168, 7, 3);
    send(false);
    assert(replies == 5);
    start(3, 2);
    address(50, 10, 0, 0, 2);
    send(false);
    expect(5, 6, 0);
    start(3, 2);
    address(50, 192, 168, 7, 1);
    send(false);
    expect(6, 6, 0);
    start(3, 2);
    address(50, 192, 168, 7, 255);
    send(false);
    expect(7, 6, 0);
    start(3, 2);
    address(50, 192, 168, 7, 0);
    send(false);
    expect(8, 6, 0);
    start(1, 2);
    request[236] = 0;
    send(false);
    assert(replies == 9);
    start(1, 2);
    request[2] = 16;
    send(false);
    assert(replies == 9);
    start(1, 2);
    length = 241;
    send(false);
    assert(replies == 9);
    struct pbuf *short_packet = pbuf_alloc(PBUF_RAW, 100, PBUF_RAM);
    assert(short_packet);
    callback(callback_arg, &pcb, short_packet, IP_ANY_TYPE, 68);
    assert(replies == 9);
    /* Repeated reconnects must release both input fragments and the reply. */
    for (unsigned i = 0; i < 1024; i++) {
        start(3, (uint8_t)i);
        address(50, 192, 168, 7, 2);
        send(true);
        expect(9 + i, 5, 2);
        assert(lwip_stats.mem.used == 0);
    }
    puts("DHCP: discover/request, ciaddr renewal, changed MAC, real chained pbufs, server "
         "selection, NAK, malformed packets and 1024 leak-free renewals passed");
}
