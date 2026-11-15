/*
 * Omi - embedded root CA table
 */
#ifndef _OMI_ROOTS_H
#define _OMI_ROOTS_H

#include "types.h"

typedef struct {
  const char *name;
  const unsigned char *der;
  unsigned len;
} omi_root_t;

extern const omi_root_t omi_roots[];
extern const int omi_root_count;

#endif
