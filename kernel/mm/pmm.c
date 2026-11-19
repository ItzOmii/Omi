/* Physical page allocator: a bitmap over LIMINE_MEMMAP_USABLE regions.
 * See kernel/include/pmm.h for the contract. */

#include "../include/pmm.h"
#include "../include/limine_modules.h"
#include "../include/types.h"
#include "../include/spinlock.h"
#include "../include/percpu.h"

extern uint64_t hhdm_offset;
extern uint64_t kernel_phys_base;
extern uint64_t kernel_virt_base;
extern char __bss_end[];              /* from linker.ld */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t v);

/* One bit per 4 KB page, 1 = in use. 8 GB of coverage costs 256 KB. */
#define PMM_PAGES (PMM_MAX_BYTES / PMM_PAGE)
static uint8_t bitmap[PMM_PAGES / 8];

static uint64_t total_pages;
static uint64_t usable_pages;
static uint64_t first_free_hint;

static inline int bit_get(uint64_t i) { return bitmap[i >> 3] & (1u << (i & 7)); }
static inline void bit_set(uint64_t i) { bitmap[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bit_clr(uint64_t i) { bitmap[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static void mark_used(uint64_t phys, uint64_t len) {
  uint64_t first = phys / PMM_PAGE;
  uint64_t last = (phys + len + PMM_PAGE - 1) / PMM_PAGE;
  if (last > PMM_PAGES) last = PMM_PAGES;
  for (uint64_t i = first; i < last; i++) bit_set(i);
}

static void release_usable(uint64_t base, uint64_t len) {
  /* page-align inwards: never hand out a page we only partly own */
  uint64_t first = (base + PMM_PAGE - 1) / PMM_PAGE;
  uint64_t last = (base + len) / PMM_PAGE;
  if (last > PMM_PAGES) last = PMM_PAGES;
  for (uint64_t i = first; i < last; i++) {
    if (bit_get(i)) {
      bit_clr(i);
      usable_pages++;
    }
  }
}

void pmm_init(struct limine_memmap_response *mm, void *module_response) {
  for (uint64_t i = 0; i < sizeof bitmap; i++) bitmap[i] = 0xFF;   /* all used */
  total_pages = 0;
  usable_pages = 0;
  first_free_hint = 0;

  if (mm) {
    for (uint64_t i = 0; i < mm->entry_count; i++) {
      struct limine_memmap_entry *e = mm->entries[i];
      if (!e) continue;
      if (e->type != LIMINE_MEMMAP_USABLE) continue;
      release_usable(e->base, e->length);
      total_pages += e->length / PMM_PAGE;
    }
  }

  /* Reserved territory, whatever the map says: the first megabyte (BIOS and
   * VGA holes), the kernel image itself, and every boot module. */
  mark_used(0, 0x100000);

  if (kernel_phys_base) {
    uint64_t kstart = kernel_phys_base;
    uint64_t kend = (uint64_t)(uintptr_t)__bss_end - kernel_virt_base + kernel_phys_base;
    if (kend > kstart) mark_used(kstart, kend - kstart);
  }

  if (module_response) {
    struct meep_module_response *mr = (struct meep_module_response *)module_response;
    for (uint64_t i = 0; i < mr->module_count; i++) {
      void *addr = mr->modules[i] ? mr->modules[i]->address : 0;
      uint64_t size = mr->modules[i] ? mr->modules[i]->size : 0;
      if (!addr || !size) continue;
      uint64_t phys = (uint64_t)(uintptr_t)addr;
      if (phys >= hhdm_offset) phys -= hhdm_offset;   /* modules arrive via HHDM */
      mark_used(phys, size);
    }
  }

  /* Recount after the reservations: usable_pages was counted before them. */
  usable_pages = 0;
  for (uint64_t i = 0; i < PMM_PAGES; i++) if (!bit_get(i)) usable_pages++;
}

/* One bitmap, one hint, one lock. Without it two cores scanning at the same time
 * both see a clear bit, both set it, and both hand out the same frame - which
 * shows up much later as two subsystems corrupting each other. */
static spinlock_t pmm_lock = SPINLOCK_INIT;

uint64_t pmm_alloc_page(void) {
  int irq = irq_save();
  spin_lock(&pmm_lock);
  for (uint64_t i = first_free_hint; i < PMM_PAGES; i++) {
    if (!bit_get(i)) {
      bit_set(i);
      first_free_hint = i + 1;
      if (usable_pages) usable_pages--;
      uint64_t phys = i * PMM_PAGE;
      /* hand out zeroed pages: a stale frame must never leak old memory */
      uint8_t *p = (uint8_t *)(uintptr_t)(hhdm_offset + phys);
      for (uint64_t b = 0; b < PMM_PAGE; b += 8) *(uint64_t *)(p + b) = 0;
      spin_unlock(&pmm_lock);
      irq_restore(irq);
      percpu_t *me = percpu_self();
      if (me) me->pages_allocated++;
      return phys;
    }
  }
  first_free_hint = 0;              /* wraps once, in case of a late free */
  spin_unlock(&pmm_lock);
  irq_restore(irq);
  return 0;
}

void pmm_free_page(uint64_t phys) {
  uint64_t i = phys / PMM_PAGE;
  if (i >= PMM_PAGES) return;
  int irq = irq_save();
  spin_lock(&pmm_lock);
  if (!bit_get(i)) { spin_unlock(&pmm_lock); irq_restore(irq); return; } /* double free */
  bit_clr(i);
  usable_pages++;
  if (i < first_free_hint) first_free_hint = i;
  spin_unlock(&pmm_lock);
  irq_restore(irq);
}

uint64_t pmm_total_pages(void) { return total_pages; }
uint64_t pmm_free_pages(void) { return usable_pages; }

void pmm_report(void) {
  serial_puts("[pmm] pages total=");
  serial_puthex(total_pages);
  serial_puts(" free=");
  serial_puthex(usable_pages);
  serial_puts(" (");
  serial_puthex(usable_pages * PMM_PAGE / (1024 * 1024));
  serial_puts(" MB free)\n");
}