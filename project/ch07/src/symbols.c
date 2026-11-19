/* symbols.c - the program's symbol table (.symtab): names for addresses.
 *
 * An ELF file is a header, then sections, described by a table of section
 * headers. The symbol table is the section of type SHT_SYMTAB: an array of
 * Elf64_Sym. Each entry's st_name is an offset into a string table, the
 * section whose index is in the symbol table's sh_link.
 */
#include "symbols.h"

#include <elf.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf.h"

static struct symbol *syms;
static int nsyms;
static char *strings;        /* a copy of the string table: the names */

void sym_free(void)
{
    free(syms);
    free(strings);
    syms = NULL;
    strings = NULL;
    nsyms = 0;
}

/* Does [off, off + len) lie inside a file of size bytes? */
static int fits(uint64_t off, uint64_t len, size_t size)
{
    return off <= size && len <= size - off;
}

static int by_addr(const void *a, const void *b)
{
    const struct symbol *x = a, *y = b;
    return (x->addr > y->addr) - (x->addr < y->addr);
}

int sym_load(const char *path)
{
    size_t size;
    unsigned char *file = elf_read_file(path, &size);

    sym_free();
    if (file == NULL)
        return -1;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
        !fits(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr), size)) {
        free(file);
        return -1;
    }
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(file + eh->e_shoff);

    /* Prefer the full symbol table; a stripped file only has .dynsym, the
     * symbols the dynamic linker needs. */
    const Elf64_Shdr *symtab = NULL;
    for (int i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB)
            symtab = &sh[i];
    for (int i = 0; symtab == NULL && i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_DYNSYM)
            symtab = &sh[i];
    if (symtab == NULL || symtab->sh_link >= eh->e_shnum) {
        free(file);
        return 0;
    }
    const Elf64_Shdr *strtab = &sh[symtab->sh_link];
    if (!fits(symtab->sh_offset, symtab->sh_size, size) ||
        !fits(strtab->sh_offset, strtab->sh_size, size) || strtab->sh_size == 0) {
        free(file);
        return -1;
    }

    size_t count = symtab->sh_size / sizeof(Elf64_Sym);
    const Elf64_Sym *st = (const Elf64_Sym *)(file + symtab->sh_offset);
    syms = calloc(count ? count : 1, sizeof *syms);
    strings = malloc(strtab->sh_size + 1);
    if (syms == NULL || strings == NULL) {
        sym_free();
        free(file);
        return -1;
    }
    memcpy(strings, file + strtab->sh_offset, strtab->sh_size);
    strings[strtab->sh_size] = '\0';    /* in case the last name is cut off */

    for (size_t i = 0; i < count; i++) {
        int type = ELF64_ST_TYPE(st[i].st_info);
        if ((type != STT_FUNC && type != STT_OBJECT) || st[i].st_value == 0 ||
            st[i].st_name >= strtab->sh_size)
            continue;
        syms[nsyms++] = (struct symbol){
            .name = strings + st[i].st_name,
            .addr = st[i].st_value,
            .size = st[i].st_size,
            .kind = type == STT_FUNC ? SYM_FUNC : SYM_OBJECT,
        };
    }
    free(file);
    qsort(syms, nsyms, sizeof *syms, by_addr);
    return nsyms;
}

const struct symbol *sym_by_name(const char *name)
{
    for (int i = 0; i < nsyms; i++)
        if (strcmp(syms[i].name, name) == 0)
            return &syms[i];
    return NULL;
}

const struct symbol *sym_by_addr(uint64_t addr)
{
    /* Sorted by address: the last symbol that starts at or before addr is
     * the only candidate (symbols do not overlap). */
    const struct symbol *best = NULL;
    for (int i = 0; i < nsyms && syms[i].addr <= addr; i++)
        best = &syms[i];
    if (best == NULL)
        return NULL;
    if (addr < best->addr + best->size || addr == best->addr)
        return best;
    return NULL;
}

/* Parse a whole string as a number: decimal, or hex with 0x. */
static int parse_number(const char *s, uint64_t *out)
{
    char *end;

    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || *s == '-')
        return -1;
    *out = v;
    return 0;
}

int sym_parse(const char *text, uint64_t base, uint64_t *addr)
{
    char name[256];
    uint64_t offset = 0;

    if (text == NULL || *text == '\0')
        return -1;
    if (text[0] >= '0' && text[0] <= '9')
        return parse_number(text, addr);

    const char *plus = strchr(text, '+');
    size_t len = plus ? (size_t)(plus - text) : strlen(text);
    if (len >= sizeof name || (plus != NULL && parse_number(plus + 1, &offset) < 0))
        return -1;
    memcpy(name, text, len);
    name[len] = '\0';

    const struct symbol *s = sym_by_name(name);
    if (s == NULL)
        return -1;
    *addr = base + s->addr + offset;
    return 0;
}

char *sym_describe(uint64_t addr, char *buf, size_t size)
{
    const struct symbol *s = sym_by_addr(addr);

    if (s == NULL)
        return NULL;
    if (addr == s->addr)
        snprintf(buf, size, "%s", s->name);
    else
        snprintf(buf, size, "%s+%llu", s->name, (unsigned long long)(addr - s->addr));
    return buf;
}
