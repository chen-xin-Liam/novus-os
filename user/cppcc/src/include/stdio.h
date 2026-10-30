/* cppcc freestanding <stdio.h>: unbuffered character output only. */
#ifndef _CPPCC_STDIO_H
#define _CPPCC_STDIO_H

#include <stddef.h>

#define EOF (-1)

int puts(const char* text);
int putchar(int character);

#endif
