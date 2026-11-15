/* freestanding shim: Gumbo's stdlib needs onto the kernel heap */
#ifndef _GUMBO_PORT_STDLIB_H
#define _GUMBO_PORT_STDLIB_H
#include "types.h"
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void free(void *p);
void abort(void);
#endif
