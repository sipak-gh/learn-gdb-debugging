/* regs.c - registers by name: the fields of struct user_regs_struct.
 *
 * PTRACE_GETREGS fills in a struct user_regs_struct (<sys/user.h>), one
 * unsigned long long per register. A table of names and offsetof() values
 * lets the commands find a register by name without a long switch.
 */
#include "regs.h"

#include <stdio.h>
#include <string.h>

#define REG(name) { #name, offsetof(struct user_regs_struct, name) }

const struct reg_info reg_table[] = {
    REG(rax), REG(rbx), REG(rcx), REG(rdx), REG(rsi), REG(rdi), REG(rbp), REG(rsp),
    REG(r8),  REG(r9),  REG(r10), REG(r11), REG(r12), REG(r13), REG(r14), REG(r15),
    REG(rip), REG(eflags), REG(cs), REG(ss), REG(ds), REG(es), REG(fs), REG(gs),
    REG(fs_base), REG(gs_base), REG(orig_rax),
};

const int reg_count = sizeof reg_table / sizeof reg_table[0];

const struct reg_info *reg_find(const char *name)
{
    if (name[0] == '$')
        name++;
    for (int i = 0; i < reg_count; i++)
        if (strcmp(reg_table[i].name, name) == 0)
            return &reg_table[i];
    return NULL;
}

uint64_t reg_get(const struct user_regs_struct *regs, const struct reg_info *r)
{
    uint64_t value;
    memcpy(&value, (const char *)regs + r->offset, sizeof value);
    return value;
}

void reg_set(struct user_regs_struct *regs, const struct reg_info *r, uint64_t value)
{
    memcpy((char *)regs + r->offset, &value, sizeof value);
}

char *reg_flags(uint64_t eflags, char *buf, size_t size)
{
    /* Bit numbers from the Intel manual (the EFLAGS register), low to high
     * as gdb lists them. */
    static const struct {
        int bit;
        const char *name;
    } flags[] = {
        { 0, "CF" }, { 2, "PF" }, { 4, "AF" }, { 6, "ZF" }, { 7, "SF" }, { 8, "TF" },
        { 9, "IF" }, { 10, "DF" }, { 11, "OF" }, { 14, "NT" }, { 16, "RF" }, { 17, "VM" },
        { 18, "AC" }, { 19, "VIF" }, { 20, "VIP" }, { 21, "ID" },
    };
    size_t len = (size_t)snprintf(buf, size, "[ ");

    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++)
        if ((eflags >> flags[i].bit) & 1 && len < size)
            len += (size_t)snprintf(buf + len, size - len, "%s ", flags[i].name);
    if (len < size)
        snprintf(buf + len, size - len, "]");
    return buf;
}
