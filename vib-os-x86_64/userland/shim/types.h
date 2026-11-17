/* A stand-in for the kernel's types.h, for compiling kernel *data* files in user
 * mode.
 *
 * roots.c is a table of DER blobs and nothing else, and the kernel headers it
 * pulls in exist only to give the word "types" a meaning. Including the kernel's
 * real types.h here would put its typedefs next to libc's and invite the
 * collision the userland build is designed to avoid, so this shim sits earlier in
 * the include path and says the same thing in the standard library's words.
 */
#ifndef OMI_USERLAND_TYPES_SHIM_H
#define OMI_USERLAND_TYPES_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#endif