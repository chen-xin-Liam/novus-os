#ifndef NOVUS_BOOT_MENU_H
#define NOVUS_BOOT_MENU_H

/* Boot choice set by the boot menu. 0 = desktop (default), 1 = shell only.
 * Declared here so kmain can branch on it after the menu resolves. */
extern int g_boot_no_desktop;
/* 1 once a session has handed the framebuffer to the kernel text console
 * (the shell build). Keeps the later no-desktop block from clearing the
 * screen it already owns. */
extern int g_console_session;

/* Show the boot menu on the framebuffer, read the keyboard and count down.
 * Defaults to the desktop after five seconds with no key press. Requires the
 * display to be up (nd_init) and the USB HID keyboard to be enumerated
 * (net_init) so arrow keys and Enter are delivered. Sets g_boot_no_desktop. */
void boot_menu_select(void);

/* Tear down the boot splash and clear the screen for the shell-only path,
 * which does not paint the desktop. Safe to call once the menu has resolved. */
void boot_menu_finish(void);

#endif /* NOVUS_BOOT_MENU_H */
