/* Omi - the wall clock.
 *
 * Ticks since boot are free; the date is not. TLS needs it, because a
 * certificate is only valid between two dates and a machine that believes it is
 * 1970 rejects every chain as not-yet-valid. The only time source on the board
 * is the CMOS chip, which raises no interrupt this kernel handles, so the date
 * is read once at boot and carried forward on the tick counter.
 *
 * If the chip does not answer, or answers with bytes that cannot be a date, the
 * clock starts at a date known to sit inside the current roots' windows and says
 * so (walltime_have_rtc() == 0). That is a lie in the useful direction: a fixed
 * date still rejects a certificate that expired, whereas 1970 rejects all of
 * them, and a program can ask whether the date came from hardware.
 */
#include "types.h"
#include "walltime.h"
#include "task.h"

#define CMOS_PORT 0x70
#define CMOS_DATA 0x71
#define CMOS_NMI  0x80        /* bit 7 of the index port: keep NMI disabled */

/* 1970-01-01 as a count of days since year 0 - what br_x509_minimal_set_time
 * adds to a Unix day count. */
#define EPOCH_YEAR0_DAYS 719528

static void cmos_out(uint8_t reg) { __asm__ volatile("outb %0, %1" :: "a"(reg), "Nd"((uint16_t)CMOS_PORT)); }
static uint8_t cmos_in(void) {
  uint8_t v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"((uint16_t)CMOS_DATA));
  return v;
}
static uint8_t cmos(uint8_t reg) { cmos_out((uint8_t)(reg | CMOS_NMI)); return cmos_in(); }

static int bcd2(int v) { return (v & 0x0F) + ((v >> 4) * 10); }

/* Days since 1970-01-01 from a civil date (Howard Hinnant's algorithm, which is
 * exact for every date the chip can report). */
static int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - (int)(era * 400));
  unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static int     s_ready;
static int     s_have_rtc;
static int64_t s_base_unix;      /* seconds at boot */
static uint64_t s_base_ticks;

/* Read the chip once. Returns 1 when the bytes formed a real date. */
static int rtc_read(int64_t *out_unix) {
  for (int i = 0; i < 100000; i++)
    if (!(cmos(0x0A) & 0x80)) break;               /* update in progress */

  uint8_t sec = cmos(0x00), min = cmos(0x02), hr = cmos(0x04);
  uint8_t day = cmos(0x07), mon = cmos(0x08), yr = cmos(0x09);
  uint8_t regb = cmos(0x0B);

  int s, mi, h, d, mo, y;
  if (regb & 0x04) {                               /* binary mode */
    s = sec; mi = min; h = hr & 0x7F; d = day; mo = mon; y = yr;
  } else {                                         /* BCD, which is the default */
    s = bcd2(sec); mi = bcd2(min); h = bcd2(hr & 0x7F);
    d = bcd2(day); mo = bcd2(mon); y = bcd2(yr);
  }
  if (!(regb & 0x02)) {                            /* 12-hour clock */
    int pm = (hr & 0x80) != 0;
    if (h == 12) h = 0;
    if (pm) h += 12;
  }

  y += 2000;                                       /* century: assume 20xx */
  if (y < 2020 || y > 2099) return 0;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  if (h > 23 || mi > 59 || s > 59) return 0;

  *out_unix = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
  return 1;
}

void walltime_init(void) {
  if (s_ready) return;
  s_ready = 1;
  s_base_ticks = task_total_ticks();
  if (rtc_read(&s_base_unix)) {
    s_have_rtc = 1;
  } else {
    /* Inside the roots' windows, and honest about being a guess. */
    s_base_unix = days_from_civil(2026, 1, 1) * 86400;
    s_have_rtc = 0;
  }
}

int walltime_have_rtc(void) { return s_have_rtc; }

void walltime_now(uint64_t *secs, uint32_t *nsecs) {
  walltime_init();
  uint64_t ticks = task_total_ticks();
  uint64_t delta = ticks >= s_base_ticks ? ticks - s_base_ticks : 0;
  uint64_t whole = delta / TASK_TICK_HZ;
  uint64_t frac  = (delta % TASK_TICK_HZ) * (1000000000 / TASK_TICK_HZ);
  if (secs)  *secs  = (uint64_t)(s_base_unix + (int64_t)whole);
  if (nsecs) *nsecs = (uint32_t)frac;
}

void walltime_unix(uint64_t *secs, uint32_t *frac_day) {
  uint64_t s = 0; uint32_t ns = 0;
  walltime_now(&s, &ns);
  if (secs) *secs = s;
  if (frac_day) *frac_day = (uint32_t)(s % 86400);
}

int64_t walltime_days_since_year0(uint32_t *secs_in_day) {
  uint64_t s = 0; uint32_t ns = 0;
  walltime_now(&s, &ns);
  if (secs_in_day) *secs_in_day = (uint32_t)(s % 86400);
  return (int64_t)(s / 86400) + EPOCH_YEAR0_DAYS;
}