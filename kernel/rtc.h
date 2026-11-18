/**
 * Novus OS - CMOS real-time clock (M9 desktop clock).
 *
 * The taskbar shows the wall clock, and the only source of it on a PC is
 * the RTC behind ports 0x70/0x71. Reading it needs no interrupt and no
 * setup: pick the register, wait for the update-in-progress bit to clear,
 * convert from BCD if the chip is in BCD mode.
 */

#ifndef NOVUS_RTC_H
#define NOVUS_RTC_H

#include "uefi.h"

/* Current local time. Returns 1 on success, 0 when the chip did not
 * settle (or the machine has no RTC), in which case nothing is written. */
int rtc_now(UINT8 *hour, UINT8 *minute);

/* Current local date. Returns 1 on success, 0 when the chip reads back
 * something out of range. The taskbar's second clock line needs this,
 * and rtc_unix() alone cannot give it without doing the civil calendar
 * arithmetic twice. */
int rtc_now_date(UINT16 *year, UINT8 *month, UINT8 *day);

/* Current Unix timestamp (seconds since 1970-01-01) from the CMOS RTC, or 0
 * when the clock reads invalid. Used for TLS certificate validity checks. */
UINT64 rtc_unix(void);

#endif /* NOVUS_RTC_H */
