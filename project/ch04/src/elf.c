/* elf.c - what tdb needs from the program's ELF file. */
#include "elf.h"

#include <elf.h>
#include <stdio.h>

/* Look up type in /proc/PID/auxv: the auxiliary vector, a list of
 * (type, value) pairs the kernel gives every new program. */
static int auxv_get(pid_t pid, uint64_t type, uint64_t *value)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/auxv", (int)pid);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;

    Elf64_auxv_t av;
    int found = -1;
    while (fread(&av, sizeof av, 1, f) == 1 && av.a_type != AT_NULL) {
        if (av.a_type == type) {
            *value = av.a_un.a_val;
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

int elf_load_base(const char *path, pid_t pid, uint64_t *base)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    Elf64_Ehdr eh;
    size_t n = fread(&eh, sizeof eh, 1, f);
    fclose(f);
    if (n != 1)
        return -1;

    if (eh.e_type == ET_EXEC) {
        *base = 0;
        return 0;
    }
    /* A PIE: AT_ENTRY is where the entry point really is in memory;
     * e_entry is where the file says it is. */
    uint64_t entry;
    if (auxv_get(pid, AT_ENTRY, &entry) < 0)
        return -1;
    *base = entry - eh.e_entry;
    return 0;
}
