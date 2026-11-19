/* syscalls.h - system call names and numbers (x86-64). */
#ifndef TDB_SYSCALLS_H
#define TDB_SYSCALLS_H

/* "write" for 1; NULL if the number has no name. */
const char *sys_name(long nr);

/* 1 for "write" (or for "1"); -1 if there is no such system call. */
long sys_number(const char *name);

#endif
