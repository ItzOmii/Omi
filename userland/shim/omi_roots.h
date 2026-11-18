/* Forward the kernel's root table into a user-mode build without putting
 * kernel/include on the include path.
 *
 * The table is data, so there is no reason to keep a second copy of it - but
 * `-I../kernel/include` would also make `<string.h>` resolve to the kernel's
 * string.h, which has no business in a libc build. A relative include reaches the
 * one definition and leaves the rest of the search path alone.
 */
#ifndef OMI_USERLAND_ROOTS_SHIM_H
#define OMI_USERLAND_ROOTS_SHIM_H
#include "../../kernel/include/omi_roots.h"
#endif