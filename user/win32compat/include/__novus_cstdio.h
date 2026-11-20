#ifndef NOVUS_CSTDIO_H
#define NOVUS_CSTDIO_H
#include "__novus_rt.h"
#include <cstdarg>
typedef struct novus_FILE novus_FILE;
struct novus_FILE { void* h; int dummy; };
extern "C" {
  inline int printf(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    const char* p = fmt;
    while (*p) {
      if (*p != '%') { novus_putc(*p++); continue; }
      p++;
      if (!*p) break;
      if (*p == 's') { const char* s = __builtin_va_arg(ap, const char*); novus_puts(s ? s : "(null)"); }
      else if (*p == 'c') { int c = __builtin_va_arg(ap, int); novus_putc((char)c); }
      else if (*p == 'd' || *p == 'i') { int v = __builtin_va_arg(ap, int); novus_puti(v); }
      else if (*p == 'u') { unsigned int v = __builtin_va_arg(ap, unsigned int); novus_putu(v); }
      else if (*p == 'x') { unsigned int v = __builtin_va_arg(ap, unsigned int); novus_putx(v); }
      else if (*p == 'p') { void* v = __builtin_va_arg(ap, void*); novus_puts("0x"); novus_putx((unsigned long long)v); }
      else { novus_putc(*p); }
      p++;
    }
    __builtin_va_end(ap);
    return 0;
  }
  inline int vprintf(const char* fmt, __builtin_va_list ap) {
    const char* p = fmt;
    while (*p) {
      if (*p != '%') { novus_putc(*p++); continue; }
      p++;
      if (!*p) break;
      if (*p == 's') { const char* s = __builtin_va_arg(ap, const char*); novus_puts(s ? s : "(null)"); }
      else if (*p == 'c') { int c = __builtin_va_arg(ap, int); novus_putc((char)c); }
      else if (*p == 'd' || *p == 'i') { int v = __builtin_va_arg(ap, int); novus_puti(v); }
      else if (*p == 'u') { unsigned int v = __builtin_va_arg(ap, unsigned int); novus_putu(v); }
      else novus_putc(*p);
      p++;
    }
    return 0;
  }
  inline int fprintf(void*, const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); int r = vprintf(fmt, ap); __builtin_va_end(ap); return r; }
  inline int fputs(const char* s, void*) { novus_puts(s); return 0; }
  inline int fputc(int c, void*) { novus_putc((char)c); return c; }
  inline int puts(const char* s) { novus_puts(s); novus_putc('\n'); return 0; }
  inline novus_FILE* tmpfile(void) { static novus_FILE f; f.h = 0; return &f; }
  inline int fclose(novus_FILE*) { return 0; }
}
namespace std { using ::printf; using ::vprintf; using ::fprintf; using ::fputs; using ::fputc; using ::puts; typedef novus_FILE FILE; }
#endif
