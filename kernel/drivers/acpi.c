/*
 * Minimal ACPI table parsing (RSDP -> RSDT/XSDT -> MADT)
 */

#include "../include/acpi.h"
#include "../include/gui.h"   /* serial: what was found, or why not */

static const acpi_madt_t *madt = NULL;

static int checksum_ok(const uint8_t *ptr, uint32_t len) {
  uint8_t sum = 0;
  for (uint32_t i = 0; i < len; i++) {
    sum = (uint8_t)(sum + ptr[i]);
  }
  return sum == 0;
}

static const acpi_sdt_header_t *find_sdt(const acpi_rsdp_t *rsdp, const char *sig) {
  if (!rsdp) return NULL;

  if (rsdp->revision >= 2 && rsdp->xsdt_address) {
    const acpi_sdt_header_t *xsdt = (const acpi_sdt_header_t *)(uintptr_t)rsdp->xsdt_address;
    if (!checksum_ok((const uint8_t *)xsdt, xsdt->length)) return NULL;
    uint32_t count = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
    const uint64_t *entries = (const uint64_t *)((const uint8_t *)xsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < count; i++) {
      const acpi_sdt_header_t *hdr = (const acpi_sdt_header_t *)(uintptr_t)entries[i];
      if (!hdr) continue;
      if (hdr->signature[0] == sig[0] && hdr->signature[1] == sig[1] &&
          hdr->signature[2] == sig[2] && hdr->signature[3] == sig[3]) {
        if (checksum_ok((const uint8_t *)hdr, hdr->length)) {
          return hdr;
        }
      }
    }
  } else if (rsdp->rsdt_address) {
    const acpi_sdt_header_t *rsdt = (const acpi_sdt_header_t *)(uintptr_t)rsdp->rsdt_address;
    if (!checksum_ok((const uint8_t *)rsdt, rsdt->length)) return NULL;
    uint32_t count = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
    const uint32_t *entries = (const uint32_t *)((const uint8_t *)rsdt + sizeof(acpi_sdt_header_t));
    for (uint32_t i = 0; i < count; i++) {
      const acpi_sdt_header_t *hdr = (const acpi_sdt_header_t *)(uintptr_t)entries[i];
      if (!hdr) continue;
      if (hdr->signature[0] == sig[0] && hdr->signature[1] == sig[1] &&
          hdr->signature[2] == sig[2] && hdr->signature[3] == sig[3]) {
        if (checksum_ok((const uint8_t *)hdr, hdr->length)) {
          return hdr;
        }
      }
    }
  }
  return NULL;
}

/* ACPI tables are addressed physically: every pointer inside one is a physical
 * address, including the RSDP the bootloader hands over. The kernel's direct map
 * turns any of them into something dereferenceable, which is why this used to be
 * a stub - reading a physical address as a pointer faults, so the MADT was never
 * found and the machine was assumed to have one core because nothing said
 * otherwise. */
extern uint64_t hhdm_offset;

static const void *phys_to_virt_ptr(uint64_t phys) {
  if (!phys) return NULL;
  return (const void *)(uintptr_t)(phys + hhdm_offset);
}

void acpi_init(void *rsdp_ptr) {
  madt = NULL;
  if (!rsdp_ptr) return;

  /* The bootloader's RSDP response is physical, but a caller may already have
   * mapped it: look for the signature before adding the offset, not after. */
  const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)(uintptr_t)rsdp_ptr;
  if (!(rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' &&
        rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ')) {
    rsdp = (const acpi_rsdp_t *)phys_to_virt_ptr((uint64_t)(uintptr_t)rsdp_ptr);
    if (!(rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' &&
          rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ')) return;
  }

  /* Copy rather than patch: the 32-bit RSDT field is too small for a mapped
   * address and the XSDT field next to it belongs to the same table. */
  acpi_rsdp_t fixed = *rsdp;
  if (fixed.rsdt_address)
    fixed.rsdt_address = (uint32_t)(uintptr_t)phys_to_virt_ptr(fixed.rsdt_address);
  if (fixed.xsdt_address)
    fixed.xsdt_address = (uint64_t)(uintptr_t)phys_to_virt_ptr(fixed.xsdt_address);

  const acpi_sdt_header_t *hdr = find_sdt(&fixed, "APIC");
  if (!hdr) { serial_puts("[acpi] no MADT in this machine's tables\n"); return; }
  if (hdr->length < sizeof(acpi_madt_t) || hdr->length > 0x10000) {
    serial_puts("[acpi] MADT length is not believable\n");
    return;
  }
  madt = (const acpi_madt_t *)hdr;
  serial_puts("[acpi] MADT at 0x");
  serial_puthex((uint64_t)(uintptr_t)hdr);
  serial_puts(" length=");
  serial_puthex((uint64_t)hdr->length);
  serial_puts("\n");
}

const acpi_madt_t *acpi_get_madt(void) {
  return madt;
}
