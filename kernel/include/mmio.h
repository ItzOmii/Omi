/*
 * Minimal MMIO mapping helpers
 */

#ifndef _MMIO_H
#define _MMIO_H

#include "types.h"

/* Map a physical MMIO range into virtual space.
 * Returns virtual base address or 0 on failure.
 */
uint64_t mmio_map_range(uint64_t phys_addr, size_t size);

/* Map physical memory at a virtual address of the caller's choosing. Needed by
 * anything that must live at a fixed low address - the AP startup trampoline,
 * which a startup IPI can only reach through real mode. Returns 1 on success. */
int mmio_map_at(uint64_t virt, uint64_t phys, size_t size);

#endif /* _MMIO_H */
