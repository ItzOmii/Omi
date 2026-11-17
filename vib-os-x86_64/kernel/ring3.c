/* User mode (ring 3): address spaces, user page mapping, the int 0x80 gate and
 * the syscall table.
 *
 * Each user task owns a PML4 whose entries 0..255 are kernel-only copies of the
 * kernel's own tables (the kernel image at 0xFFFFFFFF80000000 and the HHDM stay
 * reachable at CPL 0) and whose slot 1 - virtual 0x0000008000000000 - is built
 * here for the program being loaded. Nothing user-visible exists outside that
 * window, so the framebuffer, the heap and the kernel text are unreachable from
 * CPL 3 even though they are mapped in the same address space.
 *
 * Frames for page tables and user pages come from a static page-aligned pool:
 * kmalloc only guarantees 16 bytes, and these have to be real 4 KB frames whose
 * physical addresses can go straight into a PTE. */

#include "include/ring3.h"
#include "include/pipe.h"
#include "include/task.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/types.h"
#include "include/guiipc.h"
#include "include/pmm.h"
#include "include/errno.h"
#include "include/elf.h"   /* elf_prepare_image: the load half of execve */
#include "include/epoll.h"  /* registration sets: the ABI epoll_ctl and friends speak */
#include "include/shfile.h"  /* the pages a MAP_SHARED mapping windows onto */
#include "include/kmalloc.h"
#include "include/net.h"
#include "include/omi_net.h"
#include "include/socket.h"
#include "include/vfs.h"   /* stat() answers from the boot-module filesystem */
#include "include/fd.h"    /* rfs_expose: boot modules are openable files */
#include "include/limine_modules.h"
#include "include/cow.h"
#include "include/walltime.h"

/* From the boot path. */
extern uint64_t hhdm_offset;
extern uint64_t kernel_phys_base;
extern uint64_t kernel_virt_base;

/* Early debug console. */
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t v);

/* Set by the syscall stub: the CS the cpu was in when it took the interrupt. */
/* The user rsp parked between the two instructions of the SYSCALL stack switch,
 * and the kernel stack top it is replaced with (maintained by gdt.c). */
uint64_t r3_user_rsp;
/* The syscall frame base, parked here by the shared tail. One syscall runs at a
 * time, so a static is enough - and it is the only way to reach the caller's
 * saved registers, which is what clone has to hand a new thread. */
uint64_t r3_frame_base;
/* The fifth and sixth Linux argument registers (r8, r9). They are parked here
 * rather than passed along because only clone takes five arguments today; a
 * second such syscall is the signal to widen the dispatch signature instead. */
uint64_t r3_arg4, r3_arg5;
extern uint64_t gdt_syscall_rsp;

volatile uint64_t r3_last_cs;
volatile uint64_t r3_last_rip;

/* PTE bits, from ring3.h */
#define PTE_P RING3_P
#define PTE_W RING3_W
#define PTE_U RING3_U

/* Frames come from the physical allocator; the kernel reaches them through the
 * direct map, page-table entries take the physical address. */

static uint64_t kpml4_phys;    /* the kernel's own PML4, seen through the HHDM */
static uint64_t *kpml4_virt;
static int      ready;

/* ------------------------------------------------------------- primitives */

uint64_t ring3_read_cr3(void) {
  uint64_t v;
  __asm__ volatile("mov %%cr3, %0" : "=r"(v));
  return v & ~0xFFFULL;
}

void ring3_write_cr3(uint64_t cr3) {
  __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uint64_t ring3_kernel_cr3(void) { return kpml4_phys; }
int ring3_ready(void) { return ready; }

/* Two kinds of kernel address reach here: image symbols (linked at
 * 0xFFFFFFFF80000000, loaded at a 1 MB physical base) and pages from the direct
 * map. Both have to turn back into the physical address a PTE wants. */
static uint64_t to_phys(const void *p) {
  uint64_t v = (uint64_t)(uintptr_t)p;
  /* Order matters: the kernel image sits at 0xFFFFFFFF80000000, which is
   * numerically *above* the direct map at 0xFFFF800000000000. Testing for the
   * direct map first would silently turn image pointers into nonsense. */
  if (v >= kernel_virt_base && kernel_virt_base)
    return v - kernel_virt_base + kernel_phys_base;
  return v - hhdm_offset;
}

/* A page-table frame or user page: physical allocator, zeroed, via the HHDM. */
static void *alloc_frame(void) {
  uint64_t phys = pmm_alloc_page();
  if (!phys) {
    serial_puts("[mm] out of physical pages\n");
    return 0;
  }
  return (void *)(uintptr_t)(hhdm_offset + phys);
}

static void log_hex(const char *label, uint64_t v) {
  serial_puts(label);
  serial_puthex(v);
  serial_puts("\n");
}

/* --------------------------------------------------------- address spaces */

/* A fresh space: the kernel's tables copied wholesale, nothing user-visible. */
/* A page that belongs to a shared segment rather than to this address space.
 * Bit 9 is ignored by the MMU and free for software, so the teardown walk can
 * tell "my private page" from "someone else's frame I merely mapped". */
#define PTE_SHARED (1ULL << 9)
/* Software bit on a table entry: this table was allocated for this address
 * space. Without it a table is the kernel's own (inherited through PML4 slot 0)
 * and must never be written or freed from here. */
#define PTE_OWNED  (1ULL << 10)

uint64_t ring3_new_space(void) {
  uint64_t *pml4 = (uint64_t *)alloc_frame();
  if (!pml4) return 0;
  for (int i = 0; i < 512; i++) pml4[i] = kpml4_virt[i];
  /* Slot 1 is the user window: the kernel keeps a tree there for its own
   * reasons, and copying it in made every process share one PDPT/PD/PT - so
   * they aliased each other's code and stacks, and reclaiming one space tore
   * pages out of the others. Each space builds its own window from scratch. */
  pml4[1] = 0;
  return to_phys(pml4);
}

/* ------------------------------------------------------------ address spaces */

static void mm_free_tables(mm_t *mm);

mm_t *ring3_new_mm(void) {
  uint64_t cr3 = ring3_new_space();
  if (!cr3) return 0;
  mm_t *mm = (mm_t *)kmalloc(sizeof(mm_t));
  if (!mm) {
    pmm_free_page(cr3);
    return 0;
  }
  uint8_t *b = (uint8_t *)mm;
  for (uint64_t i = 0; i < sizeof(mm_t); i++) b[i] = 0;
  mm->cr3 = cr3;
  mm->mmap_hint = 0;
  mm->stack_hint = RING3_MMAP_TOP;
  mm->refcount = 0;        /* every thread, including the first, adds one */
  mm->fds = fdtable_new();
  if (!mm->fds) { mm_free_tables(mm); kfree(mm); return 0; }
  return mm;
}

/* An address space cannot be freed while its CR3 is the one the CPU and the
 * kernel are still standing on. So the scheduler reaps: it has just loaded the
 * incoming task's CR3, which is exactly the moment the old space is dead. */
/* Every place an area stops existing goes through here. A shared area holds a
 * reference on its file, and a reference that is dropped in three places out of
 * four is a file that never frees its pages - a leak that looks like nothing
 * until a program maps and unmaps in a loop. */
static void  area_release(vm_area_t *a);
/* Reads a NUL-terminated string out of the caller's space, or says why not. */
static int   user_str(char *dst, uint64_t max, uint64_t uptr);
static mm_t *zombie[8];
static int   nzombies;

void mm_on_exit(task_t *t) {
  if (!t->mm) return;
  mm_t *mm = t->mm;
  if (t->cow_faults) mm->cow_participant = 1;
  t->mm = 0;
  t->cr3 = 0;
  if (--mm->refcount > 0) {
    serial_puts("[mm] space still in use: threads=");
    serial_puthex((uint64_t)mm->refcount);
    serial_puts(mm->cow_participant ? " participant\n" : "\n");
    return;
  }
  /* Reached only when the space is really going away - a thread exiting leaves
   * the space to its siblings, and their mappings still need these files. A
   * mapping's claim ends with the mapping, and nothing will walk these areas
   * again after the space is on the zombie list. */
  for (int i = 0; i < TASK_AREAS; i++) {
    if (mm->areas[i].used && mm->areas[i].shf) {
      shfile_unref((shfile_t *)mm->areas[i].shf);
      mm->areas[i].shf = 0;
    }
  }
  if (nzombies < (int)(sizeof zombie / sizeof zombie[0])) {
    zombie[nzombies++] = mm;
    return;
  }
  int socks = fd_count_kind(mm->fds, FK_SOCKET);
  int socks_live = sock_live();
  fdtable_free(mm->fds);
  sock_report_after_space(socks, socks_live);
  mm_free_tables(mm);          /* table full: nothing sane left to do */
  kfree(mm);
}

/* Give every page and every table in one half of an address space back to the
 * allocator. Only tables this space owns are touched at all: an entry without
 * PTE_OWNED points into the kernel's own page tables (the inherited identity
 * map in slot 0), and freeing or clearing that would take the machine down on
 * the next task switch. Shared segment pages are skipped for the same reason in
 * the other direction - they belong to the shm slot, and the other process
 * mapping them is still running. */
static void free_region(uint64_t *pml4, int i1) {
  if (!(pml4[i1] & PTE_P) || !(pml4[i1] & PTE_OWNED)) return;
  uint64_t *pdpt = (uint64_t *)(hhdm_offset + (pml4[i1] & ~0xFFFULL));
  for (int i2 = 0; i2 < 512; i2++) {
    if (!(pdpt[i2] & PTE_P) || !(pdpt[i2] & PTE_OWNED)) continue;
    uint64_t *pd = (uint64_t *)(hhdm_offset + (pdpt[i2] & ~0xFFFULL));
    for (int i3 = 0; i3 < 512; i3++) {
      if (!(pd[i3] & PTE_P) || !(pd[i3] & PTE_OWNED)) continue;
      if (pd[i3] & 0x80ULL) {                 /* 2 MB page: unused here */
        pmm_free_page(pd[i3] & ~0x1FFFFFULL);
        continue;
      }
      uint64_t *pt = (uint64_t *)(hhdm_offset + (pd[i3] & ~0xFFFULL));
      for (int i4 = 0; i4 < 512; i4++) {
        if (!(pt[i4] & PTE_P)) continue;
        if (pt[i4] & PTE_SHARED) continue;
        /* A frame shared with a forked child is released, not freed: the last
         * space to drop it is the one that frees it. An unshared frame has no
         * entry and comes straight back here as the owner. */
        {
          uint64_t phys = pt[i4] & ~0xFFFULL;
          if (cow_release(phys)) pmm_free_page(phys);
        }
      }
      pmm_free_page(pd[i3] & ~0xFFFULL);
    }
    pmm_free_page(pdpt[i2] & ~0xFFFULL);
  }
  pmm_free_page(pml4[i1] & ~0xFFFULL);
  pml4[i1] = 0;
}

static void mm_free_tables(mm_t *mm) {
  uint64_t *pml4 = (uint64_t *)(hhdm_offset + mm->cr3);
  free_region(pml4, 1);      /* our window */
  free_region(pml4, 0);      /* the low half, if a low program took any of it */
}

/* Frames a live space maps, one bit each: 512 MB of frames is 16 KB of bitmap.
 * Marking them once and then checking the bank is 40 times cheaper than walking
 * the page tables once per bank entry, which was slow enough to stall the boot. */
#define COW_MAP_BITS (1u << 17)
static uint8_t cow_map_bits[COW_MAP_BITS / 8];

static void cow_map_mark(uint64_t phys) {
  uint32_t bit = (uint32_t)(phys >> 12);
  if (bit < COW_MAP_BITS) cow_map_bits[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static int cow_map_has(uint64_t phys) {
  uint32_t bit = (uint32_t)(phys >> 12);
  if (bit >= COW_MAP_BITS) return 1;      /* out of range: assume real, don't cry wolf */
  return (cow_map_bits[bit >> 3] >> (bit & 7)) & 1;
}

/* One pass over every live space, marking what is mapped. */
static void cow_map_build(void) {
  for (unsigned i = 0; i < sizeof cow_map_bits; i++) cow_map_bits[i] = 0;
  for (int i = 1; i < task_count(); i++) {
    task_t *c = task_at(i);
    if (!c || !c->mm || !c->cr3 || c->state == TASK_DEAD) continue;
    if (c->cr3 & 0xFFFULL) continue;             /* not a frame: stale, skip it */
    uint64_t *pml4 = (uint64_t *)(hhdm_offset + c->cr3);
    for (int s = 0; s < 2; s++) {
      if (!(pml4[s] & PTE_P)) continue;
      uint64_t *pdpt = (uint64_t *)(hhdm_offset + (pml4[s] & ~0xFFFULL));
      for (int i2 = 0; i2 < 512; i2++) {
        if (!(pdpt[i2] & PTE_P)) continue;
        uint64_t *pd = (uint64_t *)(hhdm_offset + (pdpt[i2] & ~0xFFFULL));
        for (int i3 = 0; i3 < 512; i3++) {
          if (!(pd[i3] & PTE_P) || (pd[i3] & 0x80)) continue;   /* 2 MB page: not ours */
          uint64_t *pt = (uint64_t *)(hhdm_offset + (pd[i3] & ~0xFFFULL));
          for (int i4 = 0; i4 < 512; i4++)
            if (pt[i4] & PTE_P) cow_map_mark(pt[i4] & ~0xFFFULL);
        }
      }
    }
  }
}

static int cow_audit_cb(uint64_t phys, int refs, void *ctx) {
  int *orphans = (int *)ctx;
  if (cow_map_has(phys)) return 0;        /* accounted for: a live space has it */
  (*orphans)++;
  if (*orphans <= 8) {
    serial_puts("[cow] ORPHAN frame=");
    serial_puthex(phys);
    serial_puts(" refs=");
    serial_puthex((uint64_t)refs);
    serial_puts(": the bank says shared, no live space maps it\n");
  }
  return 0;
}

/* Run at address-space teardown, which is the only moment the bank can be asked a
 * question with a known answer: every space that ever fork()ed is gone, so a
 * frame still in the bank must be mapped by someone, or the accounting leaked.
 * Capped at two audits because each one walks every live page table. */
static void cow_audit(void) {
  static int done;
  if (done >= 64) return;                     /* a runaway guard, not a budget */
  cow_stats_t cs;
  cow_get_stats(&cs);
  done++;
  /* Printed even when the bank is empty: "nothing outstanding" is the answer this
   * whole exercise is asking for, and silence cannot be read as an answer. */
  cow_map_build();
  int orphans = 0;
  int entries = cow_walk(cow_audit_cb, &orphans);
  serial_puts("[cow] audit live=");
  serial_puthex((uint64_t)cs.live_frames);
  serial_puts(" entries=");
  serial_puthex((uint64_t)entries);
  serial_puts(" orphans=");
  serial_puthex((uint64_t)orphans);
  serial_puts(orphans ? " - ACCOUNTING LEAK\n" : " - every frame accounted for\n");
}

/* Called by the scheduler after it has switched CR3. */
void mm_reap(void) {
  if (!nzombies) return;
  uint64_t now = ring3_read_cr3();
  for (int i = 0; i < nzombies; i++) {
    if (zombie[i]->cr3 == now) continue;      /* still the live one */
    int socks = fd_count_kind(zombie[i]->fds, FK_SOCKET);
    int socks_live = sock_live();
    fdtable_free(zombie[i]->fds);
    sock_report_after_space(socks, socks_live);   /* endpoints must not outlive their space */
    mm_free_tables(zombie[i]);
    kfree(zombie[i]);
    serial_puts("[mm] address space freed\n");
    if (zombie[i]->cow_participant) {
      serial_puts("[mm] cow participant's space freed\n");
      cow_audit();                     /* the space is gone: the bank must agree */
    }
    /* Shared files outlive the descriptors that made them, by design: a mapping
     * keeps the pages. So the number that matters is not "did this program free
     * it" but "is anything left once every program has exited", and the last line
     * before the boot audit is that answer. */
    serial_puts("[shf] live=");
    serial_puthex((uint64_t)shfile_live_objects());
    serial_puts(" pages=");
    serial_puthex((uint64_t)shfile_live_pages());
    serial_puts("\n");
    zombie[i] = zombie[--nzombies];
    i--;
  }
}

/* A private copy of a table: same contents, its own frame, marked so it is
 * freed with the space and never written through by anyone else. `src_phys` 0
 * makes an empty one. */
static uint64_t *table_clone(uint64_t src_phys) {
  uint64_t *dst = (uint64_t *)alloc_frame();
  if (!dst) return 0;
  const uint64_t *src = src_phys ? (const uint64_t *)(hhdm_offset + src_phys) : 0;
  for (int i = 0; i < 512; i++) dst[i] = src ? src[i] : 0;
  return dst;
}

/* Follow (building as needed) the tables that hold `vaddr`, and return the page
 * table for it. Everything is addressed through the HHDM so a table reached
 * from a PTE and a table returned by alloc_frame() are the same memory.
 *
 * Two halves, and they are not alike:
 *
 *   slot 1 is our window at 0x8000000000 - one PML4 entry, created empty, owned
 *   by this address space from the first page.
 *
 *   slot 0 is the low half, where a program from an ordinary toolchain is
 *   linked (musl cannot be moved: its startup uses 32-bit absolute
 *   relocations). That slot is the kernel's own identity map, shared by every
 *   address space, so it is never written. The first low mapping copies the
 *   PDPT and works on the copy, and a page directory is copied the same way the
 *   first time a 1 GB region is touched. Entries the copy inherited keep
 *   pointing at the kernel's tables, so the identity map is still there for
 *   everything the program does not own - and the kernel reaches physical
 *   memory through the HHDM regardless, which is why a copy is safe.
 */
static uint64_t *walk_user(uint64_t cr3, uint64_t vaddr) {
  uint64_t *pml4 = (uint64_t *)(hhdm_offset + cr3);
  int i1 = (int)((vaddr >> 39) & 511);
  int i2 = (int)((vaddr >> 30) & 511);
  int i3 = (int)((vaddr >> 21) & 511);

  uint64_t *pdpt;
  if (i1 == 1) {
    if (pml4[1] & PTE_P) {
      pdpt = (uint64_t *)(hhdm_offset + (pml4[1] & ~0xFFFULL));
    } else {
      pdpt = table_clone(0);
      if (!pdpt) return 0;
      pml4[1] = to_phys(pdpt) | PTE_P | PTE_W | PTE_U | PTE_OWNED;
    }
  } else if (i1 == 0) {
    if (!(pml4[0] & PTE_P)) {
      pdpt = table_clone(0);
      if (!pdpt) return 0;
      pml4[0] = to_phys(pdpt) | PTE_P | PTE_W | PTE_U | PTE_OWNED;
    } else if (!(pml4[0] & PTE_OWNED)) {
      pdpt = table_clone(pml4[0] & ~0xFFFULL);
      if (!pdpt) return 0;
      pml4[0] = to_phys(pdpt) | (pml4[0] & 0xFFF) | PTE_OWNED;
    } else {
      pdpt = (uint64_t *)(hhdm_offset + (pml4[0] & ~0xFFFULL));
    }
  } else {
    return 0;                       /* nothing else is user addressable */
  }

  uint64_t *pd;
  if (pdpt[i2] & PTE_P) {
    if (!(pdpt[i2] & PTE_OWNED)) {
      pd = table_clone(pdpt[i2] & ~0xFFFULL);
      if (!pd) return 0;
      pdpt[i2] = to_phys(pd) | (pdpt[i2] & 0xFFF) | PTE_OWNED;
    } else {
      pd = (uint64_t *)(hhdm_offset + (pdpt[i2] & ~0xFFFULL));
    }
  } else {
    pd = table_clone(0);
    if (!pd) return 0;
    pdpt[i2] = to_phys(pd) | PTE_P | PTE_W | PTE_U | PTE_OWNED;
  }

  if (pd[i3] & PTE_P) {
    if (pd[i3] & 0x80ULL) {
      /* The kernel's identity map is built from 2 MB pages, so a low program
       * lands on one. This directory is already our private copy, so the big
       * page is simply replaced by a table of our own here: no other address
       * space and none of the kernel's tables see the change, and the kernel
       * reaches that physical range through the HHDM rather than through the
       * identity map. */
      uint64_t *pt = table_clone(0);
      if (!pt) return 0;
      pd[i3] = to_phys(pt) | PTE_P | PTE_W | PTE_U | PTE_OWNED;
      return pt;
    }
    if (!(pd[i3] & PTE_OWNED)) {
      uint64_t *pt = table_clone(pd[i3] & ~0xFFFULL);
      if (!pt) return 0;
      pd[i3] = to_phys(pt) | (pd[i3] & 0xFFF) | PTE_OWNED;
      return pt;
    }
    return (uint64_t *)(hhdm_offset + (pd[i3] & ~0xFFFULL));
  }
  uint64_t *pt = table_clone(0);
  if (!pt) return 0;
  pd[i3] = to_phys(pt) | PTE_P | PTE_W | PTE_U | PTE_OWNED;
  return pt;
}

/* Map one fresh, zeroed page at `vaddr`; optionally hand back its kernel
 * address so the caller can fill it. */
int ring3_map(uint64_t cr3, uint64_t vaddr, uint64_t flags, void **kva) {
  if (vaddr < RING3_USER_MIN || vaddr >= RING3_LIMIT) return -1;
  uint64_t *pt = walk_user(cr3, vaddr & ~0xFFFULL);
  if (!pt) return -1;
  int i4 = (int)((vaddr >> 12) & 511);
  if (pt[i4] & PTE_P) {
    /* Segments share pages: .rodata and .bss can sit in the same 4 KB as the
     * code. Reusing the frame is right, but the flags have to be merged or a
     * writable byte lands in a read-only page. */
    pt[i4] |= flags;
    if (kva) *kva = (void *)(hhdm_offset + (pt[i4] & ~0xFFFULL));
    return 0;
  }
  void *frame = alloc_frame();
  if (!frame) return -1;
  pt[i4] = to_phys(frame) | flags;
  if (kva) *kva = frame;
  return 0;
}

/* Copy `len` bytes from `src` (0 = leave the pages zeroed) at `vaddr`. */
int ring3_map_copy(uint64_t cr3, uint64_t vaddr, const uint8_t *src, uint64_t len,
                   uint64_t flags) {
  uint64_t done = 0;
  while (done < len) {
    uint64_t page = (vaddr + done) & ~0xFFFULL;
    uint64_t off = (vaddr + done) - page;
    void *kva = 0;
    if (ring3_map(cr3, page, flags, &kva) != 0) return -1;
    uint64_t n = 4096 - off;
    if (n > len - done) n = len - done;
    if (src) {
      uint8_t *dst = (uint8_t *)kva + off;
      for (uint64_t i = 0; i < n; i++) dst[i] = src[done + i];
    }
    done += n;
  }
  return 0;
}

/* Map an existing physical frame (not a fresh one) at `vaddr`. This is what
 * makes shared memory shared: two address spaces get PTEs for the same frame. */
int ring3_map_existing(uint64_t cr3, uint64_t vaddr, uint64_t phys, uint64_t flags) {
  if (vaddr < RING3_USER_MIN || vaddr >= RING3_LIMIT) return -1;
  uint64_t *pt = walk_user(cr3, vaddr & ~0xFFFULL);
  if (!pt) return -1;
  int i4 = (int)((vaddr >> 12) & 511);
  pt[i4] = (phys & ~0xFFFULL) | flags;
  return 0;
}

/* A one-slot text clipboard: the simplest thing that lets two processes hand
 * something to each other without agreeing on a segment first. */
static char clip_buf[4096];
static int  clip_len;

/* Named shared segments: a fixed set of slots, each backed by frames from the
 * kernel pool and mapped at RING3_SHM_BASE in every process that attaches. */
#define SHM_SLOTS 4

static struct {
  int      used, pages;
  void    *kva[RING3_SHM_MAX];
} shm[SHM_SLOTS];

static int shm_get(int slot, int pages) {
  if (slot < 0 || slot >= SHM_SLOTS) return -1;
  if (pages < 1) pages = 1;
  if (pages > RING3_SHM_MAX) pages = RING3_SHM_MAX;
  if (shm[slot].used) return shm[slot].pages >= pages ? 0 : -1;
  for (int i = 0; i < pages; i++) {
    shm[slot].kva[i] = alloc_frame();
    if (!shm[slot].kva[i]) return -1;
  }
  shm[slot].pages = pages;
  shm[slot].used = 1;
  return 0;
}

static uint64_t shm_attach(int slot) {
  if (slot < 0 || slot >= SHM_SLOTS || !shm[slot].used) return 0;
  task_t *t = task_at(task_current());
  if (!t || !t->cr3) return 0;
  for (int i = 0; i < shm[slot].pages; i++) {
    if (ring3_map_existing(t->cr3, RING3_SHM_BASE + (uint64_t)i * 4096,
                           to_phys(shm[slot].kva[i]),
                           PTE_P | PTE_W | PTE_U | PTE_SHARED) != 0) return 0;
  }
  return RING3_SHM_BASE;
}

/* Pixel surfaces: frames the kernel owns, mapped writable into one process,
 * which is then responsible for what they contain. Kept apart from the shm
 * slots because a surface is tied to a window - it dies with the window, and
 * the compositor asks for its pixels by window id.
 *
 * The frames are not contiguous, so a surface is not one buffer: it is a list
 * of pages and a width. Everything that reads pixels goes a row at a time. */
#define SURF_PAGES ((int)(RING3_SURF_AREA / 4096))

static struct {
  int       used, win, w, h, pages;
  void     *kva[SURF_PAGES];
  uint64_t  presented;          /* present() calls, for the log */
  int       has_damage;         /* dam is the union of what was presented */
  int       dam_x, dam_y, dam_w, dam_h;
} surf[RING3_SURF_SLOTS];

/* Local rather than <string.h>: the kernel header pulls in types this file does
 * not want, and a 4 KB zero is a loop. */
static void surf_zero(void *p, size_t n) {
  uint8_t *b = (uint8_t *)p;
  for (size_t i = 0; i < n; i++) b[i] = 0;
}

static int surf_slot_for_win(int win_id) {
  if (win_id < 0) return -1;
  for (int i = 0; i < RING3_SURF_SLOTS; i++)
    if (surf[i].used && surf[i].win == win_id) return i;
  return -1;
}

uint64_t surf_create(int win_id, int w, int h) {
  if (w < 1 || h < 1 || win_id < 0) return (uint64_t)-EINVAL;
  if (w > SURF_MAX_W) w = SURF_MAX_W;
  if (h > SURF_MAX_H) h = SURF_MAX_H;
  int pages = (int)(((size_t)w * (size_t)h * 4 + 4095) / 4096);
  if (pages > SURF_PAGES) return (uint64_t)-EINVAL;
  if (surf_slot_for_win(win_id) >= 0) return (uint64_t)-EEXIST;

  int slot = -1;
  for (int i = 0; i < RING3_SURF_SLOTS; i++) if (!surf[i].used) { slot = i; break; }
  if (slot < 0) return (uint64_t)-ENOSPC;

  task_t *t = task_at(task_current());
  if (!t || !t->cr3) return (uint64_t)-EINVAL;

  int got = 0;
  for (; got < pages; got++) {
    surf[slot].kva[got] = alloc_frame();
    if (!surf[slot].kva[got]) break;
    surf_zero(surf[slot].kva[got], 4096);     /* defined from the first frame */
  }
  if (got < pages) {                          /* give back what we took */
    for (int j = 0; j < got; j++) pmm_free_page(to_phys(surf[slot].kva[j]));
    return (uint64_t)-ENOMEM;
  }

  uint64_t base = RING3_SURF_BASE + (uint64_t)slot * RING3_SURF_AREA;
  for (int i = 0; i < pages; i++) {
    if (ring3_map_existing(t->cr3, base + (uint64_t)i * 4096,
                           to_phys(surf[slot].kva[i]),
                           PTE_P | PTE_W | PTE_U | PTE_SHARED) != 0) {
      for (int j = 0; j < pages; j++) pmm_free_page(to_phys(surf[slot].kva[j]));
      return (uint64_t)-ENOMEM;
    }
  }

  surf[slot].used = 1;
  surf[slot].win = win_id;
  surf[slot].w = w;
  surf[slot].h = h;
  surf[slot].pages = pages;
  surf[slot].has_damage = 1;                  /* the first frame is all of it */
  surf[slot].dam_x = 0; surf[slot].dam_y = 0;
  surf[slot].dam_w = w; surf[slot].dam_h = h;
  return base;
}

int surf_present(int win_id, int x, int y, int w, int h) {
  int slot = surf_slot_for_win(win_id);
  if (slot < 0) return -EINVAL;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > surf[slot].w) w = surf[slot].w - x;
  if (y + h > surf[slot].h) h = surf[slot].h - y;
  if (w <= 0 || h <= 0) return 0;             /* outside: nothing changed */
  surf[slot].presented++;

  /* union with what is already pending */
  if (!surf[slot].has_damage) {
    surf[slot].dam_x = x; surf[slot].dam_y = y;
    surf[slot].dam_w = w; surf[slot].dam_h = h;
    surf[slot].has_damage = 1;
    return 0;
  }
  int x0 = x < surf[slot].dam_x ? x : surf[slot].dam_x;
  int y0 = y < surf[slot].dam_y ? y : surf[slot].dam_y;
  int x1 = x + w, y1 = y + h;
  int dx1 = surf[slot].dam_x + surf[slot].dam_w;
  int dy1 = surf[slot].dam_y + surf[slot].dam_h;
  if (x1 > dx1) dx1 = x1;
  if (y1 > dy1) dy1 = y1;
  surf[slot].dam_x = x0; surf[slot].dam_y = y0;
  surf[slot].dam_w = dx1 - x0; surf[slot].dam_h = dy1 - y0;
  return 0;
}

int surf_size(int win_id, int *w, int *h) {
  int slot = surf_slot_for_win(win_id);
  if (slot < 0) return 0;
  if (w) *w = surf[slot].w;
  if (h) *h = surf[slot].h;
  return 1;
}

uint32_t *surf_row(int win_id, int y) {
  int slot = surf_slot_for_win(win_id);
  if (slot < 0 || y < 0 || y >= surf[slot].h) return 0;
  size_t off = (size_t)y * (size_t)surf[slot].w * 4;
  return (uint32_t *)((uint8_t *)surf[slot].kva[off >> 12] + (off & 4095));
}

/* Hand the compositor the rectangle that changed since the last frame, and
 * clear it. Returns 0 when the process has not presented anything new. */
int surf_take_damage(int win_id, int *x, int *y, int *w, int *h) {
  int slot = surf_slot_for_win(win_id);
  if (slot < 0 || !surf[slot].has_damage) return 0;
  if (x) *x = surf[slot].dam_x;
  if (y) *y = surf[slot].dam_y;
  if (w) *w = surf[slot].dam_w;
  if (h) *h = surf[slot].dam_h;
  surf[slot].has_damage = 0;
  return 1;
}

void surf_release_window(int win_id) {
  int slot = surf_slot_for_win(win_id);
  if (slot < 0) return;
  for (int i = 0; i < surf[slot].pages; i++)
    if (surf[slot].kva[i]) pmm_free_page(to_phys(surf[slot].kva[i]));
  surf_zero(&surf[slot], sizeof surf[slot]);
}

/* A stack in the same window, just below the limit. */
/* Copy into a user address space that is not the active one. The pages must
 * already be mapped - this is for laying out argv on a fresh stack, not for
 * demand paging, and a #PF inside the kernel is a panic rather than a fixup. */
int ring3_copy_to_mm(uint64_t cr3, uint64_t vaddr, const void *src, uint64_t n) {
  if (n == 0) return 0;
  if (vaddr < RING3_USER_MIN || vaddr + n > RING3_LIMIT) return -1;
  uint64_t save = ring3_read_cr3();
  int switched = (save != cr3);
  if (switched) ring3_write_cr3(cr3);
  const uint8_t *s = (const uint8_t *)src;
  uint8_t *d = (uint8_t *)(uintptr_t)vaddr;
  uint64_t done = 0;
  while (done < n) {
    uint64_t va = vaddr + done;
    /* walk_user hands back the page table, not one entry: the last-level index
     * still has to be applied here. Reading index 0 instead is a check that
     * looks present-or-not entirely at random. */
    uint64_t *pt = walk_user(cr3, va & ~0xFFFULL);
    if (!pt || !(pt[(va >> 12) & 511] & PTE_P)) break;
    uint64_t chunk = 0x1000 - (va & 0xFFF);
    if (chunk > n - done) chunk = n - done;
    for (uint64_t i = 0; i < chunk; i++) d[done + i] = s[done + i];
    done += chunk;
  }
  if (switched) ring3_write_cr3(save);
  return done == n ? 0 : -1;
}

int ring3_make_stack(uint64_t cr3) {
  const int pages = 8;                          /* 32 KB */
  uint64_t base = RING3_LIMIT - (uint64_t)pages * 4096;
  for (int i = 0; i < pages; i++) {
    if (ring3_map(cr3, base + (uint64_t)i * 4096, PTE_P | PTE_W | PTE_U, 0) != 0)
      return -1;
  }
  return 0;
}

/* ------------------------------------------------------- lazy vm regions */

/* Fill in one page of a region on its first touch. Returns 1 when the page
 * appeared (the faulting instruction can simply be retried), 0 when the address
 * is not part of any region and the fault is therefore real. */
static int vm_fault_in(uint64_t addr, int err) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return 0;
  uint64_t page = addr & ~0xFFFULL;
  for (int i = 0; i < TASK_AREAS; i++) {
    vm_area_t *a = &t->mm->areas[i];
    if (!a->used) continue;
    if (page < a->base || page >= a->base + a->len) continue;
    /* What the fault was asking for decides whether the range may give it. A
     * page inside a PROT_NONE area is not waiting to be paged in, it is refusing
     * to be touched - musl's allocator puts one in front of its heap so that an
     * overflow faults instead of quietly eating the page after it. */
    /* x86 has no write-only page, so PROT_WRITE implies read - which is why
     * mmap(PROT_WRITE) is readable under Linux. Only a store demands the write
     * bit; a read is satisfied by read or write. */
    int ok = (err & 2) ? (a->prot & 2) : ((err & 16) ? (a->prot & 4) : (a->prot & 3));
    if (!ok) return 0;
    /* A file region's page is filled from the blob, not left zero. The last page
     * of a mapping may start before EOF and end after it - that is how ld.so
     * maps a segment whose size is not a page multiple, and the tail is meant to
     * be zeros, which a fresh frame already is. */
    /* A shared region maps the file's frames themselves, marked so that nothing
     * frees them with the space: they belong to the file, they survive the
     * descriptor, and a fork carries the same frames into the child instead of
     * copying them. This is the only place a mapping and a descriptor end up
     * looking at one page of memory, so it is the only place that has to be
     * right. */
    if (a->shf) {
      int idx = (int)((page - a->base) >> 12);
      void *fr = shfile_page((shfile_t *)a->shf, idx, 1);
      if (!fr) return 0;                 /* past the file's end: a real fault */
      if (a->flags & PTE_SHARED) {
        /* Shared: the page the program touches is the page the file owns, so a
         * write through this mapping is a write to the file, visible to every
         * other mapping of it and to read(2). */
        if (ring3_map_existing(t->cr3, page, to_phys(fr), a->flags) != 0) return 0;
        return 1;
      }
      /* Private: the bytes come from the file, the frame does not. alloc_frame
       * hands back a zeroed page, so a page the file has never written needs no
       * copy - the zeroes are already right. */
      void *copy = alloc_frame();
      if (!copy) return 0;
      const uint8_t *s = fr;
      uint8_t *d = copy;
      for (int i = 0; i < 4096; i++) d[i] = s[i];
      if (ring3_map_existing(t->cr3, page, to_phys(copy), a->flags) != 0) return 0;
      return 1;
    }
    if (a->src) {
      uint64_t off = page - a->base;
      if (off < a->src_len) {
        uint64_t n = a->src_len - off;
        if (n > 4096) n = 4096;
        if (ring3_map_copy(t->cr3, page, a->src + a->src_off + off, n, a->flags) != 0) return 0;
        return 1;
      }
      /* Entirely past EOF: refused at mmap time, so reaching here means the area
       * and the blob disagree. Leave the fault real rather than invent zeros. */
      return 0;
    }
    void *kva = 0;
    if (ring3_map(t->cr3, page, a->flags, &kva) != 0) return 0;
    return 1;
  }
  return 0;
}

/* Find a descriptor slot, or say why there is none. A silent ENOMEM here becomes
 * a NULL malloc somewhere in a library that does not check, and the fault that
 * follows points at the write rather than at the reason the mapping had nowhere
 * to live - which is how the browser cost an afternoon. */
static int vm_area_slot(mm_t *mm, uint64_t want, const char *what) {
  for (int i = 0; i < TASK_AREAS; i++) if (!mm->areas[i].used) return i;
  static int warned;
  if (!warned) {
    warned = 1;
    serial_puts("[vm] out of area slots in task ");
    serial_puts(task_name(task_current()));
    serial_puts(": ");
    serial_puts(what);
    serial_puts(" wants ");
    serial_puthex(want);
    serial_puts(" bytes (TASK_AREAS=");
    serial_puthex(TASK_AREAS);
    serial_puts(")\n");
  }
  return -1;
}

static long sys_vm_alloc(uint64_t len, uint64_t prot) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  mm_t *mm = t->mm;
  if (len == 0) return -EINVAL;
  len = (len + 0xFFF) & ~0xFFFULL;
  if (len > (RING3_MMAP_TOP - RING3_MMAP_BASE)) return -ENOMEM;

  int slot = vm_area_slot(mm, len, "mmap");
  if (slot < 0) return -ENOMEM;

  if (!mm->mmap_hint) mm->mmap_hint = RING3_MMAP_BASE;
  uint64_t base = mm->mmap_hint;
  if (base + len > RING3_MMAP_TOP) return -ENOMEM;

  uint64_t flags = PTE_P | PTE_U;
  if (prot & 2) flags |= PTE_W;
  mm->areas[slot].base = base;
  mm->areas[slot].len = len;
  mm->areas[slot].flags = flags;
  mm->areas[slot].prot = (uint8_t)(prot & 7);
  mm->areas[slot].used = 1;
  /* A reused slot may have described a file region before; anonymous means no
   * source, and a stale pointer here would fill this heap with a library's
   * bytes. Clear it where the area is created, not where it is read. */
  mm->areas[slot].src = 0;
  mm->areas[slot].src_off = 0;
  mm->areas[slot].src_len = 0;
  mm->mmap_hint = base + len + 0x1000;     /* leave one guard page between regions */
  return (long)base;
}

/* Release the frames behind [base, base+len), one page at a time, with the same
 * policy the exit path uses: a frame another space still maps is released to the
 * bank, not freed to the allocator, and a PTE_SHARED frame belongs to the pool
 * and is not ours to free at all. */
static uint64_t vm_release_range(task_t *t, uint64_t base, uint64_t len, uint64_t *kept) {
  uint64_t freed = 0;
  *kept = 0;
  /* Walk per page: one page table covers 2 MB, so a 4 MB region spans two of
   * them. Indexing a single table across the whole range reads into whatever
   * physical page follows it and "frees" page tables that are still in use. */
  for (uint64_t off = 0; off < len; off += 4096) {
    uint64_t va = base + off;
    uint64_t *pt = walk_user(t->cr3, va);
    if (!pt) continue;
    int i4 = (int)((va >> 12) & 511);
    if (!(pt[i4] & PTE_P)) continue;
    if (!(pt[i4] & PTE_SHARED)) {
      uint64_t phys = pt[i4] & ~0xFFFULL;
      if (cow_release(phys)) pmm_free_page(phys);
      else (*kept)++;
    }
    pt[i4] = 0;
    freed++;
  }
  ring3_write_cr3(ring3_read_cr3());       /* flush the stale translations */
  return freed;
}

static int vm_overlaps(mm_t *mm, uint64_t base, uint64_t len) {
  for (int i = 0; i < TASK_AREAS; i++) {
    vm_area_t *a = &mm->areas[i];
    if (!a->used) continue;
    if (base < a->base + a->len && a->base < base + len) return 1;
  }
  return 0;
}

static long sys_vm_free(uint64_t addr, uint64_t len) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm || len == 0) return -EINVAL;
  uint64_t base = addr & ~0xFFFULL;
  len = (len + 0xFFF) & ~0xFFFULL;

  for (int i = 0; i < TASK_AREAS; i++) {
    vm_area_t *a = &t->mm->areas[i];
    if (!a->used) continue;
    if (base < a->base || base + len > a->base + a->len) continue;

    uint64_t kept = 0;
    uint64_t freed = vm_release_range(t, base, len, &kept);
    serial_puts("[vm] released ");
    serial_puthex(freed);
    serial_puts(" pages (");
    serial_puthex(kept);
    serial_puts(" kept: another space still maps them), ");
    serial_puthex(pmm_free_pages());
    serial_puts(" free of ");
    serial_puthex(pmm_total_pages());
    serial_puts("\n");
    if (base == a->base && len >= a->len) area_release(a);
    return 0;
  }
  return -EINVAL;                          /* no region covers that range */
}

/* ------------------------------------------------------ threads and futexes */

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* SYS_THREAD_CREATE(entry, arg, stack_bytes, flags): a second thread in the same
 * address space. The stack is a lazily-populated region growing down from the
 * top of the window, so a thread that never uses its stack costs nothing. */
static long sys_thread_create(uint64_t entry, uint64_t arg, uint64_t stack_bytes,
                              uint64_t flags) {
  (void)flags;
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  if (entry < RING3_USER_MIN || entry >= RING3_LIMIT) return -EFAULT;

  if (!stack_bytes) stack_bytes = 65536;
  stack_bytes = (stack_bytes + 0xFFF) & ~0xFFFULL;
  if (stack_bytes < 8192) stack_bytes = 8192;

  mm_t *mm = t->mm;
  if (mm->stack_hint < RING3_MMAP_BASE + stack_bytes + 0x1000) return -ENOMEM;
  uint64_t top = mm->stack_hint - 0x1000;         /* guard page above the stack */
  uint64_t base = top - stack_bytes;
  mm->stack_hint = base - 0x1000;                 /* and one between threads */

  int slot = vm_area_slot(mm, stack_bytes, "thread stack");
  if (slot < 0) return -ENOMEM;
  mm->areas[slot].base = base;
  mm->areas[slot].len = stack_bytes;
  mm->areas[slot].flags = PTE_P | PTE_W | PTE_U;
  mm->areas[slot].prot = 3;            /* a stack is read and written */
  mm->areas[slot].used = 1;
  mm->areas[slot].src = 0;             /* a stack is not a file */
  mm->areas[slot].src_off = 0;
  mm->areas[slot].src_len = 0;

  static int seq;
  char name[24];
  seq++;
  int n = 0;
  name[n++] = 't'; name[n++] = 'h'; name[n++] = 'r';
  if (seq >= 10) name[n++] = (char)('0' + (seq / 10) % 10);
  name[n++] = (char)('0' + seq % 10);
  name[n] = 0;

  int id = task_spawn_user_mm(name, entry, top - 16, mm, arg);
  if (id < 0) {
    mm->areas[slot].used = 0;
    mm->stack_hint = top + 0x1000;
    return -EAGAIN;
  }
  return id;
}

static int user_put(uint64_t uptr, const void *src, uint64_t n);

/* One address, one TLB entry: enough after rewriting a single page table entry
 * in the space that is currently running. */
static void flush_page(uint64_t va) {
  __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/* One line per split, carrying every number the claim depends on: which process,
 * which address, which frame it had, which frame it got, and the reference count
 * on either side of the split. Nothing here is inferred from the outcome - the
 * outcome (a child that exits 7 instead of dying) is checked separately by the
 * test, and the two together are the evidence that this ran and worked. */
static void cow_report_copy(task_t *me, uint64_t va, uint64_t old_phys,
                            uint64_t new_phys, int refs_before, int refs_after) {
  serial_puts("[cow] pid=");
  serial_puthex((uint64_t)me->id);
  serial_puts(" va=");
  serial_puthex(va);
  serial_puts(" old=");
  serial_puthex(old_phys);
  serial_puts(" new=");
  serial_puthex(new_phys);
  serial_puts(" refs=");
  serial_puthex((uint64_t)refs_before);
  serial_puts("->");
  serial_puthex((uint64_t)refs_after);
  serial_puts("\n");
}

/* Give the current address space a private writable copy of a page it shares.
 *
 * The last space to hold a frame has nothing to protect: it takes the write bit
 * instead of a copy, which is the case that makes a fork-then-exec cost nothing
 * on the pages it touched once. Returns 1 when the fault is now satisfied and the
 * instruction can be retried. */
static int cow_unshare_page(uint64_t va) {
  task_t *me = task_at(task_current());
  if (!me || !me->cr3) return 0;
  uint64_t *pt = walk_user(me->cr3, va);
  if (!pt) return 0;
  int idx = (int)((va >> 12) & 511);
  if (!(pt[idx] & PTE_P)) return 0;
  uint64_t phys = pt[idx] & ~0xFFFULL;
  if (pt[idx] & PTE_SHARED) return 0;        /* a shared segment is not ours to copy */

  int refs = cow_amount(phys);
  if (refs <= 1) {                            /* sole owner: just allow the store */
    pt[idx] |= PTE_W;
    flush_page(va);
    if (refs == 1) cow_release(phys);         /* drop the bookkeeping with it */
    me->cow_steals++;
    serial_puts("[cow] pid=");
    serial_puthex((uint64_t)me->id);
    serial_puts(" va=");
    serial_puthex(va);
    serial_puts(" frame=");
    serial_puthex(phys);
    serial_puts(" refs=");
    serial_puthex((uint64_t)refs);
    serial_puts("->");
    serial_puthex((uint64_t)(refs ? 1 : 0));
    serial_puts(" steal: write bit granted, no copy\n");
    return 1;
  }

  uint64_t fresh = pmm_alloc_page();
  if (!fresh) return 0;                       /* out of memory: the fault is real */
  const uint8_t *from = (const uint8_t *)(hhdm_offset + phys);
  uint8_t *to = (uint8_t *)(hhdm_offset + fresh);
  for (int i = 0; i < 4096; i++) to[i] = from[i];
  pt[idx] = fresh | (pt[idx] & 0xFFF) | PTE_W;
  flush_page(va);
  int last = cow_release(phys);               /* 1 = nobody else had it after all */
  me->cow_copies++;
  cow_report_copy(me, va, phys, fresh, refs, last ? 0 : refs - 1);
  if (last) pmm_free_page(phys);
  return 1;
}

/* The write half of fork, entered from the page fault handler. The store that
 * brought us here has not happened yet: this returns 1 and the instruction runs
 * again, now against a frame nobody else maps. That the retry *works* is checked
 * by the same address not faulting twice - which is what the repeat line below
 * exists to catch. */
/* The area a page belongs to, or NULL. Every fault asks this first: whether an
 * address may be filled, copied or refused is a property of its area, and the
 * page tables only ever describe what is there now. */
static vm_area_t *vm_area_of(mm_t *mm, uint64_t page) {
  for (int i = 0; i < TASK_AREAS; i++) {
    vm_area_t *a = &mm->areas[i];
    if (a->used && page >= a->base && page < a->base + a->len) return a;
  }
  return 0;
}

static int cow_fault(uint64_t va) {
  task_t *me = task_at(task_current());
  if (!me) return 0;
  /* A store into a range mapped without write permission is a violation, not a
   * copy: granting it would make the mapping's protection a suggestion. This is
   * the path that catches a write to PROT_READ and to PROT_NONE alike.
   *
   * Areas describe ranges that are filled lazily. Pages the kernel mapped
   * directly - the initial stack, an image's segments - have no area and carry
   * their permissions in the PTE from the moment they appear, so a missing area
   * is not a violation: refusing here would kill a task for a bookkeeping gap. */
  vm_area_t *area = vm_area_of(me->mm, va & ~0xFFFULL);
  if (area && !(area->prot & 2)) return 0;
  /* A genuine repeat: the same store faulting twice with no fork in between. A
   * fault at an address this task already unshared *after* another fork is the
   * mechanism working, not failing - the new fork re-protected the page. */
  if (me->cow_faults && me->last_cow_va == va && me->last_cow_gen == me->cow_gen) {
    serial_puts("[cow] REPEAT-FAULT pid=");
    serial_puthex((uint64_t)me->id);
    serial_puts(" va=");
    serial_puthex(va);
    serial_puts(": the copy did not take\n");
  }
  me->last_cow_va = va;
  me->last_cow_gen = me->cow_gen;
  int ok = cow_unshare_page(va);
  if (ok) me->cow_faults++;
  return ok;
}

/* ------------------------------------------------- a copied address space */

/* fork() is exactly this and nothing else: the same program at the same
 * instruction, on its own memory. The memory starts shared and becomes private on
 * the first store - a writable page is mapped read-only in both spaces and its
 * frame counted, so the page only costs a copy if someone writes it. A page that
 * is already read-only (code, rodata) is handed over as it stands, which is what
 * makes forking a large program cheap without any further cleverness.
 *
 * Pages that are present but not user-accessible are the kernel's own mappings
 * inside this address space, and each space makes its own; copying them would
 * duplicate the kernel's identity map per process. */
static mm_t *mm_clone(mm_t *src) {
  if (!src) return 0;
  mm_t *dst = ring3_new_mm();
  if (!dst) return 0;

  int shared = 0, copied = 0, writable = 0;
  uint64_t *spml4 = (uint64_t *)(hhdm_offset + src->cr3);
  for (int s = 0; s < 2; s++) {           /* slot 0: a low program, slot 1: the window */
    if (!(spml4[s] & PTE_P)) continue;
    uint64_t *spdpt = (uint64_t *)(hhdm_offset + (spml4[s] & ~0xFFFULL));
    for (int i2 = 0; i2 < 512; i2++) {
      if (!(spdpt[i2] & PTE_P) || (spdpt[i2] & (1ULL << 7))) continue;
      uint64_t *spd = (uint64_t *)(hhdm_offset + (spdpt[i2] & ~0xFFFULL));
      for (int i3 = 0; i3 < 512; i3++) {
        if (!(spd[i3] & PTE_P)) continue;
        uint64_t base = ((uint64_t)s << 39) | ((uint64_t)i2 << 30) | ((uint64_t)i3 << 21);
        if (spd[i3] & (1ULL << 7)) {                       /* a 2 MB page */
          if (!(spd[i3] & PTE_U)) continue;
          uint64_t flags = RING3_P | RING3_U | (spd[i3] & PTE_W);
          if (ring3_map_copy(dst->cr3, base,
                             (const uint8_t *)(hhdm_offset + (spd[i3] & ~0xFFFULL)),
                             1ULL << 21, flags) != 0) goto fail;
          continue;
        }
        uint64_t *spt = (uint64_t *)(hhdm_offset + (spd[i3] & ~0xFFFULL));
        for (int i4 = 0; i4 < 512; i4++) {
          if (!(spt[i4] & PTE_P) || !(spt[i4] & PTE_U)) continue;
          uint64_t va = base | ((uint64_t)i4 << 12);
          uint64_t phys = spt[i4] & ~0xFFFULL;
          uint64_t flags = RING3_P | RING3_U | (spt[i4] & PTE_W);
          if (spt[i4] & PTE_SHARED) {
            /* A shared segment stays shared and stays writable: that is the whole
             * point of the window, and copying it would silently break two
             * processes that are talking to each other through it. */
            if (ring3_map_existing(dst->cr3, va, phys, flags | PTE_SHARED) != 0) goto fail;
            continue;
          }
          if (!cow_share(phys)) {          /* table full: copy rather than lie */
            if (ring3_map_copy(dst->cr3, va,
                               (const uint8_t *)(hhdm_offset + phys),
                               4096, flags) != 0) goto fail;
            copied++;
            continue;
          }
          /* Shared, and read-only on both sides until someone stores. A page the
           * parent cannot write is already safe to hand over as it is. */
          if (ring3_map_existing(dst->cr3, va, phys, flags & ~PTE_W) != 0) goto fail;
          if (spt[i4] & PTE_W) {
            spt[i4] &= ~PTE_W;
            flush_page(va);                /* the parent is the running task */
            writable++;
          }
          shared++;
        }
      }
    }
  }

  serial_puts("[fork] pages shared=");
  serial_puthex((uint64_t)shared);
  serial_puts(" made-read-only=");
  serial_puthex((uint64_t)writable);
  serial_puts(" copied=");
  serial_puthex((uint64_t)copied);
  serial_puts(" free=");
  serial_puthex(pmm_free_pages());
  serial_puts("\n");

  /* The lazy regions travel with the pages: a child that faults where its parent
   * had already allocated must land on the same area, or it will grow into
   * addresses the parent owns. */
  for (int i = 0; i < TASK_AREAS; i++) {
    dst->areas[i] = src->areas[i];
    /* The child's areas are new claims on the same files, and the frames travel
     * as PTE_SHARED rather than being copied: a child that writes into a shared
     * region is writing into its parent's memory, which is what MAP_SHARED is
     * for and what a forked worker expects. */
    if (dst->areas[i].shf) shfile_ref((shfile_t *)dst->areas[i].shf);
  }
  for (int i = 0; i < 32; i++) dst->sig_act[i] = src->sig_act[i];
  dst->mmap_hint = src->mmap_hint;
  dst->stack_hint = src->stack_hint;
  dst->brk_base = src->brk_base;
  dst->brk_cur = src->brk_cur;
  dst->brk_end = src->brk_end;

  fdtable_t *fds = fdtable_clone(src->fds);
  if (!fds) goto fail;
  fdtable_free(dst->fds);            /* the empty table ring3_new_mm made */
  dst->fds = fds;
  return dst;

fail:
  mm_free_tables(dst);
  kfree(dst);
  return 0;
}

/* ------------------------------------------------------------- wait4 (61) */

#define WNOHANG_L 1
#define ECHILD_L  10   /* no child exists: the answer a shell expects */

/* There are no signals, so a child's death cannot arrive as one: the parent
 * blocks and the kernel wakes it, which task_kill_current does by parent id.
 * Only children this task spawned are visible, and a pid that matches nothing is
 * -ECHILD - that refusal is what tells a shell there is nothing left to reap,
 * and it is also what keeps a mistaken wait from blocking forever. The status
 * word is the one shape wait4 has ever had: exit code in bits 8-15. */
static long linux_wait4(int pid, uint64_t status_u, int options, uint64_t rusage_u) {
  int me = task_current();
  for (;;) {
    int found = 0, mine = 0;
    for (int i = 1; i < task_count(); i++) {
      task_t *c = task_at(i);
      if (!c || c->parent != me) continue;
      mine++;
      if (pid > 0 && c->id != pid) continue;
      if (c->state == TASK_DEAD) { found = i; break; }
    }
    if (found) {
      task_t *c = task_at(found);
      int status = (int)((c->work & 0xFF) << 8);
      if (status_u && user_put(status_u, &status, sizeof status) != 0) return -EFAULT;
      if (rusage_u) {
        int64_t zero[18];
        for (int i = 0; i < 18; i++) zero[i] = 0;
        if (user_put(rusage_u, zero, sizeof zero) != 0) return -EFAULT;
      }
      int id = c->id;
      c->parent = 0;                    /* reaped once, never reported twice */
      return id;
    }
    if (!mine) return -ECHILD_L;
    if (options & WNOHANG_L) return 0;
    task_block_current();
  }
}

/* -------------------------------------------------------------- fork (57) */

/* musl's fork() does not go through clone on x86-64 - it issues the fork
 * syscall directly - so both numbers have to mean the same thing here, and they
 * do: one function, two doors. The child resumes at the instruction after the
 * syscall holding the same stack pointer, which is a *copy* of the caller's
 * stack because the whole address space was copied. Its rax is 0; the parent's
 * is the child's id. */
static long sys_fork(void) {
  {
    task_t *self = task_at(task_current());
    if (self) self->cow_gen++;    /* this fork will re-protect pages it already split */
  }
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  const uint64_t *cf = (const uint64_t *)(uintptr_t)r3_frame_base;
  mm_t *child_mm = mm_clone(t->mm);
  if (!child_mm) return -ENOMEM;
  /* cf[17] is the saved rsp. The frame is laid out in ctx_t order - r15 first,
   * rip at 14, cs 15, rflags 16, rsp 17 - and not in interrupt push order, which
   * is the mistake the first version of this made: it handed the child r11 as a
   * stack pointer and the child faulted writing at 0x23e. */
  int id = task_spawn_clone("child", child_mm, cf, cf[17], t->fs_base, 0);
  if (id < 0) {
    mm_free_tables(child_mm);
    kfree(child_mm);
    return -EAGAIN;
  }
  return id;
}

/* ---------------------------------------------------- clone (Linux 56) */

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* clone(flags, stack, ptid, ctid) - the thread half of it. A libc never has the
 * function and its argument in registers the kernel can see: its own wrapper
 * passes them to itself, and what the kernel has to deliver is the caller's
 * register state, the caller's next instruction, and a different stack. So the
 * new thread starts where the `syscall` returned to, with rax zero, and the
 * wrapper's first branch takes it down the child path.
 *
 * Without CLONE_VM this is fork: the same registers, the same stack pointer,
 * the same instruction - in a copied address space. Which is why the answer to
 * "did the caller want a thread or a process" is the flag and not the syscall
 * number: musl's fork() *is* clone(SIGCHLD, 0). */
static long sys_clone(uint64_t flags, uint64_t child_stack, uint64_t ptid,
                      uint64_t ctid) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;

  if (!(flags & CLONE_VM)) return sys_fork();     /* the process half of clone */

  if (!child_stack) return -EINVAL;

  static int seq;
  char name[24];
  int n = 0;
  name[n++] = 't'; name[n++] = 'h'; name[n++] = 'r';
  seq++;
  if (seq >= 10) name[n++] = (char)('0' + (seq / 10) % 10);
  name[n++] = (char)('0' + seq % 10);
  name[n] = 0;

  uint64_t tls = (flags & CLONE_SETTLS) ? r3_arg4 : t->fs_base;
  int id = task_spawn_clone(name, t->mm,
                            (const uint64_t *)(uintptr_t)r3_frame_base,
                            child_stack, tls,
                            (flags & CLONE_CHILD_CLEARTID) ? ctid : 0);
  if (id < 0) return -EAGAIN;

  uint32_t tid = (uint32_t)id;
  if ((flags & CLONE_PARENT_SETTID) && ptid) user_put(ptid, &tid, 4);
  if ((flags & CLONE_CHILD_SETTID) && ctid) user_put(ctid, &tid, 4);
  return id;
}

/* Wake up to `want` threads blocked on `addr` in `mm`. Also what a dying thread
 * does to its CLONE_CHILD_CLEARTID word, which is how pthread_join is woken. */
static int futex_wake_addr(mm_t *mm, uint64_t addr, uint64_t want) {
  int woke = 0;
  for (int i = 1; i < task_count(); i++) {
    task_t *o = task_at(i);
    if (!o || o->state != TASK_BLOCKED) continue;
    if (o->wait_addr != addr || o->wait_mm != mm) continue;
    o->wait_addr = 0;
    o->wait_mm = 0;
    task_wake(i);
    woke++;
    if ((uint64_t)woke >= want) break;
  }
  return woke;
}

/* SYS_FUTEX(addr, op, val, timeout_ticks). A wait that would sleep forever gets
 * a deadline when timeout_ticks is non-zero: the timer wakes it, so a wake that
 * arrives before the sleeper is actually blocked costs a few ticks rather than
 * wedging the thread. */
static long sys_futex(uint64_t addr, uint64_t op, uint64_t val, uint64_t timeout_ticks) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  if (addr < RING3_USER_MIN || addr + 8 > RING3_LIMIT) return -EFAULT;

  if ((op & 0x7F) == FUTEX_WAIT) {
    volatile long *word = (volatile long *)(uintptr_t)addr;
    if ((uint64_t)*word != val) return -EAGAIN;

    /* Publish the wait, then re-read before actually blocking. On a
     * uniprocessor these two steps cannot be interleaved - the CPU is inside a
     * non-preemptible syscall, so no other thread runs between them - which is
     * what makes check-then-block atomic here. The re-read is what turns that
     * from a scheduler argument into a local one, and it is the thing that
     * stays correct if this kernel ever grows preemption or a second CPU.
     * Returning -EAGAIN (not blocking) is the Linux contract for "value moved
     * on while you were asking". */
    t->wait_addr = addr;
    t->wait_mm = t->mm;
    t->timed_out = 0;
    if ((uint64_t)*word != val) {
      t->wait_addr = 0;
      t->wait_mm = 0;
      return -EAGAIN;
    }
    if (timeout_ticks) {
      t->wake_at = task_total_ticks() + timeout_ticks;
      t->state = TASK_BLOCKED;
      task_yield();
      t->wake_at = 0;
    } else {
      task_block_current();
    }
    int timed_out = t->timed_out;
    t->timed_out = 0;
    t->wait_addr = 0;
    t->wait_mm = 0;
    return timed_out ? -ETIMEDOUT : 0;      /* timeout is not "value changed" */
  }

  if ((op & 0x7F) == FUTEX_WAKE) {
    return futex_wake_addr(t->mm, addr, val ? val : 1);
  }
  return -ENOSYS;
}

/* -------------------------------------------------------------- syscalls */

/* Copy out of a user pointer after checking it really is inside that task's
 * user window. Anything else is rejected rather than dereferenced, which is
 * the point of the exercise. */
static int user_copy(char *dst, uint64_t uptr, uint64_t n) {
  if (n == 0) return 0;
  if (uptr < RING3_USER_MIN || uptr + n > RING3_LIMIT) return -1;
  const char *src = (const char *)(uintptr_t)uptr;
  for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
  return 0;
}

/* Copy back to user space, same window check as the read direction. */
static int user_put(uint64_t uptr, const void *src, uint64_t n) {
  if (n == 0) return 0;
  if (uptr < RING3_USER_MIN || uptr + n > RING3_LIMIT) return -1;
  uint8_t *dst = (uint8_t *)(uintptr_t)uptr;
  const uint8_t *s = (const uint8_t *)src;
  for (uint64_t i = 0; i < n; i++) dst[i] = s[i];
  return 0;
}

/* Is this page actually mapped right now? Demand-paged regions are not until
 * something touches them, and the fault handler only rescues faults taken in
 * ring 3 - so a kernel-side load or store to an untouched page is a panic, not
 * a -EFAULT. Anything the kernel is about to write into goes through here. */
static int user_page_present(uint64_t page) {
  task_t *t = task_at(task_current());
  if (!t || !t->cr3) return 0;
  uint64_t *pml4 = (uint64_t *)(hhdm_offset + t->cr3);
  /* Which half of the address space this page lives in is decided by the page
   * itself, not by a constant: a program linked low at 0x400000 is in slot 0 and
   * a program in the high window is in slot 1. Reading slot 1 for both reported
   * every byte of a low program as unmapped, which turned a perfectly good path
   * string into -EFAULT the first time a low program called open(). */
  int i1 = (int)((page >> 39) & 511);
  if (!(pml4[i1] & PTE_P)) return 0;
  uint64_t *pdpt = (uint64_t *)(hhdm_offset + (pml4[i1] & ~0xFFFULL));
  int i2 = (int)((page >> 30) & 511);
  if (!(pdpt[i2] & PTE_P)) return 0;
  uint64_t *pd = (uint64_t *)(hhdm_offset + (pdpt[i2] & ~0xFFFULL));
  int i3 = (int)((page >> 21) & 511);
  if (!(pd[i3] & PTE_P)) return 0;
  if (pd[i3] & 0x80ULL) return 1;
  uint64_t *pt = (uint64_t *)(hhdm_offset + (pd[i3] & ~0xFFFULL));
  int i4 = (int)((page >> 12) & 511);
  return (pt[i4] & PTE_P) ? 1 : 0;
}

/* Copy in from a user pointer the way a syscall has to: the window check, and a
 * demand-paged page faulted in first. The kernel-side load cannot take a fault,
 * and a buffer handed to sendto() has no reason to have been written yet - the
 * program may be sending an uninitialised struct, which is legal. */
static int user_get(uint64_t uptr, void *dst, uint64_t n) {
  if (n == 0) return 0;
  if (uptr < RING3_USER_MIN || uptr + n > RING3_LIMIT) return -1;
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)(uintptr_t)uptr;
  for (uint64_t i = 0; i < n; i++) {
    uint64_t at = uptr + i;
    if (!(at & 0xFFF)) {
      if (!user_page_present(at) && !vm_fault_in(at, 0)) return -1;
    }
    d[i] = s[i];
  }
  return 0;
}


static int user_prep(uint64_t uptr, uint64_t n) {
  if (n == 0) return 0;
  if (uptr < RING3_USER_MIN || uptr + n > RING3_LIMIT) return -1;
  for (uint64_t page = uptr & ~0xFFFULL; page < uptr + n; page += 0x1000)
    if (!user_page_present(page) && !vm_fault_in(page, 0)) return -1;
  return 0;
}

/* Copy a NUL-terminated string out of user memory without insisting the whole
 * maximum length be mapped. A path built on the stack sits a few bytes below the
 * top of the window, so requiring RFILE_NAME bytes of headroom rejects it with
 * -EFAULT and the caller sees a perfectly good file reported as a bad address. */
static long user_string(char *dst, uint64_t src, int max) {
  for (int i = 0; i < max; i++) {
    uint64_t a = src + (uint64_t)i;
    if (a >= RING3_LIMIT) break;
    if (i == 0 || (a & 0xFFF) == 0) {
      uint64_t page = a & ~0xFFFULL;
      if (!user_page_present(page) && !vm_fault_in(page, 0)) return -1;
    }
    if (user_copy(dst + i, a, 1) != 0) return -1;
    if (dst[i] == 0) return i;
  }
  return -1;                    /* no terminator inside the window */
}

static fdtable_t *current_fds(void) {
  task_t *t = task_at(task_current());
  return (t && t->mm) ? t->mm->fds : 0;
}

static long sys_write(uint64_t fdn, uint64_t buf, uint64_t len) {
  fdtable_t *fds = current_fds();
  if (!fds) return -EBADF;
  if (len > 4096) len = 4096;
  if (buf >= RING3_LIMIT) return -EFAULT;
  if (buf + len > RING3_LIMIT) len = RING3_LIMIT - buf;   /* ends at the edge */
  char tmp[4097];
  if (user_copy(tmp, buf, len) != 0) {
    serial_puts("[ring3] write: bad user pointer rejected\n");
    return -EFAULT;
  }
  tmp[len] = 0;
  return fd_write(fds, (int)fdn, tmp, len);
}

static long sys_read(uint64_t fd, uint64_t buf, uint64_t len);
static long sys_write(uint64_t fd, uint64_t buf, uint64_t len);

/* A socket is just another descriptor: 0 is what the caller gets back, and the
 * fact that it is a TCP stream rather than a file shows up nowhere else. */
/* Both descriptor numbers go out as a pair, and only after both exist: a caller
 * that got one number and a failure would leak it, since nothing else knows to
 * close it. The two ints are written once, together, at the end. */
static long sys_pipe(uint64_t uptr) {
  fdtable_t *t = current_fds();
  if (!t) return -EBADF;
  int fds[2] = { -1, -1 };
  int r = fd_pipe(t, fds);
  if (r < 0) return r;
  if (user_put(uptr, fds, sizeof fds) != 0) {
    fd_close(t, fds[0]);
    fd_close(t, fds[1]);
    return -EFAULT;
  }
  return 0;
}

static long sys_socket(uint64_t domain, uint64_t type) {
  if (domain != 2) return -EAFNOSUPPORT;      /* AF_INET only, for now */
  if (type != 1) return -EPROTOTYPE;          /* SOCK_STREAM only */
  fdtable_t *fds = current_fds();
  if (!fds) return -EMFILE;
  return fd_socket(fds);
}

static long sys_connect(uint64_t fd, uint64_t ip, uint64_t port) {
  fdtable_t *fds = current_fds();
  if (!fds) return -EBADF;
  if (port == 0 || port > 65535) return -EINVAL;
  return fd_connect(fds, (int)fd, (uint32_t)ip, (uint16_t)port);
}

/* send is write and recv is read: one path, two names. The distinction is kept
 * only because a program written against BSD sockets will call both. */
static long sys_recv(uint64_t fd, uint64_t buf, uint64_t len) {
  return sys_read(fd, buf, len);
}

static long sys_send(uint64_t fd, uint64_t buf, uint64_t len) {
  return sys_write(fd, buf, len);
}

/* Resolve a hostname through the DHCP-provided resolver, writing 4 bytes of IPv4
 * back into the caller's buffer. */
static long sys_resolve(uint64_t host_u, uint64_t out_u) {
  char host[64];
  if (user_string(host, host_u, sizeof host) < 0) return -EFAULT;
  if (user_prep(out_u, 4) != 0) return -EFAULT;
  uint8_t ip[4];
  const net_cfg_t *cfg = net_get();
  if (!net_dns_resolve(cfg, host, ip)) return -ENOENT;
  return user_put(out_u, ip, 4) == 0 ? 0 : -EFAULT;
}

/* poll(2), C layout: 8 bytes per entry, {int fd; uint16 events; uint16 revents}.
 *
 * One task can only park on one object today (wait_obj is a single pointer), so
 * this cannot register interest in N sockets at once. It paces instead: compute
 * every descriptor's readiness, and if nothing is ready, sleep a tick and look
 * again. Correct, no lost wakeups, at most one tick of added latency - and it is
 * honest about being a check loop rather than a multi-object wait. A real wait
 * for many objects needs a registration that names a set, which is a scheduler
 * change, not a socket one. */
typedef struct { int32_t fd; uint16_t events; uint16_t revents; } kpollfd_t;
_Static_assert(__builtin_offsetof(kpollfd_t, revents) == 6, "pollfd layout");

static long sys_poll(uint64_t fds_u, uint64_t nfds, uint64_t timeout_ms) {
  fdtable_t *t = current_fds();
  if (!t) return -EBADF;
  if (nfds == 0 || nfds > 16) return -EINVAL;
  if (user_prep(fds_u, nfds * 8) != 0) return -EFAULT;

  kpollfd_t set[16];
  for (uint64_t i = 0; i < nfds; i++)
    if (user_copy(&set[i], fds_u + i * 8, 8) != 0) return -EFAULT;

  uint64_t end = task_total_ticks() + (timeout_ms + 9) / 10;
  for (;;) {
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
      /* ERR and HUP come back whether or not they were asked for: they are not
       * optional edge conditions, they are the reasons a wait ends. */
      unsigned r = fd_poll(t, set[i].fd) &
                   (unsigned)(set[i].events | OMI_POLLERR | OMI_POLLHUP);
      if (r) ready++;
      set[i].revents = (uint16_t)r;
    }
    if (ready || task_total_ticks() >= end) {
      /* +6, not +4: the layout is {int fd; uint16 events; uint16 revents}, and
       * writing at +4 lands on events - which reads back as "ready, no bits". */
      for (uint64_t i = 0; i < nfds; i++)
        if (user_put(fds_u + i * 8 + 6, &set[i].revents, 2) != 0) return -EFAULT;
      return ready;
    }
    /* Sleeping here is what keeps poll from burning the cpu: the tick that wakes
     * us also lets whatever produces the data run. */
    task_sleep_ticks(1);
  }
}

/* A sleep, which this system did not have: nanosleep returned -ENOSYS, so every
 * sleep() and usleep() in a program returned at once and the program went on to
 * spin. It was found by a test that expected a child to be late and found it
 * early - the failing check was measuring a sleep that never happened.
 *
 * The wake-up is a deadline in ticks, and the loop re-sleeps if something else
 * woke the task first. There are no signals here, so there is no interrupted case
 * to report: -EINTR would name an event that cannot occur.
 */
static long sleep_ticks_for(uint64_t nsec) {
  uint64_t ticks = nsec / (1000000000ull / TASK_TICK_HZ);
  if (!ticks) ticks = 1;   /* shorter than a tick still yields, or 1ns in a loop is a spin */
  uint64_t deadline = task_total_ticks() + ticks;
  while (task_total_ticks() < deadline) {
    task_sleep_ticks(deadline - task_total_ticks());
  }
  return 0;
}

static long linux_nanosleep(uint64_t req_u, uint64_t rem_u) {
  if (user_prep(req_u, 16) != 0) return -EFAULT;
  int64_t tv[2];
  if (user_copy((char *)tv, req_u, sizeof tv) != 0) return -EFAULT;
  if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000000) return -EINVAL;
  uint64_t nsec = (uint64_t)tv[0] * 1000000000ull + (uint64_t)tv[1];
  long rc = sleep_ticks_for(nsec);
  if (rc == 0 && rem_u) {
    /* Nothing was left over, because the loop above runs to the deadline. */
    int64_t zero[2] = { 0, 0 };
    if (user_prep(rem_u, 16) == 0) user_put(rem_u, zero, sizeof zero);
  }
  return rc;
}

/* clock_nanosleep(230): the same wait, with the two clocks a program actually
 * passes and one difference in the arguments - an absolute deadline rather than a
 * duration. Other clock ids are refused rather than treated as 0, because a
 * program asking for a clock this kernel does not keep deserves to hear no. */
static long linux_clock_nanosleep(uint64_t clk, uint64_t flags, uint64_t req_u, uint64_t rem_u) {
  if (clk != 0 && clk != 1) return -EINVAL;        /* CLOCK_REALTIME, CLOCK_MONOTONIC */
  if (flags & ~1ull) return -EINVAL;               /* TIMER_ABSTIME is the only flag */
  if (!(flags & 1)) return linux_nanosleep(req_u, rem_u);
  if (user_prep(req_u, 16) != 0) return -EFAULT;
  int64_t tv[2];
  if (user_copy((char *)tv, req_u, sizeof tv) != 0) return -EFAULT;
  if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000000) return -EINVAL;
  /* The same conversion linux_clock_gettime does, done locally: that function
   * writes to a *user* pointer, and handing it a kernel address would be a way to
   * make the kernel write where the program said rather than where it meant. */
  uint64_t ticks = task_total_ticks();
  int64_t want = tv[0] * 1000000000ll + tv[1];
  int64_t have = (int64_t)(ticks / TASK_TICK_HZ) * 1000000000ll +
                 (int64_t)((ticks % TASK_TICK_HZ) * (1000000000 / TASK_TICK_HZ));
  if (want <= have) return 0;
  (void)rem_u;   /* an absolute sleep has no remainder to report */
  return sleep_ticks_for((uint64_t)(want - have));
}

/* ================================================= sockets, Linux ABI (10)
 *
 * Until now a socket existed only behind the native numbers, which take an
 * address as a bare u32 and a port as a bare u16 - fine for a program written
 * against this kernel, useless to a libc. musl's socket() is syscall 41 and its
 * send() is sendto(44) with no address, so every real library saw -ENOSYS and
 * there was no way to put a TLS stack in user mode at all.
 *
 * So these calls translate, and the translation is the interesting part: the
 * address a program passes is bytes in network order, and it must be read as
 * bytes. Loading sin_addr as a u32 on a little-endian machine reverses it, and
 * a connect to 1.1.1.1 that lands on 1.1.1.1 is a coincidence of four equal
 * octets - with 93.184.216.34 it would be 34.216.184.93, which fails slowly and
 * looks like a network problem.
 *
 * What is refused is refused on purpose. There is no listener in the transport,
 * so bind and listen are -ENOSYS rather than a socket that accepts nothing;
 * there is no UDP, so SOCK_DGRAM is -EAFNOSUPPORT; there is no half-close, so
 * SHUT_RD is -ENOSYS. Each of those is a feature this kernel does not have, and
 * a program told "yes" would wait for something that never comes.
 */
#define SOCKADDR_IN_LEN 16

/* Read a sockaddr_in the way the ABI lays it out: family as a host u16, port as
 * two network-order bytes, address as four bytes in the order they go on the
 * wire. */
static int sockaddr_in_read(uint64_t uptr, uint32_t *ip, uint16_t *port) {
  uint8_t b[8];
  if (!uptr) return -EFAULT;
  if (user_get(uptr, b, sizeof b) != 0) return -EFAULT;
  uint16_t family = (uint16_t)(b[0] | (b[1] << 8));
  if (family != 2) return -EAFNOSUPPORT;          /* AF_INET only */
  if (port) *port = (uint16_t)(((uint16_t)b[2] << 8) | b[3]);
  if (ip) *ip = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
                ((uint32_t)b[6] << 8) | b[7];
  return 0;
}

static long linux_socket(uint64_t domain, uint64_t type, uint64_t proto) {
  fdtable_t *fds = current_fds();
  if (!fds) return -EMFILE;
  if (proto != 0) return -EPROTONOSUPPORT;
  if (domain != 2) return -EAFNOSUPPORT;          /* no AF_INET6, no AF_UNIX */
  /* SOCK_DGRAM is a real protocol this transport does not carry: saying
   * EAFNOSUPPORT would be a claim about the family, and it is the type that is
   * missing. */
  if ((type & 0xF) == 2) return -EPROTONOSUPPORT;
  if ((type & 0xF) != 1) return -EPROTONOSUPPORT;
  /* SOCK_NONBLOCK and SOCK_CLOEXEC: neither exists here. Non-blocking is the
   * one that matters - a program that asked for it and got a blocking socket
   * hangs inside read() with no timeout, so the request is refused rather than
   * quietly ignored. */
  if (type & ~0xFull) return -EINVAL;
  return fd_socket(fds);
}

static long linux_connect(uint64_t fd, uint64_t addr, uint64_t len) {
  if (len < SOCKADDR_IN_LEN) return -EINVAL;
  uint32_t ip; uint16_t port;
  int r = sockaddr_in_read(addr, &ip, &port);
  if (r != 0) return r;
  if (!port) return -EINVAL;
  fdtable_t *fds = current_fds();
  if (!fds) return -EBADF;
  return fd_connect(fds, (int)fd, ip, port);
}

/* Flags a stream send can honour: none, and MSG_NOSIGNAL because nothing here
 * delivers SIGPIPE in the first place. DONTWAIT would need a non-blocking
 * socket. */
#define MSG_NOSIGNAL_FLAG 0x4000

static long linux_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                         uint64_t addr, uint64_t alen) {
  (void)addr; (void)alen;                  /* a connected stream ignores these */
  if (flags & ~(uint64_t)MSG_NOSIGNAL_FLAG) return -EINVAL;
  return sys_write(fd, buf, len);
}

static long linux_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                           uint64_t addr, uint64_t alen) {
  if (flags & ~(uint64_t)MSG_NOSIGNAL_FLAG) return -EINVAL;
  long n = sys_read(fd, buf, len);
  /* The peer's address is not tracked by the transport, so the length comes back
   * zero: a connected socket is allowed to return no address, and inventing one
   * would be worse than saying nothing. */
  if (n >= 0 && addr && alen) {
    uint32_t zero = 0;
    if (user_put(alen, &zero, sizeof zero) != 0) return -EFAULT;
  }
  return n;
}

/* sendmsg/recvmsg: the same calls with the bytes scattered across iovecs. A
 * library that writes a header and a body separately should not have to join
 * them itself, so they are joined here - into one kernel buffer, because the
 * transport takes a contiguous range. */
#define IOV_LEN 16
/* One message is one page. The rest of the kernel moves bytes through a page-
 * sized bounce buffer (read(2) and write(2) both cap at 4096), and a sendmsg
 * that claimed to take more would have to loop anyway - so it loops here and
 * says so, rather than growing a second buffer discipline nobody else uses. */
#define MSG_MAX 4096

static long linux_sendmsg(uint64_t fd, uint64_t msg_u, uint64_t flags) {
  if (flags & ~(uint64_t)MSG_NOSIGNAL_FLAG) return -EINVAL;
  if (!msg_u) return -EFAULT;
  uint64_t hdr[4];                          /* msg_name, msg_namelen, msg_iov, msg_iovlen */
  if (user_get(msg_u, hdr, sizeof hdr) != 0) return -EFAULT;
  uint32_t iovlen = (uint32_t)hdr[3];
  if (iovlen > IOV_LEN) return -EINVAL;
  /* Ancillary data (SCM_RIGHTS and friends) is how one process passes a
   * descriptor to another. Silently dropping it would break that handshake in a
   * way that looks like a protocol bug at the far end, so it is refused. */
  uint64_t ctl[2];
  if (user_get(msg_u + 32, ctl, sizeof ctl) == 0 && ctl[0] && ctl[1]) return -ENOSYS;

  uint64_t total = 0;
  uint64_t iov[2 * IOV_LEN];
  if (iovlen && user_get(hdr[2], iov, iovlen * 16) != 0) return -EFAULT;
  /* struct iovec is {base, len}: the length is the odd slot, and summing the
   * bases asks for a buffer the size of the address space. */
  for (uint32_t i = 0; i < iovlen; i++) total += iov[2 * i + 1];
  if (total > MSG_MAX) return -EMSGSIZE;
  if (!total) return 0;

  uint8_t *kbuf = kmalloc((size_t)total);
  if (!kbuf) return -ENOMEM;
  uint64_t at = 0;
  for (uint32_t i = 0; i < iovlen; i++) {
    if (!iov[2 * i]) continue;
    if (user_get(iov[2 * i], kbuf + at, iov[2 * i + 1]) != 0) { kfree(kbuf); return -EFAULT; }
    at += iov[2 * i];
  }
  /* fd_write, not sys_write: the bytes already live in a kernel buffer, and
   * sys_write's first act is to copy from a user pointer and refuse anything
   * outside the user window - which is exactly what a kernel address is. */
  fdtable_t *fds = current_fds();
  if (!fds) { kfree(kbuf); return -EBADF; }
  uint64_t done = 0;
  while (done < total) {
    long w = fd_write(fds, (int)fd, kbuf + done, total - done);
    if (w <= 0) { if (!done) { kfree(kbuf); return w; } break; }
    done += (uint64_t)w;
  }
  kfree(kbuf);
  return (long)done;
}

static long linux_recvmsg(uint64_t fd, uint64_t msg_u, uint64_t flags) {
  if (flags & ~(uint64_t)MSG_NOSIGNAL_FLAG) return -EINVAL;
  if (!msg_u) return -EFAULT;
  uint64_t hdr[4];
  if (user_get(msg_u, hdr, sizeof hdr) != 0) return -EFAULT;
  uint32_t iovlen = (uint32_t)hdr[3];
  if (iovlen > IOV_LEN) return -EINVAL;
  uint64_t ctl[2];
  if (user_get(msg_u + 32, ctl, sizeof ctl) == 0 && ctl[0] && ctl[1]) return -ENOSYS;

  uint64_t total = 0;
  uint64_t iov[2 * IOV_LEN];
  if (iovlen && user_get(hdr[2], iov, iovlen * 16) != 0) return -EFAULT;
  for (uint32_t i = 0; i < iovlen; i++) total += iov[2 * i + 1];
  if (total > MSG_MAX) return -EMSGSIZE;
  if (!total) return 0;

  uint8_t *kbuf = kmalloc((size_t)total);
  if (!kbuf) return -ENOMEM;
  fdtable_t *fds = current_fds();
  if (!fds) { kfree(kbuf); return -EBADF; }
  long n = fd_read(fds, (int)fd, kbuf, total);
  if (n > 0) {
    uint64_t at = 0, left = (uint64_t)n;
    for (uint32_t i = 0; i < iovlen && left; i++) {
      uint64_t take = iov[2 * i] < left ? iov[2 * i] : left;
      if (take && user_put(iov[2 * i], kbuf + at, take) != 0) { kfree(kbuf); return -EFAULT; }
      at += take; left -= take;
    }
    /* Every byte went somewhere, or the count would be a lie. */
    n = (long)(at);
  }
  kfree(kbuf);
  return n;
}

/* setsockopt/getsockopt. The accepted options are the ones that are already
 * true here or that only describe intent; the refused ones are the ones a
 * program would build behaviour on. TCP_NODELAY is accepted because Nagle is
 * not implemented - every send leaves immediately, which is what the caller
 * asked for. */
static long linux_setsockopt(uint64_t fd, uint64_t level, uint64_t opt,
                             uint64_t val_u, uint64_t len) {
  fdtable_t *fds = current_fds();
  if (!fds || fd >= FD_MAX || !fds->fd[fd]) return -EBADF;
  if (fds->fd[fd]->kind != FK_SOCKET) return -ENOTSOCK;
  (void)val_u; (void)len;
  if (level == 1) {                        /* SOL_SOCKET */
    if (opt == 2 || opt == 9) return 0;    /* SO_REUSEADDR, SO_KEEPALIVE: no-op */
    return -ENOPROTOOPT;
  }
  if (level == 6 && opt == 1) return 0;    /* TCP_NODELAY: already the behaviour */
  return -ENOPROTOOPT;
}

static long linux_getsockopt(uint64_t fd, uint64_t level, uint64_t opt,
                             uint64_t val_u, uint64_t len_u) {
  fdtable_t *fds = current_fds();
  if (!fds || fd >= FD_MAX || !fds->fd[fd]) return -EBADF;
  if (fds->fd[fd]->kind != FK_SOCKET) return -ENOTSOCK;
  uint32_t len = 0;
  if (user_get(len_u, &len, sizeof len) != 0) return -EFAULT;
  if (level == 1 && opt == 4) {            /* SO_ERROR: errors surface where they happen */
    int32_t zero = 0;
    if (len < 4) return -EINVAL;
    if (user_put(val_u, &zero, sizeof zero) != 0) return -EFAULT;
    uint32_t four = 4;
    if (user_put(len_u, &four, sizeof four) != 0) return -EFAULT;
    return 0;
  }
  if (level == 1 && opt == 3) {            /* SO_TYPE */
    int32_t stream = 1;
    if (len < 4) return -EINVAL;
    if (user_put(val_u, &stream, sizeof stream) != 0) return -EFAULT;
    uint32_t four = 4;
    if (user_put(len_u, &four, sizeof four) != 0) return -EFAULT;
    return 0;
  }
  return -ENOPROTOOPT;
}

/* getrandom(318). RDRAND where the CPU has it, and a splitmix of the timestamp
 * counter where it does not - which includes QEMU's default model, so the
 * fallback is the path that boots. Said plainly because it is the honest
 * version: this is a source good enough to seed a test, and the ABI doc says it
 * is not a guarantee to build key material on. */
static uint64_t rng_state;

static int cpu_has_rdrand(void) {
  uint32_t a, b, c, d;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  (void)a; (void)b; (void)d;
  return (c >> 30) & 1;
}

/* RDRAND spelled out in bytes: the kernel is compiled without -mrdrnd, so the
 * mnemonic and its intrinsic are both unavailable, but the instruction is just
 * REX.W + 0F C7 /6 and the CPUID check above has already said it is there. */
static int rdrand64(uint64_t *out) {
  unsigned char ok;
  __asm__ volatile(".byte 0x48, 0x0f, 0xc7, 0xf0\n\tsetc %1"
                   : "=a"(*out), "=qm"(ok));
  return ok;
}

static uint64_t rng_step(void) {
  static int checked, have;
  if (!checked) { checked = 1; have = cpu_has_rdrand(); }
  if (have) {
    uint64_t x = 0;
    for (int i = 0; i < 10; i++)          /* RDRAND can fail; retry like the ISA says */
      if (rdrand64(&x)) return x;
  }
  rng_state += 0x9E3779B97F4A7C15ull ^ task_total_ticks();
  uint64_t z = rng_state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

static long linux_getrandom(uint64_t buf, uint64_t len, uint64_t flags) {
  if (flags) return -EINVAL;
  if (len > 4096) len = 4096;              /* one page is plenty for a seed */
  long wanted = (long)len;                 /* the answer is the count, as with read(2) */
  uint8_t tmp[256];
  while (len) {
    uint64_t n = len < sizeof tmp ? len : sizeof tmp;
    for (uint64_t i = 0; i < n; i += 8) {
      uint64_t v = rng_step();
      for (uint64_t j = 0; j < 8 && i + j < n; j++) tmp[i + j] = (uint8_t)(v >> (8 * j));
    }
    if (user_put(buf, tmp, n) != 0) return -EFAULT;
    buf += n; len -= n;
  }
  return wanted;
}

/* ==================================================== memfd (roadmap 9)
 *
 * An anonymous file whose only purpose is to be mapped. Chrome asks for one per
 * shared region, hands the descriptor to the other end of a pipe and maps it, so
 * the thing that matters is not the file at all: it is that two processes can
 * look at the same bytes without a copy in between.
 *
 * The name is accepted and dropped. There is no /proc to carry it and no
 * directory entry to invent, and a name that goes nowhere is still better than
 * refusing a call whose whole payload is the descriptor.
 *
 * MFD_CLOEXEC (1) and MFD_ALLOW_SEALING (2) are accepted and ignored, and the
 * header says so: nothing in this system is marked close-on-exec, so honouring
 * the bit would describe an inheritance rule that does not exist. MFD_HUGETLB (4)
 * is -EINVAL - a program that asked for huge pages and got ordinary ones has been
 * lied to about the one thing it asked for.
 */
static long linux_memfd_create(uint64_t name_u, uint64_t flags) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  if (flags & ~3ull) return -EINVAL;
  if (name_u) {
    char name[32];
    /* Validated, then dropped: a program that passes a bad pointer gets -EFAULT
     * for it rather than a descriptor for a file it never named. */
    if (user_str(name, sizeof name, name_u) != 0) return -EFAULT;
  }
  return fd_shfile(t->mm->fds);
}

/* ftruncate(77), for the one kind of file whose length is a program's to set.
 * A boot module is read-only and says so with -EROFS rather than -EINVAL: those
 * mean different things to a caller, and only one of them is true here. */
static long linux_ftruncate(uint64_t fd, uint64_t len) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  shfile_t *f = fd_shfile_of(t->mm->fds, (int)fd);
  if (f) return shfile_truncate(f, len);
  if (fd_file(t->mm->fds, (int)fd)) return -EROFS;
  return -EBADF;
}

/* ============================================================ epoll (roadmap 8)
 *
 * epoll is poll with the set kept. The wait below is the same readiness loop
 * sys_poll runs - ask each descriptor, sleep a tick, look again - so its cost is
 * the set size, not the number ready. What it buys a program is the thing poll
 * cannot give it: a set that outlives the call, so an event loop is built once
 * instead of rebuilt on every iteration.
 *
 * Edge triggering is refused in the kernel (see epoll.c). EPOLL_CLOEXEC is
 * accepted and ignored: nothing here is marked close-on-exec, and honouring the
 * flag for this one call would describe an inheritance rule the rest of the
 * system does not have.
 */
static long sys_epoll_create1(uint64_t flags) {
  fdtable_t *t = current_fds();
  if (!t) return -EBADF;
  (void)flags;
  return fd_epoll(t);
}

static long sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fdn, uint64_t ev_u) {
  fdtable_t *t = current_fds();
  if (!t) return -EBADF;
  if (epfd >= FD_MAX || !t->fd[epfd]) return -EBADF;
  ofile_t *o = t->fd[epfd];
  if (o->kind != FK_EPOLL || !o->ep) return -EINVAL;

  uint32_t events = 0;
  uint64_t data = 0;
  if (op != EPOLL_CTL_DEL) {
    /* struct epoll_event is *packed* on x86-64: four bytes of bits, then eight of
     * data at offset 4. Copying it as a struct would insert the padding the ABI
     * does not have and read the program's data from the wrong place. */
    if (user_copy(&events, ev_u, 4) != 0) return -EFAULT;
    if (user_copy(&data, ev_u + 4, 8) != 0) return -EFAULT;
    if (fdn >= FD_MAX || !t->fd[fdn]) return -EBADF;
  }
  return epoll_ctl(o->ep, (int)op, (int)fdn, events, data);
}

static long sys_epoll_wait(uint64_t epfd, uint64_t ev_u, uint64_t maxevents, uint64_t timeout_ms) {
  fdtable_t *t = current_fds();
  if (!t) return -EBADF;
  if (epfd >= FD_MAX || !t->fd[epfd]) return -EBADF;
  ofile_t *o = t->fd[epfd];
  if (o->kind != FK_EPOLL || !o->ep) return -EINVAL;
  if (maxevents == 0 || maxevents > EPOLL_MAX) return -EINVAL;
  if (user_prep(ev_u, maxevents * 12) != 0) return -EFAULT;

  /* A negative timeout means wait forever, which is how an event loop spends
   * almost all of its life; the unsigned argument arrives sign-extended. */
  int forever = (int64_t)timeout_ms < 0;
  uint64_t end = task_total_ticks() + (timeout_ms + 9) / 10;

  epitem_t got[EPOLL_MAX];
  for (;;) {
    int n = epoll_ready(o->ep, t, got, (int)maxevents);
    if (n > 0 || (!forever && task_total_ticks() >= end)) {
      for (int i = 0; i < n; i++) {
        /* Packed, twelve bytes per event: bits at +0, data at +4. */
        if (user_put(ev_u + (uint64_t)i * 12, &got[i].events, 4) != 0) return -EFAULT;
        if (user_put(ev_u + (uint64_t)i * 12 + 4, &got[i].data, 8) != 0) return -EFAULT;
        /* After the report, never during the scan: a one-shot registration is
         * removed once it has been handed over, so the next wait does not see it. */
        epoll_drop_oneshot(o->ep, got[i].fd);
      }
      return n;
    }
    task_sleep_ticks(1);
  }
}

/* Defined with the mmap path below; sys_open sits above it and needs them. */
extern int r3_dyn_trace;
extern int r3_dyn_trace_pid;

static long sys_open(uint64_t path_u, uint64_t flags) {
  fdtable_t *fds = current_fds();
  if (!fds) return -EBADF;
  char path[RFILE_NAME];
  if (user_string(path, path_u, RFILE_NAME) < 0) return -EFAULT;
  long r = fd_open(fds, path, (int)flags);
  if (r3_dyn_trace && task_current() == r3_dyn_trace_pid) {
    serial_puts("[dyn] open ");
    serial_puts(path);
    serial_puts(" -> ");
    serial_puthex((uint64_t)r);
    serial_puts("\n");
  }
  return r;
}

static long sys_read(uint64_t fdn, uint64_t buf, uint64_t len) {
  fdtable_t *fds = current_fds();
  if (!fds) return -EBADF;
  if (len > 4096) len = 4096;
  if (buf >= RING3_LIMIT) return -EFAULT;
  if (buf + len > RING3_LIMIT) len = RING3_LIMIT - buf;
  /* Pre-fault the destination first: read() into fresh SYS_VM_ALLOC memory is
   * the normal case, and those pages do not exist yet. */
  if (user_prep(buf, len) != 0) return -EFAULT;
  static char tmp[4096];
  long r = fd_read(fds, (int)fdn, tmp, len);
  if (r <= 0) return r;
  if (user_put(buf, tmp, (uint64_t)r) != 0) return -EFAULT;
  return r;
}

/* ================================================== the Linux ABI (roadmap 5)
 *
 * A second door into the same kernel: the `syscall` instruction, carrying Linux
 * x86-64 numbers, mapped onto the handlers above. The goal is not to be Linux -
 * it is that a libc and its assumptions already exist, so every program that will
 * ever run here arrives speaking POSIX source or this ABI. Translating at the
 * door keeps one implementation of each mechanism instead of two dialects of it.
 *
 * The numbers are a distinct space from SYS_* on purpose: 34 is a pipe through
 * int $0x80 and `pause` through `syscall`, so a number alone means nothing. The
 * entry that delivered it decides which table reads it.
 *
 * Return convention already matches: negative errno, in rax.
 */

#define ABI_OMI   0
#define ABI_LINUX 1

#define MSR_EFER     0xC0000080
#define MSR_STAR     0xC0000081
#define MSR_LSTAR    0xC0000082
#define MSR_SFMASK   0xC0000084
#define MSR_FS_BASE  0xC0000100

static inline uint64_t msr_rd(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

static inline void msr_wr(uint32_t msr, uint64_t v) {
  __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static int cpu_has_syscall(void) {
  uint32_t a, b, c, d;
  b = c = d = 0;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0));
  return (int)((d >> 11) & 1);
}

void r3_linux_stub(void);   /* the entry stub, in the asm block below */

/* Armed once, before any task runs. If the CPU cannot take SYSCALL the gate is
 * left shut rather than pointed at a vector that never fires, and everything
 * still works through int $0x80. */
static void linux_gate_init(void) {
  if (!cpu_has_syscall()) {
    serial_puts("[linux] no SYSCALL instruction: gate stays shut\n");
    return;
  }
  msr_wr(MSR_EFER, msr_rd(MSR_EFER) | 1);          /* SCE */
  /* STAR[47:32] = kernel CS 0x08 (SS is CS+8 = 0x10 by construction).
   * STAR[63:48] = 0x10, which SYSRET would turn into CS 0x23 / SS 0x1b. We
   * return with iretq, so that half exists only to keep the MSR coherent. */
  msr_wr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
  msr_wr(MSR_LSTAR, (uint64_t)(uintptr_t)r3_linux_stub);
  /* Clear IF on entry. The stub's stack switch is two instructions with the user
   * stack live in between; with interrupts off nothing can nest there. */
  msr_wr(MSR_SFMASK, 0x200);
  serial_puts("[linux] syscall gate armed: LSTAR=");
  serial_puthex((uint64_t)(uintptr_t)r3_linux_stub);
  serial_puts(" SFMASK=IF\n");
}

/* One exit path for both ABIs, so the log line and the kill cannot drift. */
static void r3_exit_now(uint64_t code) {
  /* CLONE_CHILD_CLEARTID: zero the word the thread was born with and wake
   * whoever is waiting on it. This is the whole of pthread_join - musl waits on
   * that futex, and a thread that never clears it looks alive forever. */
  task_t *t = task_at(task_current());
  if (t && t->ctid) {
    uint32_t zero = 0;
    if (user_put(t->ctid, &zero, sizeof zero) == 0)
      futex_wake_addr(t->mm, t->ctid, 1);
    t->ctid = 0;
  }
  /* The windows a dying task opened go with it, before the log line, so that a
   * test can see both the exit and the reaping in one place. */
  winipc_close_owned((int)task_current());

  serial_puts("[ring3] task ");
  serial_puts(task_name(task_current()));
  serial_puts(" exited with code ");
  serial_puthex(code);
  serial_puts("\n");
  /* The per-process tally, printed only where something was actually split: a
   * fork's cost is how many of its shared pages the child really wrote. */
  if (t && t->cow_faults) {
    serial_puts("[cow] pid=");
    serial_puthex((uint64_t)t->id);
    serial_puts(" faults=");
    serial_puthex(t->cow_faults);
    serial_puts(" copies=");
    serial_puthex(t->cow_copies);
    serial_puts(" steals=");
    serial_puthex(t->cow_steals);
    cow_stats_t cs;
    cow_get_stats(&cs);
    serial_puts(" | bank shares=");
    serial_puthex(cs.shares);
    serial_puts(" releases=");
    serial_puthex(cs.releases);
    serial_puts(" live=");
    serial_puthex(cs.live_frames);
    serial_puts(" saturated=");
    serial_puthex(cs.saturations);
    serial_puts(" free=");
    serial_puthex(pmm_free_pages());
    serial_puts("\n");
  }
  task_kill_current(code);          /* does not return */
}

/* --------------------------------------------------------------- handlers */

#define LINUX_TCGETS     0x5401
#define LINUX_TIOCGWINSZ 0x5413
#define LINUX_ARCH_SET_FS 0x1002
#define LINUX_ARCH_GET_FS 0x1003

/* Termios is a shape, not a driver: musl asks once whether stdout is a terminal
 * and line-buffers if it is. There is no line discipline here yet, so the flags
 * describe the intent (canonical, echo, signals) rather than an implementation.
 * Answering honestly with -ENOTTY would make every program's output block-buffered
 * until exit, which is the wrong lie to tell. */
static long linux_ioctl(uint64_t req, uint64_t arg) {
  if (req == LINUX_TCGETS) {
    uint8_t tio[36];
    for (int i = 0; i < (int)sizeof tio; i++) tio[i] = 0;
    tio[4] = 1;                                  /* c_oflag = OPOST */
    tio[12] = 0x0B;                              /* c_lflag = ISIG|ICANON|ECHO */
    return user_put(arg, tio, sizeof tio) ? -EFAULT : 0;
  }
  if (req == LINUX_TIOCGWINSZ) {
    uint16_t ws[4] = { 24, 80, 0, 0 };            /* rows, cols, xpix, ypix */
    return user_put(arg, ws, sizeof ws) ? -EFAULT : 0;
  }
  return -EINVAL;
}

typedef struct { uint64_t base, len; } linux_iovec_t;

static long linux_writev(uint64_t fd, uint64_t iov, uint64_t cnt) {
  long total = 0;
  for (uint64_t i = 0; i < cnt; i++) {
    linux_iovec_t v;
    if (user_copy((char *)&v, iov + i * sizeof v, sizeof v) != 0) return -EFAULT;
    long r = sys_write(fd, v.base, v.len);
    if (r < 0) return total ? total : r;           /* a partial write is a success */
    total += r;
  }
  return total;
}

/* Two clocks now. The PIT tick counter answers anything that means "how long
 * since", and walltime.c answers "when" - the CMOS date read once at boot plus
 * the ticks since. Before that, CLOCK_REALTIME reported time-since-boot, which
 * is only right for a machine that was switched on in 1970; TLS made the
 * difference visible, because every certificate looked not-yet-valid. */
static long linux_clock_gettime(uint64_t clk, uint64_t ts) {
  /* CLOCK_REALTIME (0) and CLOCK_REALTIME_COARSE (5) are the date; every other
   * id musl can ask for - MONOTONIC, MONOTONIC_RAW, BOOTTIME, PROCESS_CPUTIME -
   * means "how long since something", which is what ticks answer. */
  uint64_t ticks = task_total_ticks();
  int64_t out[2];
  if (clk == 0 || clk == 5) {
    uint64_t secs = 0; uint32_t ns = 0;
    walltime_now(&secs, &ns);
    out[0] = (int64_t)secs;
    out[1] = (int64_t)ns;
  } else {
    out[0] = (int64_t)(ticks / TASK_TICK_HZ);
    out[1] = (int64_t)((ticks % TASK_TICK_HZ) * (1000000000 / TASK_TICK_HZ));
  }
  return user_put(ts, out, sizeof out) ? -EFAULT : 0;
}

/* FS base is per-CPU state (IA32_FS_BASE), not per-task, so a second thread with
 * its own TLS block would clobber the first. Per-task save/restore belongs with
 * the rest of the %fs gap in step 3; until then this is correct for one thread,
 * which is every program that exists today. */
/* %fs belongs to the thread, not to the cpu. The MSR is written for the caller
 * that is running now; the task's own copy is what the scheduler reloads when
 * some other thread gets the cpu, and without it two threads share one thread
 * pointer. */
static long linux_arch_prctl(uint64_t op, uint64_t addr) {
  task_t *self = task_at(task_current());
  if (op == LINUX_ARCH_SET_FS) {
    msr_wr(MSR_FS_BASE, addr);
    if (self) self->fs_base = addr;
    return 0;
  }
  if (op == LINUX_ARCH_GET_FS) {
    uint64_t v = self ? self->fs_base : msr_rd(MSR_FS_BASE);
    return user_put(addr, &v, 8) ? -EFAULT : 0;
  }
  return -EINVAL;
}

/* Address space, not memory: the area is reserved and nothing is mapped until a
 * byte is touched, so this number costs no RAM. It is not what broke the browser -
 * that was TASK_AREAS, and the faults I first blamed on this ceiling were pages in
 * mallocng's guard gaps. It is raised anyway, because 16 MB of process heap is a
 * floor a page parser reaches and the failure at the ceiling is an ENOMEM that a
 * library turns into a NULL dereference. */
#define LINUX_BRK_MAX (64u << 20)

/* brk reserves one area and then moves a pointer inside it; nothing is mapped
 * until a byte is touched, so asking for 16 MB costs one area slot. Returning the
 * current break on failure - not an error - is the contract: the caller compares
 * what it got with what it asked for. */
static long linux_brk(uint64_t want) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  mm_t *mm = t->mm;
  if (!mm->brk_base) {
    long b = sys_vm_alloc(LINUX_BRK_MAX, 3);
    if (b < 0) return b;
    mm->brk_base = (uint64_t)b;
    mm->brk_cur = (uint64_t)b;
    mm->brk_end = (uint64_t)b + LINUX_BRK_MAX;
  }
  if (want == 0) return (long)mm->brk_cur;
  if (want < mm->brk_base || want > mm->brk_end) return (long)mm->brk_cur;
  mm->brk_cur = want;
  return (long)want;
}

/* Unmap [base, base+len) the way Linux does: the pages go, and an area that
 * only partly overlaps is trimmed or split in two, never dropped whole.
 *
 * "Dropped whole" was the shortcut this system shipped with, and it is wrong in
 * a way that costs a process its heap: mallocng reserves memory with brk and then
 * MAP_FIXEDs a one-page PROT_NONE guard over the *first* page of that
 * reservation. Under Linux the rest of the reservation survives; under a
 * whole-area drop the reservation is gone and the allocator's next write lands in
 * an unmapped hole. Library loaders do the same thing at library granularity, so
 * this has to be exact rather than approximately safe. */
static void area_drop_front(vm_area_t *a, uint64_t d) {
  if (a->src) {
    if (a->src_off + d < a->src_len) { a->src_len -= (uint32_t)d; a->src_off += d; }
    else { a->src = 0; a->src_off = 0; a->src_len = 0; }
  }
  a->base += d;
  a->len  -= d;
}

static void area_release(vm_area_t *a) {
  if (!a || !a->used) return;
  if (a->shf) {
    shfile_unref((shfile_t *)a->shf);
    a->shf = 0;
  }
  a->used = 0;
}

static void area_drop_tail(vm_area_t *a, uint64_t keep) {
  a->len = keep;
  if (a->src && a->src_len > keep) a->src_len = (uint32_t)keep;
}

static int vm_unmap_range(task_t *t, mm_t *mm, uint64_t base, uint64_t len) {
  uint64_t end = base + len;
  uint64_t kept = 0;
  vm_release_range(t, base, len, &kept);

  for (int i = 0; i < TASK_AREAS; i++) {
    vm_area_t *a = &mm->areas[i];
    if (!a->used) continue;
    uint64_t ab = a->base, ae = a->base + a->len;
    if (ae <= base || ab >= end) continue;              /* no overlap */

    if (ab >= base && ae <= end) {                       /* swallowed whole */
      area_release(a);
      continue;
    }

    if (ab < base && ae > end) {                         /* fixed range inside it */
      int slot = -1;
      for (int k = 0; k < TASK_AREAS; k++) if (!mm->areas[k].used) { slot = k; break; }
      if (slot < 0) return -ENOMEM;                      /* caller keeps what it had */
      vm_area_t *high = &mm->areas[slot];
      *high = *a;
      /* A split leaves two areas where there was one, so the file is now claimed
       * twice and the count has to say so. */
      if (high->shf) shfile_ref((shfile_t *)high->shf);
      area_drop_front(high, end - ab);
      area_drop_tail(a, base - ab);
      continue;
    }

    if (ab < base) area_drop_tail(a, base - ab);          /* keep the low part */
    else           area_drop_front(a, end - ab);          /* keep the high part */
  }
  return 0;
}

/* Linux mmap (9). Two kinds of region, one entry point:
 *
 *   anonymous   - pages appear zeroed on first touch (the original behaviour);
 *   file-backed - pages appear filled from a boot module's bytes, and the frame
 *                 is private to this space, which is what MAP_PRIVATE means.
 *
 *   shared      - pages are the file's own, in a memfd, and everybody who maps
 *                 the same file sees the same bytes (MAP_SHARED).
 *
 * MAP_SHARED over a boot module is -ENOSYS, and stays that way: the module is
 * read-only kernel memory with no write-back path, and a "shared" mapping that
 * silently is not shared is worse than a refusal. MAP_SHARED over a memfd is
 * real sharing, because there is nothing to write back - the pages *are* the
 * file. MAP_SHARED|MAP_ANONYMOUS used to fall through to the anonymous path and
 * be silently private; it now gets a file of its own, so a forked child shares
 * those pages the way every program that assumes a forked buffer is shared
 * expects.
 *
 * A mapping is refused (-EINVAL) when its offset is not page-aligned or when the
 * offset starts at or past EOF: an area with no file behind it is a bug in the
 * caller, not an empty file. A length whose *last* page runs past EOF is fine and
 * its tail reads zero - ld.so maps segments that way, and Linux does the same.
 *
 * PROT_NONE is not implemented: a reservation is readable rather than
 * unreadable. ld.so only uses reservations as placeholders that it MAP_FIXEDs
 * over, so nothing depends on it yet. That is a gap, not a design decision.
 */
#define LINUX_MAP_SHARED     0x01
#define LINUX_MAP_PRIVATE    0x02
#define LINUX_MAP_FIXED      0x10
#define LINUX_MAP_ANONYMOUS  0x20

static long linux_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t lflags,
                       int fd, uint64_t off) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm || !len) return -EINVAL;
  mm_t *mm = t->mm;
  int anon = (lflags & LINUX_MAP_ANONYMOUS) != 0;
  int shared = (lflags & LINUX_MAP_SHARED) != 0;
  /* Exactly one of the two, as Linux insists: a request that says both has not
   * decided, and guessing which half to honour hides the bug in the caller. */
  if (shared && (lflags & LINUX_MAP_PRIVATE)) return -EINVAL;
  shfile_t *backing = 0;
  int backing_shared = 1;
  /* The anonymous case builds the file, so this function owns the reference that
   * shfile_new handed back; the descriptor case borrows the file table's, which
   * is not ours to drop. Every early return below has to know which it is - an
   * extra unref here is a file freed while its descriptor still points at it, and
   * the frames come back one short. */
  int backing_owns_ref = 0;
  if (!anon) {
    shfile_t *shf = fd_shfile_of(mm->fds, fd);
    if (shf) {
      /* Both modes start from the same bytes - the difference is whether the
       * mapping sees the file's pages or its own copy of them. */
      backing = shf;
      backing_shared = shared;
    } else if (shared) {
      return -ENOSYS;                  /* a boot module: read-only, no write-back */
    }
    /* otherwise a boot module, mapped privately below out of the blob */
  } else if (shared) {
    backing = shfile_new();            /* a file for nobody but this mapping */
    if (!backing) return -ENOMEM;
    backing_owns_ref = 1;
    if (shfile_truncate(backing, len) != 0) { shfile_unref(backing); return -ENOMEM; }
  }

  len = (len + 0xFFF) & ~0xFFFULL;
  if (len > (RING3_MMAP_TOP - RING3_MMAP_BASE)) return -ENOMEM;

  uint64_t pte = PTE_P | PTE_U;
  if (prot & 2) pte |= PTE_W;

  const uint8_t *src = 0;
  uint32_t src_len = 0;
  if (backing) {
    /* Neither mode may name a page the file does not have: the fault would have
     * nothing to hand out, and a mapping longer than its file is a program bug
     * that Linux reports rather than satisfies. The blob path cannot describe a
     * memfd at all - its bytes are pages, not a contiguous image - so a private
     * mapping of one is filled page by page from those same frames under a plain
     * PTE, which makes the copy real and keeps later writes local. */
    if (off & 0xFFF) { if (backing_owns_ref) shfile_unref(backing); return -EINVAL; }
    if (off + len > shfile_size(backing)) { if (backing_owns_ref) shfile_unref(backing); return -EINVAL; }
  } else if (!anon) {
    if (off & 0xFFF) return -EINVAL;
    rfile_t *f = fd_file(mm->fds, fd);
    if (!f) return -EBADF;
    if (off >= f->size) return -EINVAL;
    src = f->data + off;
    src_len = f->size - (uint32_t)off;
  }

  int slot = -1;
  for (int i = 0; i < TASK_AREAS; i++) if (!mm->areas[i].used) { slot = i; break; }
  if (slot < 0) { if (backing_owns_ref) shfile_unref(backing); return -ENOMEM; }

  uint64_t base;
  if (lflags & LINUX_MAP_FIXED) {
    if (addr & 0xFFF) return -EINVAL;
    base = addr;
    if (base < RING3_MMAP_BASE || base + len > RING3_MMAP_TOP) {
      if (backing_owns_ref) shfile_unref(backing);
      return -ENOMEM;
    }
    /* The caller is taking these addresses: exactly these, no more. */
    if (vm_unmap_range(t, mm, base, len) != 0) return -ENOMEM;
  } else {
    if (!mm->mmap_hint) mm->mmap_hint = RING3_MMAP_BASE;
    base = mm->mmap_hint;
    /* Skip past anything already here rather than handing out an address that a
     * MAP_FIXED caller already owns: the overlap is growable, the loop is not. */
    for (int guard = 0; guard < TASK_AREAS && vm_overlaps(mm, base, len); guard++) {
      for (int i = 0; i < TASK_AREAS; i++) {
        vm_area_t *a = &mm->areas[i];
        if (!a->used) continue;
        if (base < a->base + a->len && a->base < base + len) {
          base = a->base + a->len + 0x1000;
          break;
        }
      }
    }
    if (vm_overlaps(mm, base, len)) { if (backing_owns_ref) shfile_unref(backing); return -ENOMEM; }
    if (base + len > RING3_MMAP_TOP) { if (backing_owns_ref) shfile_unref(backing); return -ENOMEM; }
    mm->mmap_hint = base + len + 0x1000;   /* leave one guard page between regions */
  }

  vm_area_t *a = &mm->areas[slot];
  if (backing) shfile_ref(backing);    /* the area's claim, dropped with the area */
  a->shf = backing;
  /* The mapping now holds the file, so the caller's own claim is handed over
   * rather than kept: an anonymous region is freed by nobody else. */
  if (backing_owns_ref) shfile_unref(backing);
  /* a->flags already carries the bits the pages get, and the fault path reads
   * PTE_SHARED there to tell "hand out the file's frame" from "take a copy of
   * it": one flag, one meaning, on the area the pages belong to. */
  if (backing && backing_shared) pte |= PTE_SHARED;
  a->base = base;
  a->len = len;
  a->flags = pte;
  a->prot = (uint8_t)(prot & 7);
  a->used = 1;
  a->src = src;
  a->src_off = 0;
  a->src_len = src_len;
  return (long)base;
}

/* mprotect closes a step-2 gap: the area's flags decide what future pages get,
 * and the pages already present have their write bit rewritten here. */
/* Temporary instrumentation for bring-up of the dynamic loader: when set, every
 * mapping call a user process makes is logged with its arguments and result.
 * A loader is a state machine over mmap/mprotect, so the sequence is the
 * evidence - guessing at it from a fault address wastes runs. */
int r3_dyn_trace = 0;
int r3_dyn_trace_pid = -1;

static void dynmap(const char *op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, long r) {
  if (!r3_dyn_trace || task_current() != r3_dyn_trace_pid) return;
  serial_puts("[dyn] ");
  serial_puts(op);
  serial_puts(" "); serial_puthex(a);
  serial_puts(" "); serial_puthex(b);
  serial_puts(" "); serial_puthex(c);
  if (d) { serial_puts(" fd="); serial_puthex(d); }
  serial_puts(" -> "); serial_puthex((uint64_t)r);
  serial_puts("\n");
}

static long linux_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm || !len) return -EINVAL;
  uint64_t base = addr & ~0xFFFULL;
  uint64_t end = (addr + len + 0xFFF) & ~0xFFFULL;
  int hit = 0;
  for (int i = 0; i < TASK_AREAS; i++) {
    vm_area_t *a = &t->mm->areas[i];
    if (!a->used) continue;
    /* A range may cross several areas - a loader protecting a whole library in
     * one call does exactly that - so each area takes its own intersection. */
    uint64_t lo = base > a->base ? base : a->base;
    uint64_t hi = end < a->base + a->len ? end : a->base + a->len;
    if (lo >= hi) continue;
    hit = 1;
    a->prot = (uint8_t)(prot & 7);
    if (prot & 2) a->flags |= PTE_W;
    else          a->flags &= ~PTE_W;

    /* No access at all: the pages go. Nothing records a frame once its PTE is
     * cleared, so keeping them would leak every one of them, and a guard page is
     * the one mapping nobody ever takes back. Linux does preserve the bytes
     * across a no-access span, and this does not; that difference is written
     * down in docs/linux-abi.md rather than papered over. */
    if (!(prot & 7)) {
      uint64_t kept = 0;
      vm_release_range(t, lo, hi - lo, &kept);
      continue;
    }

    for (uint64_t p = lo; p < hi; p += 4096) {
      uint64_t *pt = walk_user(t->cr3, p);
      if (!pt) continue;
      int idx = (int)((p >> 12) & 511);
      if (!(pt[idx] & PTE_P)) continue;
      if (prot & 2) {
        /* Turning a page writable while another space maps the same frame is a
         * fork waiting to corrupt itself: take the copy here, not at the store. */
        uint64_t phys = pt[idx] & ~0xFFFULL;
        if (!(pt[idx] & PTE_SHARED) && cow_amount(phys) > 1 &&
            !cow_unshare_page(p)) continue;
        pt[idx] |= PTE_W;
        flush_page(p);
      } else {
        pt[idx] &= ~PTE_W;
        flush_page(p);
      }
    }
  }
  return hit ? 0 : -ENOMEM;      /* Linux says ENOMEM for an unmapped range */
}

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_PRIVATE 128
#define LINUX_FUTEX_CLOCK_REALTIME 256

/* Timeouts arrive as a timespec here and as ticks inside; convert rather than
 * inventing a second wait path. */
static long linux_futex(uint64_t addr, uint64_t op, uint64_t val, uint64_t ts) {
  uint64_t o = op & ~(uint64_t)(LINUX_FUTEX_PRIVATE | LINUX_FUTEX_CLOCK_REALTIME);
  if (o != LINUX_FUTEX_WAIT && o != LINUX_FUTEX_WAKE) return -EINVAL;
  uint64_t ticks = 0;
  if (o == LINUX_FUTEX_WAIT && ts) {
    int64_t tv[2];
    if (user_copy((char *)tv, ts, sizeof tv) != 0) return -EFAULT;
    if (tv[0] < 0 || tv[1] < 0) return -EINVAL;
    ticks = (uint64_t)tv[0] * TASK_TICK_HZ + (uint64_t)tv[1] / (1000000000 / TASK_TICK_HZ);
    if (!ticks) ticks = 1;      /* a sub-tick timeout must not become "for ever" */
  }
  return sys_futex(addr, o, val, ticks);
}

/* Unknown numbers are logged once each: a libc that probes for a syscall in a
 * loop would otherwise fill the serial log and hide the interesting line. */
static uint32_t linux_seen[8];
static void linux_unknown(uint64_t num) {
  int first = 0;
  if (num < 256) {
    if (!(linux_seen[num >> 5] & (1u << (num & 31)))) {
      linux_seen[num >> 5] |= 1u << (num & 31);
      first = 1;
    }
  } else {
    static int more_than_one;
    if (!more_than_one) { more_than_one = 1; first = 1; }
  }
  if (!first) return;
  serial_puts("[linux] unhandled syscall ");
  serial_puthex(num);
  serial_puts(" (returned -ENOSYS)\n");
}

/* ------------------------------------------------------- stat (4, 5, 6, 262) */

/* x86-64 struct stat, 144 bytes. The layout is not ours to choose: it is what
 * the libc compiled into the program expects, including the padding after
 * st_gid that makes st_rdev land on an 8-byte boundary. */
typedef struct {
  uint64_t st_dev;
  uint64_t st_ino;
  uint64_t st_nlink;
  uint32_t st_mode;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t __pad0;
  uint64_t st_rdev;
  int64_t  st_size;
  int64_t  st_blksize;
  int64_t  st_blocks;
  int64_t  st_atime;
  uint64_t st_atime_nsec;
  int64_t  st_mtime;
  uint64_t st_mtime_nsec;
  int64_t  st_ctime;
  uint64_t st_ctime_nsec;
  int64_t  __unused[3];
} linux_stat_t;

#define S_IFMT_L   0170000
#define S_IFREG_L  0100000
#define S_IFDIR_L  0040000
#define S_IFCHR_L  0020000
#define S_IFIFO_L  0010000
#define S_IFSOCK_L 0140000

#define AT_FDCWD_L (-100)

static void stat_fill(linux_stat_t *s, uint32_t mode, int64_t size, uint64_t ino) {
  for (unsigned i = 0; i < sizeof *s; i++) ((uint8_t *)s)[i] = 0;
  uint64_t ticks = task_total_ticks();
  s->st_dev = 1;                       /* one filesystem, and it is not a disk */
  s->st_ino = ino ? ino : (uint64_t)(mode & S_IFMT_L);
  s->st_nlink = ((mode & S_IFMT_L) == S_IFDIR_L) ? 2 : 1;
  s->st_mode = mode;
  s->st_size = size;
  s->st_blksize = 4096;
  s->st_blocks = (size + 511) / 512;
  s->st_atime = s->st_mtime = s->st_ctime = (int64_t)(ticks / TASK_TICK_HZ);
  s->st_atime_nsec = s->st_mtime_nsec = s->st_ctime_nsec =
      (uint64_t)(ticks % TASK_TICK_HZ) * (1000000000ull / TASK_TICK_HZ);
}

/* A descriptor's own description, described. Consoles, pipes and sockets get a
 * truthful type rather than a plausible one: a program that stats stdout and is
 * told it is a regular file will try to seek it. */
static long stat_by_fd(int fd, uint64_t st_u) {
  fdtable_t *fds = current_fds();
  if (!fds) return -EBADF;
  ofile_t *o = fd_ofile(fds, fd);
  if (!o) return -EBADF;

  linux_stat_t s;
  if (o->kind == FK_CONSOLE)          stat_fill(&s, S_IFCHR_L | 0620, 0, 2);
  else if (o->kind == FK_PIPE)        stat_fill(&s, S_IFIFO_L | 0600, 0, 3);
  else if (o->kind == FK_SOCKET)      stat_fill(&s, S_IFSOCK_L | 0600, 0, 4);
  else if (o->file)                   stat_fill(&s, S_IFREG_L | 0644,
                                                (int64_t)o->file->size,
                                                5 + (uint64_t)o->file->name[0]);
  else                                return -EBADF;
  return user_put(st_u, &s, sizeof s) == 0 ? 0 : -EFAULT;
}

static long stat_by_path(uint64_t path_u, uint64_t st_u) {
  char path[RFILE_NAME];
  if (user_string(path, path_u, RFILE_NAME) < 0) return -EFAULT;

  size_t size = 0;
  int is_dir = 0;
  if (vfs_stat(path, &size, &is_dir) != 0) {
    /* Not a boot module: a file we made in RAM can still be asked about. */
    rfile_t *f = rfs_find(path);
    if (!f) return -ENOENT;
    linux_stat_t s;
    stat_fill(&s, S_IFREG_L | 0644, (int64_t)f->size, 5 + (uint64_t)f->name[0]);
    return user_put(st_u, &s, sizeof s) == 0 ? 0 : -EFAULT;
  }
  linux_stat_t s;
  if (is_dir) stat_fill(&s, S_IFDIR_L | 0755, 0, 1);
  else        stat_fill(&s, S_IFREG_L | 0644, (int64_t)size, 5 + (uint64_t)path[0]);
  return user_put(st_u, &s, sizeof s) == 0 ? 0 : -EFAULT;
}

/* newfstatat: a path, or - with an empty path - the descriptor itself. That
 * second form is how fstat() and fstatat(fd, "", ...) are answered on this ABI,
 * and libc's own fstat goes through it. */
static long linux_newfstatat(int dirfd, uint64_t path_u, uint64_t st_u, uint64_t flags) {
  char path[RFILE_NAME];
  if (user_string(path, path_u, RFILE_NAME) < 0) return -EFAULT;
  (void)flags;
  if (!path[0]) return stat_by_fd(dirfd, st_u);
  if (path[0] != '/' && dirfd != AT_FDCWD_L && dirfd >= 0) {
    /* Relative to a descriptor: we have no directory-descriptor model, so say
     * so instead of quietly resolving against the root and lying. */
    return -EOPNOTSUPP;
  }
  return stat_by_path(path_u, st_u);
}

/* ------------------------------------------------------ signals (13, 14, 34) */

/* A libc installs handlers the moment it starts and asks for its mask back. None
 * of it can ever fire: the kernel raises no signals, and a fault kills the task
 * rather than calling into it. Storing the request and answering it truthfully
 * is what keeps the startup path from failing - "remembered but never called" is
 * a smaller lie than -ENOSYS, and the gap is written down rather than guessed at.
 * SIGKILL and SIGSTOP cannot be blocked on Linux; neither can they here, since
 * the mask is not consulted. */
static long linux_rt_sigaction(int sig, uint64_t act_u, uint64_t old_u, uint64_t sigsetsize) {
  if (sig <= 0 || sig > 31 || sigsetsize != 8) return -EINVAL;
  task_t *me = task_at(task_current());
  mm_t *mm = me ? me->mm : 0;
  if (!mm) return -EFAULT;
  if (old_u) {
    uint64_t old[4] = { mm->sig_act[sig - 1], 0, 0, 0 };
    if (user_put(old_u, old, sizeof old) != 0) return -EFAULT;
  }
  if (act_u) {
    uint64_t act[4];                       /* handler, flags, restorer, mask */
    if (user_copy((char *)act, act_u, sizeof act) != 0) return -EFAULT;
    mm->sig_act[sig - 1] = act[0];
  }
  return 0;
}

static long linux_rt_sigprocmask(int how, uint64_t set_u, uint64_t old_u, uint64_t sigsetsize) {
  task_t *t = task_at(task_current());
  if (!t || sigsetsize != 8) return -EINVAL;
  uint64_t set = 0;
  if (set_u && user_copy((char *)&set, set_u, sizeof set) != 0) return -EFAULT;
  uint64_t old = t->sig_blocked;
  switch (how) {
    case 0: t->sig_blocked |= set; break;               /* SIG_BLOCK */
    case 1: t->sig_blocked &= ~set; break;              /* SIG_UNBLOCK */
    case 2: t->sig_blocked = set; break;                /* SIG_SETMASK */
    default: return -EINVAL;
  }
  if (old_u && user_put(old_u, &old, sizeof old) != 0) return -EFAULT;
  return 0;
}

/* ------------------------------------------------------------------ fcntl (72) */

static long linux_fcntl(int fd, int cmd, uint64_t arg) {
  fdtable_t *fds = current_fds();
  if (!fds) return -EBADF;
  ofile_t *o = fd_ofile(fds, fd);
  if (!o) return -EBADF;
  switch (cmd) {
    case 0:  return -EINVAL;        /* F_DUPFD: no dup yet, say so */
    case 1:  return 0;              /* F_GETFD: nothing is close-on-exec */
    case 2:  return 0;              /* F_SETFD: accepted, there is no exec */
    case 3:  return o->flags;       /* F_GETFL */
    case 4:  o->flags = (int)arg;   /* F_SETFL */
             return 0;
    default: return -EINVAL;
  }
}

/* Copy a NUL-terminated string out of the caller's space.
 *
 * user_copy's window check is not a presence check, and an argument block that
 * crosses a page boundary is the normal case rather than the exotic one, so each
 * page is faulted in as the walk reaches it. A string that does not terminate
 * inside the budget is refused: guessing past a boundary the caller never wrote
 * is how a kernel ends up reading whatever comes next.
 */
static int user_str(char *dst, uint64_t max, uint64_t uptr) {
  if (uptr < RING3_USER_MIN || uptr >= RING3_LIMIT) return -1;
  for (uint64_t i = 0; i < max; i++) {
    uint64_t at = uptr + i;
    /* The budget is not the string. A string that starts near the top of the
     * stack has an address space that ends before its budget does, so the window
     * is checked per byte: refusing the whole read because the search *could*
     * have run off the end refuses the common case to protect against the rare
     * one. */
    if (at < RING3_USER_MIN || at >= RING3_LIMIT) return -1;
    if (!user_page_present(at & ~0xFFFULL) && !vm_fault_in(at, 0)) return -1;
    dst[i] = *(const char *)(uintptr_t)at;
    if (!dst[i]) return 0;
  }
  return -1;
}

/* execve replaces the calling process: image, stack, areas, page-table root -
 * everything but the descriptor table and the pid.
 *
 * The old space cannot be freed here. This code is running on that space's kernel
 * stack and its tables are the live ones in CR3, so it goes on the zombie list
 * and the scheduler frees it once CR3 has moved, exactly as it does for a process
 * that exits. That ordering is also what makes the case that matters correct: a
 * child of fork that execs must leave its parent's pages alone, and it does,
 * because releasing a shared frame only decrements its count.
 *
 * envp is accepted and ignored. Nothing in this system has an environment, and
 * inventing one for execve alone would make the two loading paths disagree about
 * what a program can expect to find.
 */
static long linux_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp) {
  char path[128];
  static char argb[ELF_ARG_BLK];
  const char *argv[ELF_ARG_MAX + 1];
  int argc = 0;
  (void)uenvp;

  if (user_str(path, sizeof path, upath) != 0) {
    serial_puts("[exec] path could not be read out of user space\n");
    return -EFAULT;
  }

  uint64_t used = 0;
  if (uargv) {
    for (; argc < ELF_ARG_MAX; argc++) {
      uint64_t p = 0;
      if (user_copy((char *)&p, uargv + (uint64_t)argc * 8, 8) != 0) {
        serial_puts("[exec] ");
        serial_puts(path);
        serial_puts(": argv pointer unreadable\n");
        return -EFAULT;
      }
      if (!p) break;
      if (used >= ELF_ARG_BLK) {
        serial_puts("[exec] ");
        serial_puts(path);
        serial_puts(": argv larger than the block\n");
        return -E2BIG;
      }
      argv[argc] = &argb[used];
      if (user_str(&argb[used], ELF_ARG_BLK - used, p) != 0) {
        serial_puts("[exec] ");
        serial_puts(path);
        serial_puts(": an argv string is unreadable or unterminated\n");
        return -EFAULT;
      }
      while (used < ELF_ARG_BLK && argb[used]) used++;
      used++;                                  /* the terminator goes with it */
    }
  }
  argv[argc] = 0;

  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  mm_t *old = t->mm;
  /* Another thread in this space would keep running the image that is about to be
   * replaced. Linux ends those threads; this refuses the call rather than leaving
   * them to execute into an unmapped space. */
  if (old->refcount > 1) {
    serial_puts("[exec] refusing: the address space is shared with a thread\n");
    return -EINVAL;
  }

  uint64_t entry = 0, sp = 0;
  mm_t *fresh = 0;
  long rc = elf_prepare_image(path, argc, argv, &entry, &fresh, &sp);
  if (rc != 0) {
    serial_puts("[exec] ");
    serial_puts(path);
    serial_puts(": the image could not be prepared, rc=");
    serial_puthex((uint64_t)rc);
    serial_puts("\n");
    return rc;
  }

  /* Descriptors belong to the process, so they travel: the new space arrives with
   * an empty table of its own and the real one moves across. */
  fdtable_free(fresh->fds);
  fresh->fds = old->fds;
  old->fds = 0;

  t->mm = fresh;
  t->cr3 = fresh->cr3;
  ring3_write_cr3(fresh->cr3);       /* the old tables must stop being the live ones */

  /* Now the old space is nobody's. mm_on_exit() queues it for the scheduler to
   * reap, which is only safe because CR3 already points at the new tables. */
  t->mm = old;
  mm_on_exit(t);
  t->mm = fresh;
  t->cr3 = fresh->cr3;

  /* execve does not return: the frame this syscall was entered through is
   * rewritten to land at the new entry on the new stack. Fourteen saved
   * registers come first (f[13] is rax, which the dispatcher overwrites with 0),
   * then ss, rsp, rflags, cs, rip as r3_dispatch_tail built them. */
  uint64_t *f = (uint64_t *)(uintptr_t)r3_frame_base;
  for (int i = 0; i < 14; i++) f[i] = 0;
  f[14] = entry;
  f[17] = sp;

  serial_puts("[exec] ");
  serial_puts(path);
  serial_puts(" entry=");
  serial_puthex(entry);
  serial_puts(" sp=");
  serial_puthex(sp);
  serial_puts(" argc=");
  serial_puthex((uint64_t)argc);
  serial_puts("\n");
  return 0;
}

static long linux_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  fdtable_t *fds;
  switch (num) {
    case 0:   return sys_read(a0, a1, a2);                 /* read */
    case 1:   return sys_write(a0, a1, a2);                /* write */
    case 2:   return sys_open(a0, a1);                     /* open (mode ignored) */
    case 3:   fds = current_fds();                         /* close */
              return fds ? fd_close(fds, (int)a0) : -EBADF;
    case 4:   return stat_by_path(a0, a1);                 /* stat */
    case 5:   return stat_by_fd((int)a0, a1);              /* fstat */
    case 6:   return stat_by_path(a0, a1);                 /* lstat (no symlinks) */
    case 72:  return linux_fcntl((int)a0, (int)a1, a2);    /* fcntl */
    case 8:   fds = current_fds();                         /* lseek */
              return fds ? fd_seek(fds, (int)a0, (int64_t)a1, (int)a2) : -EBADF;
    case 9: { long r = linux_mmap(a0, a1, a2, a3, (int)r3_arg4, r3_arg5);
              if (r3_dyn_trace && task_current() == r3_dyn_trace_pid) {
                serial_puts("[dyn] mmap addr="); serial_puthex(a0);
                serial_puts(" len="); serial_puthex(a1);
                serial_puts(" prot="); serial_puthex(a2);
                serial_puts(" flags="); serial_puthex(a3);
                serial_puts(" fd="); serial_puthex(r3_arg4);
                serial_puts(" off="); serial_puthex(r3_arg5);
                serial_puts(" -> "); serial_puthex((uint64_t)r);
                serial_puts("\n");
              }
              return r; }                                              /* mmap */
    case 10: { long r = linux_mprotect(a0, a1, a2);
               dynmap("mprotect", a0, a1, a2, 0, r); return r; }        /* mprotect */
    case 11: { long r = sys_vm_free(a0, a1);
               dynmap("munmap", a0, a1, 0, 0, r); return r; }           /* munmap */
    case 12:  return linux_brk(a0);                        /* brk */
    case 13:  return linux_rt_sigaction((int)a0, a1, a2, a3); /* rt_sigaction */
    case 14:  return linux_rt_sigprocmask((int)a0, a1, a2, a3); /* rt_sigprocmask */
    case 16:  return linux_ioctl(a1, a2);                  /* ioctl */
    case 34:  return -EINTR;                               /* pause: nothing can
                                                            * arrive, so nothing
                                                            * can interrupt it */
    case 20:  return linux_writev(a0, a1, a2);             /* writev */
    case 22:  return sys_pipe(a0);                         /* pipe */
    case 56:  return sys_clone(a0, a1, a2, a3);             /* clone / fork */
    case 57:  return sys_fork();                            /* fork */
    case 59:  return linux_execve(a0, a1, a2);              /* execve */
    case 35:  return linux_nanosleep(a0, a1);               /* nanosleep */
    case 77:  return linux_ftruncate(a0, a1);               /* ftruncate */
    case 319: return linux_memfd_create(a0, a1);            /* memfd_create */
    case 318: return linux_getrandom(a0, a1, a2);            /* getrandom */
    case 41:  return linux_socket(a0, a1, a2);               /* socket */
    case 42:  return linux_connect(a0, a1, a2);              /* connect */
    case 44:  return linux_sendto(a0, a1, a2, a3, r3_arg4, r3_arg5); /* sendto */
    case 45:  return linux_recvfrom(a0, a1, a2, a3, r3_arg4, r3_arg5); /* recvfrom */
    case 46:  return linux_sendmsg(a0, a1, a2);              /* sendmsg */
    case 47:  return linux_recvmsg(a0, a1, a2);              /* recvmsg */
    case 48: {                                               /* shutdown */
      fdtable_t *f = current_fds();
      if (!f) return -EBADF;
      return fd_shutdown(f, (int)a0, (int)a1);
    }
    case 49: case 50: return -ENOSYS;                        /* bind, listen: no listener */
    case 51: return -ENOSYS;                                 /* getsockname: no local address */
    case 54: return linux_setsockopt(a0, a1, a2, a3, r3_arg4); /* setsockopt */
    case 55: return linux_getsockopt(a0, a1, a2, a3, r3_arg4); /* getsockopt */
    case 230: return linux_clock_nanosleep(a0, a1, a2, a3); /* clock_nanosleep */
    case 232: return sys_epoll_wait(a0, a1, a2, a3);        /* epoll_wait */
    case 233: return sys_epoll_ctl(a0, a1, a2, a3);         /* epoll_ctl */
    case 291: return sys_epoll_create1(a0);                 /* epoll_create1 */
    case 61:  return linux_wait4((int)a0, a1, (int)a2, a3); /* wait4 */
    case 39:  return (long)task_current();                 /* getpid */
    case 60:  r3_exit_now(a0); return 0;                   /* exit */
    case 96: {                                             /* gettimeofday */
      /* Wall-clock time, not uptime: a program that asks what time it is wants
       * the date, and only one of the two clocks here can answer that. */
      uint64_t secs = 0; uint32_t ns = 0;
      walltime_now(&secs, &ns);
      int64_t tv[2];
      tv[0] = (int64_t)secs;
      tv[1] = (int64_t)(ns / 1000);
      if (user_put(a0, tv, sizeof tv) != 0) return -EFAULT;
      return 0;
    }
    case 102: case 104: case 107: case 108: return 0;      /* getuid/gid/euid/egid */
    case 110: return 0;                                    /* getppid */
    case 158: return linux_arch_prctl(a0, a1);             /* arch_prctl */
    case 186: return (long)task_current();                 /* gettid */
    case 202: return linux_futex(a0, a1, a2, a3);          /* futex */
    case 218: return (long)task_current();                 /* set_tid_address */
    case 228: return linux_clock_gettime(a0, a1);          /* clock_gettime */
    case 262: return linux_newfstatat((int)a0, a1, a2, a3);/* newfstatat */
    case 231: r3_exit_now(a0); return 0;                   /* exit_group */
    default:  linux_unknown(num); return -ENOSYS;
  }
}

/* The number and arguments of the syscall in flight: without them a kernel fault
 * is a rip and a cr2 with no story. */
uint64_t r3_cur_syscall, r3_cur_arg0;

long syscall_dispatch(uint64_t abi, uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  if (abi == ABI_LINUX) {
    r3_cur_syscall = ~num;      /* distinguishable from our own numbers in a crash report */
    r3_cur_arg0 = a0;
    return linux_dispatch(num, a0, a1, a2, a3);
  }
  r3_cur_syscall = num;
  r3_cur_arg0 = a0;
  static int first = 1;
  if (first) {
    first = 0;
    serial_puts("[ring3] first syscall: cs=");
    serial_puthex(r3_last_cs);
    serial_puts(" rip=");
    serial_puthex(r3_last_rip);
    serial_puts((r3_last_cs & 3) == 3 ? "  (cpl 3)\n" : "  (NOT cpl 3!)\n");
  }

  switch (num) {
    case SYS_WRITE:  return sys_write(a0, a1, a2);
    case SYS_EXIT:
      serial_puts("[ring3] task ");
      serial_puts(task_name(task_current()));
      serial_puts(" exited with code ");
      serial_puthex(a0);
      serial_puts("\n");
      task_kill_current(a0);          /* does not return */
      return 0;
    case SYS_YIELD:  task_yield(); return 0;
    case SYS_TICKS:  return (long)task_total_ticks();
    case SYS_GETPID: return (long)task_current();

    /* ---- the compositor as a server ---------------------------------- */
    case SYS_SURF_CREATE: {
      uint64_t r = surf_create((int)a0, (int)a1, (int)a2);
      return (long)r;
    }
    case SYS_SURF_PRESENT: {
      int x = (int)((a1 >> 16) & 0xFFFF), y = (int)(a1 & 0xFFFF);
      int w = (int)((a2 >> 16) & 0xFFFF), h = (int)(a2 & 0xFFFF);
      return surf_present((int)a0, x, y, w, h);
    }
    case SYS_WIN_OPEN: {
      char title[32];
      if (user_copy(title, a0, 31) != 0) return -EFAULT;
      title[31] = 0;
      for (int i = 0; i < 31; i++) if (!title[i]) break;   /* keep it short */
      int wid = winipc_open(title, (int32_t)a1, (int32_t)a2);
      return wid < 0 ? -ENOSPC : (long)wid;
    }
    case SYS_WIN_CMD: {
      win_cmd_u cmd;
      if (a1 < RING3_USER_MIN || a1 + sizeof(cmd) > RING3_LIMIT) return -EFAULT;
      const uint8_t *src = (const uint8_t *)(uintptr_t)a1;
      for (uint64_t i = 0; i < sizeof(cmd); i++) ((uint8_t *)&cmd)[i] = src[i];

      /* Text is a user pointer inside the command: copy it here, so the
       * compositor never dereferences user memory. */
      char text[WINIPC_TEXT_MAX];
      text[0] = 0;
      if (cmd.kind == WCMD_TEXT || cmd.kind == WCMD_TEXT_LARGE) {
        if (cmd.text < RING3_USER_MIN || cmd.text >= RING3_LIMIT) return -EFAULT;
        if (user_copy(text, cmd.text, WINIPC_TEXT_MAX - 1) != 0) return -EFAULT;
        text[WINIPC_TEXT_MAX - 1] = 0;
        for (int i = 0; i < WINIPC_TEXT_MAX; i++) if (!text[i]) break;
      }
      int r = winipc_add((int)a0, &cmd, text);
      return r < 0 ? -EBADF : r;
    }
    case SYS_WIN_CLEAR: {
      int r = winipc_clear((int)a0, (uint32_t)a1);
      return r < 0 ? -EBADF : r;
    }
    case SYS_WIN_WAIT: {
      win_event_t ev;
      for (;;) {
        int r = winipc_poll((int)a0, &ev);
        if (r == 1) {
          if (user_put(a1, &ev, sizeof ev) != 0) return -EFAULT;
          return 1;
        }
        if (r < 0) return -EBADF;          /* window gone: nothing to wait for */
        task_block_current();              /* sleeps until an event is queued */
      }
    }
    case SYS_WIN_RECT: {
      int wh = (int)(uint32_t)a3;
      int r = winipc_set_rect((int)a0, (int32_t)a1, (int32_t)(a1 >> 32),
                              wh >> 16, wh & 0xFFFF);
      return r < 0 ? -EBADF : r;
    }
    case SYS_WIN_GEOM: {
      int32_t g[4];
      if (winipc_get_rect((int)a0, &g[0], &g[1], &g[2], &g[3]) != 0) return -EBADF;
      if (user_put(a1, g, sizeof g) != 0) return -EFAULT;
      return 0;
    }
    case SYS_CLIP_SET: {
      uint64_t n = a1;
      if (n > sizeof clip_buf) n = sizeof clip_buf;
      if (user_copy(clip_buf, a0, n) != 0) return -EFAULT;
      clip_len = (int)n;
      return (long)n;
    }
    case SYS_CLIP_GET: {
      if (clip_len <= 0) return 0;
      uint64_t n = a1;
      if (n > (uint64_t)clip_len) n = (uint64_t)clip_len;
      if (user_put(a0, clip_buf, n) != 0) return -EFAULT;
      return (long)n;
    }
    case SYS_THREAD_CREATE: return sys_thread_create(a0, a1, a2, a3);
    case SYS_FUTEX:         return sys_futex(a0, a1, a2, a3);
    case SYS_OPEN:          return sys_open(a0, a1);
    case SYS_READ:          return sys_read(a0, a1, a2);
    case SYS_CLOSE: {
      fdtable_t *fds = current_fds();
      return fds ? fd_close(fds, (int)a0) : -EBADF;
    }
    case SYS_DUP: {
      fdtable_t *fds = current_fds();
      return fds ? fd_dup(fds, (int)a0) : -EBADF;
    }
    case SYS_SOCKET:       return sys_socket(a0, a1);
    case SYS_CONNECT:      return sys_connect(a0, a1, a2);
    case SYS_SEND:         return sys_send(a0, a1, a2);
    case SYS_RECV:         return sys_recv(a0, a1, a2);
    case SYS_RESOLVE:      return sys_resolve(a0, a1);
    case SYS_POLL:         return sys_poll(a0, a1, a2);
    case SYS_PIPE:         return sys_pipe(a0);
    case SYS_LSEEK: {
      fdtable_t *fds = current_fds();
      return fds ? fd_seek(fds, (int)a0, (int64_t)a1, (int)a2) : -EBADF;
    }
    case SYS_VM_ALLOC:   return sys_vm_alloc(a0, a1);
    case SYS_VM_FREE:    return sys_vm_free(a0, a1);
    case SYS_SHM_GET: {
      int r = shm_get((int)a0, (int)a1);
      return r < 0 ? -ENOMEM : (long)r;
    }
    case SYS_SHM_ATTACH: {
      /* uint64_t, not int: truncating the address here handed every process
       * 0x80000 instead of 0x8000080000 and it faulted on first touch. */
      uint64_t addr = shm_attach((int)a0);
      return addr ? (long)addr : -EINVAL;
    }
    case SYS_WIN_POLL: {
      win_event_t ev;
      int r = winipc_poll((int)a0, &ev);
      if (r == 1 && user_put(a1, &ev, sizeof ev) != 0) return -EFAULT;
      return r < 0 ? -EBADF : (long)r;   /* EBADF = the window is gone */
    }
    case SYS_WIN_PRESENT: winipc_present((int)a0); return 0;
    case SYS_WIN_CLOSE:   winipc_close((int)a0); return 0;
  }
  serial_puts("[ring3] unknown syscall ");
  serial_puthex(num);
  serial_puts("\n");
  return -ENOSYS;
}

/* The stub. Entered by `int 0x80` from CPL 3, on the TSS rsp0 stack, with the
 * hardware frame at rsp: rip, cs, rflags, rsp, ss.
 *
 * Every general register is saved and restored: the user program's inline asm
 * declares only rax as an output, so anything else we clobber silently corrupts
 * its state (a stale rdi once made the second write() go to a garbage fd). Only
 * rax changes, and it carries the result back. */
__asm__(
".text\n"
".global r3_syscall_stub\n"
".type r3_syscall_stub, @function\n"
"r3_syscall_stub:\n"
"  pushq %rax\n"
"  pushq %rbx\n"
"  pushq %rcx\n"
"  pushq %rdx\n"
"  pushq %rsi\n"
"  pushq %rdi\n"
"  pushq %r8\n"
"  pushq %r9\n"
"  pushq %r10\n"
"  pushq %r11\n"
"  pushq %r12\n"
"  pushq %r13\n"
"  pushq %r14\n"
"  pushq %r15\n"
/* The hardware frame now sits above our 14 saved registers (112 bytes), so
 * read rip/cs from it instead of borrowing a register first: clobbering r11
 * before it was pushed handed the user a kernel pointer back. */
"  movq 120(%rsp), %rax\n"        /* cs of the interrupted context */
"  movq %rax, r3_last_cs(%rip)\n"
"  movq 112(%rsp), %rax\n"        /* rip */
"  movq %rax, r3_last_rip(%rip)\n"
"  sti\n"                          /* syscalls run with interrupts on */
"  movq task_fpu_slot(%rip), %rax\n"   /* the kernel uses xmm freely: save the */
"  fxsave (%rax)\n"                    /* caller's state before the handler runs */
"  xorl %edi, %edi\n"   /* abi = OMI: int $0x80 numbers */
"  movq 104(%rsp), %rsi\n"   /* number (saved rax) */
"  movq 64(%rsp), %rdx\n"   /* a0 (saved rdi) */
"  movq 72(%rsp), %rcx\n"   /* a1 (saved rsi) */
"  movq 80(%rsp), %r8\n"   /* a2 (saved rdx) */
"  movq 56(%rsp), %r9\n"   /* a3 (saved r8) */
"  jmp r3_dispatch_tail\n"
".size r3_syscall_stub, .-r3_syscall_stub\n"
"\n"
/* ---------------------------------------------------------------------------
 * The SYSCALL entry. This instruction does not touch rsp - the user stack is
 * still live when control arrives - so the first thing here has to be a stack
 * switch, and doing it without a scratch register means two rip-relative
 * accesses. ia32_sfmask cleared IF on entry and the kernel never preempts CPL 0,
 * so nothing can run between them on this uniprocessor.
 *
 * Then the frame is rebuilt in the shape int $0x80 would have left (ss, rsp,
 * rflags, cs, rip) so one return path serves both doors. rcx and r11 are already
 * destroyed by the instruction itself - SYSCALL puts the return rip in rcx and
 * the flags in r11 - so saving them costs nothing and the ABI does not promise
 * them back.
 * --------------------------------------------------------------------------- */
".global r3_linux_stub\n"
".type r3_linux_stub, @function\n"
"r3_linux_stub:\n"
"  movq %rsp, r3_user_rsp(%rip)\n"
"  movq gdt_syscall_rsp(%rip), %rsp\n"
"  pushq $0x1b\n"   /* ss: user data | 3 */
"  pushq r3_user_rsp(%rip)\n"
"  pushq %r11\n"   /* rflags, as SYSCALL captured it */
"  pushq $0x23\n"   /* cs: user code | 3 */
"  pushq %rcx\n"   /* return rip, as SYSCALL captured it */
"  pushq %rax\n"
"  pushq %rbx\n"
"  pushq %rcx\n"
"  pushq %rdx\n"
"  pushq %rsi\n"
"  pushq %rdi\n"
"  pushq %r8\n"
"  pushq %r9\n"
"  pushq %r10\n"
"  pushq %r11\n"
"  pushq %r12\n"
"  pushq %r13\n"
"  pushq %r14\n"
"  pushq %r15\n"
"  movq 120(%rsp), %rax\n"
"  movq %rax, r3_last_cs(%rip)\n"
"  movq 112(%rsp), %rax\n"
"  movq %rax, r3_last_rip(%rip)\n"
"  sti\n"
"  movq task_fpu_slot(%rip), %rax\n"
"  fxsave (%rax)\n"
"  movq $1, %rdi\n"   /* abi = LINUX */
"  movq 104(%rsp), %rsi\n"   /* number (user rax) */
"  movq 64(%rsp), %rdx\n"   /* a0 = rdi */
"  movq 72(%rsp), %rcx\n"   /* a1 = rsi */
"  movq 80(%rsp), %r8\n"   /* a2 = rdx */
"  movq 40(%rsp), %r9\n"   /* a3 = r10, where the Linux ABI puts the 4th */
"  movq 56(%rsp), %rax\n"   /* the 5th and 6th, for the rare syscall that has */
"  movq %rax, r3_arg4(%rip)\n"   /* them: clone's tls arrives in r8 */
"  movq 48(%rsp), %rax\n"
"  movq %rax, r3_arg5(%rip)\n"
"  jmp r3_dispatch_tail\n"
".size r3_linux_stub, .-r3_linux_stub\n"
"\n"
/* One dispatch and one return, shared by both doors. */
"r3_dispatch_tail:\n"
"  movq %rsp, %rbx\n"
"  movq %rbx, r3_frame_base(%rip)\n"
"  andq $-16, %rsp\n"   /* keep the C ABI happy */
"  call syscall_dispatch\n"
"  movq %rbx, %rsp\n"
"  movq %rax, 104(%rsp)\n"   /* result becomes the user's rax */
"  movq task_fpu_slot(%rip), %rax\n"
"  fxrstor (%rax)\n"
"  popq %r15\n"
"  popq %r14\n"
"  popq %r13\n"
"  popq %r12\n"
"  popq %r11\n"
"  popq %r10\n"
"  popq %r9\n"
"  popq %r8\n"
"  popq %rdi\n"
"  popq %rsi\n"
"  popq %rdx\n"
"  popq %rcx\n"
"  popq %rbx\n"
"  popq %rax\n"
"  iretq\n"
);

/* #PF and #GP both push an error code, so the hardware frame is
 *   [0] error  [1] rip  [2] cs  [3] rflags  [4] rsp  [5] ss
 *
 * All fifteen general registers are saved around the handler: a fault that is
 * *handled* (demand paging) returns to the faulting instruction, and C code in
 * between would otherwise leave the process with garbage in every caller-saved
 * register. Same story for xmm, so the FPU state is saved around the call too. */
__asm__(
".text\n"
".global r3_isr_fault\n"
".type r3_isr_fault, @function\n"
"r3_isr_fault:\n"
"  pushq %rax\n"
"  pushq %rbx\n"
"  pushq %rcx\n"
"  pushq %rdx\n"
"  pushq %rsi\n"
"  pushq %rdi\n"
"  pushq %rbp\n"
"  pushq %r8\n"
"  pushq %r9\n"
"  pushq %r10\n"
"  pushq %r11\n"
"  pushq %r12\n"
"  pushq %r13\n"
"  pushq %r14\n"
"  pushq %r15\n"
"  movq task_fpu_slot(%rip), %rax\n"
"  fxsave (%rax)\n"
"  leaq 120(%rsp), %rdi\n"        /* 15 saved registers, then the frame */
"  call ring3_fault\n"
"  movq task_fpu_slot(%rip), %rax\n"
"  fxrstor (%rax)\n"
"  popq %r15\n"
"  popq %r14\n"
"  popq %r13\n"
"  popq %r12\n"
"  popq %r11\n"
"  popq %r10\n"
"  popq %r9\n"
"  popq %r8\n"
"  popq %rbp\n"
"  popq %rdi\n"
"  popq %rsi\n"
"  popq %rdx\n"
"  popq %rcx\n"
"  popq %rbx\n"
"  popq %rax\n"
"  addq $8, %rsp\n"                /* drop the error code */
"  iretq\n"
".size r3_isr_fault, .-r3_isr_fault\n"
);

extern void *task_fpu_slot;        /* set by the scheduler on every switch */
extern void r3_syscall_stub(void);
extern void r3_isr_fault(void);

void ring3_fault(uint64_t *f) {
  uint64_t err = f[0], rip = f[1], cs = f[2];
  uint64_t cr2 = 0;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

  serial_puts("[ring3] fault: rip=");
  serial_puthex(rip);
  serial_puts(" cs=");
  serial_puthex(cs);
  serial_puts(" err=");
  serial_puthex(err);
  serial_puts(" cr2=");
  serial_puthex(cr2);
  serial_puts(" rsp=");
  serial_puthex(f[4]);

  serial_puts(" fs=");
  {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100u));
    serial_puthex(((uint64_t)hi << 32) | lo);
  }
  serial_puts("\n");

  if ((cs & 3) == 3) {
    /* Not-present faults inside a SYS_VM_ALLOC region are demand paging, not a
     * violation: map the page and let the instruction retry. */
    /* A *write* to a page that is present but read-only, in a space that shares
     * it, is the other half of fork: the copy happens now and the instruction
     * retries. Checked before the not-present case because the page is there -
     * what it lacks is permission, and permission is ours to grant. */
    if ((err & 1) && (err & 2) && cow_fault(cr2 & ~0xFFFULL)) return;

    if (!(err & 1) && vm_fault_in(cr2, err)) {
      static int shown;
      if (shown < 4) {                 /* a few, then stay quiet */
        shown++;
        serial_puts("[vm] page faulted in at ");
        serial_puthex(cr2);
        serial_puts("\n");
      }
      return;
    }

    /* A user task stepped outside its window. Kill that task only: the
     * compositor and the other tasks keep running. */
    serial_puts("[ring3] killed user task ");
    serial_puts(task_name(task_current()));
    serial_puts(" (isolation held)\n");
    /* no exit code: this task did not leave through sys_exit */
    task_kill_current((uint64_t)-1);
  }

  serial_puts("[ring3] FATAL: fault in kernel mode, task=");
  serial_puts(task_name(task_current()));
  serial_puts(" syscall=");
  serial_puthex(r3_cur_syscall);
  serial_puts(" arg0=");
  serial_puthex(r3_cur_arg0);
  serial_puts(" cr3=");
  serial_puthex(ring3_read_cr3());
  {
    task_t *ct = task_at(task_current());
    serial_puts(" want=");
    serial_puthex(ct ? ct->cr3 : 0);
  }
  serial_puts("\n");
  for (;;) __asm__ volatile("cli; hlt");
}

/* ------------------------------------------------------- isolation test */

/* The four physical addresses that map `va` in a space (PML4, PDPT, PD, PT),
 * zero where a level is absent. Reading the tables directly is the only way to
 * tell "two processes can't see each other" from "two processes happen to be
 * running the same image at the same addresses", which is what hid the aliasing
 * bug for weeks. */
static void space_tables(uint64_t cr3, uint64_t va, uint64_t out[4]) {
  out[0] = cr3; out[1] = 0; out[2] = 0; out[3] = 0;
  uint64_t *pml4 = (uint64_t *)(hhdm_offset + cr3);
  if (!(pml4[1] & PTE_P)) return;
  out[1] = pml4[1] & ~0xFFFULL;
  uint64_t *pdpt = (uint64_t *)(hhdm_offset + out[1]);
  int i2 = (int)((va >> 30) & 511);
  if (!(pdpt[i2] & PTE_P)) return;
  out[2] = pdpt[i2] & ~0xFFFULL;
  uint64_t *pd = (uint64_t *)(hhdm_offset + out[2]);
  int i3 = (int)((va >> 21) & 511);
  if (!(pd[i3] & PTE_P)) return;
  out[3] = pd[i3] & ~0xFFFULL;
}

static void iso_report(const char *what, int ok) {
  serial_puts(ok ? "[iso] ok   " : "[iso] FAIL ");
  serial_puts(what);
  serial_puts("\n");
}

/* Boot-time regression test for process isolation. The bug this guards against
 * was invisible to every behavioural test Omi had: ring3_new_space() copied all
 * 512 PML4 entries, slot 1 included, so processes had distinct CR3s pointing at
 * one shared PDPT/PD/PT. Two processes writing "their own" page at the same
 * address each read back their own bytes - because the second loader had simply
 * overwritten the shared PTE - so the demo passed. Checking the tables is what
 * distinguishes the two worlds. */
static void iso_selftest(void) {
  mm_t *a = ring3_new_mm();
  mm_t *b = ring3_new_mm();
  if (!a || !b) { serial_puts("[iso] FAIL no address space available\n"); return; }

  uint64_t *pa4 = (uint64_t *)(hhdm_offset + a->cr3);
  uint64_t *pb4 = (uint64_t *)(hhdm_offset + b->cr3);
  iso_report("fresh space starts with an empty user window",
             a->cr3 != b->cr3 && pa4[1] == 0 && pb4[1] == 0);

  uint64_t va = RING3_MMAP_BASE;
  void *ka = 0, *kb = 0;
  int mapped = ring3_map(a->cr3, va, PTE_P | PTE_W | PTE_U, &ka) == 0 &&
               ring3_map(b->cr3, va, PTE_P | PTE_W | PTE_U, &kb) == 0;
  iso_report("the same virtual address maps in both spaces", mapped);

  if (mapped) {
    uint64_t ta[4], tb[4];
    space_tables(a->cr3, va, ta);
    space_tables(b->cr3, va, tb);
    int shared = 0;
    for (int i = 1; i < 4; i++)
      for (int j = 1; j < 4; j++)
        if (ta[i] && ta[i] == tb[j]) shared = 1;
    iso_report("no page-table level is shared between them", !shared);
    iso_report("the two addresses are backed by different frames", ka != kb);

    uint8_t *x = (uint8_t *)ka, *y = (uint8_t *)kb;
    for (int i = 0; i < 4096; i++) { x[i] = 0xAA; y[i] = 0x55; }
    int clean = 1;
    for (int i = 0; i < 4096; i++) if (x[i] != 0xAA || y[i] != 0x55) clean = 0;
    iso_report("a write in one space is invisible in the other", clean);
  }

  fdtable_free(a->fds); mm_free_tables(a); kfree(a);
  fdtable_free(b->fds); mm_free_tables(b); kfree(b);
}

/* The isolation test above maps one page per region, so it never notices how a
 * page is found *inside* a region. This one writes to three pages of one 2 MB
 * region - first, middle, last - because walk_user() hands back the page table
 * and the caller applies the last-level index. Reading entry 0 instead of the
 * requested entry makes every page in a region look like whatever page 0 is:
 * absent here, so all three writes fail and the bug is loud instead of quiet. */
static void pt_selftest(void) {
  mm_t *m = ring3_new_mm();
  if (!m) { serial_puts("[pt] FAIL no address space available\n"); return; }
  if (ring3_make_stack(m->cr3) != 0) { serial_puts("[pt] FAIL no stack pages\n"); return; }

  uint64_t top = RING3_LIMIT;
  uint64_t va[3] = { top - 8 * 0x1000, top - 4 * 0x1000, top - 0x1000 };
  static const char *labels[3] = {
    "a write reaches the first page of a 2 MB region",
    "a write reaches a middle page of the same region",
    "a write reaches the last page of the same region",
  };

  for (int i = 0; i < 3; i++) {
    char want[8], got[8];
    for (int k = 0; k < 8; k++) want[k] = (char)(0xA0 + i * 0x10 + k);
    int ok = ring3_copy_to_mm(m->cr3, va[i], want, 8) == 0;
    if (ok) {
      uint64_t save = ring3_read_cr3();
      ring3_write_cr3(m->cr3);
      const char *p = (const char *)(uintptr_t)va[i];
      for (int k = 0; k < 8; k++) got[k] = p[k];
      ring3_write_cr3(save);
      for (int k = 0; k < 8; k++) if (got[k] != want[k]) ok = 0;
    }
    iso_report(labels[i], ok);
  }

  char b[8] = { 0 };
  iso_report("a write to an unmapped page is refused, not attempted",
             ring3_copy_to_mm(m->cr3, top - 20 * 0x1000, b, 8) != 0);

  fdtable_free(m->fds); mm_free_tables(m); kfree(m);
}

/* ------------------------------------------------------------------ setup */

/* The pump is deliberately small: one frame, then a tick of sleep. It is a
 * service, so it must be cheap enough to run for ever and must never hold the
 * NIC while a connection call wants it (tcp_service handles that with rx_busy). */
static void netpump(void) {
  for (;;) {
    tcp_service();
    task_sleep_ticks(1);
  }
}

void ring3_init(void) {
  kpml4_phys = ring3_read_cr3();
  kpml4_virt = (uint64_t *)(hhdm_offset + kpml4_phys);

  idt_set_gate_raw(0x80, (void *)r3_syscall_stub, 1);   /* DPL 3: callable */
  idt_set_gate_raw(13, (void *)r3_isr_fault, 0);        /* #GP */
  idt_set_gate_raw(14, (void *)r3_isr_fault, 0);        /* #PF */

  rfs_init();                     /* seed /hello.txt and friends */

  /* Every boot module becomes a file at the path Limine was given for it, so a
   * program can open("/lib/libfoo.so") and get the same bytes the kernel loaded.
   * Done after rfs_init, which zeroes the table. Read-only: these are the
   * bootloader's blobs, not storage. */
  {
    const char *mp; void *mptr; uint64_t mlen; int exposed = 0, skipped = 0;
    for (uint64_t i = 0; i < meep_module_count(); i++) {
      if (!meep_module_at(i, &mp, &mptr, &mlen)) continue;
      if (rfs_expose(mp, (uint8_t *)mptr, (uint32_t)mlen) == 0) exposed++;
      else skipped++;
    }
    serial_puts("[rfs] module files exposed=");
    serial_puthex((uint64_t)exposed);
    serial_puts(" skipped=");
    serial_puthex((uint64_t)skipped);
    serial_puts(" total=");
    serial_puthex((uint64_t)rfs_count());
    serial_puts(" rom=");
    serial_puthex((uint64_t)rfs_rom_count());
    serial_puts("\n");
  }
  ready = 1;
  log_hex("[ring3] kernel cr3      ", kpml4_phys);
  log_hex("[ring3] user window     ", RING3_USER_BASE);
  iso_selftest();
  pt_selftest();
  sock_selftest();
  /* Readiness runs here, on the fake backend, before any client exists: the
   * scheduler is up (task_demo_start ran first) but no real traffic is in
   * flight, so a temporary backend swap cannot disturb anything. */
  /* Pipes before the socket suites: the pipe test spawns a helper task, and a
   * spawn is cheaper to diagnose before the readiness suite starts killing its
   * own workers. */
  pipe_selftest();
  linux_gate_init();      /* arm SYSCALL before any program can reach ring 3 */
  sock_readiness_selftest();
  /* The card's failure modes, driven with an injected return value. Runs here
   * because it touches the real e1000_send path and would disturb live traffic
   * later; nothing has been sent yet at this point. */
  tcp_test_unconfirmed();

  /* Independent network progress. A task parked in a socket wait cannot drain
   * the NIC for itself - and with a real sleeping read that stopped being a
   * theoretical concern: a client that slept never saw a response that a client
   * that spun did. One frame per tick, in its own task, so delivery depends on
   * the kernel and not on what ring 3 happens to be doing. */
  task_spawn("netpump", netpump);

  /* Then ask whether that pump can run at all when ring 3 is busy. Separate from
   * every socket test on purpose: a failure here is a scheduler answer. */
  preempt_selftest();
}