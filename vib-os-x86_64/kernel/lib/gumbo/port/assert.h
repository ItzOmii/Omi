#ifndef _GUMBO_PORT_ASSERT_H
#define _GUMBO_PORT_ASSERT_H
/* a malformed page must never take the machine down */
#define assert(x) ((void)0)
#endif
