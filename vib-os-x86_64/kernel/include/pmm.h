/* Physical page allocator.
 *
 * Limine hands over a memory map and a direct map at hhdm_offset, so "allocate a
 * page" is: find a free bit, clear the other meaning, zero the page through the
 * direct map, and return its *physical* address. Callers that want to touch it
 * add hhdm_offset; callers filling a page-table entry take the address as-is.
 *
 * The bitmap starts with every page marked used, and only LIMINE_MEMMAP_USABLE
 * regions are released. That is deliberately conservative: bootloader
 * structures, ACPI tables, the framebuffer and anything the firmware wants stay
 * untouchable because we never free them in the first place. */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "limine.h"

#define PMM_PAGE      4096ULL
#define PMM_MAX_BYTES (8ULL * 1024 * 1024 * 1024)   /* bitmap covers 8 GB */

void     pmm_init(struct limine_memmap_response *mm, void *module_response);
uint64_t pmm_alloc_page(void);      /* physical address of a zeroed page, 0 = out */
void     pmm_free_page(uint64_t phys);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
void     pmm_report(void);          /* one line on the serial console */

#endif