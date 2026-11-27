// User-mode networking through libslirp (the NAT QEMU uses for "-netdev user"):
// the guest sees a private 10.0.2.0/24 network with
//   10.0.2.2   gateway, and the Mac itself (connections to it reach the host's loopback)
//   10.0.2.3   DNS forwarder (uses the Mac's resolver)
//   10.0.2.15  the guest's address, handed out by DHCP
// Outgoing TCP/UDP becomes ordinary sockets on the Mac: no root, no tap device.
// Incoming connections need port forwards (host port on 127.0.0.1 -> guest port).
#ifndef NET_SLIRP_H
#define NET_SLIRP_H

#include <stdbool.h>
#include <stddef.h>
#include "net.h"

typedef struct {
    bool udp;
    int  host_port, guest_port;
} net_fwd_t;

// NULL with a message in err if libslirp refuses the setup (e.g. a port in use).
net_backend_t *net_slirp_create(const net_fwd_t *fwd, int n_fwd, char *err, size_t errlen);

#endif
