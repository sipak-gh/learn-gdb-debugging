/* regs.h - registers by name: the fields of struct user_regs_struct. */
#ifndef TDB_REGS_H
#define TDB_REGS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/user.h>

struct reg_info {
    const char *name;    /* "rax" */
    size_t offset;       /* where it is in struct user_regs_struct */
};

/* The registers in the order gdb's "info registers" shows them. */
extern const struct reg_info reg_table[];
extern const int reg_count;

/* The register called name ("rax" or "$rax"), or NULL. */
const struct reg_info *reg_find(const char *name);

uint64_t reg_get(const struct user_regs_struct *regs, const struct reg_info *r);
void reg_set(struct user_regs_struct *regs, const struct reg_info *r, uint64_t value);

/* Write the set flags of eflags, as gdb shows them ("[ PF ZF IF ]"), into
 * buf. Returns buf. */
char *reg_flags(uint64_t eflags, char *buf, size_t size);

#endif
