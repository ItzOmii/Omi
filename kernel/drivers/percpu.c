/*
 * Per-CPU state: an array indexed by APIC id, initialised from the MADT.
 *
 * APIC ids are not guaranteed to be dense - firmware may hand out 0, 2, 4 - so
 * the array is indexed by the id itself and each slot carries the dense index
 * for anything that needs one.
 */

#include "../include/percpu.h"
#include "../include/lapic.h"

static percpu_t cpus[SMP_MAX_CPUS];

void percpu_init(void) {
  for (int i = 0; i < SMP_MAX_CPUS; i++) {
    cpus[i].online = 0;
    cpus[i].index = -1;
    cpus[i].apic_id = (uint32_t)i;
    cpus[i].pages_allocated = 0;
    cpus[i].ticks = 0;
    cpus[i].cur_task = -1;
    cpus[i].idle = 0;
    cpus[i].recent[0] = cpus[i].recent[1] = 0;
    cpus[i].busy_ticks = cpus[i].idle_ticks = 0;
  }
  int n = smp_cpu_count();
  for (int i = 0; i < n && i < SMP_MAX_CPUS; i++) {
    uint32_t id = smp_cpu_apic_id(i);
    if (id >= SMP_MAX_CPUS) continue;
    cpus[id].index = i;
    cpus[id].apic_id = id;
    cpus[id].online = 1;
  }
}

/* The APIC id register is per-core, so this answers "which slot am I" on any
 * core without being told. Before the APIC is up there is only one core, so the
 * bootstrap processor's slot is the right answer. */
percpu_t *percpu_self(void) {
  uint32_t id = lapic_this_id();
  if (id < SMP_MAX_CPUS && cpus[id].index >= 0) return &cpus[id];
  return &cpus[0];
}

void percpu_set_cur(int task) { percpu_self()->cur_task = (int32_t)task; }
void percpu_set_idle(int on) { percpu_self()->idle = on ? 1u : 0u; }

/* One bit per tick is the whole trick: 128 bits of history, read in 64-bit
 * halves, so the percentage and the shape of the last second come from the same
 * number. A core that has been up for less than a second shows the samples it
 * really had, which is the honest answer. */
static int bits_in_window(const percpu_t *c, int window) {
  if (window > 64) {
    int hi = window - 64;
    uint64_t mask = (hi >= 64) ? ~0ULL : ((1ULL << hi) - 1);
    return __builtin_popcountll(c->recent[0]) + __builtin_popcountll(c->recent[1] & mask);
  }
  uint64_t mask = (window >= 64) ? ~0ULL : ((1ULL << window) - 1);
  return __builtin_popcountll(c->recent[0] & mask);
}

int percpu_busy_pct(int index) {
  percpu_t *c = percpu_by_index(index);
  if (!c || !c->online) return -1;
  return bits_in_window(c, 100);        /* 100 ticks at 100 Hz = one second */
}

int percpu_cpu_pct(void) {
  int sum = 0, n = 0;
  for (int i = 0; i < smp_cpu_count(); i++) {
    int p = percpu_busy_pct(i);
    if (p < 0) continue;
    sum += p; n++;
  }
  return n ? sum / n : 0;
}

percpu_t *percpu_by_index(int i) {
  uint32_t id = smp_cpu_apic_id(i);
  return (id < SMP_MAX_CPUS) ? &cpus[id] : 0;
}