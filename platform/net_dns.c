#include "net_dns.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define MAX_INFLIGHT 32
#define MAX_ANSWERS  8

typedef struct reply {
    struct reply *next;
    size_t len;
    uint8_t frame[];
} reply_t;

struct net_dns {
    pthread_mutex_t lock;
    pthread_cond_t  idle;
    reply_t *head, *tail;
    int inflight;
};

typedef struct {
    net_dns_t *d;
    uint8_t  guest_mac[6], dns_mac[6];
    uint32_t guest_ip, dns_ip;          // network byte order
    uint16_t guest_port;
    uint8_t  query[512];
    size_t   qlen;                      // header + question
    size_t   qend;                      // offset just past the question
    char     name[256];
    uint16_t qtype;
} job_t;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint16_t ip_checksum(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += rd16(p + i);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

// Append a name in DNS label form; false if it does not fit.
static bool put_name(uint8_t *out, size_t *pos, size_t cap, const char *name)
{
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (n == 0 || n > 63 || *pos + 1 + n >= cap) return false;
        out[(*pos)++] = (uint8_t)n;
        memcpy(out + *pos, p, n);
        *pos += n;
        p += n;
        if (*p == '.') p++;
    }
    if (*pos >= cap) return false;
    out[(*pos)++] = 0;
    return true;
}

static void push_reply(net_dns_t *d, const job_t *j, const uint8_t *dns, size_t dlen)
{
    size_t len = 14 + 20 + 8 + dlen;
    reply_t *r = malloc(sizeof *r + len);
    if (!r) return;
    uint8_t *f = r->frame;
    memcpy(f, j->guest_mac, 6);                       // Ethernet
    memcpy(f + 6, j->dns_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14;                             // IPv4
    memset(ip, 0, 20);
    ip[0] = 0x45;
    wr16(ip + 2, (uint16_t)(20 + 8 + dlen));
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, &j->dns_ip, 4);
    memcpy(ip + 16, &j->guest_ip, 4);
    wr16(ip + 10, ip_checksum(ip, 20));
    uint8_t *udp = ip + 20;                           // UDP (checksum 0 = none, allowed on IPv4)
    wr16(udp, 53);
    wr16(udp + 2, j->guest_port);
    wr16(udp + 4, (uint16_t)(8 + dlen));
    wr16(udp + 6, 0);
    memcpy(udp + 8, dns, dlen);
    r->len = len;
    r->next = NULL;
    pthread_mutex_lock(&d->lock);
    if (d->tail) d->tail->next = r; else d->head = r;
    d->tail = r;
    pthread_mutex_unlock(&d->lock);
}

static void *resolve(void *arg)
{
    job_t *j = arg;
    uint8_t out[512];
    size_t pos = j->qend;
    memcpy(out, j->query, j->qend);                   // header + the question, as asked
    uint16_t qflags = rd16(j->query + 2);
    int rcode = 0, answers = 0;

    if (j->qtype == 1 || j->qtype == 255) {           // A (or ANY): IPv4 addresses
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
        int e = getaddrinfo(j->name, NULL, &hints, &res);
        if (e == EAI_NONAME) rcode = 3;               // NXDOMAIN
        else if (e) rcode = 2;                        // SERVFAIL
        uint32_t seen[MAX_ANSWERS];
        for (struct addrinfo *a = res; a && answers < MAX_ANSWERS; a = a->ai_next) {
            uint32_t ip = ((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr;
            bool dup = false;
            for (int k = 0; k < answers; k++) dup |= seen[k] == ip;
            if (dup || pos + 16 > sizeof out) continue;
            seen[answers++] = ip;
            wr16(out + pos, 0xC00C);                  // the name in the question
            wr16(out + pos + 2, 1);                   // A
            wr16(out + pos + 4, 1);                   // IN
            out[pos + 6] = 0; out[pos + 7] = 0; wr16(out + pos + 8, 60);   // TTL 60 s
            wr16(out + pos + 10, 4);
            memcpy(out + pos + 12, &ip, 4);
            pos += 16;
        }
        if (res) freeaddrinfo(res);
    } else if (j->qtype == 12) {                      // PTR: d.c.b.a.in-addr.arpa
        unsigned a, b, c, dd;
        char host[256];
        struct sockaddr_in sin = { .sin_len = sizeof sin, .sin_family = AF_INET };
        if (sscanf(j->name, "%u.%u.%u.%u.in-addr.arpa", &dd, &c, &b, &a) == 4 && a < 256 && b < 256 && c < 256 && dd < 256) {
            sin.sin_addr.s_addr = htonl(a << 24 | b << 16 | c << 8 | dd);
            if (getnameinfo((struct sockaddr *)&sin, sizeof sin, host, sizeof host, NULL, 0, NI_NAMEREQD) == 0) {
                size_t rdata = pos + 12, p2 = rdata;
                if (p2 < sizeof out && put_name(out, &p2, sizeof out, host)) {
                    wr16(out + pos, 0xC00C);
                    wr16(out + pos + 2, 12);
                    wr16(out + pos + 4, 1);
                    out[pos + 6] = 0; out[pos + 7] = 0; wr16(out + pos + 8, 60);
                    wr16(out + pos + 10, (uint16_t)(p2 - rdata));
                    pos = p2;
                    answers = 1;
                }
            } else {
                rcode = 3;
            }
        } else {
            rcode = 3;
        }
    }
    // Anything else (AAAA, MX, ...): "no such record", which is right for an IPv4-only guest.

    wr16(out + 2, (uint16_t)(0x8000 | (qflags & 0x7900) | 0x0080 | rcode));   // QR, opcode+RD, RA
    wr16(out + 4, 1);
    wr16(out + 6, (uint16_t)answers);
    wr16(out + 8, 0);
    wr16(out + 10, 0);
    push_reply(j->d, j, out, pos);

    net_dns_t *d = j->d;
    free(j);
    pthread_mutex_lock(&d->lock);
    d->inflight--;
    pthread_cond_signal(&d->idle);
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

bool net_dns_intercept(net_dns_t *d, const uint8_t *f, size_t len, uint32_t dns_ip)
{
    if (len < 14 + 20 + 8 + 12 || f[12] != 0x08 || f[13] != 0x00) return false;
    const uint8_t *ip = f + 14;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17 || memcmp(ip + 16, &dns_ip, 4)) return false;
    if (rd16(ip + 6) & 0x3FFF) return false;          // fragments: leave them to slirp
    size_t ip_len = rd16(ip + 2);
    if (ip_len > len - 14 || ip_len < ihl + 8) return false;
    const uint8_t *udp = ip + ihl;
    if (rd16(udp + 2) != 53) return false;
    size_t ulen = rd16(udp + 4);
    if (ulen < 8 + 12 || ihl + ulen > ip_len) return true;   // malformed query to the DNS port: drop
    const uint8_t *dns = udp + 8;
    size_t dlen = ulen - 8;
    if ((dns[2] & 0x80) || rd16(dns + 4) != 1) return true;  // not a single-question query: drop

    job_t *j = calloc(1, sizeof *j);
    if (!j) return true;
    size_t p = 12, n = 0;                             // the question name (no compression in queries)
    while (p < dlen && dns[p]) {
        size_t l = dns[p];
        if (l > 63 || p + 1 + l >= dlen || n + l + 1 >= sizeof j->name) { free(j); return true; }
        if (n) j->name[n++] = '.';
        memcpy(j->name + n, dns + p + 1, l);
        n += l;
        p += 1 + l;
    }
    if (p + 5 > dlen || dlen > sizeof j->query) { free(j); return true; }
    j->name[n] = 0;
    j->qtype = rd16(dns + p + 1);
    j->qend = p + 5;
    memcpy(j->query, dns, j->qend);
    memcpy(j->guest_mac, f + 6, 6);
    memcpy(j->dns_mac, f, 6);
    memcpy(&j->guest_ip, ip + 12, 4);
    j->dns_ip = dns_ip;
    j->guest_port = rd16(udp);
    j->d = d;

    pthread_mutex_lock(&d->lock);
    bool busy = d->inflight >= MAX_INFLIGHT;
    if (!busy) d->inflight++;
    pthread_mutex_unlock(&d->lock);
    if (busy) { free(j); return true; }               // the guest's resolver retries
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, resolve, j) != 0) {
        free(j);
        pthread_mutex_lock(&d->lock);
        d->inflight--;
        pthread_mutex_unlock(&d->lock);
    }
    pthread_attr_destroy(&attr);
    return true;
}

void net_dns_drain(net_dns_t *d, void (*cb)(void *, const uint8_t *, size_t), void *opaque)
{
    pthread_mutex_lock(&d->lock);
    reply_t *r = d->head;
    d->head = d->tail = NULL;
    pthread_mutex_unlock(&d->lock);
    while (r) {
        reply_t *next = r->next;
        if (cb) cb(opaque, r->frame, r->len);
        free(r);
        r = next;
    }
}

net_dns_t *net_dns_create(void)
{
    net_dns_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->idle, NULL);
    return d;
}

void net_dns_destroy(net_dns_t *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    while (d->inflight) pthread_cond_wait(&d->idle, &d->lock);
    pthread_mutex_unlock(&d->lock);
    net_dns_drain(d, NULL, NULL);
    pthread_mutex_destroy(&d->lock);
    pthread_cond_destroy(&d->idle);
    free(d);
}
