// Host side of the emulated network: Ethernet frames in and out of the guest's NIC.
// The core only sees this interface; backends (libslirp user-mode NAT, ...) live
// outside core/ (see platform/net_slirp.c), the way SDL stays behind platform.h.
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

typedef struct net_backend net_backend_t;
struct net_backend {
    // Guest -> network: one Ethernet frame (no CRC).
    void (*send)(net_backend_t *nb, const uint8_t *frame, size_t len);
    // Do pending host I/O and timers without blocking. Frames for the guest are
    // handed to deliver(); this is the only place a backend calls it from.
    void (*poll)(net_backend_t *nb);
    void (*destroy)(net_backend_t *nb);
    // Set by the machine before the first poll.
    void (*deliver)(void *opaque, const uint8_t *frame, size_t len);
    void  *deliver_opaque;
};

#endif
