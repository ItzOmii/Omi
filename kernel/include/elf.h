/* ELF64 user-program loader. See kernel/elf.c. */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include "task.h"

/* Load a boot module (matched by path suffix) into a fresh user address space.
 * Returns 0 and fills entry/cr3 on success. */

/* elf_load + task_spawn_user, with `arg` delivered in the program's rdi. */
int ring3_spawn_elf(const char *name, const char *path, uint64_t arg);

/* The limits an execve argument block is copied under. elf.c builds the stack the
 * same way for a spawn and for an exec, so the numbers live in one place. */
#define ELF_ARG_MAX 16
#define ELF_ARG_BLK 2048

/* Load `path` into a space of its own and build its stack, but do not start it.
 * execve is the only caller: starting a process and replacing one differ only in
 * who ends up attached to the result. Returns 0 and fills the outputs, -ENOENT
 * when no program of that name is on the medium, -ENOMEM when it is there but
 * could not be loaded. On -ENOMEM the half-built space is released; on success
 * the caller owns *mm_out. */
int elf_prepare_image(const char *path, int argc, const char *const *argv,
                      uint64_t *entry_out, mm_t **mm_out, uint64_t *sp_out);
int ring3_spawn_elf_argv(const char *name, const char *path, int argc, const char *const *argv);

#endif
