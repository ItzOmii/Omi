/* Power and lifecycle, callable from ring 3.
 *
 * Two calls, both of which can fail: power_off() and power_reset(). They return
 * only when the machine is still on, which is a real outcome - a firmware table
 * that names a control block the chipset ignores is not rare - so the caller gets
 * -1 and power_why() in the serial log rather than a promise.
 */
#ifndef OMI_POWER_H
#define OMI_POWER_H

#include <stdint.h>

/* Empty every block device's write cache. 0 if all of them agreed. */
int power_sync(void);

/* Switch the machine off. Returns -1 (and does not return otherwise). */
int power_off(void);

/* Restart the machine. Returns -1 (and does not return otherwise). */
int power_reset(void);

/* What the last failed attempt tried, for the log. */
const char *power_why(void);

/* The two values SYS_POWER accepts. */
#define POWER_OFF   0
#define POWER_RESET 1

#endif