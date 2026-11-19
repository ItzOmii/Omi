/*
 * SMP: how many CPUs this machine has, and where they live.
 *
 * Step one of running on more than one core is knowing the cores exist: the
 * MADT lists every processor, its APIC id and whether firmware enabled it, and
 * the same table carries the local APIC's physical address. Nothing here starts
 * a second core; it is the list that starting one will walk.
 */

#ifndef _SMP_H
#define _SMP_H

#include "types.h"

#define SMP_MAX_CPUS 32

void     smp_init(void);
void     smp_start_aps(void);                    /* INIT-SIPI-SIPI each AP       */
void     smp_start_workers(void);                /* one pinned kernel thread each */
void     smp_ap_main(uint64_t cpu);              /* where an AP lands, in C      */
int      smp_cpu_count(void);                    /* processors firmware enabled */
uint32_t smp_cpu_apic_id(int i);                 /* APIC id of the i'th one      */
uint32_t smp_lapic_addr(void);                   /* local APIC MMIO, physical    */
int      smp_lapic_present(void);                /* MADT said there is one       */

#endif /* _SMP_H */