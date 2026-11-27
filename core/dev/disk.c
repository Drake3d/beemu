#include "disk.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Overlay: 64 KB chunks, allocated on first write, with a bitmap of the
// 512-byte sectors in the chunk that hold overlay data.
#define CHUNK_SHIFT 16
#define CHUNK_SIZE  (1u << CHUNK_SHIFT)
#define SECS_PER_CHUNK (CHUNK_SIZE / 512)

typedef struct {
    uint64_t index;           // chunk number + 1 (0 = empty slot)
    uint8_t  dirty[SECS_PER_CHUNK / 8];
    uint8_t *data;
} chunk_t;

struct disk {
    int      fd;
    uint64_t size;
    bool     writable;
    chunk_t *chunks;          // open-addressing hash table
    size_t   n_slots, n_used;
};

disk_t *disk_open(const char *path, bool writable, char *err, size_t errlen)
{
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) { snprintf(err, errlen, "%s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    uint8_t magic[4] = { 0 };
    if (fstat(fd, &st) != 0 || pread(fd, magic, 4, 0) < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno)); close(fd); return NULL;
    }
    if (!memcmp(magic, "QFI\xfb", 4)) {
        snprintf(err, errlen, "%s is a QCOW2 image; convert it first: qemu-img convert -O raw %s disk.raw", path, path);
        close(fd);
        return NULL;
    }
    if (st.st_size <= 0) { snprintf(err, errlen, "%s: empty image", path); close(fd); return NULL; }
    disk_t *d = calloc(1, sizeof *d);
    if (!d) { close(fd); snprintf(err, errlen, "out of memory"); return NULL; }
    d->fd = fd;
    d->size = (uint64_t)st.st_size;
    d->writable = writable;
    return d;
}

void disk_close(disk_t *d)
{
    if (!d) return;
    for (size_t i = 0; i < d->n_slots; i++) free(d->chunks[i].data);
    free(d->chunks);
    close(d->fd);
    free(d);
}

uint64_t disk_size(const disk_t *d) { return d->size; }
bool disk_writable(const disk_t *d) { return d->writable; }

static chunk_t *find_chunk(disk_t *d, uint64_t idx, bool create)
{
    if (create && (d->n_used + 1) * 2 > d->n_slots) {         // grow at 50% load
        size_t n = d->n_slots ? d->n_slots * 2 : 64;
        chunk_t *t = calloc(n, sizeof *t);
        if (!t) return NULL;
        for (size_t i = 0; i < d->n_slots; i++) {
            if (!d->chunks[i].index) continue;
            size_t j = (size_t)(d->chunks[i].index * 0x9E3779B97F4A7C15ull) & (n - 1);
            while (t[j].index) j = (j + 1) & (n - 1);
            t[j] = d->chunks[i];
        }
        free(d->chunks);
        d->chunks = t;
        d->n_slots = n;
    }
    if (!d->n_slots) return NULL;
    uint64_t key = idx + 1;
    size_t j = (size_t)(key * 0x9E3779B97F4A7C15ull) & (d->n_slots - 1);
    while (d->chunks[j].index) {
        if (d->chunks[j].index == key) return &d->chunks[j];
        j = (j + 1) & (d->n_slots - 1);
    }
    if (!create) return NULL;
    uint8_t *data = malloc(CHUNK_SIZE);
    if (!data) return NULL;
    d->chunks[j].index = key;
    d->chunks[j].data = data;
    memset(d->chunks[j].dirty, 0, sizeof d->chunks[j].dirty);
    d->n_used++;
    return &d->chunks[j];
}

static bool file_read(disk_t *d, uint64_t off, uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = pread(d->fd, buf, len, (off_t)off);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) { memset(buf, 0, len); return true; }    // past the end: zeros
        buf += n; off += (uint64_t)n; len -= (size_t)n;
    }
    return true;
}

bool disk_read(disk_t *d, uint64_t off, void *out, size_t len)
{
    if (off > d->size || len > d->size - off) return false;
    uint8_t *buf = out;
    if (!file_read(d, off, buf, len)) return false;
    if (!d->n_used) return true;
    // Patch in overlay sectors.
    for (uint64_t s = off / 512; s * 512 < off + len; s++) {
        chunk_t *c = find_chunk(d, s / SECS_PER_CHUNK, false);
        uint32_t k = (uint32_t)(s % SECS_PER_CHUNK);
        if (!c || !(c->dirty[k / 8] & (1u << (k % 8)))) continue;
        uint64_t a = s * 512 > off ? s * 512 : off, b = (s + 1) * 512 < off + len ? (s + 1) * 512 : off + len;
        memcpy(buf + (a - off), c->data + (a % CHUNK_SIZE), (size_t)(b - a));
    }
    return true;
}

bool disk_write(disk_t *d, uint64_t off, const void *in, size_t len)
{
    if (off > d->size || len > d->size - off) return false;
    const uint8_t *buf = in;
    if (d->writable) {
        while (len) {
            ssize_t n = pwrite(d->fd, buf, len, (off_t)off);
            if (n < 0) { if (errno == EINTR) continue; return false; }
            buf += n; off += (uint64_t)n; len -= (size_t)n;
        }
        return true;
    }
    for (uint64_t s = off / 512; s * 512 < off + len; s++) {
        chunk_t *c = find_chunk(d, s / SECS_PER_CHUNK, true);
        if (!c) return false;
        uint32_t k = (uint32_t)(s % SECS_PER_CHUNK);
        uint8_t *sec = c->data + (size_t)k * 512;
        if (!(c->dirty[k / 8] & (1u << (k % 8)))) {               // first write: start from the file
            if (!file_read(d, s * 512, sec, 512)) return false;
            c->dirty[k / 8] |= (uint8_t)(1u << (k % 8));
        }
        uint64_t a = s * 512 > off ? s * 512 : off, b = (s + 1) * 512 < off + len ? (s + 1) * 512 : off + len;
        memcpy(sec + (a - s * 512), buf + (a - off), (size_t)(b - a));
    }
    return true;
}
