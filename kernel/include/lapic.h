/*
 * The local APIC: the register window each core owns.
 *
 * Three things live here that the PIC cannot do. It has an id that names the
 * core, so an interrupt can be addressed to one CPU rather than broadcast. It has
 * an end-of-interrupt of its own. And it can interrupt *itself* and other cores,
 * which is how one core tells another that its scheduler has work.
 *
 * The 8259 stays in place for device interrupts: it delivers to the bootstrap
 * processor exactly as before, while the APIC carries the inter-processor side.
 */

#ifndef _LAPIC_H
#define _LAPIC_H

#include "types.h"

#define LAPIC_VECTOR_IPI 0x30    /* our timed self-interrupt / IPI vector */

void     lapic_init(void);
int      lapic_ready(void);              /* enabled and reachable             */
uint32_t lapic_id(void);                 /* this core's APIC id               */
uint32_t lapic_version(void);
uint32_t lapic_base(void);               /* MMIO base we mapped               */
void     lapic_eoi(void);
void     lapic_self_ipi(uint8_t vector); /* interrupt this very core          */
uint32_t lapic_this_id(void);           /* id of whichever core is asking     */
void     lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void     lapic_send_init(uint32_t apic_id);          /* reset it, hold it       */
void     lapic_send_init_deassert(uint32_t apic_id);
void     lapic_send_startup(uint32_t apic_id, uint8_t vector); /* run vec:0     */
uint32_t lapic_ipi_count(void);          /* how many IPIs were taken          */
uint32_t lapic_icr_low(void);            /* last ICR low word, for bring-up   */
uint32_t lapic_icr_high(void);
uint32_t lapic_esr(void);                /* and clear the error status        */

#endif /* _LAPIC_H */