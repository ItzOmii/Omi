/* ELF64 loader for user programs.
 *
 * A program is a static, position-dependent ELF64 executable carried on the
 * boot medium as a Limine module (iso_root/apps/init.elf, built from
 * kernel/user/init.c). There is no dynamic linker and no relocation support:
 * the program is linked to run at RING3_USER_BASE, and every PT_LOAD segment is
 * copied into a fresh address space at its own p_vaddr.
 *
 * What this deliberately does not do yet: filesystem lookup (the path is a
 * suffix match on the boot modules), shared libraries and relocations (step 6),
 * and demand paging (all pages are filled up front). */

#include "include/elf.h"
#include "include/limine_modules.h"
#include "include/ring3.h"
#include "include/task.h"
#include "include/types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t v);

#define PT_LOAD   1
#define ET_EXEC   2
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

static void log_hex(const char *label, uint64_t v) {
  serial_puts(label);
  serial_puthex(v);
  serial_puts("\n");
}

typedef struct {
  uint64_t entry, phdr, phent, phnum;
} elf_img_t;

int elf_load_ex(const char *path_suffix, elf_img_t *img, mm_t **mm_out) {
  void *blob = 0;
  uint64_t len = 0;
  if (!meep_module_find(path_suffix, &blob, &len) || !blob || len < sizeof(elf64_ehdr_t)) {
    serial_puts("[elf] no such module: ");
    serial_puts(path_suffix ? path_suffix : "(null)");
    serial_puts("\n");
    return -1;
  }

  const uint8_t *base = (const uint8_t *)blob;
  const elf64_ehdr_t *eh = (const elf64_ehdr_t *)base;

  if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
      eh->ident[2] != 'L' || eh->ident[3] != 'F') {
    serial_puts("[elf] not an ELF file\n");
    return -1;
  }
  if (eh->ident[4] != 2) { serial_puts("[elf] not 64-bit\n"); return -1; }
  if (eh->type != ET_EXEC) { serial_puts("[elf] not a static executable\n"); return -1; }
  if (eh->machine != EM_X86_64) { serial_puts("[elf] wrong machine\n"); return -1; }
  if (eh->phentsize != sizeof(elf64_phdr_t) || eh->phnum == 0) {
    serial_puts("[elf] bad program header table\n");
    return -1;
  }

  mm_t *mm = ring3_new_mm();
  if (!mm) { serial_puts("[elf] no address space\n"); return -1; }
  uint64_t cr3 = mm->cr3;
  if (!cr3) { serial_puts("[elf] out of page-table frames\n"); return -1; }

  int loaded = 0;
  uint64_t image_base = ~0ULL;
  for (uint16_t i = 0; i < eh->phnum; i++) {
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(base + eh->phoff + (uint64_t)i * eh->phentsize);
    if (ph->type != PT_LOAD || ph->memsz == 0) continue;

    if (ph->vaddr < RING3_USER_MIN || ph->vaddr + ph->memsz > RING3_LIMIT) {
      serial_puts("[elf] segment outside the user window, refusing\n");
      return -1;
    }
    if (ph->filesz > ph->memsz || ph->offset + ph->filesz > len) {
      serial_puts("[elf] segment sizes lie, refusing\n");
      return -1;
    }

    /* The BSS tail (memsz - filesz) has to read as zero: mapping fresh pages
     * and copying only filesz bytes gives exactly that. */
    uint64_t flags = RING3_P | RING3_U | ((ph->flags & 0x2) ? RING3_W : 0);
    if (ring3_map_copy(cr3, ph->vaddr, base + ph->offset, ph->filesz, flags) != 0) {
      serial_puts("[elf] could not map a segment\n");
      return -1;
    }
    if (ph->memsz > ph->filesz) {
      if (ring3_map_copy(cr3, ph->vaddr + ph->filesz, 0, ph->memsz - ph->filesz, flags) != 0) {
        serial_puts("[elf] could not map the bss tail\n");
        return -1;
      }
    }
    if (ph->vaddr < image_base) image_base = ph->vaddr;
    loaded++;
  }

  if (!loaded) { serial_puts("[elf] no loadable segments\n"); return -1; }
  if (ring3_make_stack(cr3) != 0) { serial_puts("[elf] no stack frames\n"); return -1; }
  if (eh->entry < RING3_USER_MIN || eh->entry >= RING3_LIMIT) {
    serial_puts("[elf] entry point outside the user window\n");
    return -1;
  }

  log_hex("[elf] loaded ", len);
  serial_puts("[elf]   bytes from ");
  serial_puts(path_suffix);
  serial_puts(", entry ");
  serial_puthex(eh->entry);
  serial_puts(", cr3 ");
  serial_puthex(cr3);
  serial_puts("\n");

  /* AT_PHDR has to be an address the program can read, so it is the program
   * header table as mapped rather than as it sits in the module: base of the
   * image plus the file offset. Only correct for ET_EXEC, which is all this
   * loader accepts - a PIE would need the load bias added, and step 6 is where
   * that arrives. */
  img->entry = eh->entry;
  img->phdr  = image_base + eh->phoff;
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
 * rest, so this costs them nothing. A musl program reads all of it, and three of
 * the auxv entries are not optional:
 *
 *   AT_PHDR/AT_PHENT/AT_PHNUM - the static startup walks them to find PT_TLS and
 *   set up the thread pointer. Without them %fs is never set, and the first
 *   thread-local access is a fault in a program that looked correct.
 *   AT_PAGESZ - becomes libc.page_size, which malloc and stdio divide by.
 *   AT_RANDOM - seeds the stack canary. Zeros would make the canary check pass
 *   trivially, which is worse than not having one: say so and move on.
 *
 * rsp ends up 16-byte aligned, which is what the ABI promises at _start.
 */
#define ARG_MAX 16
#define ARG_BLK 2048

static uint64_t build_initial_stack(uint64_t cr3, int argc, const char *const *argv,
                                    const elf_img_t *img) {
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

  uint64_t aux[24];
  int na = 0;
  aux[na++] = 3;  aux[na++] = img->phdr;          /* AT_PHDR  */
  aux[na++] = 4;  aux[na++] = img->phent;         /* AT_PHENT */
  aux[na++] = 5;  aux[na++] = img->phnum;         /* AT_PHNUM */
  aux[na++] = 6;  aux[na++] = 4096;               /* AT_PAGESZ */
  aux[na++] = 9;  aux[na++] = img->entry;         /* AT_ENTRY */
  aux[na++] = 11; aux[na++] = 0;                  /* AT_UID  */
  aux[na++] = 12; aux[na++] = 0;                  /* AT_EUID */
  aux[na++] = 13; aux[na++] = 0;                  /* AT_GID  */
  aux[na++] = 14; aux[na++] = 0;                  /* AT_EGID */
  aux[na++] = 23; aux[na++] = 0;                  /* AT_SECURE: not setuid */
  aux[na++] = 25; aux[na++] = random_at;          /* AT_RANDOM */
  aux[na++] = 31; aux[na++] = argc > 0 ? argp[0] : 0;  /* AT_EXECFN */

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
  uint64_t sp = build_initial_stack(mm->cr3, argc, argv, &img);
  if (!sp) {
    /* Two causes, one symptom: argv bigger than the block, or the stack page
     * could not be written. Say which, and which program. */
    serial_puts("[elf] ");
    serial_puts(name);
    serial_puts(": user stack allocation failed (argv too large or page refused)\n");
    return -1;
  }
  int id = task_spawn_user_mm(name, img.entry, sp, mm, (uint64_t)argc);
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
  uint64_t sp = build_initial_stack(mm->cr3, 0, 0, &img);
  if (!sp) {
    serial_puts("[elf] ");
    serial_puts(name);
    serial_puts(": user stack allocation failed\n");
    return -1;
  }
  int id = task_spawn_user_mm(name, img.entry, sp, mm, arg);
  if (id < 0) {
    serial_puts("[elf] no task slot for ");
    serial_puts(name);
    serial_puts("\n");
  }
  return id;
}
