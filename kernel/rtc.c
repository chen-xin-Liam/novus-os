/**
 * Novus OS - CMOS real-time clock.
 *
 * Registers of interest: 0x00 seconds, 0x02 minutes, 0x04 hours, 0x0A
 * status A (bit 7 = update in progress), 0x0B status B (bit 2 = binary
 * mode, bit 1 = 24 hour). QEMU and every PC implement this, so it is the
 * cheapest way to put a real clock on the taskbar.
 */

#include "kernel.h"
#include "cpu.h"
#include "rtc.h"

#define CMOS_INDEX 0x70u
#define CMOS_DATA  0x71u

#define RTC_STAT_A 0x0Au
#define RTC_STAT_B 0x0Bu

static UINT8 cmos_read(UINT8 reg) {
  io_out8(CMOS_INDEX, reg);
  return io_in8(CMOS_DATA);
}

static UINT8 bcd_to_bin(UINT8 v) { return (UINT8)((v & 0x0Fu) + ((v >> 4) * 10u)); }

/* Correction from NTP, in seconds, applied on top of the CMOS clock. The
 * hardware keeps ticking; only the offset changes when a sync succeeds. */
static volatile INT64 g_clock_adj;

/* Days since 1970-01-01 (Howard Hinnant's civil-from-days), 32-bit years. */
static UINT64 rtc_days_from_civil(int y, int m, int d) {
  int era, yoe, doy, doe;
  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (UINT64)(era * 146097 + doe - 719468);
}

/* Inverse of the above: Unix days -> Gregorian date. */
static void rtc_civil_from_days(UINT64 z, INT32 *year, UINT32 *month,
                                UINT32 *day) {
  UINT64 era;
  UINT32 doe, yoe, doy, mp;

  z += 719468u;
  era = z / 146097u;
  doe = (UINT32)(z - era * 146097u);
  yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  mp = (5u * doy + 2u) / 153u;
  *day = doy - (153u * mp + 2u) / 5u + 1u;
  *month = (mp < 10u) ? mp + 3u : mp - 9u;
  *year = (INT32)yoe + (INT32)era * 400 + ((*month <= 2u) ? 1 : 0);
}

/* Wall-clock reads derive from rtc_unix() so an NTP correction is visible
 * everywhere, including the taskbar and the Win32 compatibility layer. */
int rtc_now(UINT8 *hour, UINT8 *minute) {
  UINT64 t;
  UINT64 sod;

  if (hour == 0 || minute == 0) {
    return 0;
  }
  t = rtc_unix();
  if (t == 0u) {
    return 0;
  }
  sod = t % 86400u;
  *hour = (UINT8)(sod / 3600u);
  *minute = (UINT8)((sod % 3600u) / 60u);
  return 1;
}

int rtc_now_date(UINT16 *year, UINT8 *month, UINT8 *day) {
  INT32 y;
  UINT32 m;
  UINT32 d;
  UINT64 t;

  if (year == 0 || month == 0 || day == 0) {
    return 0;
  }
  t = rtc_unix();
  if (t == 0u) {
    return 0;
  }
  rtc_civil_from_days(t / 86400u, &y, &m, &d);
  if (y < 1970 || y > 65535 || m < 1u || m > 12u || d < 1u || d > 31u) {
    return 0;
  }
  *year = (UINT16)y;
  *month = (UINT8)m;
  *day = (UINT8)d;
  return 1;
}

/* Raw CMOS time, before any NTP correction. */
static UINT64 rtc_unix_raw(void) {
  UINT8 s, mi, h, d, mo, y, c, b;
  UINT32 spins;

  for (spins = 0; spins < 100000u; spins++) {
    if ((cmos_read(RTC_STAT_A) & 0x80u) == 0u) {
      break;
    }
  }
  s = cmos_read(0x00);
  mi = cmos_read(0x02);
  h = cmos_read(0x04);
  d = cmos_read(0x07);
  mo = cmos_read(0x08);
  y = cmos_read(0x09);
  c = cmos_read(0x32);          /* century register (0x20 => 2000s) */
  b = cmos_read(RTC_STAT_B);

  if ((b & 0x04u) == 0u) {     /* BCD mode */
    s = bcd_to_bin(s);
    mi = bcd_to_bin(mi);
    h = bcd_to_bin((UINT8)(h & 0x7Fu));
    d = bcd_to_bin(d);
    mo = bcd_to_bin(mo);
    y = bcd_to_bin(y);
    c = bcd_to_bin(c);
  }
  if ((b & 0x02u) == 0u) {     /* 12 hour mode */
    UINT8 pm = (UINT8)(h & 0x80u);
    h = (UINT8)(h & 0x7Fu);
    if (h == 12u) {
      h = 0;
    }
    if (pm != 0u) {
      h = (UINT8)(h + 12u);
    }
  }
  if (h > 23u || mi > 59u || s > 59u || d < 1u || d > 31u || mo < 1u ||
      mo > 12u || y > 99u) {
    return 0;
  }
  {
    int yy = (c == 0u ? 20 : (int)c) * 100 + (int)y;
    UINT64 days = rtc_days_from_civil(yy, (int)mo, (int)d);
    return days * 86400u + (UINT64)h * 3600u + (UINT64)mi * 60u +
           (UINT64)s;
  }
}

UINT64 rtc_unix(void) {
  UINT64 base = rtc_unix_raw();
  INT64  adj;

  if (base == 0u) {
    return 0u;
  }
  adj = g_clock_adj;
  if (adj >= 0) {
    return base + (UINT64)adj;
  }
  {
    UINT64 back = (UINT64)(-adj);

    return (base > back) ? base - back : 0u;
  }
}

void rtc_set_unix(UINT64 now) {
  UINT64 base = rtc_unix_raw();

  if (base == 0u || now == 0u) {
    return;
  }
  g_clock_adj = (now >= base) ? (INT64)(now - base) : -(INT64)(base - now);
}
