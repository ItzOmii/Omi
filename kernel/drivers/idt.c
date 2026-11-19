/*
 * Minimal IDT and PIC setup (polling-focused kernel)
 */

#include "../include/idt.h"
#include "../include/gui.h"   /* serial_puts: the only way a fault can talk */
#include "../include/lapic.h"   /* the APIC vector, above the PIC range */

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI 0x20

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

typedef struct {
  uint16_t offset_low;
  uint16_t selector;
  uint8_t ist;
  uint8_t type_attr;
  uint16_t offset_mid;
  uint32_t offset_high;
  uint32_t zero;
} __attribute__((packed)) idt_entry_t;

static idt_entry_t idt[256];
static isr_handler_t isr_handlers[256];

static void idt_handle_irq(uint8_t irq, interrupt_frame_t *frame) {
  uint8_t vector = (uint8_t)(0x20 + irq);
  if (isr_handlers[vector]) {
    isr_handlers[vector](frame);
  } else {
    UNUSED(frame);
  }
  pic_send_eoi(irq);
}

#define DEFINE_IRQ_HANDLER(n)                                                   \
  __attribute__((interrupt)) static void isr_irq##n(interrupt_frame_t *frame) { \
    idt_handle_irq((uint8_t)(n), frame);                                        \
  }

DEFINE_IRQ_HANDLER(0)
DEFINE_IRQ_HANDLER(1)
DEFINE_IRQ_HANDLER(2)
DEFINE_IRQ_HANDLER(3)
DEFINE_IRQ_HANDLER(4)
DEFINE_IRQ_HANDLER(5)
DEFINE_IRQ_HANDLER(6)
DEFINE_IRQ_HANDLER(7)
DEFINE_IRQ_HANDLER(8)
DEFINE_IRQ_HANDLER(9)
DEFINE_IRQ_HANDLER(10)
DEFINE_IRQ_HANDLER(11)
DEFINE_IRQ_HANDLER(12)
DEFINE_IRQ_HANDLER(13)
DEFINE_IRQ_HANDLER(14)
DEFINE_IRQ_HANDLER(15)

/* Install any gate: `dpl3` marks it callable from ring 3 (int 0x80), which is
 * the difference between a trap a user task may take and one it may not. */
void idt_set_gate_raw(uint8_t vector, void *handler, int dpl3) {
  uint64_t addr = (uint64_t)handler;
  idt[vector].offset_low = addr & 0xFFFF;
  idt[vector].selector = 0x08;
  idt[vector].ist = 0;
  idt[vector].type_attr = (uint8_t)(0x8E | (dpl3 ? 0x60 : 0x00));
  idt[vector].offset_mid = (addr >> 16) & 0xFFFF;
  idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
  idt[vector].zero = 0;
}

/* An application processor starts with an empty IDT, so its first fault is a
 * triple fault and the whole machine resets. Handing it this one turns that into
 * a fault dump on the serial line, which is the difference between debugging and
 * guessing. */
static idt_ptr_t idtr_saved;
void idt_get_idtr(idt_ptr_t *out) { *out = idtr_saved; }

static void idt_set_gate(uint8_t vector, void *handler) {
  idt_set_gate_raw(vector, handler, 0);
}

/* Our own APIC vector. It is not a PIC IRQ, so it ends with the APIC's EOI and
 * not the 8259's: sending the wrong one leaves the unit thinking the interrupt
 * is still in service and the next one is never delivered. */
__attribute__((interrupt)) static void isr_apic(interrupt_frame_t *frame) {
  if (isr_handlers[LAPIC_VECTOR_IPI]) isr_handlers[LAPIC_VECTOR_IPI](frame);
  lapic_eoi();
}

/* ------------------------------------------------------------ fault reporting
 *
 * Until now every exception vector pointed at a stub that did nothing, which on
 * a fault with an error code left the stack unbalanced: the iretq returned to
 * whatever was under the error code and the machine triple-faulted with no
 * output at all. A core that faults deserves one line on the serial port naming
 * the vector, the error code and the address, and then it can stop.
 *
 * The frame a fault leaves on the stack is the CPU's own: error code first (for
 * the vectors that push one), then the interrupted context.
 */
typedef struct {
  uint64_t err, rip, cs, rflags, rsp, ss;
} fault_frame_t;

static const char *fault_name(uint64_t v) {
  switch (v) {
    case 0:  return "divide by zero";
    case 6:  return "invalid opcode";
    case 8:  return "double fault";
    case 13: return "general protection";
    case 14: return "page fault";
    case 17: return "alignment check";
    case 21: return "control protection";
    default: return "exception";
  }
}

static void fault_report(fault_frame_t *f, uint64_t vector) {
  serial_puts("[fault] ");
  serial_puts(fault_name(vector));
  serial_puts(" vector=0x");
  serial_puthex(vector);
  serial_puts(" err=0x");
  serial_puthex(f->err);
  serial_puts(" rip=0x");
  serial_puthex(f->rip);
  if (vector == 14) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    serial_puts(" cr2=0x");
    serial_puthex(cr2);
  }
  serial_puts(" apic=");
  serial_puthex((uint64_t)lapic_this_id());
  serial_puts("\n");
}

/* Naked so the compiler does not build a frame of its own over the CPU's. */
__attribute__((naked)) static void isr_fault_df(void) {
  __asm__ volatile("movb $0x44, %al\n\t outb %al, $0xE9\n\t"   /* 'D' to debugcon first */
                   "pushq $8\n\t movq %rsp, %rdi\n\t call fault_enter\n\t"
                   "1: cli\n\t hlt\n\t jmp 1b\n\t");
}
__attribute__((naked)) static void isr_fault_gp(void) {
  __asm__ volatile("movb $0x47, %al\n\t outb %al, $0xE9\n\t"   /* 'G' to debugcon first */
                   "pushq $13\n\t movq %rsp, %rdi\n\t call fault_enter\n\t"
                   "1: cli\n\t hlt\n\t jmp 1b\n\t");
}
__attribute__((naked)) static void isr_fault_pf(void) {
  __asm__ volatile("movb $0x50, %al\n\t outb %al, $0xE9\n\t"   /* 'P' to debugcon first */
                   "pushq $14\n\t movq %rsp, %rdi\n\t call fault_enter\n\t"
                   "1: cli\n\t hlt\n\t jmp 1b\n\t");
}

__attribute__((interrupt)) static void isr_default(interrupt_frame_t *frame) {
  UNUSED(frame);
}

void idt_register_handler(uint8_t vector, isr_handler_t handler) {
  if (vector < 256) {
    isr_handlers[vector] = handler;
  }
}

void idt_init(void) {
  for (int i = 0; i < 256; i++) {
    idt_set_gate((uint8_t)i, isr_default);
    isr_handlers[i] = 0;
  }

  /* IRQ vectors 0x20 - 0x2F */
  idt_set_gate(0x20, isr_irq0);
  idt_set_gate(0x21, isr_irq1);
  idt_set_gate(0x22, isr_irq2);
  idt_set_gate(0x23, isr_irq3);
  idt_set_gate(0x24, isr_irq4);
  idt_set_gate(0x25, isr_irq5);
  idt_set_gate(0x26, isr_irq6);
  idt_set_gate(0x27, isr_irq7);
  idt_set_gate(0x28, isr_irq8);
  idt_set_gate(0x29, isr_irq9);
  idt_set_gate(0x2A, isr_irq10);
  idt_set_gate(0x2B, isr_irq11);
  idt_set_gate(0x2C, isr_irq12);
  idt_set_gate(0x2D, isr_irq13);
  idt_set_gate(0x2E, isr_irq14);
  idt_set_gate(0x2F, isr_irq15);

  /* Above the PIC's range, so a stray IRQ cannot land on it. */
  idt_set_gate(LAPIC_VECTOR_IPI, isr_apic);

  /* Faults report and stop; the do-nothing stub left the stack unbalanced. */
  idt_set_gate(8,  isr_fault_df);
  idt_set_gate(13, isr_fault_gp);
  idt_set_gate(14, isr_fault_pf);

  idt_ptr_t idtr;
  idtr.limit = (uint16_t)(sizeof(idt) - 1);
  idtr.base = (uint64_t)&idt[0];

  __asm__ volatile("lidt %0" : : "m"(idtr));
  idtr_saved = idtr;
}

void pic_init(void) {
  uint8_t a1 = inb(PIC1_DATA);
  uint8_t a2 = inb(PIC2_DATA);

  /* Start initialization */
  outb(PIC1_CMD, 0x11);
  outb(PIC2_CMD, 0x11);

  /* Remap offsets: IRQ0-7 -> 0x20, IRQ8-15 -> 0x28 */
  outb(PIC1_DATA, 0x20);
  outb(PIC2_DATA, 0x28);

  /* Tell PICs about cascade */
  outb(PIC1_DATA, 0x04);
  outb(PIC2_DATA, 0x02);

  /* 8086 mode */
  outb(PIC1_DATA, 0x01);
  outb(PIC2_DATA, 0x01);

  /* Restore masks */
  outb(PIC1_DATA, a1);
  outb(PIC2_DATA, a2);
}

void pic_send_eoi(uint8_t irq) {
  if (irq >= 8) {
    outb(PIC2_CMD, PIC_EOI);
  }
  outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
  uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
  uint8_t value = inb(port) | (uint8_t)(1U << (irq & 7));
  outb(port, value);
}

void pic_clear_mask(uint8_t irq) {
  uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
  uint8_t value = inb(port) & (uint8_t)~(1U << (irq & 7));
  outb(port, value);
}

/* Called from the naked stubs with the pushed vector at (%rdi). */
void fault_enter(uint64_t *sp) {
  uint64_t vector = sp[0];
  fault_report((fault_frame_t *)&sp[1], vector);
  for (;;) __asm__ volatile("cli; hlt");
}
