#include "test_harness.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    nd_test_fn fn;
} nd_test_case;

static nd_test_case s_tests[128];
static int s_count;
static int s_failed;

void nd_test_register(const char *name, nd_test_fn fn) {
    if (s_count < 128) {
        s_tests[s_count].name = name;
        s_tests[s_count].fn = fn;
        s_count++;
    }
}

void nd_test_fail(const char *file, int line, const char *expr) {
    printf("FAIL %s:%d: %s\n", file, line, expr);
    s_failed++;
}

void nd_test_fail_eq(const char *file, int line, const char *expr, long got, long want) {
    printf("FAIL %s:%d: %s (got %ld, want %ld)\n", file, line, expr, got, want);
    s_failed++;
}

int nd_test_run(void) {
    int i;
    for (i = 0; i < s_count; i++) {
        int before = s_failed;
        printf("[ RUN ] %s\n", s_tests[i].name);
        s_tests[i].fn();
        printf("[ %s ] %s\n", before == s_failed ? "OK  " : "FAIL", s_tests[i].name);
    }
    printf("\n%d tests, %d failures\n", s_count, s_failed);
    return s_failed == 0 ? 0 : 1;
}

int main(void) { return nd_test_run(); }
