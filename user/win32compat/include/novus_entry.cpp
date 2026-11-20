/* Startup shim for self-hosted C++98 Win32 executables. */
extern int main();

extern "C" void __main(void) { /* no global constructors in this runtime */ }

extern "C" void novus_win32_main(void) {
    int rc = main();
    (void)rc; /* trampoline reads RAX as exit code */
}
