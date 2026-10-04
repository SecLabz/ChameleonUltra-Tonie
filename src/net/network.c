#include "network.h"
#include "http.h"
#include "dhcp.h"
#include "platform/platform.h"
#include "tusb.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include <string.h>
extern uint8_t tud_network_mac_address[6];
static struct netif net;
static struct pbuf *incoming;
typedef struct {
    struct tcp_pcb *pcb;
    uint8_t input[HTTP_REQUEST_MAX_BYTES], output[HTTP_RESPONSE_MAX_BYTES];
    size_t received, sent;
    unsigned idle;
    bool responding;
    http_response_t response;
} connection_t;
static connection_t connections[2];
u32_t sys_now(void) {
    return platform_millis();
}

static void release(connection_t *c) {
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_sent(c->pcb, NULL);
        tcp_poll(c->pcb, NULL, 0);
        tcp_err(c->pcb, NULL);
    }
    c->pcb = NULL;
}

static void error(void *arg, err_t err) {
    (void)err;
    if (arg)
        ((connection_t *)arg)->pcb = NULL;
}

static err_t flush(connection_t *c) {
    while (c->sent < c->response.header_size + c->response.size) {
        const void *p;
        size_t n;
        if (c->sent < c->response.header_size) {
            p = c->response.header + c->sent;
            n = c->response.header_size - c->sent;
        } else {
            size_t offset = c->sent - c->response.header_size;
            p = c->response.body + offset;
            n = c->response.size - offset;
        }
        if (n > tcp_sndbuf(c->pcb))
            n = tcp_sndbuf(c->pcb);
        if (!n)
            break;
        err_t err = tcp_write(c->pcb, p, n, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK)
            break;
        c->sent += n;
    }
    tcp_output(c->pcb);
    return ERR_OK;
}

static err_t sent(void *arg, struct tcp_pcb *pcb, u16_t n) {
    (void)n;
    connection_t *c = arg;
    c->idle = 0;
    if (c->sent == c->response.header_size + c->response.size && pcb->unacked == NULL &&
        pcb->unsent == NULL) {
        if (tcp_close(pcb) == ERR_OK) {
            release(c);
            return ERR_OK;
        }
    }
    return flush(c);
}

static err_t poll(void *arg, struct tcp_pcb *pcb) {
    connection_t *c = arg;
    if (++c->idle > 10) {
        release(c);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return c->responding ? flush(c) : ERR_OK;
}

static err_t receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    connection_t *c = arg;
    if (!p) {
        release(c);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    if (c->responding || p->tot_len > sizeof(c->input) - c->received) {
        pbuf_free(p);
        release(c);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    pbuf_copy_partial(p, c->input + c->received, p->tot_len, 0);
    c->received += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    c->idle = 0;
    http_request_t request;
    int complete = http_parse_request(c->input, c->received, &request);
    if (complete) {
        c->response = http_dispatch_request(&request, c->output);
        c->responding = true;
        return flush(c);
    }
    return ERR_OK;
}

static err_t accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK)
        return err;
    for (unsigned i = 0; i < 2; i++)
        if (!connections[i].pcb) {
            connection_t *c = &connections[i];
            memset(c, 0, sizeof(*c));
            c->pcb = pcb;
            tcp_arg(pcb, c);
            tcp_recv(pcb, receive);
            tcp_sent(pcb, sent);
            tcp_poll(pcb, poll, 2);
            tcp_err(pcb, error);
            return ERR_OK;
        }
    tcp_abort(pcb);
    return ERR_ABRT;
}

static err_t transmit(struct netif *n, struct pbuf *p) {
    (void)n;
    if (!tud_ready())
        return ERR_USE;
    if (!tud_network_can_xmit(p->tot_len))
        return ERR_MEM;
    tud_network_xmit(p, 0);
    return ERR_OK;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)arg;
    struct pbuf *p = ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

bool tud_network_recv_cb(const uint8_t *p, uint16_t size) {
    if (incoming)
        return false;
    if (!size)
        return true;
    incoming = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!incoming)
        return false;
    if (pbuf_take(incoming, p, size) != ERR_OK) {
        pbuf_free(incoming);
        incoming = NULL;
        return false;
    }
    return true;
}

void tud_network_init_cb(void) {
    if (incoming) {
        pbuf_free(incoming);
        incoming = NULL;
    }
}

static err_t init(struct netif *n) {
    n->name[0] = 'f';
    n->name[1] = 'n';
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, tud_network_mac_address, 6);
    n->hwaddr[5] ^= 1;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    n->linkoutput = transmit;
    n->output = etharp_output;
    return ERR_OK;
}

bool network_init(void) {
    lwip_init();
    ip4_addr_t ip, mask, lease, gw;
    IP4_ADDR(&ip, 192, 168, 7, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&lease, 192, 168, 7, 2);
    IP4_ADDR(&gw, 0, 0, 0, 0);
    if (!netif_add(&net, &ip, &mask, &gw, NULL, init, ethernet_input))
        return false;
    netif_set_default(&net);
    netif_set_up(&net);
    netif_set_link_up(&net);
    if (web_dhcp_init(&net, &ip, &mask, &lease) != ERR_OK)
        return false;
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb || tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK)
        return false;
    pcb = tcp_listen(pcb);
    if (!pcb)
        return false;
    tcp_accept(pcb, accept);
    return true;
}

void network_poll(void) {
    if (incoming) {
        struct pbuf *p = incoming;
        incoming = NULL;
        if (net.input(p, &net) != ERR_OK)
            pbuf_free(p);
        tud_network_recv_renew();
    }
    sys_check_timeouts();
}
