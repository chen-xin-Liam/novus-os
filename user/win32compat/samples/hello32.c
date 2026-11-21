/**
 * Novus OS - M9 Win32 sample: 32-bit image, on purpose.
 *
 * This one exists to be refused: the compatibility layer runs x86-64 code, so
 * a real PE32 image built with -m32 is exactly the input that has to come back
 * as "32-bit needs the WOW executor" instead of "corrupt file". Nothing here
 * runs; the loader's single rejection line is the test.
 */

void novus_win32_main(void) {
  /* A loop that would spin if the image ever were started by mistake. */
  for (;;) {
  }
}
