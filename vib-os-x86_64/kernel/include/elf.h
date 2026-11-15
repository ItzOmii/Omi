/* ELF64 user-program loader. See kernel/elf.c. */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* Load a boot module (matched by path suffix) into a fresh user address space.
 * Returns 0 and fills entry/cr3 on success. */
int elf_load(const char *path_suffix, uint64_t *entry_out, uint64_t *cr3_out);

/* elf_load + task_spawn_user, with `arg` delivered in the program's rdi. */
int ring3_spawn_elf(const char *name, const char *path, uint64_t arg);

#endif
