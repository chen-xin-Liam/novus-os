#ifndef CPPCC_STDLIB_WIN64_KERNEL32_H
#define CPPCC_STDLIB_WIN64_KERNEL32_H

/* Declarations for functions the Win64 backend can resolve through its PE
 * import table. Only signatures used by the current runtime builtins are
 * listed; the full Win32 surface belongs to a later stage. */
extern void *GetStdHandle(unsigned long n_std_handle);
extern int WriteFile(void *file, const void *buffer, unsigned long bytes,
                     unsigned long *written, void *overlapped);
extern void ExitProcess(unsigned int exit_code);
extern int SwitchToThread(void);

#define STD_OUTPUT_HANDLE ((unsigned long)-11)

#endif
