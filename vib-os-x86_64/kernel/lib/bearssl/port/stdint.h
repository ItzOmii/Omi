/* Omi port shim: BearSSL wants <stdint.h>; the kernel's types.h owns those typedefs. */
#ifndef OMI_PORT_STDINT_H
#define OMI_PORT_STDINT_H
#include "types.h"
typedef int64_t intmax_t;
typedef uint64_t uintmax_t;
#endif
