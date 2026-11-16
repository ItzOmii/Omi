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
#include "include/task.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/types.h"
#include "include/guiipc.h"
#include "include/pmm.h"
#include "include/errno.h"
#include "include/kmalloc.h"
#include "include/net.h"
#include "include/omi_net.h"
#include "include/socket.h"

/* From the boot path. */
extern uint64_t hhdm_offset;
extern uint64_t kernel_phys_base;
extern uint64_t kernel_virt_base;

/* Early debug console. */
extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t v);

/* Set by the syscall stub: the CS the cpu was in when it took the interrupt. */
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

/* Give every page and every table in the window back to the allocator. Shared
 * segment pages are skipped: they belong to the shm slot, and the other process
 * mapping them is still running. */
static void mm_free_tables(mm_t *mm) {
  uint64_t *pml4 = (uint64_t *)(hhdm_offset + mm->cr3);
  if (pml4[1] & PTE_P) {
    uint64_t *pdpt = (uint64_t *)(hhdm_offset + (pml4[1] & ~0xFFFULL));
    for (int i2 = 0; i2 < 512; i2++) {
      if (!(pdpt[i2] & PTE_P)) continue;
      uint64_t *pd = (uint64_t *)(hhdm_offset + (pdpt[i2] & ~0xFFFULL));
      for (int i3 = 0; i3 < 512; i3++) {
        if (!(pd[i3] & PTE_P)) continue;
        if (pd[i3] & 0x80ULL) {                 /* 2 MB page: unused here */
          pmm_free_page(pd[i3] & ~0x1FFFFFULL);
          continue;
        }
        uint64_t *pt = (uint64_t *)(hhdm_offset + (pd[i3] & ~0xFFFULL));
        for (int i4 = 0; i4 < 512; i4++) {
          if (!(pt[i4] & PTE_P)) continue;
          if (pt[i4] & PTE_SHARED) continue;
          pmm_free_page(pt[i4] & ~0xFFFULL);
        }
        pmm_free_page(pd[i3] & ~0xFFFULL);
      }
      pmm_free_page(pdpt[i2] & ~0xFFFULL);
    }
    pmm_free_page(pml4[1] & ~0xFFFULL);
  }
  pmm_free_page(mm->cr3);
}

/* Address spaces whose last thread has exited, waiting for the CPU to be off
 * them. Freeing a page table while its CR3 is loaded is a page fault waiting to
 * happen, and doing the CR3 dance inside the exit syscall means freeing memory
 * the kernel is still standing on. So the scheduler reaps: it has just loaded
 * the incoming task's CR3, which is exactly the moment the old space is dead. */
static mm_t *zombie[8];
static int   nzombies;

void mm_on_exit(task_t *t) {
  if (!t->mm) return;
  mm_t *mm = t->mm;
  t->mm = 0;
  t->cr3 = 0;
  if (--mm->refcount > 0) return;
  if (nzombies < (int)(sizeof zombie / sizeof zombie[0])) {
    zombie[nzombies++] = mm;
    return;
  }
  int socks = fd_count_kind(mm->fds, FK_SOCKET);
  fdtable_free(mm->fds);
  sock_report_after_space(socks);
  mm_free_tables(mm);          /* table full: nothing sane left to do */
  kfree(mm);
}

/* Called by the scheduler after it has switched CR3. */
void mm_reap(void) {
  if (!nzombies) return;
  uint64_t now = ring3_read_cr3();
  for (int i = 0; i < nzombies; i++) {
    if (zombie[i]->cr3 == now) continue;      /* still the live one */
    int socks = fd_count_kind(zombie[i]->fds, FK_SOCKET);
    fdtable_free(zombie[i]->fds);
    sock_report_after_space(socks);   /* endpoints must not outlive their space */
    mm_free_tables(zombie[i]);
    kfree(zombie[i]);
    serial_puts("[mm] address space freed\n");
    zombie[i] = zombie[--nzombies];
    i--;
  }
}

/* Follow (building as needed) the tables under PML4 slot 1 for `vaddr`.
 * Everything is addressed through the HHDM so a table reached from a PTE and a
 * table returned by alloc_frame() are the same memory. */
static uint64_t *walk_user(uint64_t cr3, uint64_t vaddr) {
  uint64_t *pml4 = (uint64_t *)(hhdm_offset + cr3);
  int i2 = (int)((vaddr >> 30) & 511);
  int i3 = (int)((vaddr >> 21) & 511);

  uint64_t *pdpt;
  if (pml4[1] & PTE_P) {
    pdpt = (uint64_t *)(hhdm_offset + (pml4[1] & ~0xFFFULL));
  } else {
    pdpt = (uint64_t *)alloc_frame();
    if (!pdpt) return 0;
    pml4[1] = to_phys(pdpt) | PTE_P | PTE_W | PTE_U;
  }

  uint64_t *pd;
  if (pdpt[i2] & PTE_P) {
    pd = (uint64_t *)(hhdm_offset + (pdpt[i2] & ~0xFFFULL));
  } else {
    pd = (uint64_t *)alloc_frame();
    if (!pd) return 0;
    pdpt[i2] = to_phys(pd) | PTE_P | PTE_W | PTE_U;
  }

  if (pd[i3] & PTE_P) {
    if (pd[i3] & 0x80ULL) return 0;            /* 2 MB page: not used here */
    return (uint64_t *)(hhdm_offset + (pd[i3] & ~0xFFFULL));
  }
  uint64_t *pt = (uint64_t *)alloc_frame();
  if (!pt) return 0;
  pd[i3] = to_phys(pt) | PTE_P | PTE_W | PTE_U;
  return pt;
}

/* Map one fresh, zeroed page at `vaddr`; optionally hand back its kernel
 * address so the caller can fill it. */
int ring3_map(uint64_t cr3, uint64_t vaddr, uint64_t flags, void **kva) {
  if (vaddr < RING3_USER_BASE || vaddr >= RING3_LIMIT) return -1;
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
  if (vaddr < RING3_USER_BASE || vaddr >= RING3_LIMIT) return -1;
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

/* A stack in the same window, just below the limit. */
/* Copy into a user address space that is not the active one. The pages must
 * already be mapped - this is for laying out argv on a fresh stack, not for
 * demand paging, and a #PF inside the kernel is a panic rather than a fixup. */
int ring3_copy_to_mm(uint64_t cr3, uint64_t vaddr, const void *src, uint64_t n) {
  if (n == 0) return 0;
  if (vaddr < RING3_USER_BASE || vaddr + n > RING3_LIMIT) return -1;
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
static int vm_fault_in(uint64_t addr) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return 0;
  uint64_t page = addr & ~0xFFFULL;
  for (int i = 0; i < TASK_AREAS; i++) {
    vm_area_t *a = &t->mm->areas[i];
    if (!a->used) continue;
    if (page < a->base || page >= a->base + a->len) continue;
    void *kva = 0;
    if (ring3_map(t->cr3, page, a->flags, &kva) != 0) return 0;
    return 1;
  }
  return 0;
}

static long sys_vm_alloc(uint64_t len, uint64_t prot) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  mm_t *mm = t->mm;
  if (len == 0) return -EINVAL;
  len = (len + 0xFFF) & ~0xFFFULL;
  if (len > (RING3_MMAP_TOP - RING3_MMAP_BASE)) return -ENOMEM;

  int slot = -1;
  for (int i = 0; i < TASK_AREAS; i++) if (!mm->areas[i].used) { slot = i; break; }
  if (slot < 0) return -ENOMEM;

  if (!mm->mmap_hint) mm->mmap_hint = RING3_MMAP_BASE;
  uint64_t base = mm->mmap_hint;
  if (base + len > RING3_MMAP_TOP) return -ENOMEM;

  uint64_t flags = PTE_P | PTE_U;
  if (prot & 2) flags |= PTE_W;
  mm->areas[slot].base = base;
  mm->areas[slot].len = len;
  mm->areas[slot].flags = flags;
  mm->areas[slot].used = 1;
  mm->mmap_hint = base + len + 0x1000;     /* leave one guard page between regions */
  return (long)base;
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

    /* Walk per page: one page table covers 2 MB, so a 4 MB region spans two of
     * them. Indexing a single table across the whole range reads into whatever
     * physical page follows it and "frees" page tables that are still in use. */
    uint64_t freed = 0;
    for (uint64_t off = 0; off < len; off += 4096) {
      uint64_t va = base + off;
      uint64_t *pt = walk_user(t->cr3, va);
      if (!pt) continue;
      int i4 = (int)((va >> 12) & 511);
      if (pt[i4] & PTE_P) {
        pmm_free_page(pt[i4] & ~0xFFFULL);
        pt[i4] = 0;
        freed++;
      }
    }
    serial_puts("[vm] released ");
    serial_puthex(freed);
    serial_puts(" pages, ");
    serial_puthex(pmm_free_pages());
    serial_puts(" free of ");
    serial_puthex(pmm_total_pages());
    serial_puts("\n");
    if (base == a->base && len >= a->len) a->used = 0;
    ring3_write_cr3(ring3_read_cr3());       /* flush the stale translations */
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
  if (entry < RING3_USER_BASE || entry >= RING3_LIMIT) return -EFAULT;

  if (!stack_bytes) stack_bytes = 65536;
  stack_bytes = (stack_bytes + 0xFFF) & ~0xFFFULL;
  if (stack_bytes < 8192) stack_bytes = 8192;

  mm_t *mm = t->mm;
  if (mm->stack_hint < RING3_MMAP_BASE + stack_bytes + 0x1000) return -ENOMEM;
  uint64_t top = mm->stack_hint - 0x1000;         /* guard page above the stack */
  uint64_t base = top - stack_bytes;
  mm->stack_hint = base - 0x1000;                 /* and one between threads */

  int slot = -1;
  for (int i = 0; i < TASK_AREAS; i++) if (!mm->areas[i].used) { slot = i; break; }
  if (slot < 0) return -ENOMEM;
  mm->areas[slot].base = base;
  mm->areas[slot].len = stack_bytes;
  mm->areas[slot].flags = PTE_P | PTE_W | PTE_U;
  mm->areas[slot].used = 1;

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

/* SYS_FUTEX(addr, op, val, timeout_ticks). A wait that would sleep forever gets
 * a deadline when timeout_ticks is non-zero: the timer wakes it, so a wake that
 * arrives before the sleeper is actually blocked costs a few ticks rather than
 * wedging the thread. */
static long sys_futex(uint64_t addr, uint64_t op, uint64_t val, uint64_t timeout_ticks) {
  task_t *t = task_at(task_current());
  if (!t || !t->mm) return -EINVAL;
  if (addr < RING3_USER_BASE || addr + 8 > RING3_LIMIT) return -EFAULT;

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
    uint64_t want = val ? val : 1;
    int woke = 0;
    for (int i = 1; i < task_count(); i++) {
      task_t *o = task_at(i);
      if (!o || o->state != TASK_BLOCKED) continue;
      if (o->wait_addr != addr || o->wait_mm != t->mm) continue;
      o->wait_addr = 0;
      o->wait_mm = 0;
      task_wake(i);
      woke++;
      if ((uint64_t)woke >= want) break;
    }
    return woke;
  }
  return -ENOSYS;
}

/* -------------------------------------------------------------- syscalls */

/* Copy out of a user pointer after checking it really is inside that task's
 * user window. Anything else is rejected rather than dereferenced, which is
 * the point of the exercise. */
static int user_copy(char *dst, uint64_t uptr, uint64_t n) {
  if (n == 0) return 0;
  if (uptr < RING3_USER_BASE || uptr + n > RING3_LIMIT) return -1;
  const char *src = (const char *)(uintptr_t)uptr;
  for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
  return 0;
}

/* Copy back to user space, same window check as the read direction. */
static int user_put(uint64_t uptr, const void *src, uint64_t n) {
  if (n == 0) return 0;
  if (uptr < RING3_USER_BASE || uptr + n > RING3_LIMIT) return -1;
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
  if (!(pml4[1] & PTE_P)) return 0;
  uint64_t *pdpt = (uint64_t *)(hhdm_offset + (pml4[1] & ~0xFFFULL));
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

static int user_prep(uint64_t uptr, uint64_t n) {
  if (n == 0) return 0;
  if (uptr < RING3_USER_BASE || uptr + n > RING3_LIMIT) return -1;
  for (uint64_t page = uptr & ~0xFFFULL; page < uptr + n; page += 0x1000)
    if (!user_page_present(page) && !vm_fault_in(page)) return -1;
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
      if (!user_page_present(page) && !vm_fault_in(page)) return -1;
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

static long sys_open(uint64_t path_u, uint64_t flags) {
  fdtable_t *fds = current_fds();
  if (!fds) return -EBADF;
  char path[RFILE_NAME];
  if (user_string(path, path_u, RFILE_NAME) < 0) return -EFAULT;
  return fd_open(fds, path, (int)flags);
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

/* The number and arguments of the syscall in flight: without them a kernel fault
 * is a rip and a cr2 with no story. */
uint64_t r3_cur_syscall, r3_cur_arg0;

long syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
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
      if (a1 < RING3_USER_BASE || a1 + sizeof(cmd) > RING3_LIMIT) return -EFAULT;
      const uint8_t *src = (const uint8_t *)(uintptr_t)a1;
      for (uint64_t i = 0; i < sizeof(cmd); i++) ((uint8_t *)&cmd)[i] = src[i];

      /* Text is a user pointer inside the command: copy it here, so the
       * compositor never dereferences user memory. */
      char text[WINIPC_TEXT_MAX];
      text[0] = 0;
      if (cmd.kind == WCMD_TEXT || cmd.kind == WCMD_TEXT_LARGE) {
        if (cmd.text < RING3_USER_BASE || cmd.text >= RING3_LIMIT) return -EFAULT;
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
"  movq 104(%rsp), %rdi\n"         /* syscall number (saved rax) */
"  movq 64(%rsp), %rsi\n"          /* a0 (saved rdi) */
"  movq 72(%rsp), %rdx\n"          /* a1 (saved rsi) */
"  movq 80(%rsp), %rcx\n"          /* a2 (saved rdx) */
"  movq 56(%rsp), %r8\n"           /* a3 (saved r8) */
"  movq %rsp, %rbx\n"
"  andq $-16, %rsp\n"              /* keep the C ABI happy */
"  call syscall_dispatch\n"
"  movq %rbx, %rsp\n"
"  movq %rax, 104(%rsp)\n"         /* result becomes the user's rax */
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
".size r3_syscall_stub, .-r3_syscall_stub\n"
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
  serial_puts("\n");

  if ((cs & 3) == 3) {
    /* Not-present faults inside a SYS_VM_ALLOC region are demand paging, not a
     * violation: map the page and let the instruction retry. */
    if (!(err & 1) && vm_fault_in(cr2)) {
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

void ring3_init(void) {
  kpml4_phys = ring3_read_cr3();
  kpml4_virt = (uint64_t *)(hhdm_offset + kpml4_phys);

  idt_set_gate_raw(0x80, (void *)r3_syscall_stub, 1);   /* DPL 3: callable */
  idt_set_gate_raw(13, (void *)r3_isr_fault, 0);        /* #GP */
  idt_set_gate_raw(14, (void *)r3_isr_fault, 0);        /* #PF */

  rfs_init();                     /* seed /hello.txt and friends */
  ready = 1;
  log_hex("[ring3] kernel cr3      ", kpml4_phys);
  log_hex("[ring3] user window     ", RING3_USER_BASE);
  iso_selftest();
  pt_selftest();
  sock_selftest();
  /* Readiness runs here, on the fake backend, before any client exists: the
   * scheduler is up (task_demo_start ran first) but no real traffic is in
   * flight, so a temporary backend swap cannot disturb anything. */
  sock_readiness_selftest();
}