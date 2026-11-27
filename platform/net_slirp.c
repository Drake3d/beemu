#include "net_slirp.h"
#include "net_dns.h"
#include <libslirp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct timer {
    SlirpTimerId id;
    void        *cb_opaque;
    int64_t      expire_ms;
    bool         armed;
    struct timer *next;
} timer_t_;

typedef struct {
    net_backend_t nb;           // first: the core holds a net_backend_t *
    Slirp        *slirp;
    struct pollfd *fds;
    int           nfds, cap;
    timer_t_     *timers;
    net_dns_t    *dns;          // answers queries to 10.0.2.3 through the Mac's resolver
    uint32_t      dns_ip;       // 10.0.2.3, network byte order
} slirp_be_t;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ---- callbacks from libslirp ---- */

static slirp_ssize_t cb_send(const void *buf, size_t len, void *opaque)
{
    slirp_be_t *be = opaque;
    if (be->nb.deliver) be->nb.deliver(be->nb.deliver_opaque, buf, len);
    return (slirp_ssize_t)len;
}

static void cb_guest_error(const char *msg, void *opaque)
{
    (void)opaque;
    fprintf(stderr, "beemu: network: %s\n", msg);
}

static int64_t cb_clock(void *opaque) { (void)opaque; return now_ns(); }

static void *cb_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque)
{
    slirp_be_t *be = opaque;
    timer_t_ *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->id = id;
    t->cb_opaque = cb_opaque;
    t->next = be->timers;
    be->timers = t;
    return t;
}

static void cb_timer_free(void *timer, void *opaque)
{
    slirp_be_t *be = opaque;
    for (timer_t_ **pp = &be->timers; *pp; pp = &(*pp)->next)
        if (*pp == timer) { *pp = ((timer_t_ *)timer)->next; free(timer); return; }
}

static void cb_timer_mod(void *timer, int64_t expire_ms, void *opaque)
{
    (void)opaque;
    timer_t_ *t = timer;
    t->expire_ms = expire_ms;
    t->armed = true;
}

static void cb_register_socket(slirp_os_socket s, void *opaque) { (void)s; (void)opaque; }
static void cb_notify(void *opaque) { (void)opaque; }

static const SlirpCb CALLBACKS = {
    .send_packet = cb_send,
    .guest_error = cb_guest_error,
    .clock_get_ns = cb_clock,
    .timer_free = cb_timer_free,
    .timer_mod = cb_timer_mod,
    .notify = cb_notify,
    .timer_new_opaque = cb_timer_new_opaque,
    .register_poll_socket = cb_register_socket,
    .unregister_poll_socket = cb_register_socket,
};

/* ---- polling ---- */

static int add_poll(slirp_os_socket fd, int events, void *opaque)
{
    slirp_be_t *be = opaque;
    if (be->nfds == be->cap) {
        int cap = be->cap ? be->cap * 2 : 32;
        struct pollfd *p = realloc(be->fds, sizeof *p * (size_t)cap);
        if (!p) return -1;
        be->fds = p;
        be->cap = cap;
    }
    short ev = 0;
    if (events & SLIRP_POLL_IN)  ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
    be->fds[be->nfds] = (struct pollfd){ .fd = fd, .events = ev };
    return be->nfds++;
}

static int get_revents(int idx, void *opaque)
{
    slirp_be_t *be = opaque;
    if (idx < 0 || idx >= be->nfds) return 0;
    short r = be->fds[idx].revents;
    int ev = 0;
    if (r & POLLIN)   ev |= SLIRP_POLL_IN;
    if (r & POLLOUT)  ev |= SLIRP_POLL_OUT;
    if (r & POLLPRI)  ev |= SLIRP_POLL_PRI;
    if (r & POLLERR)  ev |= SLIRP_POLL_ERR;
    if (r & POLLHUP)  ev |= SLIRP_POLL_HUP;
    return ev;
}

static void be_poll(net_backend_t *nb)
{
    slirp_be_t *be = (slirp_be_t *)nb;
    uint32_t timeout = 0;
    be->nfds = 0;
    slirp_pollfds_fill_socket(be->slirp, &timeout, add_poll, be);
    int r = be->nfds ? poll(be->fds, (nfds_t)be->nfds, 0) : 0;
    slirp_pollfds_poll(be->slirp, r < 0, get_revents, be);
    int64_t ms = now_ns() / 1000000;
    for (timer_t_ *t = be->timers; t; t = t->next)
        if (t->armed && t->expire_ms <= ms) {
            t->armed = false;
            slirp_handle_timer(be->slirp, t->id, t->cb_opaque);
        }
    if (be->dns && be->nb.deliver) net_dns_drain(be->dns, be->nb.deliver, be->nb.deliver_opaque);
}

static void be_send(net_backend_t *nb, const uint8_t *frame, size_t len)
{
    slirp_be_t *be = (slirp_be_t *)nb;
    if (be->dns && net_dns_intercept(be->dns, frame, len, be->dns_ip)) return;
    slirp_input(be->slirp, frame, (int)len);
}

static void be_destroy(net_backend_t *nb)
{
    slirp_be_t *be = (slirp_be_t *)nb;
    if (be->slirp) slirp_cleanup(be->slirp);
    net_dns_destroy(be->dns);
    while (be->timers) { timer_t_ *n = be->timers->next; free(be->timers); be->timers = n; }
    free(be->fds);
    free(be);
}

static struct in_addr ip4(const char *s)
{
    struct in_addr a;
    inet_pton(AF_INET, s, &a);
    return a;
}

net_backend_t *net_slirp_create(const net_fwd_t *fwd, int n_fwd, char *err, size_t errlen)
{
    slirp_be_t *be = calloc(1, sizeof *be);
    if (!be) { snprintf(err, errlen, "out of memory"); return NULL; }
    be->nb.send = be_send;
    be->nb.poll = be_poll;
    be->nb.destroy = be_destroy;

    SlirpConfig cfg = {
        .version = 6,
        .in_enabled = true,
        .vnetwork = ip4("10.0.2.0"),
        .vnetmask = ip4("255.255.255.0"),
        .vhost = ip4("10.0.2.2"),
        .vdhcp_start = ip4("10.0.2.15"),
        .vnameserver = ip4("10.0.2.3"),
        .vhostname = "beemu",
    };
    be->dns = net_dns_create();
    be->dns_ip = ip4("10.0.2.3").s_addr;
    be->slirp = slirp_new(&cfg, &CALLBACKS, be);
    if (!be->slirp) { snprintf(err, errlen, "libslirp setup failed"); be_destroy(&be->nb); return NULL; }
    for (int i = 0; i < n_fwd; i++) {
        if (slirp_add_hostfwd(be->slirp, fwd[i].udp, ip4("127.0.0.1"), fwd[i].host_port,
                              ip4("10.0.2.15"), fwd[i].guest_port) < 0) {
            snprintf(err, errlen, "cannot forward %s port %d (in use?)", fwd[i].udp ? "UDP" : "TCP", fwd[i].host_port);
            be_destroy(&be->nb);
            return NULL;
        }
    }
    return &be->nb;
}
