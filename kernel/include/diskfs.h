/* A block-backed file store on the ATA disk: the durable half of the RAM fs.
 *
 * Not a general filesystem - no directories, no free list, extents handed out by
 * a bump allocator that only moves forward. What it is: a place where a file
 * written before a reboot is still there after it, which is the one thing the
 * RAM filesystem could not do. Every limit here is reported (-ENOSPC when the
 * allocator runs out, -ENOENT for a name that is not there), never silent.
 *
 * Layout, sector 0 up:
 *   0        superblock (magic, record count, bump pointer, checksum)
 *   1..4     directory: 32 records of 64 bytes
 *   8..      file bytes, each file one contiguous run of sectors
 */
#ifndef OMI_DISKFS_H
#define OMI_DISKFS_H

#include <stdint.h>

#define DISK_MAX_FILE (64u * 1024)   /* per file; also the RAM cap it is loaded into */
#define DISK_MAX_REC  32

int          diskfs_init(void);      /* 0 = a disk answered and is formatted */
int          diskfs_present(void);
uint32_t     diskfs_sectors(void);
const char  *diskfs_model(void);
int          diskfs_count(void);
int          diskfs_entry(int i, const char **name, int *len);
int          diskfs_read(const char *name, uint8_t *buf, int max);      /* -> len | -errno */
int          diskfs_write(const char *name, const uint8_t *buf, int len);
int          diskfs_remove(const char *name);

#endif
