/*
 * Spinlocks.
 *
 * Everything the kernel shares today was written for one core, so the lock is
 * the first thing any of it needs. Two things make a naive lock wrong here:
 *
 * - A lock taken with interrupts on can deadlock against its own interrupt
 *   handler on the same core: the handler spins on a lock the interrupted code
 *   still holds, and that core will never release it. Every lock taken on the
 *   kernel's behalf therefore saves and restores IF, not just the flag.
 * - Plain stores are not enough on x86 for entry; `xchg` is atomic with an
 *   implicit lock and is what makes the test-and-set actually exclusive.
 */

#ifndef _SPINLOCK_H
#define _SPINLOCK_H

#include "types.h"

typedef struct {
  volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void cpu_pause(void) { __asm__ volatile("pause"); }

static inline int irq_save(void) {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
  return (int)(flags & 0x200);          /* IF, as it was */
}

static inline void irq_restore(int was_on) {
  if (was_on) __asm__ volatile("sti" : : : "memory");
}

static inline void spin_lock(spinlock_t *l) {
  while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) cpu_pause();
  }
}

static inline void spin_unlock(spinlock_t *l) {
  __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

/* Take the lock with interrupts off; the flag goes back to `irq`. */
static inline int spin_lock_irq(spinlock_t *l, int *irq) {
  *irq = irq_save();
  spin_lock(l);
  return *irq;
}

static inline void spin_unlock_irq(spinlock_t *l, int irq) {
  spin_unlock(l);
  irq_restore(irq);
}

#endif /* _SPINLOCK_H */