#ifndef _GUMBO_PORT_STDIO_H
#define _GUMBO_PORT_STDIO_H
#include "types.h"
#include "stdarg.h"
int snprintf(char *s, size_t n, const char *fmt, ...);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
#endif
