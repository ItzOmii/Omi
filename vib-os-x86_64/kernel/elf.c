/* ELF64 loader for user programs.
 *
 * A program is a static, position-dependent ELF64 executable carried on the
 * boot medium as a Limine module (iso_root/apps/init.elf, built from
 * kernel/user/init.c). There is no dynamic linker and no relocation support:
 * the program is linked to run at RING3_USER_BASE, and every PT_LOAD segment is
 * copied into a fresh address space at its own p_vaddr.
 *
 * What this deliberately does not do yet: filesystem lookup (the path is a
 * suffix match on the boot modules), argv/envp (the entry gets a single integer
 * in rdi), and demand paging (all pages are filled up front). */

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

int elf_load(const char *path_suffix, uint64_t *entry_out, mm_t **mm_out) {
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
  for (uint16_t i = 0; i < eh->phnum; i++) {
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(base + eh->phoff + (uint64_t)i * eh->phentsize);
    if (ph->type != PT_LOAD || ph->memsz == 0) continue;

    if (ph->vaddr < RING3_USER_BASE || ph->vaddr + ph->memsz > RING3_LIMIT) {
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
    loaded++;
  }

  if (!loaded) { serial_puts("[elf] no loadable segments\n"); return -1; }
  if (ring3_make_stack(cr3) != 0) { serial_puts("[elf] no stack frames\n"); return -1; }
  if (eh->entry < RING3_USER_BASE || eh->entry >= RING3_LIMIT) {
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

  *entry_out = eh->entry;
  *mm_out = mm;
  return 0;
}

/* Lay out the SysV initial stack: the strings at the top, then argc, argv[],
 * a NULL, and an empty envp, with rsp 16-byte aligned. This is the whole of the
 * process-entry ABI - a program that reads argc from (%rsp) and argv from 8(%rsp)
 * needs nothing else from the kernel to start. */
#define ARG_MAX 16
#define ARG_BLK 2048

static uint64_t build_initial_stack(uint64_t cr3, int argc, const char *const *argv) {
  static char blk[ARG_BLK];
  static uint64_t argp[ARG_MAX];
  for (int i = 0; i < ARG_BLK; i++) blk[i] = 0;
  if (argc > ARG_MAX) argc = ARG_MAX;

  uint64_t base = RING3_LIMIT - ARG_BLK;
  uint64_t off = ARG_BLK;
  for (int i = argc - 1; i >= 0; i--) {
    uint64_t n = 1;
    while (argv[i][n - 1]) n++;
    if (n > off) return 0;
    off -= n;
    for (uint64_t k = 0; k < n; k++) blk[off + k] = argv[i][k];
    argp[i] = base + off;
  }

  uint64_t table = (uint64_t)(argc + 3) * 8;      /* argc, argv[], NULL, envp NULL */
  uint64_t sp = (base + off - table) & ~15ULL;
  uint64_t p = sp - base;
  uint64_t *t = (uint64_t *)(void *)(blk + p);
  t[0] = (uint64_t)argc;
  for (int i = 0; i < argc; i++) t[1 + i] = argp[i];
  t[1 + argc] = 0;
  t[2 + argc] = 0;

  if (ring3_copy_to_mm(cr3, base, blk, ARG_BLK) != 0) return 0;
  return sp;
}

/* Load `path` and start it with a real argv. */
int ring3_spawn_elf_argv(const char *name, const char *path, int argc, const char *const *argv) {
  uint64_t entry = 0;
  mm_t *mm = 0;
  if (elf_load(path, &entry, &mm) != 0) return -1;
  uint64_t sp = build_initial_stack(mm->cr3, argc, argv);
  if (!sp) {
    /* Two causes, one symptom: argv bigger than the block, or the stack page
     * could not be written. Say which, and which program. */
    serial_puts("[elf] ");
    serial_puts(name);
    serial_puts(": user stack allocation failed (argv too large or page refused)\n");
    return -1;
  }
  int id = task_spawn_user_mm(name, entry, sp, mm, (uint64_t)argc);
  if (id < 0) {
    serial_puts("[elf] no task slot for ");
    serial_puts(name);
    serial_puts("\n");
  }
  return id;
}

/* Load `path` into a brand new address space and start it. `arg` arrives in the
 * program's rdi, which is how one binary can play two roles in the demo. */
int ring3_spawn_elf(const char *name, const char *path, uint64_t arg) {
  uint64_t entry = 0;
  mm_t *mm = 0;
  if (elf_load(path, &entry, &mm) != 0) return -1;
  int id = task_spawn_user_mm(name, entry, RING3_LIMIT - 16, mm, arg);
  if (id < 0) {
    serial_puts("[elf] no task slot for ");
    serial_puts(name);
    serial_puts("\n");
  }
  return id;
}