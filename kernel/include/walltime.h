/* Omi - the wall clock (see walltime.c). */
#ifndef OMI_WALLTIME_H
#define OMI_WALLTIME_H

#include "types.h"

/* Read the RTC once and fix the origin. Idempotent; called by everything else. */
void walltime_init(void);

/* 1 when the date came from the CMOS chip, 0 when it is the documented guess. */
int  walltime_have_rtc(void);

/* Unix seconds and nanoseconds, advanced on the tick counter. */
void walltime_now(uint64_t *secs, uint32_t *nsecs);

/* Same instant split the way TLS wants it: whole seconds, and the seconds part
 * of the day. */
void walltime_unix(uint64_t *secs, uint32_t *frac_day);

/* Days since year 0 (for br_x509_minimal_set_time) and the seconds within the
 * day. */
int64_t walltime_days_since_year0(uint32_t *secs_in_day);

#endif