/**
 * Novus OS - UI language state (M11).
 *
 * One global switch, two literal strings per call site:
 *
 *     nd_text(s, x, y, ND_L("磁盘", "Disk"), color, scale);
 *
 * The Chinese and English strings live next to the code that draws them,
 * which keeps translation review local and avoids an id table that has to
 * be kept in sync across C and Rust. `ND_L` resolves at draw time, so a
 * language change only needs a repaint, not a rebuild of the widget tree -
 * though the shell rebuilds anyway because its widgets cache text.
 *
 * Clients reach this through SYSINFO_REQ_LANG (0x030F); the value is
 * persisted by kernel/lang.c on a persistent root.
 */

#ifndef NOVUS_ND_LANG_H
#define NOVUS_ND_LANG_H

#include "uefi.h"

#define ND_LANG_ZH 0u
#define ND_LANG_EN 1u

void        nd_lang_init(UINT32 lang);
UINT32      nd_lang_get(void);
void        nd_lang_set(UINT32 lang);
UINT32      nd_lang_version(void);
const char *nd_lang_pick(const char *zh, const char *en);

/* Translation of a Chinese source string (generated table). Returns the
 * input unchanged when the language is Chinese or the string has no
 * translation, so callers never have to check. */
const char *nd_lang_lookup(const char *zh);
const char *nd_lang_tr(const char *zh);

/* Argument evaluation is harmless on both branches (string literals). */
#define ND_L(zh, en) nd_lang_pick((zh), (en))

#endif /* NOVUS_ND_LANG_H */
