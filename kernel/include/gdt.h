#ifndef GDT_H
#define GDT_H
#include <stdint.h>
void gdt_init(void);
void gdt_get_gdtr(void *out10);   /* packed limit+base, for a core with none loaded */
void gdt_set_kernel_stack(uint64_t rsp0);
#endif
