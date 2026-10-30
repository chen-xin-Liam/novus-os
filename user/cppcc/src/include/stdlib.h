/* cppcc freestanding <stdlib.h> for Novus OS. */
#ifndef _CPPCC_STDLIB_H
#define _CPPCC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void exit(int status);
void abort(void);

static inline int abs(int value) {
    return value < 0 ? -value : value;
}

static inline long labs(long value) {
    return value < 0 ? -value : value;
}

#endif
