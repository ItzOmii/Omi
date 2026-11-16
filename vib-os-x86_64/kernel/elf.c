/* ELF64 loader for user programs.
 *
 * A program is an ELF64 file carried on the boot medium as a Limine module
 * (iso_root/apps/*.elf). Two shapes arrive here, and they are loaded
 * differently:
 *
 *   ET_EXEC - static, position-dependent. It runs at the addresses it was linked
 *             for, so the load bias is zero and its entry is its own.
 *   ET_DYN  - position-independent. The kernel picks the address: the program
 *             lands at ELF_PROG_BASE, and if it names an interpreter in
 *             PT_INTERP, that interpreter is mapped at ELF_INTERP_BASE in the
 *             same address space.
 *
 * The kernel still relocates nothing. A PIE's relocations are done by the
 * dynamic loader it asks for, in ring 3, which is why this file needs no
 * relocation code at all: mapping two images and handing over the right auxv is
 * the entire contract. With an interpreter the task starts at the
 * *interpreter's* entry and AT_ENTRY names the program's, because ld.so is the
 * thing that runs first and it needs to know what it is loading.
 *
 * What this deliberately does not do yet: filesystem lookup (the path is a
 * suffix match on the boot modules), demand paging for program segments (all
 * pages are filled up front), and PROT_NONE reservations (see ring3.c). */

#include "include/elf.h"
#include "include/errno.h"
#include "include/limine_modules.h"
#include "include/ring3.h"
#include "include/task.h"
#include "include/types.h"

extern void serial_puts(const char *s);
extern int r3_dyn_trace;
extern int r3_dyn_trace_pid;
extern void serial_puthex(uint64_t v);

#define PT_LOAD   1
#define PT_INTERP 3
#define ET_EXEC   2
#define ET_DYN    3
#define EM_X86_64 62

typedef struct {
  uint8_t  ident[16];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry;
  uint64_t phoff;
  uint64_t shoff;
  uint32_t flags;
  uint16_t ehsize;
  uint16_t phentsize;
  uint16_t phnum;
  uint16_t shentsize;
  uint16_t shnum;
  uint16_t shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
  uint32_t type;
  uint32_t flags;
  uint64_t offset;
  uint64_t vaddr;
  uint64_t paddr;
  uint64_t filesz;
  uint64_t memsz;
  uint64_t align;
} __attribute__((packed)) elf64_phdr_t;

/* Where a PIE lands and where its interpreter goes. The program keeps our own
 * binaries' base, so a PIE and an ET_EXEC program are interchangeable as far as
 * everything above this file is concerned. The interpreter sits 1 MB up - above
 * any program image we build, below the first address mmap hands out - and the
 * region is registered as an area, so the mmap hint walk steps over it instead
 * of handing a library the interpreter's pages. */
#define ELF_PROG_BASE     RING3_USER_BASE
#define ELF_INTERP_BASE   (RING3_USER_BASE + 0x00100000ULL)
#define ELF_DYN_MMAP_BASE (RING3_USER_BASE + 0x00400000ULL)

static void log_hex(const char *label, uint64_t v) {
  serial_puts(label);
  serial_puthex(v);
  serial_puts("\n");
}

typedef struct {
  uint64_t entry;      /* where the task starts: the program, or the interpreter */
  uint64_t at_entry;   /* AT_ENTRY - always the program's entry */
  uint64_t at_base;    /* AT_BASE - interpreter load bias, 0 without one */
  uint64_t phdr, phent, phnum;
} elf_img_t;

static const elf64_phdr_t *phdr_at(const uint8_t *base, const elf64_ehdr_t *eh, uint16_t i) {
  return (const elf64_phdr_t *)(base + eh->phoff + (uint64_t)i * eh->phentsize);
}

/* Every PT_LOAD's lowest page. A PIE is linked from 0, but nothing promises
 * that, and the bias has to put the *lowest* segment at the base we chose. */
static uint64_t lowest_vaddr(const uint8_t *base, const elf64_ehdr_t *eh) {
  uint64_t lo = ~0ULL;
  for (uint16_t i = 0; i < eh->phnum; i++) {
    const elf64_phdr_t *ph = phdr_at(base, eh, i);
    if (ph->type != PT_LOAD || ph->memsz == 0) continue;
    uint64_t v = ph->vaddr & ~0xFFFULL;
    if (v < lo) lo = v;
  }
  return lo == ~0ULL ? 0 : lo;
}

/* Copy the segments of one image into `cr3` at vaddr + bias. Returns 0 and the
 * mapped range, so the caller can register it and check the two images do not
 * overlap. */
static int map_image(const char *what, const uint8_t *base, uint64_t len,
                     const elf64_ehdr_t *eh, uint64_t cr3, uint64_t bias,
                     uint64_t *lo_out, uint64_t *hi_out) {
  uint64_t lo = ~0ULL, hi = 0;
  int loaded = 0;

  for (uint16_t i = 0; i < eh->phnum; i++) {
    const elf64_phdr_t *ph = phdr_at(base, eh, i);
    if (ph->type != PT_LOAD || ph->memsz == 0) continue;

    /* Check the arithmetic before it happens: a vaddr near the top of the u64
     * range plus a bias would wrap into a valid-looking low address. */
    if (ph->vaddr > RING3_LIMIT || bias > RING3_LIMIT - ph->vaddr) {
      serial_puts("[elf] ");
      serial_puts(what);
      serial_puts(": segment address overflows, refusing\n");
      return -1;
    }
    uint64_t vaddr = ph->vaddr + bias;
    if (vaddr < RING3_USER_MIN || vaddr + ph->memsz > RING3_LIMIT) {
      serial_puts("[elf] ");
      serial_puts(what);
      serial_puts(": segment outside the user window, refusing\n");
      return -1;
    }
    if (ph->filesz > ph->memsz || ph->offset + ph->filesz > len) {
      serial_puts("[elf] ");
      serial_puts(what);
      serial_puts(": segment sizes lie, refusing\n");
      return -1;
    }

    /* The BSS tail (memsz - filesz) has to read as zero: mapping fresh pages
     * and copying only filesz bytes gives exactly that. */
    uint64_t flags = RING3_P | RING3_U | ((ph->flags & 0x2) ? RING3_W : 0);
    if (ph->filesz) {
      if (ring3_map_copy(cr3, vaddr, base + ph->offset, ph->filesz, flags) != 0) {
        serial_puts("[elf] ");
        serial_puts(what);
        serial_puts(": could not map a segment\n");
        return -1;
      }
    }
    if (ph->memsz > ph->filesz) {
      if (ring3_map_copy(cr3, vaddr + ph->filesz, 0, ph->memsz - ph->filesz, flags) != 0) {
        serial_puts("[elf] ");
        serial_puts(what);
        serial_puts(": could not map the bss tail\n");
        return -1;
      }
    }
    if (vaddr < lo) lo = vaddr;
    if (vaddr + ph->memsz > hi) hi = vaddr + ph->memsz;
    loaded++;
  }

  if (!loaded) {
    serial_puts("[elf] ");
    serial_puts(what);
    serial_puts(": no loadable segments\n");
    return -1;
  }
  *lo_out = lo & ~0xFFFULL;
  *hi_out = (hi + 0xFFF) & ~0xFFFULL;
  return 0;
}

/* AT_PHDR has to be an address the program can read, so it is the program header
 * table *as mapped*, not as it sits in the module: find the segment covering the
 * table's file range and translate the offset inside it. Assuming vaddr == offset
 * would work for the first segment of everything we build and quietly produce a
 * wrong pointer the first time a linker emits a segment with a different skew -
 * the executable we just tested has exactly that in its last segment. */
static int phdr_mapped_addr(const uint8_t *base, const elf64_ehdr_t *eh, uint64_t bias,
                            uint64_t *out) {
  uint64_t p = eh->phoff;
  uint64_t n = (uint64_t)eh->phnum * eh->phentsize;
  for (uint16_t i = 0; i < eh->phnum; i++) {
    const elf64_phdr_t *ph = phdr_at(base, eh, i);
    if (ph->type != PT_LOAD) continue;
    if (p >= ph->offset && p + n <= ph->offset + ph->filesz) {
      *out = bias + ph->vaddr + (p - ph->offset);
      return 0;
    }
  }
  return -1;
}

/* Reserve a range so that hint allocation (SYS_VM_ALLOC, mmap without MAP_FIXED)
 * steps over it. Program and interpreter pages are mapped up front rather than
 * faulted, but the area still has to exist: a library mapped on top of the
 * interpreter would be a very quiet way to break a process. */
static int area_reserve(mm_t *mm, uint64_t base, uint64_t len) {
  for (int i = 0; i < TASK_AREAS; i++) {
    if (mm->areas[i].used) continue;
    mm->areas[i].base = base;
    mm->areas[i].len = len;
    mm->areas[i].flags = RING3_P | RING3_W | RING3_U;
    mm->areas[i].prot = 3;          /* an image is read and written */
    mm->areas[i].used = 1;
    mm->areas[i].src = 0;          /* not a file region: pages are already here */
    mm->areas[i].src_off = 0;
    mm->areas[i].src_len = 0;
    return 0;
  }
  return -1;
}

/* Validate one module as an image and hand back its header. */
static int open_image(const char *path, const uint8_t **base_out, uint64_t *len_out,
                      const elf64_ehdr_t **eh_out) {
  void *blob = 0;
  uint64_t len = 0;
  if (!meep_module_find(path, &blob, &len) || !blob || len < sizeof(elf64_ehdr_t)) {
    serial_puts("[elf] no such module: ");
    serial_puts(path ? path : "(null)");
    serial_puts("\n");
    return -1;
  }

  const uint8_t *base = (const uint8_t *)blob;
  const elf64_ehdr_t *eh = (const elf64_ehdr_t *)base;

  if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
      eh->ident[2] != 'L' || eh->ident[3] != 'F') {
    serial_puts("[elf] not an ELF file: ");
    serial_puts(path);
    serial_puts("\n");
    return -1;
  }
  if (eh->ident[4] != 2) { serial_puts("[elf] not 64-bit\n"); return -1; }
  if (eh->type != ET_EXEC && eh->type != ET_DYN) {
    serial_puts("[elf] neither executable nor position-independent: ");
    serial_puts(path);
    serial_puts("\n");
    return -1;
  }
  if (eh->machine != EM_X86_64) { serial_puts("[elf] wrong machine\n"); return -1; }
  if (eh->phentsize != sizeof(elf64_phdr_t) || eh->phnum == 0) {
    serial_puts("[elf] bad program header table\n");
    return -1;
  }

  *base_out = base;
  *len_out = len;
  *eh_out = eh;
  return 0;
}

/* PT_INTERP's string, which names the dynamic loader the program wants. */
static int find_interp(const uint8_t *base, const elf64_ehdr_t *eh, char *out, uint64_t cap) {
  for (uint16_t i = 0; i < eh->phnum; i++) {
    const elf64_phdr_t *ph = phdr_at(base, eh, i);
    if (ph->type != PT_INTERP) continue;
    if (!ph->filesz || ph->filesz > cap) return -1;
    for (uint64_t k = 0; k < ph->filesz; k++) out[k] = (char)base[ph->offset + k];
    out[ph->filesz - 1] = 0;        /* the last byte is the NUL, but do not rely on it */
    return 0;
  }
  return 1;                          /* no PT_INTERP: a static image */
}

int elf_load_ex(const char *path_suffix, elf_img_t *img, mm_t **mm_out) {
  const uint8_t *base, *ibase;
  uint64_t len, ilen;
  const elf64_ehdr_t *eh, *ieh;

  if (open_image(path_suffix, &base, &len, &eh) != 0) return -1;

  r3_dyn_trace = 0;              /* only the process that names a loader is traced */
  mm_t *mm = ring3_new_mm();
  if (!mm) { serial_puts("[elf] no address space\n"); return -1; }
  uint64_t cr3 = mm->cr3;
  if (!cr3) { serial_puts("[elf] out of page-table frames\n"); return -1; }

  /* A position-independent program is moved to our base; a static one stays
   * where it was linked. */
  uint64_t bias = 0;
  if (eh->type == ET_DYN) {
    uint64_t min_v = lowest_vaddr(base, eh);
    if (min_v > ELF_PROG_BASE) {
      serial_puts("[elf] ");
      serial_puts(path_suffix);
      serial_puts(": image starts at ");
      serial_puthex(min_v);
      serial_puts(", above the base it would be moved to\n");
      return -1;
    }
    bias = ELF_PROG_BASE - min_v;
  }

  uint64_t lo = 0, hi = 0;
  if (map_image(path_suffix, base, len, eh, cr3, bias, &lo, &hi) != 0) return -1;
  if (area_reserve(mm, lo, hi - lo) != 0) {
    serial_puts("[elf] no area slot for the program image\n");
    return -1;
  }

  uint64_t at_base = 0;
  uint64_t start = bias + eh->entry;

  /* ---- the interpreter, if the program names one ---- */
  char interp_path[128];
  int ii = find_interp(base, eh, interp_path, sizeof(interp_path));
  if (ii < 0) {
    serial_puts("[elf] ");
    serial_puts(path_suffix);
    serial_puts(": PT_INTERP path is unusable\n");
    return -1;
  }

  /* AT_PHDR is only meaningful if it points at bytes the program can read. It
   * is mandatory for a dynamic program - ld.so finds the program's PT_DYNAMIC
   * through it - and for a libc's static startup, which walks it for PT_TLS.
   * Our own programs are linked with their headers outside every PT_LOAD, so
   * there is no such address for them; they read argc and argv and nothing
   * else, so zero is the honest answer rather than a pointer into nothing. */
  uint64_t phdr = 0;
  if (phdr_mapped_addr(base, eh, bias, &phdr) != 0) {
    if (ii == 0) {
      serial_puts("[elf] ");
      serial_puts(path_suffix);
      serial_puts(": dynamic program has no mapped program headers\n");
      return -1;
    }
    phdr = 0;
    serial_puts("[elf] ");
    serial_puts(path_suffix);
    serial_puts(": headers are not in a segment, AT_PHDR = 0\n");
  }
  if (ii == 0) {
    if (open_image(interp_path, &ibase, &ilen, &ieh) != 0) {
      /* A program that asks for ld.so must not run without it: it would fault
       * on its first external call, and the reason would be nowhere in sight. */
      serial_puts("[elf] ");
      serial_puts(path_suffix);
      serial_puts(": interpreter not on the boot medium: ");
      serial_puts(interp_path);
      serial_puts("\n");
      return -1;
    }

    uint64_t ibias = 0;
    if (ieh->type == ET_DYN) {
      uint64_t min_v = lowest_vaddr(ibase, ieh);
      if (min_v > ELF_INTERP_BASE) {
        serial_puts("[elf] interpreter image starts above its base\n");
        return -1;
      }
      ibias = ELF_INTERP_BASE - min_v;
    }
    uint64_t ilo = 0, ihi = 0;
    if (map_image(interp_path, ibase, ilen, ieh, cr3, ibias, &ilo, &ihi) != 0) return -1;
    if (ilo < hi && lo < ihi) {
      serial_puts("[elf] program and interpreter overlap, refusing\n");
      return -1;
    }
    if (area_reserve(mm, ilo, ihi - ilo) != 0) {
      serial_puts("[elf] no area slot for the interpreter\n");
      return -1;
    }

    /* Hint allocation starts above both images, so a library the loader maps
     * cannot land on the loader. */
    uint64_t hint = ihi > hi ? ihi : hi;
    if (hint < ELF_DYN_MMAP_BASE) hint = ELF_DYN_MMAP_BASE;
    hint = (hint + 0xFFF) & ~0xFFFULL;
    if (hint > RING3_MMAP_TOP) {
      serial_puts("[elf] no room for the interpreter in the user window\n");
      return -1;
    }
    mm->mmap_hint = hint;

    at_base = ibias;
    start = ibias + ieh->entry;
#ifdef R3_TRACE_DYNLINK
    /* Every mmap/mprotect/munmap/open the loader makes, in order. Building a
     * loader is a matter of matching its expected syscall sequence against the
     * real one, and the fault address alone does not say which call went wrong. */
    r3_dyn_trace = 1;
#endif

    serial_puts("[elf] interpreter ");
    serial_puts(interp_path);
    serial_puts(" at ");
    serial_puthex(ilo);
    serial_puts(" (bias ");
    serial_puthex(ibias);
    serial_puts("), program at ");
    serial_puthex(lo);
    serial_puts("\n");
  } else if (eh->type == ET_DYN) {
    /* A static PIE: no interpreter, but it still does not run at the addresses
     * it was linked for until it relocates itself. Its own startup handles that,
     * so the bias is all it needs. */
    mm->mmap_hint = hi < ELF_DYN_MMAP_BASE ? ELF_DYN_MMAP_BASE : (hi + 0xFFF) & ~0xFFFULL;
  }

  if (ring3_make_stack(cr3) != 0) { serial_puts("[elf] no stack frames\n"); return -1; }
  if (start < RING3_USER_MIN || start >= RING3_LIMIT) {
    serial_puts("[elf] entry point outside the user window\n");
    return -1;
  }

  log_hex("[elf] loaded ", len);
  serial_puts("[elf]   bytes from ");
  serial_puts(path_suffix);
  serial_puts(", start ");
  serial_puthex(start);
  serial_puts(", entry ");
  serial_puthex(bias + eh->entry);
  serial_puts(", cr3 ");
  serial_puthex(cr3);
  serial_puts("\n");

  img->entry = start;
  img->at_entry = bias + eh->entry;
  img->at_base = at_base;
  img->phdr = phdr;
  img->phent = eh->phentsize;
  img->phnum = eh->phnum;
  *mm_out = mm;
  return 0;
}

/* Lay out the initial stack the way Linux does, because that is what a libc
 * expects to find: the strings at the top, then
 *
 *   argc, argv[], NULL, NULL (empty envp), auxv pairs, AT_NULL
 *
 * Our own programs read argc from (%rsp) and argv from 8(%rsp) and ignore the
 * rest, so this costs them nothing. A musl program reads all of it, and four of
 * the auxv entries are not optional:
 *
 *   AT_PHDR/AT_PHENT/AT_PHNUM - the static startup walks them to find PT_TLS and
 *   set up the thread pointer. Without them %fs is never set, and the first
 *   thread-local access is a fault in a program that looked correct.
 *   AT_PAGESZ - becomes libc.page_size, which malloc and stdio divide by.
 *   AT_RANDOM - seeds the stack canary. Zeros would make the canary check pass
 *   trivially, which is worse than not having one: say so and move on.
 *
 * With a dynamic loader two more matter, and they are different addresses:
 * AT_ENTRY is the program's entry, not the interpreter's, and AT_BASE is where
 * the interpreter was loaded. ld.so starts at the interpreter entry, finds the
 * program through AT_PHDR/AT_ENTRY, and jumps to it when it is done.
 *
 * AT_EXECFN is not optional either, and it is not allowed to be NULL: setting
 * the bit with a zero value is worse than omitting it, because musl tests the
 * bit and then compares the string against "/proc/" without looking at it again.
 * A program spawned with no argv therefore still gets its name written to the
 * stack for this entry alone.
 *
 * rsp ends up 16-byte aligned, which is what the ABI promises at _start.
 */
#define ARG_MAX ELF_ARG_MAX
#define ARG_BLK ELF_ARG_BLK

static uint64_t build_initial_stack(uint64_t cr3, int argc, const char *const *argv,
                                    const elf_img_t *img, const char *progname) {
  static char blk[ARG_BLK];
  static uint64_t argp[ARG_MAX];
  for (int i = 0; i < ARG_BLK; i++) blk[i] = 0;
  if (argc > ARG_MAX) argc = ARG_MAX;

  uint64_t base = RING3_LIMIT - ARG_BLK;
  uint64_t off = ARG_BLK;

  /* 16 bytes of AT_RANDOM first, so it is a fixed distance from nothing in
   * particular. The tick counter is the only entropy in the machine. */
  off -= 16;
  uint64_t random_at = base + off;
  uint64_t seed = task_total_ticks() * 0x9E3779B97F4A7C15ULL + 0x243F6A8885A308D3ULL;
  for (int i = 0; i < 16; i++) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    blk[off + i] = (char)(seed >> 24);
  }

  for (int i = argc - 1; i >= 0; i--) {
    uint64_t n = 1;
    while (argv[i][n - 1]) n++;
    if (n > off) return 0;
    off -= n;
    for (uint64_t k = 0; k < n; k++) blk[off + k] = argv[i][k];
    argp[i] = base + off;
  }

  /* The name always exists on the stack, even when there is no argv. */
  uint64_t execfn = argp[0];
  if (argc == 0) {
    const char *n = progname ? progname : "(program)";
    uint64_t len = 1;
    while (n[len - 1]) len++;
    if (len > off) return 0;
    off -= len;
    for (uint64_t k = 0; k < len; k++) blk[off + k] = n[k];
    execfn = base + off;
  }

  uint64_t aux[26];
  int na = 0;
  aux[na++] = 3;  aux[na++] = img->phdr;          /* AT_PHDR  */
  aux[na++] = 4;  aux[na++] = img->phent;         /* AT_PHENT */
  aux[na++] = 5;  aux[na++] = img->phnum;         /* AT_PHNUM */
  aux[na++] = 6;  aux[na++] = 4096;               /* AT_PAGESZ */
  aux[na++] = 7;  aux[na++] = img->at_base;       /* AT_BASE  */
  aux[na++] = 9;  aux[na++] = img->at_entry;      /* AT_ENTRY */
  aux[na++] = 11; aux[na++] = 0;                  /* AT_UID  */
  aux[na++] = 12; aux[na++] = 0;                  /* AT_EUID */
  aux[na++] = 13; aux[na++] = 0;                  /* AT_GID  */
  aux[na++] = 14; aux[na++] = 0;                  /* AT_EGID */
  aux[na++] = 23; aux[na++] = 0;                  /* AT_SECURE: not setuid */
  aux[na++] = 25; aux[na++] = random_at;          /* AT_RANDOM */
  aux[na++] = 31; aux[na++] = execfn;             /* AT_EXECFN */

  uint64_t tab[64];
  int k = 0;
  tab[k++] = (uint64_t)argc;
  for (int i = 0; i < argc; i++) tab[k++] = argp[i];
  tab[k++] = 0;                                   /* end of argv */
  tab[k++] = 0;                                   /* empty envp */
  for (int i = 0; i < na; i++) tab[k++] = aux[i];
  tab[k++] = 0; tab[k++] = 0;                     /* AT_NULL */

  uint64_t sp = (base + off - (uint64_t)k * 8) & ~15ULL;
  uint64_t p = sp - base;
  for (int i = 0; i < k; i++) {
    uint64_t w = tab[i];
    for (int b = 0; b < 8; b++) blk[p + i * 8 + b] = (char)(w >> (8 * b));
  }

  if (ring3_copy_to_mm(cr3, base, blk, ARG_BLK) != 0) return 0;

  return sp;
}

/* Load `path` and start it with a real argv and a Linux-shaped stack. */
int ring3_spawn_elf_argv(const char *name, const char *path, int argc, const char *const *argv) {
  elf_img_t img;
  mm_t *mm = 0;
  if (elf_load_ex(path, &img, &mm) != 0) return -1;
  uint64_t sp = build_initial_stack(mm->cr3, argc, argv, &img, name);
  if (!sp) {
    /* Two causes, one symptom: argv bigger than the block, or the stack page
     * could not be written. Say which, and which program. */
    serial_puts("[elf] ");
    serial_puts(name);
    serial_puts(": user stack allocation failed (argv too large or page refused)\n");
    return -1;
  }
  int id = task_spawn_user_mm(name, img.entry, sp, mm, (uint64_t)argc);
  if (id >= 0 && img.at_base) r3_dyn_trace_pid = id;
  if (id < 0) {
    serial_puts("[elf] no task slot for ");
    serial_puts(name);
    serial_puts("\n");
  }
  return id;
}

/* Load `path` into a brand new address space and start it. `arg` arrives in the
 * program's rdi, which is how one binary can play two roles in the demo.
 *
 * The stack is built the same way the argv path builds it, with argc = 0. It
 * used to be "start at RING3_LIMIT - 16 and let the program find zeros", which
 * is fine for a program that only reads argc and ignores the rest - and fatal
 * for a libc, whose startup walks envp until a NULL and lands in the unmapped
 * page past the top of the stack when there is no NULL to find. */
int ring3_spawn_elf(const char *name, const char *path, uint64_t arg) {
  elf_img_t img;
  mm_t *mm = 0;
  if (elf_load_ex(path, &img, &mm) != 0) return -1;
  uint64_t sp = build_initial_stack(mm->cr3, 0, 0, &img, name);
  if (!sp) {
    serial_puts("[elf] ");
    serial_puts(name);
    serial_puts(": user stack allocation failed\n");
    return -1;
  }
  int id = task_spawn_user_mm(name, img.entry, sp, mm, arg);
  if (id >= 0 && img.at_base) r3_dyn_trace_pid = id;
  if (id < 0) {
    serial_puts("[elf] no task slot for ");
    serial_puts(name);
    serial_puts("\n");
  }
  return id;
}
/* Load `path` into an address space of its own and build its stack, but do not
 * start it. execve is the only caller - starting a process and replacing one
 * differ only in who ends up attached to the result, and everything before that
 * point is the same work.
 *
 * A program that is not on the medium is told apart from one that could not be
 * loaded, because they are different answers: the first is a name the caller got
 * wrong, the second is memory the machine did not have.
 */
int elf_prepare_image(const char *path, int argc, const char *const *argv,
                      uint64_t *entry_out, mm_t **mm_out, uint64_t *sp_out) {
  const uint8_t *b;
  uint64_t l;
  const elf64_ehdr_t *e;
  if (open_image(path, &b, &l, &e) != 0) return -ENOENT;

  elf_img_t img;
  mm_t *mm = 0;
  if (elf_load_ex(path, &img, &mm) != 0) return -ENOMEM;
  uint64_t sp = build_initial_stack(mm->cr3, argc, argv, &img, path);
  if (!sp) {
    serial_puts("[elf] ");
    serial_puts(path);
    serial_puts(": user stack allocation failed\n");
    return -ENOMEM;
  }
  *entry_out = img.entry;
  *mm_out = mm;
  *sp_out = sp;
  return 0;
}
