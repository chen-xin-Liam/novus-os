#ifndef CPPCC_STDLIB_LINUX_LIMITS_H
#define CPPCC_STDLIB_LINUX_LIMITS_H

/* Freestanding constants for the Linux ELF64 target. The ELF writer itself
 * does not require these; they are provided for source compatibility. */
#define CPPCC_LINUX_PAGE_SIZE 4096
#define CPPCC_LINUX_SYS_WRITE 1
#define CPPCC_LINUX_SYS_EXIT 60

#endif
