/* freestanding shim: keep clang's builtin <stddef.h> from colliding with
 * kernel/include/types.h (which types size_t as unsigned long long) */
#ifndef _GUMBO_PORT_STDDEF_H
#define _GUMBO_PORT_STDDEF_H
#include "types.h"
#ifndef NULL
#define NULL ((void *)0)
#endif
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif
