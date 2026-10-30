/* cppcc freestanding <stddef.h> for Novus OS x86_64 (LP64). */
#ifndef _CPPCC_STDDEF_H
#define _CPPCC_STDDEF_H

typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef unsigned long max_align_t;

#ifndef NULL
#define NULL 0
#endif

#define offsetof(type, member) ((size_t)(&(((type*)0)->member)))

#endif
