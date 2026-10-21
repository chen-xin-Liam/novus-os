#ifndef NOVUS_SYSCALL_H
#define NOVUS_SYSCALL_H

long novus_write(const char* bytes, unsigned long length);
void novus_yield();
void novus_exit(int status);

#endif