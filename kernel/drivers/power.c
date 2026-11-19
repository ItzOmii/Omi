/* Power and lifecycle: switch the machine off, or restart it, without a finger on
 * the button.
 *
 * Everything here is a write to a register some firmware table pointed at, and
 * every one of them can fail silently - the write lands, the machine stays on, and
 * nothing says why. So each path reports what it tried through power_why(), the
 * caller logs it, and the fallbacks are ordered from "the spec says this" to "this
 * works on QEMU" to "this works anywhere".
 *
 * The disk goes first. Not because a drive without FLUSH CACHE loses data on a
 * clean power-off - a drive that is told S5 is not yanked - but because a machine
 * that has decided to stop is the last moment anyone can be sure its writes are on
 * the platter rather than in a buffer with no owner.
 */
#include "../include/power.h"
#include "../include/acpi.h"
#include "../include/ata.h"
#include "../include/gui.h"

static const char *why = "not attempted";

static inline void outb_(uint16_t p, uint8_t v) {
  __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p));
}
static inline void outw_(uint16_t p, uint16_t v) {
  __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p));
}
static inline uint8_t inb_(uint16_t p) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline uint16_t inw_(uint16_t p) {
  uint16_t v;
  __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}

static void spin(uint32_t n) {
  for (volatile uint32_t i = 0; i < n; i++) __asm__ volatile("pause");
}

const char *power_why(void) {
  return why;
}

int power_sync(void) {
  if (!ata_present()) return 0;
  return ata_flush() == 0 ? 0 : -1;
}

/* SCI_EN: bit 0 of PM1a_CNT says the OS owns ACPI and can issue S5 itself. */
static int acpi_enabled(const acpi_fadt_t *f) {
  return (inw_((uint16_t)f->pm1a_cnt_blk) & 1) != 0;
}

static int acpi_enable(const acpi_fadt_t *f) {
  if (!f->smi_cmd) {
    why = "SCI_EN is clear and the FADT has no SMI_CMD to enable ACPI";
    return -1;
  }
  outb_((uint16_t)f->smi_cmd, f->acpi_enable);
  /* Firmware does the SMI and comes back; the spec allows 3 seconds. */
  for (int i = 0; i < 3000000; i++) {
    if (acpi_enabled(f)) return 0;
    spin(4);
  }
  why = "ACPI_ENABLE was written and SCI_EN never set";
  return -1;
}

int power_off(void) {
  const acpi_fadt_t *f = acpi_get_fadt();

  if (power_sync() != 0)
    serial_puts("[power] the disk would not flush - going down anyway\n");

  if (f && f->pm1a_cnt_blk) {
    if (acpi_enabled(f) || acpi_enable(f) == 0) {
      /* SLP_TYP for S5 is a number the DSDT names (\_S5) - 5 on real hardware,
       * 0 in the DSDT that SeaBIOS generates, which is why the "write 0x2000 to
       * 0x604" idiom is everywhere and why writing the spec value alone leaves a
       * QEMU machine running. Reading it properly means an AML interpreter; trying
       * both and stopping at whichever works is what a machine with no AML can do.
       * SLP_EN (bit 13) is the half the hardware insists on either way. */
      static const uint16_t s5_types[2] = {5, 0};
      for (int t = 0; t < 2; t++) {
        uint16_t s5 = (uint16_t)((s5_types[t] << 10) | (1u << 13));
        outw_((uint16_t)f->pm1a_cnt_blk, s5);
        if (f->pm1b_cnt_blk) outw_((uint16_t)f->pm1b_cnt_blk, s5);
        /* The store is not synchronous: the chipset reacts to it and a machine
         * that is going down does so within a frame or two. Still here means this
         * encoding was not the one. */
        spin(20000000);
      }
      why = "S5 was written to PM1a_CNT with SLP_TYP 5 and 0, and the machine stayed on";
    }
  } else {
    why = "no FADT, so no PM1a control block";
  }

  serial_puts("[power] ");
  serial_puts(why);
  serial_puts(" - trying qemu's own exits\n");

  /* QEMU answers these even when the emulated FADT path did not, and they are
   * harmless on real hardware (0x604 and 0xB004 are unused ports there). */
  outw_(0x604, 0x2000);   /* qemu >= 2.0 ACPI PM */
  outw_(0xB004, 0x2000);  /* older qemu / bochs */
  outb_(0x501, 0);        /* isa-debug-exit, only if the VM has that device */
  spin(2000000);

  why = "nothing switched the machine off";
  return -1;
}

static int gas_write(const acpi_gas_t *g, uint8_t value) {
  if (!g->address) return -1;
  switch (g->space) {
    case 0: /* memory */
      *(volatile uint8_t *)(uintptr_t)g->address = value;
      return 0;
    case 1: /* I/O */
      outb_((uint16_t)g->address, value);
      return 0;
    default:
      return -1;   /* PCI config space reset registers: not in this kernel */
  }
}

int power_reset(void) {
  const acpi_fadt_t *f = acpi_get_fadt();

  if (power_sync() != 0)
    serial_puts("[power] the disk would not flush - resetting anyway\n");

  /* The reset register is a revision-2 field: reading it out of a 116-byte table
   * would be reading whatever follows in memory. */
  if (f && f->header.length >= 129 && (f->flags & ACPI_FADT_RESET_REG_SUP) &&
      f->reset_reg.address) {
    serial_puts("[power] reset through the FADT reset register\n");
    gas_write(&f->reset_reg, f->reset_value);
    spin(2000000);
    why = "the FADT reset register was written and the machine stayed up";
  } else {
    why = "no FADT reset register";
  }

  /* The keyboard controller's reset line has worked since 1984 and still does:
   * 0xFE on port 0x64 pulses it. Wait for the input buffer first, or the command
   * is dropped by a controller that is still busy. */
  serial_puts("[power] ");
  serial_puts(why);
  serial_puts(" - pulsing the keyboard controller\n");
  for (int i = 0; i < 100000; i++) {
    if (!(inb_(0x64) & 0x02)) break;
    spin(4);
  }
  outb_(0x64, 0xFE);
  spin(2000000);

  /* Last resort: a null IDT and an interrupt nothing can handle, which is a triple
   * fault, which the CPU answers with a reset. It is not elegant; it is universal,
   * and it is only reached after two better paths failed. */
  serial_puts("[power] resetting by triple fault\n");
  struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
  } null_idt = {0, 0};
  __asm__ volatile("lidt %0" : : "m"(null_idt));
  __asm__ volatile("int3");

  why = "even the triple fault did not reset this machine";
  return -1;
}