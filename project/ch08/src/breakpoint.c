/* breakpoint.c - software breakpoints, hardware watchpoints and system
 * call catchpoints, in one table. */
#include "breakpoint.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugreg.h"
#include "lines.h"
#include "symbols.h"
#include "syscalls.h"

static struct breakpoint bps[BP_MAX];
static int next_id = 1;
static int next_internal = -1;

/* A free entry, filled in with id; NULL if the table is full. */
static struct breakpoint *new_entry_id(enum bp_type type, int id)
{
    for (int i = 0; i < BP_MAX; i++) {
        if (bps[i].id == 0) {
            bps[i] = (struct breakpoint){ .id = id, .type = type, .slot = -1 };
            return &bps[i];
        }
    }
    return NULL;
}

/* The same with the next user-visible id. */
static struct breakpoint *new_entry(enum bp_type type)
{
    struct breakpoint *bp = new_entry_id(type, next_id);
    if (bp != NULL)
        next_id++;
    return bp;
}

struct breakpoint *bp_add(struct proc *p, uint64_t addr, bool temporary)
{
    struct breakpoint *bp = new_entry(BP_BREAK);
    if (bp == NULL)
        return NULL;
    bp->addr = addr;
    bp->temporary = temporary;
    if (p->state == PROC_STOPPED && bp_insert(p, bp) < 0)
        printf("Warning: cannot insert breakpoint %d at 0x%" PRIx64 ".\n",
               bp->id, addr + p->load_base);
    return bp;
}

struct breakpoint *bp_add_internal(struct proc *p, uint64_t addr)
{
    struct breakpoint *bp = new_entry_id(BP_BREAK, next_internal);
    if (bp == NULL)
        return NULL;
    next_internal--;
    bp->addr = addr;
    if (p->state == PROC_STOPPED)
        bp_insert(p, bp);
    return bp;
}

/* A debug register no watchpoint uses, or -1. */
static int free_slot(void)
{
    for (int slot = 0; slot < DR_SLOTS; slot++) {
        bool used = false;
        for (int i = 0; i < BP_MAX; i++)
            if (bps[i].id != 0 && bps[i].slot == slot)
                used = true;
        if (!used)
            return slot;
    }
    return -1;
}

struct breakpoint *bp_add_watch(struct proc *p, enum bp_type type, uint64_t addr, int len,
                                const char *what)
{
    if (len != 1 && len != 2 && len != 4 && len != 8) {
        printf("A watchpoint covers 1, 2, 4 or 8 bytes, not %d.\n", len);
        return NULL;
    }
    /* The CPU compares addresses with the low bits masked off: a 4-byte
     * watchpoint must start at a multiple of 4. */
    if (addr % (uint64_t)len != 0) {
        printf("0x%" PRIx64 " is not aligned to %d bytes.\n", addr, len);
        return NULL;
    }
    int slot = free_slot();
    if (slot < 0) {
        printf("No free debug register: x86 has only %d.\n", DR_SLOTS);
        return NULL;
    }
    struct breakpoint *bp = new_entry(type);
    if (bp == NULL) {
        printf("Too many breakpoints.\n");
        return NULL;
    }
    bp->addr = addr;
    bp->len = len;
    bp->slot = slot;
    snprintf(bp->what, sizeof bp->what, "%s", what);
    if (p->state == PROC_STOPPED && bp_insert(p, bp) < 0) {
        printf("Cannot set debug register %d.\n", slot);
        bp->id = 0;
        return NULL;
    }
    return bp;
}

struct breakpoint *bp_add_syscall(long sysno)
{
    struct breakpoint *bp = new_entry(BP_SYSCALL);
    if (bp != NULL) {
        bp->sysno = sysno;
        bp->inserted = true;    /* nothing to write: run.c resumes with PTRACE_SYSCALL */
    }
    return bp;
}

int bp_delete(struct proc *p, int id)
{
    struct breakpoint *bp = bp_find(id);
    if (bp == NULL)
        return -1;
    if (bp->inserted)
        bp_remove(p, bp);
    bp_set_commands(bp, NULL);
    bp->id = 0;
    bp->slot = -1;
    return 0;
}

void bp_set_commands(struct breakpoint *bp, char *commands)
{
    free(bp->commands);
    if (commands != NULL && commands[0] == '\0') {
        free(commands);
        commands = NULL;
    }
    bp->commands = commands;
    bp->silent = commands != NULL && strncmp(commands, "silent", 6) == 0 &&
                 (commands[6] == '\n' || commands[6] == '\0');
}

struct breakpoint *bp_find(int id)
{
    for (int i = 0; i < BP_MAX; i++)
        if (id != 0 && bps[i].id == id)
            return &bps[i];
    return NULL;
}

struct breakpoint *bp_at(const struct proc *p, uint64_t addr)
{
    for (int i = 0; i < BP_MAX; i++)
        if (bps[i].id != 0 && bps[i].type == BP_BREAK && bps[i].inserted &&
            bps[i].addr + p->load_base == addr)
            return &bps[i];
    return NULL;
}

struct breakpoint *bp_by_slot(int slot)
{
    for (int i = 0; i < BP_MAX; i++)
        if (bps[i].id != 0 && slot >= 0 && bps[i].slot == slot && bps[i].inserted)
            return &bps[i];
    return NULL;
}

struct breakpoint *bp_syscall(long nr)
{
    for (int i = 0; i < BP_MAX; i++)
        if (bps[i].id != 0 && bps[i].type == BP_SYSCALL &&
            (bps[i].sysno == nr || bps[i].sysno == -1))
            return &bps[i];
    return NULL;
}

bool bp_catching_syscalls(void)
{
    for (int i = 0; i < BP_MAX; i++)
        if (bps[i].id != 0 && bps[i].type == BP_SYSCALL)
            return true;
    return false;
}

int bp_watch_value(struct proc *p, const struct breakpoint *bp, uint64_t *value)
{
    *value = 0;
    return proc_read(p, bp->addr + p->load_base, value, (size_t)bp->len);
}

int bp_insert(struct proc *p, struct breakpoint *bp)
{
    uint64_t addr = bp->addr + p->load_base;
    uint64_t word;

    if (bp->inserted)
        return 0;
    if (bp->type == BP_WATCH || bp->type == BP_RWATCH) {
        if (dr_set(p, bp->slot, addr, bp->type == BP_WATCH ? DR_WRITE : DR_ACCESS,
                   bp->len) < 0)
            return -1;
        bp_watch_value(p, bp, &bp->old);
        bp->inserted = true;
        return 0;
    }
    /* Another breakpoint at the same address (break main twice, or the
     * breakpoint next uses at a return address): the 0xCC is there already.
     * Reading it now would save 0xCC as the original byte. */
    const struct breakpoint *other = bp_at(p, addr);
    if (other != NULL) {
        bp->saved = other->saved;
        bp->inserted = true;
        return 0;
    }
    /* ptrace reads and writes whole 8-byte words: change only the low byte
     * (x86-64 is little-endian, so that is the byte at addr). */
    if (proc_peek(p, addr, &word) < 0)
        return -1;
    bp->saved = word & 0xff;
    if (proc_poke(p, addr, (word & ~(uint64_t)0xff) | 0xcc) < 0)
        return -1;
    bp->inserted = true;
    return 0;
}

int bp_remove(struct proc *p, struct breakpoint *bp)
{
    uint64_t addr = bp->addr + p->load_base;
    uint64_t word;

    if (!bp->inserted || bp->type == BP_SYSCALL)
        return 0;
    if (bp->type == BP_WATCH || bp->type == BP_RWATCH) {
        bp->inserted = false;
        return p->state == PROC_STOPPED ? dr_clear(p, bp->slot) : 0;
    }
    /* Leave the 0xCC if another breakpoint still needs it. */
    bp->inserted = false;
    if (bp_at(p, addr) != NULL)
        return 0;
    if (proc_peek(p, addr, &word) < 0 ||
        proc_poke(p, addr, (word & ~(uint64_t)0xff) | bp->saved) < 0) {
        bp->inserted = true;
        return -1;
    }
    return 0;
}

int bp_read_code(struct proc *p, uint64_t addr, unsigned char *buf, size_t len)
{
    if (proc_read(p, addr, buf, len) < 0)
        return -1;
    for (size_t i = 0; i < len; i++) {
        const struct breakpoint *bp = bp_at(p, addr + i);
        if (bp != NULL)
            buf[i] = bp->saved;
    }
    return 0;
}

int bp_insert_all(struct proc *p)
{
    int ret = 0;

    for (int i = 0; i < BP_MAX; i++) {
        if (bps[i].id != 0 && bp_insert(p, &bps[i]) < 0) {
            printf("Warning: cannot insert breakpoint %d at 0x%" PRIx64 ".\n",
                   bps[i].id, bps[i].addr + p->load_base);
            ret = -1;
        }
    }
    return ret;
}

void bp_forget_inserted(void)
{
    for (int i = 0; i < BP_MAX; i++)
        if (bps[i].type != BP_SYSCALL)
            bps[i].inserted = false;
}

void bp_remove_all(struct proc *p)
{
    for (int i = 0; i < BP_MAX; i++)
        if (bps[i].id != 0)
            bp_remove(p, &bps[i]);
}

void bp_remove_copy(struct proc *c)
{
    for (int i = 0; i < BP_MAX; i++) {
        const struct breakpoint *bp = &bps[i];
        uint64_t addr = bp->addr + c->load_base, word;
        if (bp->id == 0 || bp->type != BP_BREAK || !bp->inserted)
            continue;
        if (proc_peek(c, addr, &word) == 0)
            proc_poke(c, addr, (word & ~(uint64_t)0xff) | bp->saved);
    }
}

void bp_list(const struct proc *p)
{
    static const char *const types[] = {
        [BP_BREAK] = "breakpoint", [BP_WATCH] = "watchpoint",
        [BP_RWATCH] = "rwatch", [BP_SYSCALL] = "catch",
    };
    int n = 0;
    char name[256];
    const struct line_row *row;
    const struct symbol *s;

    /* In number order: a new breakpoint may reuse a deleted one's entry. */
    for (int id = 1; id < next_id; id++) {
        const struct breakpoint *bp = bp_find(id);
        if (bp == NULL)
            continue;
        if (n++ == 0)
            printf("Num  Type        Disp  Address  In memory           Hits  What\n");
        printf("%-4d %-11s %-5s ", bp->id, types[bp->type], bp->temporary ? "del" : "keep");
        if (bp->type == BP_SYSCALL) {
            printf("%-8s %-18s  %-4u  syscall %s", "", "", bp->hits,
                   bp->sysno < 0 ? "(any)" : sys_name(bp->sysno));
        } else {
            printf("0x%-6" PRIx64 " ", bp->addr);
            if (p->state == PROC_STOPPED)
                printf("0x%016" PRIx64 "  %-4u", bp->addr + p->load_base, bp->hits);
            else
                printf("%-18s  %-4u", "(not running)", bp->hits);
            if (bp->type != BP_BREAK)
                printf("  %s (%d bytes, DR%d)", bp->what, bp->len, bp->slot);
            else if ((row = line_at(bp->addr)) != NULL && (s = sym_by_addr(bp->addr)) != NULL)
                printf("  in %s at %s:%d", s->name, line_file_name(row->file), row->line);
            else if (sym_describe(bp->addr, name, sizeof name) != NULL)
                printf("  <%s>", name);
        }
        if (bp->ignore > 0)
            printf("  (ignore next %u hits)", bp->ignore);
        putchar('\n');
        /* Its commands, one per line, indented. */
        for (const char *c = bp->commands; c != NULL && *c != '\0';) {
            int len = (int)strcspn(c, "\n");
            printf("        %.*s\n", len, c);
            c += len + (c[len] == '\n');
        }
    }
    if (n == 0)
        printf("No breakpoints.\n");
}
