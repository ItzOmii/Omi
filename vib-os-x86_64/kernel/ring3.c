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

#define POOL_FRAMES 256    /* 1 MB of page-aligned frames: tables + pages */

static uint8_t  frame_pool[POOL_FRAMES * 4096] __attribute__((aligned(4096)));
static int      frames_used;

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

/* Kernel image addresses are linked at 0xFFFFFFFF80000000 and loaded at a 1 MB
 * physical base, so the two are a fixed distance apart. */
static uint64_t to_phys(const void *p) {
  return (uint64_t)(uintptr_t)p - kernel_virt_base + kernel_phys_base;
}

/* A page-table frame or user page, zeroed. */
static void *alloc_frame(void) {
  if (frames_used >= POOL_FRAMES) return 0;
  uint8_t *f = frame_pool + (uintptr_t)frames_used * 4096;
  frames_used++;
  for (int i = 0; i < 4096; i++) f[i] = 0;
  return f;
}

static void log_hex(const char *label, uint64_t v) {
  serial_puts(label);
  serial_puthex(v);
  serial_puts("\n");
}

/* --------------------------------------------------------- address spaces */

/* A fresh space: the kernel's tables copied wholesale, nothing user-visible. */
uint64_t ring3_new_space(void) {
  uint64_t *pml4 = (uint64_t *)alloc_frame();
  if (!pml4) return 0;
  for (int i = 0; i < 512; i++) pml4[i] = kpml4_virt[i];
  return to_phys(pml4);
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
                           PTE_P | PTE_W | PTE_U) != 0) return 0;
  }
  return RING3_SHM_BASE;
}

/* A stack in the same window, just below the limit. */
int ring3_make_stack(uint64_t cr3) {
  const int pages = 8;                          /* 32 KB */
  uint64_t base = RING3_LIMIT - (uint64_t)pages * 4096;
  for (int i = 0; i < pages; i++) {
    if (ring3_map(cr3, base + (uint64_t)i * 4096, PTE_P | PTE_W | PTE_U, 0) != 0)
      return -1;
  }
  return 0;
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

static long sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
  if (fd != 1 && fd != 2) return -1;
  if (len > 256) len = 256;
  char tmp[257];
  if (user_copy(tmp, buf, len) != 0) {
    serial_puts("[ring3] write: bad user pointer rejected\n");
    return -1;
  }
  tmp[len] = 0;
  serial_puts(tmp);
  return (long)len;
}

long syscall_dispatch(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
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
      if (user_copy(title, a0, 31) != 0) return -1;
      title[31] = 0;
      for (int i = 0; i < 31; i++) if (!title[i]) break;   /* keep it short */
      return (long)winipc_open(title, (int32_t)a1, (int32_t)a2);
    }
    case SYS_WIN_CMD: {
      win_cmd_u cmd;
      if (a1 < RING3_USER_BASE || a1 + sizeof(cmd) > RING3_LIMIT) return -1;
      const uint8_t *src = (const uint8_t *)(uintptr_t)a1;
      for (uint64_t i = 0; i < sizeof(cmd); i++) ((uint8_t *)&cmd)[i] = src[i];

      /* Text is a user pointer inside the command: copy it here, so the
       * compositor never dereferences user memory. */
      char text[WINIPC_TEXT_MAX];
      text[0] = 0;
      if (cmd.kind == WCMD_TEXT || cmd.kind == WCMD_TEXT_LARGE) {
        if (cmd.text < RING3_USER_BASE || cmd.text >= RING3_LIMIT) return -1;
        if (user_copy(text, cmd.text, WINIPC_TEXT_MAX - 1) != 0) return -1;
        text[WINIPC_TEXT_MAX - 1] = 0;
        for (int i = 0; i < WINIPC_TEXT_MAX; i++) if (!text[i]) break;
      }
      return (long)winipc_add((int)a0, &cmd, text);
    }
    case SYS_WIN_CLEAR:   return (long)winipc_clear((int)a0, (uint32_t)a1);
    case SYS_WIN_WAIT: {
      win_event_t ev;
      for (;;) {
        int r = winipc_poll((int)a0, &ev);
        if (r == 1) {
          if (user_put(a1, &ev, sizeof ev) != 0) return -1;
          return 1;
        }
        if (r < 0) return -1;              /* window gone: nothing to wait for */
        task_block_current();              /* sleeps until an event is queued */
      }
    }
    case SYS_WIN_RECT: {
      int wh = (int)(uint32_t)a3;
      return (long)winipc_set_rect((int)a0, (int32_t)a1, (int32_t)(a1 >> 32),
                                   wh >> 16, wh & 0xFFFF);
    }
    case SYS_WIN_GEOM: {
      int32_t g[4];
      if (winipc_get_rect((int)a0, &g[0], &g[1], &g[2], &g[3]) != 0) return -1;
      if (user_put(a1, g, sizeof g) != 0) return -1;
      return 0;
    }
    case SYS_CLIP_SET: {
      uint64_t n = a1;
      if (n > sizeof clip_buf) n = sizeof clip_buf;
      if (user_copy(clip_buf, a0, n) != 0) return -1;
      clip_len = (int)n;
      return (long)n;
    }
    case SYS_CLIP_GET: {
      if (clip_len <= 0) return 0;
      uint64_t n = a1;
      if (n > (uint64_t)clip_len) n = (uint64_t)clip_len;
      if (user_put(a0, clip_buf, n) != 0) return -1;
      return (long)n;
    }
    case SYS_SHM_GET:    return (long)shm_get((int)a0, (int)a1);
    case SYS_SHM_ATTACH: return (long)shm_attach((int)a0);
    case SYS_WIN_POLL: {
      win_event_t ev;
      int r = winipc_poll((int)a0, &ev);
      if (r == 1 && user_put(a1, &ev, sizeof ev) != 0) return -1;
      return (long)r;                 /* -1 = the window is gone */
    }
    case SYS_WIN_PRESENT: winipc_present((int)a0); return 0;
    case SYS_WIN_CLOSE:   winipc_close((int)a0); return 0;
  }
  serial_puts("[ring3] unknown syscall ");
  serial_puthex(num);
  serial_puts("\n");
  return -1;
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

/* #PF and #GP both push an error code, so the frame is
 *   [0] error  [1] rip  [2] cs  [3] rflags  [4] rsp  [5] ss */
__asm__(
".text\n"
".global r3_isr_fault\n"
".type r3_isr_fault, @function\n"
"r3_isr_fault:\n"
"  movq %rsp, %rdi\n"
"  call ring3_fault\n"
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
    /* A user task stepped outside its window. Kill that task only: the
     * compositor and the other tasks keep running. */
    serial_puts("[ring3] killed user task ");
    serial_puts(task_name(task_current()));
    serial_puts(" (isolation held)\n");
    /* no exit code: this task did not leave through sys_exit */
    task_kill_current((uint64_t)-1);
  }

  serial_puts("[ring3] FATAL: fault in kernel mode, halting\n");
  for (;;) __asm__ volatile("cli; hlt");
}

/* ------------------------------------------------------------------ setup */

void ring3_init(void) {
  kpml4_phys = ring3_read_cr3();
  kpml4_virt = (uint64_t *)(hhdm_offset + kpml4_phys);

  idt_set_gate_raw(0x80, (void *)r3_syscall_stub, 1);   /* DPL 3: callable */
  idt_set_gate_raw(13, (void *)r3_isr_fault, 0);        /* #GP */
  idt_set_gate_raw(14, (void *)r3_isr_fault, 0);        /* #PF */

  ready = 1;
  log_hex("[ring3] kernel cr3      ", kpml4_phys);
  log_hex("[ring3] user window     ", RING3_USER_BASE);
}