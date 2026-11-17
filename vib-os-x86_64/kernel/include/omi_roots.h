/*
 * Omi - embedded root CA table
 */
#ifndef _OMI_ROOTS_H
#define _OMI_ROOTS_H

/* No types.h: this header needs only the built-in words, and including the
 * kernel type vocabulary here is what stopped a user-mode program from reading the
 * same root table - kernel/include is not on a libc's include path for good
 * reason. */

typedef struct {
  const char *name;
  const unsigned char *der;
  unsigned len;
} omi_root_t;

extern const omi_root_t omi_roots[];
extern const int omi_root_count;

#endif
