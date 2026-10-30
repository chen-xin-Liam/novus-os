/* Novus OS syscall interface for cppcc programs. */
#ifndef NOVUS_SYSCALL_H
#define NOVUS_SYSCALL_H

#define NOVUS_SYS_YIELD 1
#define NOVUS_SYS_WRITE 2
#define NOVUS_SYS_EXIT 60

#define NOVUS_STDOUT 1
#define NOVUS_STDERR 2

long novus_write(const char* bytes, unsigned long length);
void novus_yield();
void novus_exit(int status);

#endif
