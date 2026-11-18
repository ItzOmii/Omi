/* Omi port shim: <stddef.h> maps onto the kernel's type set. */
#ifndef OMI_PORT_STDDEF_H
#define OMI_PORT_STDDEF_H
#include "types.h"
typedef int64_t ptrdiff_t;
#define offsetof(t, m) ((size_t)&(((t *)0)->m))
#endif
