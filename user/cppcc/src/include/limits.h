/* cppcc freestanding <limits.h> for Novus OS x86_64 (LP64). */
#ifndef _CPPCC_LIMITS_H
#define _CPPCC_LIMITS_H

#define CHAR_BIT 8

#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255

#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX

#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535

#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U

#define LONG_MIN (-9223372036854775807L - 1)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX (~0UL)

#define LLONG_MIN (-9223372036854775807L - 1)
#define LLONG_MAX 9223372036854775807L
#define ULLONG_MAX (~0UL)

#define MB_LEN_MAX 4

#endif
