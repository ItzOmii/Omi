/*
 * Minimal IDT and PIC setup (polling-focused kernel)
 */

#include "../include/idt.h"
#include "../include/gui.h"   /* serial_puts: the only way a fault can talk */
#include "../include/lapic.h"   /* the APIC vector, above the PIC range */
#include "../include/bt.h"      /* the frame pointer walk */

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
 * Every exception vector used to point at a stub that did nothing, which on a
 * fault with an error code left the stack unbalanced: the iretq returned to
 * whatever was under the error code and the machine triple-faulted with no output
 * at all. Then it printed one line - vector, error code, rip, cr2 - and stopped,
 * which says what happened and nothing about where.
 *
 * Now every exception lands in one place that saves all fifteen general
 * registers, names the running task, and walks the frame pointers out to the
 * caller chain with symbols attached. The CPU's own frame (error code, rip, cs,
 * rflags, rsp, ss) sits on top of the saved registers, so the two together are one
 * struct and the order of the pushes below is the order of the fields.
 *
 * The stubs are naked and never return: a fault is the end of this CPU's run, and
 * printing is all that is left to do with it.
 */
typedef struct {
  /* Pushed by the stub, in this order - last pushed is lowest in memory. */
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rbp, rsi, rdi, rdx, rcx, rbx, rax;
  /* Pushed by the CPU. */
  uint64_t err, rip, cs, rflags, rsp, ss;
} fault_ctx_t;

static const char *fault_name(uint64_t v) {
  switch (v) {
    case 0:  return "divide by zero";
    case 1:  return "debug";
    case 2:  return "non-maskable interrupt";
    case 3:  return "breakpoint";
    case 4:  return "overflow";
    case 5:  return "bound range";
    case 6:  return "invalid opcode";
    case 7:  return "device not available";
    case 8:  return "double fault";
    case 10: return "invalid tss";
    case 11: return "segment not present";
    case 12: return "stack segment";
    case 13: return "general protection";
    case 14: return "page fault";
    case 16: return "x87 floating point";
    case 17: return "alignment check";
    case 18: return "machine check";
    case 19: return "simd floating point";
    case 21: return "control protection";
    default: return "exception";
  }
}

/* Four registers to a line: sixteen numbers on one line is worse than
 * unreadable, it is unsearchable. */
static void reg4(const char *n0, uint64_t v0, const char *n1, uint64_t v1,
                 const char *n2, uint64_t v2, const char *n3, uint64_t v3) {
  serial_puts("[fault] ");
  serial_puts(n0); serial_puts("="); serial_puthex(v0); serial_puts(" ");
  serial_puts(n1); serial_puts("="); serial_puthex(v1); serial_puts(" ");
  serial_puts(n2); serial_puts("="); serial_puthex(v2); serial_puts(" ");
  serial_puts(n3); serial_puts("="); serial_puthex(v3); serial_puts("\n");
}

static void fault_report(fault_ctx_t *c, uint64_t vector) {
  serial_puts("\n[fault] ");
  serial_puts(fault_name(vector));
  serial_puts(" vector=");
  serial_puthex(vector);
  serial_puts(" err=");
  serial_puthex(c->err);
  if (vector == 14) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    serial_puts(" cr2=");
    serial_puthex(cr2);
  }
  serial_puts(" apic=");
  serial_puthex((uint64_t)lapic_this_id());
  serial_puts("\n");

  bt_who();

  reg4("rax", c->rax, "rbx", c->rbx, "rcx", c->rcx, "rdx", c->rdx);
  reg4("rsi", c->rsi, "rdi", c->rdi, "rbp", c->rbp, "rsp", c->rsp);
  reg4("r8", c->r8, "r9", c->r9, "r10", c->r10, "r11", c->r11);
  reg4("r12", c->r12, "r13", c->r13, "r14", c->r14, "r15", c->r15);
  serial_puts("[fault] rip=0x");
  serial_puthex(c->rip);
  serial_puts(" cs=");
  serial_puthex(c->cs);
  serial_puts(" rflags=");
  serial_puthex(c->rflags);
  serial_puts("\n");

  /* A fault taken from ring 3 has a user-mode rbp, which leads nowhere useful;
   * say so rather than printing a walk through the user stack as if it were
   * kernel frames. Killing just that task is the right answer and is not what
   * this does yet - the kernel stops either way, which is why this is a
   * limitation and not a feature. */
  if (c->cs & 3) {
    serial_puts("[fault] taken from user mode - no kernel backtrace from here\n");
  } else {
    bt_print(c->rbp, c->rip);
  }
}

/* Never returns. Called only from the naked stubs above, which is why it has to be
 * marked used and to keep external linkage: from C's point of view nothing calls
 * it, and the compiler is entitled to drop a static function like that. */
__attribute__((used)) void fault_enter(fault_ctx_t *c, uint64_t vector) {
  fault_report(c, vector);
  serial_puts("[fault] halted\n");
  for (;;) __asm__ volatile("cli; hlt");
}

/* Fifteen pushes and a call, so that fault_enter sees one contiguous struct. The
 * stack is aligned by hand before the call: the CPU's frame is 40 or 48 bytes and
 * fifteen registers is 120, so the sum is not 16-byte aligned and the ABI wants it
 * to be. Nothing returns, so moving rsp is free. */
__attribute__((used)) void fault_enter(fault_ctx_t *c, uint64_t vector);

#define PUSH_GPRS                                                              \
  "pushq %rax\n\t pushq %rbx\n\t pushq %rcx\n\t pushq %rdx\n\t"                \
  "pushq %rdi\n\t pushq %rsi\n\t pushq %rbp\n\t"                               \
  "pushq %r8\n\t  pushq %r9\n\t  pushq %r10\n\t pushq %r11\n\t"                \
  "pushq %r12\n\t pushq %r13\n\t pushq %r14\n\t pushq %r15\n\t"

#define FAULT_STUB_EC(fn, vec)                                                 \
  __attribute__((naked)) static void fn(void) {                                \
    __asm__ volatile(PUSH_GPRS                                                  \
                     "movq %rsp, %rdi\n\t movq $" #vec ", %rsi\n\t"             \
                     "andq $-16, %rsp\n\t call fault_enter\n\t"                 \
                     "1: cli\n\t hlt\n\t jmp 1b\n\t");                          \
  }

#define FAULT_STUB(fn, vec)                                                    \
  __attribute__((naked)) static void fn(void) {                                \
    __asm__ volatile("pushq $0\n\t" PUSH_GPRS                                   \
                     "movq %rsp, %rdi\n\t movq $" #vec ", %rsi\n\t"             \
                     "andq $-16, %rsp\n\t call fault_enter\n\t"                 \
                     "1: cli\n\t hlt\n\t jmp 1b\n\t");                          \
  }

FAULT_STUB(isr_exc_de, 0)
FAULT_STUB(isr_exc_db, 1)
FAULT_STUB(isr_exc_nmi, 2)
FAULT_STUB(isr_exc_bp, 3)
FAULT_STUB(isr_exc_of, 4)
FAULT_STUB(isr_exc_br, 5)
FAULT_STUB(isr_exc_ud, 6)
FAULT_STUB(isr_exc_nm, 7)
FAULT_STUB(isr_exc_mf, 16)
FAULT_STUB(isr_exc_mc, 18)
FAULT_STUB(isr_exc_xm, 19)
FAULT_STUB_EC(isr_exc_ts, 10)
FAULT_STUB_EC(isr_exc_np, 11)
FAULT_STUB_EC(isr_exc_ss, 12)
FAULT_STUB_EC(isr_exc_gp, 13)
FAULT_STUB_EC(isr_exc_pf, 14)
FAULT_STUB_EC(isr_exc_ac, 17)
FAULT_STUB_EC(isr_exc_cp, 21)

/* A double fault is reported the same way, but with one extra byte to the debug
 * console first: the frame it arrives on is the only one left, and if anything in
 * the reporting path faults again the log should at least show that it got here. */
__attribute__((naked)) static void isr_exc_df(void) {
  __asm__ volatile("movb $0x44, %al\n\t outb %al, $0xE9\n\t" PUSH_GPRS
                   "movq %rsp, %rdi\n\t movq $8, %rsi\n\t"
                   "andq $-16, %rsp\n\t call fault_enter\n\t"
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

  /* Exceptions: every vector that means "this cpu is in trouble" now reports
   * instead of pointing at an empty stub. */
  idt_set_gate(0, isr_exc_de);
  idt_set_gate(1, isr_exc_db);
  idt_set_gate(2, isr_exc_nmi);
  idt_set_gate(3, isr_exc_bp);
  idt_set_gate(4, isr_exc_of);
  idt_set_gate(5, isr_exc_br);
  idt_set_gate(6, isr_exc_ud);
  idt_set_gate(7, isr_exc_nm);
  idt_set_gate(8, isr_exc_df);
  idt_set_gate(10, isr_exc_ts);
  idt_set_gate(11, isr_exc_np);
  idt_set_gate(12, isr_exc_ss);
  idt_set_gate(13, isr_exc_gp);
  idt_set_gate(14, isr_exc_pf);
  idt_set_gate(16, isr_exc_mf);
  idt_set_gate(17, isr_exc_ac);
  idt_set_gate(18, isr_exc_mc);
  idt_set_gate(19, isr_exc_xm);
  idt_set_gate(21, isr_exc_cp);

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

