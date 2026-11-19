/* symbols.h - the program's symbol table (.symtab): names for addresses. */
#ifndef TDB_SYMBOLS_H
#define TDB_SYMBOLS_H

#include <stddef.h>
#include <stdint.h>

enum sym_kind { SYM_FUNC, SYM_OBJECT };

struct symbol {
    const char *name;
    uint64_t addr;       /* address in the file, as nm shows it */
    uint64_t size;       /* bytes; 0 if the symbol table does not say */
    enum sym_kind kind;  /* a function (code) or an object (data) */
};

/* Read the functions and data objects from the ELF file's .symtab (or
 * .dynsym if it was stripped). Replaces any table loaded before. Returns
 * the number of symbols, or -1 if path is not a 64-bit ELF file. */
int sym_load(const char *path);

/* Forget the table. */
void sym_free(void);

/* The symbol called name, or NULL. */
const struct symbol *sym_by_name(const char *name);

/* The symbol whose bytes contain the file address addr, or NULL. */
const struct symbol *sym_by_addr(uint64_t addr);

/* Parse a location: a number ("0x1149", "4425"), a symbol ("main") or a
 * symbol plus an offset ("main+4", "main+0x10"). A number is returned as
 * it is; a symbol's address has base added. 0, or -1 if text is neither. */
int sym_parse(const char *text, uint64_t base, uint64_t *addr);

/* Write "main+13" (or "main") for the file address addr into buf.
 * Returns buf, or NULL (and writes nothing) if no symbol contains addr. */
char *sym_describe(uint64_t addr, char *buf, size_t size);

#endif
