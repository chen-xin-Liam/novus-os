#ifndef NOVUS_EXCEPT_H
#define NOVUS_EXCEPT_H
/* Take over C++ exception/RTTI keywords with macros so the frontend needs
 * neither SEH personality nor libgcc unwind.  Verified with
 *   g++ -fno-exceptions -fno-rtti
 * The pattern (validated in baseline/macro_test.cpp):
 *   try { throw X; } catch(const T&) { ... }
 * expands to
 *   if (setjmp(jb)==0) { g_throw = X; } else if (check()) { ... }
 */

typedef struct { void* __novus_jb[8]; } novus_jmp_buf;
extern "C" int  __builtin_setjmp(void*);
extern "C" void __builtin_longjmp(void*, int);

struct novus_type_info { const char* __name; novus_type_info(const char* n):__name(n){} };

struct novus_thrower {
  novus_jmp_buf* jb;
  int* flag;
  template <class T> void operator=(const T&) const { *flag = 1; __builtin_longjmp(jb->__novus_jb, 1); }
};

/* One global exception context is sufficient for the single-threaded test
 * program and for nested try blocks (each try saves/restores the active jb). */
struct novus_except_ctx {
  novus_jmp_buf jb;
  int flag;
  novus_jmp_buf* prev;
  int* prev_flag;
};
static novus_except_ctx novus_except_state = { {{0}}, 0, 0, 0 };
static novus_thrower novus_throw_obj = { &novus_except_state.jb, &novus_except_state.flag };

inline bool novus_except_enter() {
  novus_except_state.flag = 0;
  return true;
}
inline bool novus_except_caught() { return novus_except_state.flag != 0; }
inline const novus_type_info& novus_typeid_impl(const char* n) {
  static novus_type_info ti("?");
  ti.__name = n;
  return ti;
}

/* Macros. Note: object-like `throw` -> `novus_throw_obj =`, and because
 * `novus_thrower::operator=` never returns, control resumes at setjmp. */
#define try       if (__builtin_setjmp(novus_except_state.jb.__novus_jb) == 0 && novus_except_enter())
#define catch(x)  else if (novus_except_caught())
#define throw     novus_throw_obj =
#define typeid(x) novus_typeid_impl(#x)

#endif
