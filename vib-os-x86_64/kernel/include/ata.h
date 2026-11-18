/* ATA PIO block device: one disk, one sector at a time, by polling.
 *
 * PIO is the slowest way to talk to a disk and the only one that needs no DMA
 * engine, no interrupt handler and no bus-master programming, which makes it the
 * honest first step: it turns "there is no disk" into "there is a disk, and it
 * is slow", and every layer above it (a filesystem, a page cache, a real driver)
 * can be built against the same two calls.
 *
 * Bounded polling, never an unbounded wait: every state that this driver waits
 * on has a counter, and running out of it is reported as an error rather than a
 * hang. A missing disk must show up as -ENODEV at init, not as a dead machine.
 */
#ifndef OMI_ATA_H
#define OMI_ATA_H

#include <stdint.h>

#define ATA_SECTOR 512

int          ata_init(void);        /* 0 if a drive answered IDENTIFY */
int          ata_present(void);
uint32_t     ata_sectors(void);
const char  *ata_model(void);       /* NUL-terminated, spaces trimmed */
int          ata_read(uint32_t lba, void *buf);            /* one sector */
int          ata_write(uint32_t lba, const void *buf);     /* one sector */
uint32_t     ata_timeouts(void);    /* how many waits ran out: 0 on a healthy disk */

#endif
