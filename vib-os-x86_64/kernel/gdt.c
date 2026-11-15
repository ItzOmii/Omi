/* The kernel's own GDT, so interrupts have valid code/data descriptors to load
 * (Limine leaves GDTR pointing at memory it no longer guarantees) and so ring 3
 * has selectors ready. Layout is the usual flat 64-bit one:

 *   0x00 null   0x08 kernel code   0x10 kernel data
 *   0x18 user data   0x20 user code   0x28 TSS

 * The TSS carries rsp0, the stack the cpu switches to when an interrupt takes
 * it from ring 3 into ring 0. */
#include "include/gdt.h"

typedef struct {
  uint16_t limit_low;
  uint16_t base_low;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  gran;
  uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed)) gdtr_t;

typedef struct {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iomap;
} __attribute__((packed)) tss_t;

static gdt_entry_t gdt[7];   /* 6 selectors + the TSS descriptor's second half */
static gdtr_t gdtr;
static tss_t tss;

static void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
  gdt[i].base_low  = (uint16_t)(base & 0xFFFF);
  gdt[i].base_mid  = (uint8_t)((base >> 16) & 0xFF);
  gdt[i].base_high = (uint8_t)((base >> 24) & 0xFF);
  gdt[i].limit_low = (uint16_t)(limit & 0xFFFF);
  gdt[i].gran      = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
  gdt[i].access    = access;
}

void gdt_set_kernel_stack(uint64_t rsp0) { tss.rsp0 = rsp0; }

void gdt_init(void) {
  set_entry(0, 0, 0, 0, 0);
  set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);   /* kernel code  */
  set_entry(2, 0, 0xFFFFF, 0x92, 0xA0);   /* kernel data  */
  set_entry(3, 0, 0xFFFFF, 0xF2, 0xA0);   /* user data    */
  set_entry(4, 0, 0xFFFFF, 0xFA, 0xA0);   /* user code    */

  /* TSS descriptor: 16 bytes wide, so it covers entries 5 and 6. */
  for (int i = 0; i < (int)sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;
  tss.iomap = (uint16_t)sizeof(tss);
  uint64_t tb = (uint64_t)(uintptr_t)&tss;
  set_entry(5, (uint32_t)(tb & 0xFFFFFFFF), (uint32_t)(sizeof(tss) - 1), 0x89, 0x00);
  gdt[6].limit_low = (uint16_t)((tb >> 32) & 0xFFFF);
  gdt[6].base_low  = (uint16_t)((tb >> 48) & 0xFFFF);
  gdt[6].base_mid = 0; gdt[6].base_high = 0;
  gdt[6].access = 0; gdt[6].gran = 0;

  gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
  gdtr.base  = (uint64_t)(uintptr_t)&gdt[0];
  __asm__ volatile("lgdt %0" : : "m"(gdtr));

  /* Reload the segment registers against the new descriptors. */
  __asm__ volatile(
      "movw $0x10, %%ax\n"
      "movw %%ax, %%ds\n"
      "movw %%ax, %%es\n"
      "movw %%ax, %%ss\n"
      /* fs/gs base back the stack canary - leave them alone */
      "pushq $0x08\n"
      "leaq 1f(%%rip), %%rax\n"
      "pushq %%rax\n"
      "lretq\n"
      "1:\n"
      : : : "rax", "memory");

  __asm__ volatile("ltr %0" : : "r"((uint16_t)0x28));
}