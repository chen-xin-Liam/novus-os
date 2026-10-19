#include <stdio.h>
#include "../kernel/sched.c"

void kputs(const char *text) { fputs(text, stdout); }

int main(void) {
  return sched_selftest() ? 0 : 1;
}