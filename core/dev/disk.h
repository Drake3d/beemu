// Disk image backend: a raw image file (hard disk image or ISO), byte addressed.
//
// Writes go to an in-memory overlay unless the image was opened writable, so
// by default the guest can never modify the user's file (the overlay is lost
// on exit). QCOW2 images are rejected with a hint to convert them.
#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct disk disk_t;

disk_t  *disk_open(const char *path, bool writable, char *err, size_t errlen);
void     disk_close(disk_t *d);
uint64_t disk_size(const disk_t *d);                 // bytes
bool     disk_read (disk_t *d, uint64_t off, void *buf, size_t len);
bool     disk_write(disk_t *d, uint64_t off, const void *buf, size_t len);
bool     disk_writable(const disk_t *d);             // writes reach the file

#endif
