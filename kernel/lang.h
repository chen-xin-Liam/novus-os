/**
 * Novus OS - language persistence (M11).
 *
 * Live boots keep the choice in RAM: the live root is an image in
 * reserved pages and nothing in it survives a reboot. An installed system
 * stores one word in /language.cfg and reads it before the shell paints.
 */

#ifndef NOVUS_LANG_H
#define NOVUS_LANG_H

#include "uefi.h"

void lang_init(void);
void lang_save(UINT32 lang);

#endif /* NOVUS_LANG_H */
