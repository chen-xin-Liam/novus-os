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

int rtc_now(UINT8 *hour, UINT8 *minute) {
  UINT8 h;
  UINT8 m;
  UINT8 b;
  UINT32 spins;

  if (hour == 0 || minute == 0) {
    return 0;
  }
  /* Wait out an update in progress; a read that straddles it can return a
   * mix of the old and the new second. */
  for (spins = 0; spins < 100000u; spins++) {
    if ((cmos_read(RTC_STAT_A) & 0x80u) == 0u) {
      break;
    }
  }
  m = cmos_read(0x02);
  h = cmos_read(0x04);
  b = cmos_read(RTC_STAT_B);

  if ((b & 0x04u) == 0u) { /* BCD mode */
    m = bcd_to_bin(m);
    h = bcd_to_bin((UINT8)(h & 0x7Fu));
  }
  if ((b & 0x02u) == 0u) { /* 12 hour mode: convert */
    UINT8 pm = (UINT8)(h & 0x80u);

    h = (UINT8)(h & 0x7Fu);
    if (h == 12u) {
      h = 0;
    }
    if (pm != 0u) {
      h = (UINT8)(h + 12u);
    }
  }
  if (h > 23u || m > 59u) {
    return 0;
  }
  *hour = h;
  *minute = m;
  return 1;
}
