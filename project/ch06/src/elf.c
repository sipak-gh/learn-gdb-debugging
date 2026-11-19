/* elf.c - what tdb needs from the program's ELF file. */
#include "elf.h"

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

unsigned char *elf_read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0, n;
    do {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            unsigned char *bigger = realloc(buf, cap);
            if (bigger == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = bigger;
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
    } while (n > 0);
    fclose(f);
    *size = len;
    return buf;
}

/* Does [off, off + len) lie inside a file of size bytes? */
static int fits(uint64_t off, uint64_t len, size_t size)
{
    return off <= size && len <= size - off;
}

const unsigned char *elf_section(const unsigned char *file, size_t size, const char *name,
                                 size_t *len)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
        !fits(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr), size) ||
        eh->e_shstrndx >= eh->e_shnum)
        return NULL;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(file + eh->e_shoff);

    /* Section names are offsets into the section-name string table, the
     * section whose index is in e_shstrndx. */
    const Elf64_Shdr *names = &sh[eh->e_shstrndx];
    if (!fits(names->sh_offset, names->sh_size, size))
        return NULL;
    const char *strings = (const char *)file + names->sh_offset;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_name >= names->sh_size || sh[i].sh_type == SHT_NOBITS)
            continue;
        if (strncmp(strings + sh[i].sh_name, name, names->sh_size - sh[i].sh_name) == 0 &&
            fits(sh[i].sh_offset, sh[i].sh_size, size)) {
            *len = sh[i].sh_size;
            return file + sh[i].sh_offset;
        }
    }
    return NULL;
}
