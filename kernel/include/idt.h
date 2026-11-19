/*
 * Minimal IDT/PIC interface
 */

#ifndef _IDT_H
#define _IDT_H

#include "types.h"

typedef struct interrupt_frame {
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
} interrupt_frame_t;

typedef void (*isr_handler_t)(interrupt_frame_t *frame);

typedef struct {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed)) idt_ptr_t;

void idt_init(void);
void idt_get_idtr(idt_ptr_t *out);   /* for a core that has none loaded yet */
void idt_register_handler(uint8_t vector, isr_handler_t handler);
void idt_set_gate_raw(uint8_t vector, void *handler, int dpl3);

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif /* _IDT_H */
