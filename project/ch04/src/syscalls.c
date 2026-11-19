/* syscalls.c - system call names and numbers (x86-64). */
#include "syscalls.h"

#include <stdlib.h>
#include <string.h>

#include "sysnames.h"

#define NSYS ((long)(sizeof sys_names / sizeof sys_names[0]))

const char *sys_name(long nr)
{
    if (nr < 0 || nr >= NSYS)
        return NULL;
    return sys_names[nr];
}

long sys_number(const char *name)
{
    char *end;

    if (name[0] >= '0' && name[0] <= '9') {
        long nr = strtol(name, &end, 10);
        return *end == '\0' && nr < NSYS ? nr : -1;
    }
    for (long nr = 0; nr < NSYS; nr++)
        if (sys_names[nr] != NULL && strcmp(sys_names[nr], name) == 0)
            return nr;
    return -1;
}
