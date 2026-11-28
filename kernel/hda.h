#ifndef NOVUS_HDA_H
#define NOVUS_HDA_H

/* Intel HD Audio: controller discovery plus one playback stream. The driver
 * is deliberately small - it answers two questions the boot log could never
 * answer before: is there an HDA controller on this machine, and does a
 * stream descriptor actually move PCM through DMA. */

#ifndef NOVUS_HDA_SELFTEST
#define NOVUS_HDA_SELFTEST 0
#endif

/* Probe the controller, reset it, and read the codec's own node tree.
 * Returns 0 when a codec answered STATESTS. */
int hda_init(void);

/* One 440 Hz tone through the first output stream. 0 when the DMA pointer
 * advanced and the stream was stopped cleanly. */
int hda_selftest(void);

/* One line for the diagnostics strip: hda=/codec=/nsdo=/lpib=. */
void hda_banner(char *out, UINT32 cap);

#endif /* NOVUS_HDA_H */
