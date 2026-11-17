/* User mode (ring 3).
 *
 * Each user task gets its own PML4: entries 0..255 are copied straight out of
 * the kernel's own page tables so the kernel and the HHDM stay reachable at CPL
 * 0, and PML4 slot 1 (virtual 0x0000008000000000) is built per program from the
 * ELF that gets loaded there. Nothing in the copied half is user-accessible, so
 * a user task cannot see the framebuffer, the heap or the kernel image even
 * though the kernel is mapped in its address space.
 *
 * Entry to CPL 3 is a hand-built iretq frame (user_enter), and the return path
 * is an int 0x80 gate with DPL 3 whose handler starts on the TSS rsp0 stack.
 *
 * Layout of a user address space:
 *
 *   0x0000008000000000  program segments at the ELF's own p_vaddr
 *   0x00000080000F8000  32 KB stack (8 pages), rsp starts at RING3_LIMIT-16
 *   0x0000008000100000  end of the window: nothing maps past this
 */
#ifndef RING3_H
#define RING3_H

#include <stdint.h>
#include "task.h"

/* Virtual base of a user address space (PML4 slot 1) and the end of the window
 * that a program is allowed to occupy. */
/* Where our own programs are linked, and where kernel-mediated allocations land. */
#define RING3_USER_BASE   0x0000008000000000ULL

/* The low edge of what counts as a user address. Our programs sit at
 * RING3_USER_BASE, but a program from an ordinary toolchain is linked at 0x400000
 * - musl in particular uses 32-bit absolute relocations in its startup, so it
 * cannot be moved high at all. Both windows are real, so every user-pointer check
 * tests against this edge rather than against our linker script's choice.
 * 0x10000 stays below it, so a null dereference is still a fault. */
#define RING3_USER_MIN    0x0000000000010000ULL
/* The window a process may occupy: 256 MB. Segments land near the base, the
 * stack at the top, and SYS_VM_ALLOC hands out ranges in between. Page tables
 * for it are built on demand, so an empty window costs nothing. */
#define RING3_LIMIT       0x0000008010000000ULL
#define RING3_MMAP_BASE   0x0000008000100000ULL   /* SYS_VM_ALLOC starts here */
#define RING3_MMAP_TOP    0x000000800F000000ULL   /* below the surface region */
/* Shared segments are mapped here in every process that attaches them: the
 * same physical frames, so a write in one process is visible in the other. */
#define RING3_SHM_BASE    (RING3_USER_BASE + 0x00080000ULL)
#define RING3_SHM_MAX     32                      /* pages per segment */
/* Pixel surfaces get their own region, taken out of the top of the window and
 * sized so a maximum window (620x420) fits in one slot with room to spare: 2 MB
 * a slot, four slots, mapped only in the process that created them. VM_ALLOC
 * now stops below this, so a growing heap cannot run into a surface. */
#define RING3_SURF_BASE   (RING3_USER_BASE + 0x0F000000ULL)
#define RING3_SURF_AREA   0x00400000ULL           /* 4 MB per slot: a 1280x768
                                                   * client at 4 bytes a pixel,
                                                   * rounded to the window region */
#define RING3_SURF_SLOTS  3                       /* 3 x 4 MB = 12 MB, which
                                                   * leaves the top 4 MB of the
                                                   * window for the stack */

/* PTE flags */
#define RING3_P   0x001ULL   /* present */
#define RING3_W   0x002ULL   /* writable */
#define RING3_U   0x004ULL   /* user-accessible */

/* Syscall numbers, shared with the user programs. */
#define SYS_WRITE       1
#define SYS_EXIT        2
#define SYS_YIELD       3
#define SYS_TICKS       4
#define SYS_GETPID      5
#define SYS_WIN_OPEN    6     /* (title, w, h) -> window id            */
#define SYS_WIN_CMD     7     /* (id, win_cmd_u*) -> 0                 */
#define SYS_WIN_CLEAR   8     /* (id, colour)                          */
#define SYS_WIN_PRESENT 9     /* (id)                                  */
#define SYS_WIN_CLOSE   10    /* (id)                                  */
#define SYS_WIN_POLL    11    /* (id, win_event_t*) -> 1 event, 0 none, -1 gone */
#define SYS_WIN_WAIT    12    /* (id, win_event_t*) -> blocks until an event  */
#define SYS_WIN_RECT    13    /* (id, x, y, (w<<16)|h) -> resize/move self   */
#define SYS_WIN_GEOM    14    /* (id, int32_t out[4]) -> x,y,w,h             */
#define SYS_SHM_GET     15    /* (slot, pages) -> 0, creates if needed       */
#define SYS_SHM_ATTACH  16    /* (slot) -> user base address, or -1          */
#define SYS_CLIP_SET    17    /* (char*, len) -> len stored                  */
#define SYS_CLIP_GET    18    /* (char*, max) -> len copied                  */
#define SYS_VM_ALLOC    19    /* (len, prot) -> user address, pages on fault */
#define SYS_VM_FREE     20    /* (addr, len) -> 0                             */
#define SYS_THREAD_CREATE 21  /* (entry, arg, stack, 0) -> tid                */
#define SYS_FUTEX       22    /* (addr, FUTEX_WAIT|WAKE, val, ticks)           */
#define SYS_OPEN        23    /* (path, flags) -> fd | -ENOENT | -EMFILE       */
#define SYS_READ        24    /* (fd, buf, len) -> bytes                       */
#define SYS_LSEEK       25    /* (fd, off, whence) -> new offset               */
#define SYS_CLOSE       26    /* (fd) -> 0 | -EBADF                            */
#define SYS_DUP        27   /* (fd) -> new fd sharing the same open file description */
#define SYS_SOCKET     28   /* (AF_INET, SOCK_STREAM) -> fd | -EAFNOSUPPORT */
#define SYS_CONNECT    29   /* (fd, ipv4 as u32, port) -> 0 | -ECONNREFUSED | -EISCONN */
#define SYS_SEND       30   /* (fd, buf, len) -> bytes | -EPIPE */
#define SYS_RECV       31   /* (fd, buf, len) -> bytes | -EAGAIN (timeout) | 0 (EOF) */
#define SYS_RESOLVE    32   /* (host, ip_out[4]) -> 0 | -ENOENT            */
#define SYS_POLL       33   /* (fds, nfds, timeout_ms) -> ready count | -EAGAIN */
#define SYS_PIPE       34   /* (int fds[2]) -> 0 | -EMFILE  (fds[0] read, fds[1] write) */
#define SYS_SURF_CREATE 35  /* (win, w, h) -> user address of the pixels | -errno */
#define SYS_SURF_PRESENT 36 /* (win, (x<<16)|y, (w<<16)|h) -> 0 | -errno         */
#define SYS_SURF_RESIZE 37  /* (win, w, h) -> 0 | -errno: the client changed size  */

/* the switch, the gates and the fault handler */
void     ring3_init(void);
int      ring3_ready(void);
void     ring3_fault(uint64_t *frame);   /* from the #PF / #GP stubs */

/* address spaces */
uint64_t ring3_new_space(void);
mm_t    *ring3_new_mm(void);
int      ring3_map(uint64_t cr3, uint64_t vaddr, uint64_t flags, void **kva);
int      ring3_map_copy(uint64_t cr3, uint64_t vaddr, const uint8_t *src,
                        uint64_t len, uint64_t flags);
int      ring3_make_stack(uint64_t cr3);
int      ring3_copy_to_mm(uint64_t cr3, uint64_t vaddr, const void *src, uint64_t n);
int      ring3_map_existing(uint64_t cr3, uint64_t vaddr, uint64_t phys, uint64_t flags);

/* the kernel's own CR3, and raw access for the scheduler */
uint64_t ring3_kernel_cr3(void);
uint64_t ring3_read_cr3(void);
void     ring3_write_cr3(uint64_t cr3);

#endif