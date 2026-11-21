#ifndef ND_TEST_HARNESS_H
#define ND_TEST_HARNESS_H

#include "nd_types.h"

typedef void (*nd_test_fn)(void);

void nd_test_register(const char *name, nd_test_fn fn);
int  nd_test_run(void);

#define ND_ASSERT(cond) do { \
    if (!(cond)) { \
        nd_test_fail(__FILE__, __LINE__, #cond); \
        return; \
    } \
} while (0)

#define ND_ASSERT_EQ(a, b) do { \
    long _a = (long)(a); long _b = (long)(b); \
    if (_a != _b) { nd_test_fail_eq(__FILE__, __LINE__, #a, _a, _b); return; } \
} while (0)

void nd_test_fail(const char *file, int line, const char *expr);
void nd_test_fail_eq(const char *file, int line, const char *expr, long got, long want);

#define ND_TEST(name) \
    static void name(void); \
    __attribute__((constructor)) static void name##_registered(void) { \
        nd_test_register(#name, name); \
    } \
    static void name(void)

#endif
