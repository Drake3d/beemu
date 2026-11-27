// DNS for the guest through the Mac's own resolver.
//
// libslirp forwards the guest's DNS queries as raw UDP to the first nameserver in
// /etc/resolv.conf. On a Mac that server is often unreachable (VPNs, encrypted DNS, a
// router that only answers some clients), while the system resolver (mDNSResponder) works.
// So the backend takes queries for the virtual DNS address out of the frame stream, resolves
// them with getaddrinfo()/getnameinfo() on worker threads, and hands back reply frames.
#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct net_dns net_dns_t;

net_dns_t *net_dns_create(void);
void       net_dns_destroy(net_dns_t *d);   // waits for lookups still running

// If `frame` is an IPv4 UDP query to dns_ip (network byte order) port 53, start answering
// it and return true (the frame is consumed). Anything else: false.
bool net_dns_intercept(net_dns_t *d, const uint8_t *frame, size_t len, uint32_t dns_ip);

// Hand every finished reply (a complete Ethernet frame) to cb. Call from the poll loop.
void net_dns_drain(net_dns_t *d, void (*cb)(void *opaque, const uint8_t *frame, size_t len), void *opaque);

#endif
