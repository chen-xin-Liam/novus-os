#ifndef NOVUS_USBLOG_H
#define NOVUS_USBLOG_H

#include "kernel.h"

/* Local test aid, not part of the product surface: mirror the kernel log
 * onto the USB stick the machine booted from. See usblog.c. */

void usblog_init(void);
/* Try again once the storage stack has had time to enumerate: the first
 * call runs next to usb_init() and on this laptop finds no volume yet, so
 * the retry (called after the mounts are applied) is what actually starts
 * the copy. A no-op when a device is already attached. */
void usblog_retry(void);
void usblog_puts(const char *s);
void usblog_flush(int force);
/* 0 = no device found, 1 = attached and the header write landed, 2 = a
 * device was found but refused writes. The desktop status line shows this
 * so a single screenshot says whether the stick received the log. */
int usblog_state(void);
#endif
