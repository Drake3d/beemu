// Host monotonic clock, in nanoseconds. Devices take a clock_fn so tests can
// substitute a fake one.
#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>
#include <time.h>

typedef uint64_t (*clock_fn)(void *opaque);

static inline uint64_t clock_host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t clock_host(void *opaque) { (void)opaque; return clock_host_ns(); }

#endif
