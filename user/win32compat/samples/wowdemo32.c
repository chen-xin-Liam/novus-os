/* Minimal PE32 program for the WOW32 executor: no CRT, stdcall imports only.
 * It writes one line through WriteFile and exits with ExitProcess. */
typedef unsigned long ULONG;
typedef void *HANDLE;

HANDLE GetStdHandle(ULONG nStdHandle) __attribute__((stdcall));
int WriteFile(HANDLE hFile, const void *lpBuffer, ULONG nBytes,
              ULONG *lpNumberOfBytesWritten, void *lpOverlapped)
    __attribute__((stdcall));
void ExitProcess(ULONG uExitCode) __attribute__((stdcall));

/* i686 mingw decorates a C `_start` as `__start` in COFF. */
void _start(void) {
  HANDLE out = GetStdHandle((ULONG)-11); /* STD_OUTPUT_HANDLE */
  ULONG written = 0;
  const char msg[] = "Hello from 32-bit WOW32!\r\n";

  WriteFile(out, msg, sizeof(msg) - 1u, &written, 0);
  ExitProcess(0u);
}
