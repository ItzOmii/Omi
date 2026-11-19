/*
 * Minimal ACPI table parsing (RSDP -> RSDT/XSDT -> MADT)
 */

#ifndef _ACPI_H
#define _ACPI_H

#include "types.h"

typedef struct {
  char signature[8];
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t ext_checksum;
  uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
  acpi_sdt_header_t header;
  uint32_t lapic_addr;
  uint32_t flags;
  /* Followed by MADT entries */
} __attribute__((packed)) acpi_madt_t;

/* A generic address structure: where a register lives, and how wide it is. */
typedef struct {
  uint8_t  space;        /* 0 = memory, 1 = I/O, 2 = PCI config */
  uint8_t  bit_width;
  uint8_t  bit_offset;
  uint8_t  access_width;
  uint64_t address;
} __attribute__((packed)) acpi_gas_t;

/* The fixed ACPI description table, up to RESET_VALUE - everything past that is
 * ACPI 5.x additions this kernel does not read. Offsets matter: this struct is the
 * table, field for field on disk, so it is packed and every field is the width the
 * spec says. */
typedef struct {
  acpi_sdt_header_t header;      /* 0 */
  uint32_t firmware_ctrl;        /* 36 */
  uint32_t dsdt;                 /* 40 */
  uint8_t  reserved0;            /* 44 */
  uint8_t  preferred_profile;    /* 45 */
  uint16_t sci_int;              /* 46 */
  uint32_t smi_cmd;              /* 48 */
  uint8_t  acpi_enable;          /* 52 */
  uint8_t  acpi_disable;         /* 53 */
  uint8_t  s4bios_req;           /* 54 */
  uint8_t  pstate_cnt;           /* 55 */
  uint32_t pm1a_evt_blk;         /* 56 */
  uint32_t pm1b_evt_blk;         /* 60 */
  uint32_t pm1a_cnt_blk;         /* 64 */
  uint32_t pm1b_cnt_blk;         /* 68 */
  uint32_t pm2_cnt_blk;          /* 72 */
  uint32_t pm_tmr_blk;           /* 76 */
  uint32_t gpe0_blk;             /* 80 */
  uint32_t gpe1_blk;             /* 84 */
  uint8_t  pm1_evt_len;          /* 88 */
  uint8_t  pm1_cnt_len;          /* 89 */
  uint8_t  pm2_cnt_len;          /* 90 */
  uint8_t  pm_tmr_len;           /* 91 */
  uint8_t  gpe0_blk_len;         /* 92 */
  uint8_t  gpe1_blk_len;         /* 93 */
  uint8_t  gpe1_base;            /* 94 */
  uint8_t  cst_cnt;              /* 95 */
  uint16_t p_lvl2_lat;           /* 96 */
  uint16_t p_lvl3_lat;           /* 98 */
  uint16_t flush_size;           /* 100 */
  uint16_t flush_stride;         /* 102 */
  uint8_t  duty_offset;          /* 104 */
  uint8_t  duty_width;           /* 105 */
  uint8_t  day_alrm;             /* 106 */
  uint8_t  mon_alrm;             /* 107 */
  uint8_t  century;              /* 108 */
  uint16_t iapc_boot_arch;       /* 109 */
  uint8_t  reserved1;            /* 111 */
  uint32_t flags;                /* 112 */
  acpi_gas_t reset_reg;          /* 116 */
  uint8_t  reset_value;          /* 128 */
  uint8_t  arm_boot_arch;        /* 129 */
  uint8_t  reserved2[3];         /* 130 */
} __attribute__((packed)) acpi_fadt_t;

/* FLAGS bit 10: the reset register below is real. */
#define ACPI_FADT_RESET_REG_SUP (1u << 10)

void acpi_init(void *rsdp_ptr);
const acpi_madt_t *acpi_get_madt(void);

/* NULL when the machine has no FADT, or one too short to hold what power.c reads. */
const acpi_fadt_t *acpi_get_fadt(void);

#endif /* _ACPI_H */
