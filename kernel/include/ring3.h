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
/* The window a process may occupy: 4 GB. Segments land near the base, the
 * stack at the top, and SYS_VM_ALLOC hands out ranges in between. Page tables
 * for it are built on demand, so an empty window costs nothing - which is the
 * whole reason this can be 4 GB. A browser engine reserves a gigabyte or two
 * per renderer (V8 cage, code range, a heap that only commits what it touches)
 * and used to be refused at 256 MB while the machine sat idle. */
#define RING3_LIMIT       (RING3_USER_BASE + 0x100000000ULL)
/* The window is laid out once, here, and every region has a home in it. It used
 * to be laid out piecemeal: the shared-segment arena sat at 8 MB and grew 256 MB
 * straight through the area mmap hands out, so a process with a few shared
 * regions and a 1.5 GB allocation would have found them in the same pages. The
 * numbers below are a single map, in order, with a gigabyte kept for a growing
 * brk heap and the rest for anonymous mappings:
 *
 *   0 .. 8 MB          low addresses, where a null dereference must still fault
 *   16 MB .. 3.5 GB    mmap / SYS_VM_ALLOC, handed out from MMAP_BASE up
 *   3.5 GB .. 3.75 GB  shared-segment attaches, one 256 MB arena per process
 *   3.75 GB .. 3.875 GB pixel surfaces, RING3_SURF_SLOTS of RING3_SURF_AREA
 *   3.875 GB .. 3.9375 GB the scene, mapped only in the process that owns it
 *   top 4 MB           the stack
 */
#define RING3_MMAP_BASE   0x0000008000100000ULL  /* low, where musl expects it */
#define RING3_MMAP_TOP    RING3_SHM_BASE                      /* stops at the shm arena */
/* Shared segments are mapped here in every process that attaches them: the
 * same physical frames, so a write in one process is visible in the other. */
#define RING3_SHM_BASE    (RING3_USER_BASE + 0xE0000000ULL)   /* 3.5 GB */
#define RING3_SHM_ARENA   (256ULL << 20)          /* per-process space for attaches */
#define RING3_SHM_TOP     (RING3_SHM_BASE + RING3_SHM_ARENA)  /* 3.75 GB */
#define RING3_SHM_SLOTS   64                      /* named regions, system wide */
#define RING3_SHM_MAX     4096                    /* pages per region: 16 MB */
/* Pixel surfaces: 16 MB a slot, eight slots, mapped only in the process that
 * created them. The area is sized for a full-HD-ish window rather than for the
 * window a demo happens to use - 1920x1200 at four bytes a pixel is 9.2 MB, and
 * at 4 MB a slot the ceiling used to be 1280x768, which is not a browser window
 * on any display made this decade. The slot is address space, not memory: frames
 * are allocated for the pixels the surface actually has, so a 440x360 window
 * costs 634 KB of RAM, not 16 MB. Resizing allocates more frames as it grows.
 */
#define RING3_SURF_BASE   (RING3_USER_BASE + 0xF0000000ULL)   /* 3.75 GB */
#define RING3_SURF_AREA   0x01000000ULL           /* 16 MB per slot */
/* The scene: the frame the screen actually shows. A surface belongs to one
 * window; the scene belongs to the display, and exactly one process holds it.
 * 64 MB of address space covers 4K at four bytes a pixel (33 MB) with room to
 * spare, and it is address space, not memory: frames are allocated for the
 * screen that is actually attached. */
#define RING3_SCENE_BASE  (RING3_USER_BASE + 0xF8000000ULL)   /* 3.875 GB */
#define RING3_SCENE_AREA  (64ULL << 20)
#define RING3_SCENE_TOP   (RING3_SCENE_BASE + RING3_SCENE_AREA)
#define RING3_SURF_SLOTS  8                       /* 8 x 16 MB = 128 MB of address
                                                   * space, ending at 3.875 GB with
                                                   * the top 4 MB left for the stack.
                                                   * Three was too few: the desktop
                                                   * boots with four ring-3 demo
                                                   * windows, so a browser asking for
                                                   * a fourth slot got -ENOSPC and
                                                   * quietly went away - a click that
                                                   * opens nothing and says nothing. */

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
#define SYS_SHM_GET     15    /* (slot, pages) -> 0, creates or grows        */
#define SYS_SHM_ATTACH  16    /* (slot) -> this process's address, or -EINVAL*/
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
#define SYS_SHM_INFO    38  /* (slot) -> pages in the region | -EINVAL              */
#define SYS_SHM_DETACH  39  /* (slot) -> 0 | -EINVAL: unmap it and drop one use     */
#define SYS_SYNC        40  /* (void) -> 0: flush every dirty file to the disk      */
/* (void) -> (total_pages << 32) | free_pages. A process that is about to spend
 * memory on something discretionary - a page's images, say - should be able to ask
 * how much the machine has instead of carrying a guess around in a constant. */
#define SYS_MEMINFO     41
#define SYS_SCENE_MAP    42  /* (void) -> the scene's address | -errno      */
#define SYS_SCENE_INFO   43  /* (uint32_t out[3]) -> 0: w, h, row stride    */
#define SYS_SCENE_FLUSH  44  /* (x, y, w, h) -> 0: rect to the display      */
#define SYS_SCENE_HANDOFF 45 /* (on) -> the previous state                  */
#define SYS_SURF_MAP     47  /* (win, int32_t out[3]) -> read-only base address, 0 = fail
                              * out = w, h, stride. Another process's window pixels. */
#define SYS_SURF_LIST    48  /* (int32_t out[], max) -> count, triples of win,w,h */
#define SYS_INPUT_TAKE   49  /* (int on) -> 1 this process now owns raw input, 0 not */
#define SYS_INPUT_POLL   50  /* (int32_t out[], max) -> count; four ints per event:
                              * kind, a, b, c. 1 move, 2 down, 3 up, 4 key, 5 wheel. */
#define SYS_WIN_INPUT    51  /* (win, int32_t ev[4] = kind,x,y,code) -> 0 delivered,
                              * -1 no such window: the server routes an event to a window */
#define SYS_SCENE_OWN    46  /* (x,y), (w,h) -> 0: a rect the kernel keeps out of
                              * Its own blits. 0,0 clears the claim.         */

#define SYS_WIN_PLACE   52  /* (id, x, y, (w<<16)|h) -> 0 | -EPERM. The display server
                              * places a window it did not open: authority comes from
                              * owning the pointer, not from owning the window.     */
#define SYS_WIN_TITLE   53  /* (id, char *out, max) -> length | -1: a window's title */

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