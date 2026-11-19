/* breakpoint.c - software breakpoints: an int3 (0xCC) byte in the code. */
#include "breakpoint.h"

#include <inttypes.h>
#include <stdio.h>

static struct breakpoint bps[BP_MAX];
static int next_id = 1;

struct breakpoint *bp_add(struct proc *p, uint64_t addr, bool temporary)
{
    for (int i = 0; i < BP_MAX; i++) {
        struct breakpoint *bp = &bps[i];
        if (bp->id != 0)
            continue;
        *bp = (struct breakpoint){ .id = next_id++, .addr = addr, .temporary = temporary };
        if (p->state == PROC_STOPPED && bp_insert(p, bp) < 0)
            printf("Warning: cannot insert breakpoint %d at 0x%" PRIx64 ".\n",
                   bp->id, addr + p->load_base);
        return bp;
    }
    return NULL;
}

int bp_delete(struct proc *p, int id)
{
    struct breakpoint *bp = bp_find(id);
    if (bp == NULL)
        return -1;
    if (bp->inserted)
        bp_remove(p, bp);
    bp->id = 0;
    return 0;
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
        if (bps[i].id != 0 && bps[i].inserted && bps[i].addr + p->load_base == addr)
            return &bps[i];
    return NULL;
}

int bp_insert(struct proc *p, struct breakpoint *bp)
{
    uint64_t addr = bp->addr + p->load_base;
    uint64_t word;

    if (bp->inserted)
        return 0;
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

    if (!bp->inserted)
        return 0;
    if (proc_peek(p, addr, &word) < 0)
        return -1;
    if (proc_poke(p, addr, (word & ~(uint64_t)0xff) | bp->saved) < 0)
        return -1;
    bp->inserted = false;
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
        bps[i].inserted = false;
}

void bp_list(const struct proc *p)
{
    int n = 0;

    for (int i = 0; i < BP_MAX; i++) {
        const struct breakpoint *bp = &bps[i];
        if (bp->id == 0)
            continue;
        if (n++ == 0)
            printf("Num  Disp  Address  In memory           Hits\n");
        printf("%-4d %-5s 0x%-6" PRIx64 " ", bp->id, bp->temporary ? "del" : "keep", bp->addr);
        if (p->state == PROC_STOPPED)
            printf("0x%016" PRIx64 "  %u", bp->addr + p->load_base, bp->hits);
        else
            printf("%-18s  %u", "(not running)", bp->hits);
        if (bp->ignore > 0)
            printf("  (ignore next %u hits)", bp->ignore);
        putchar('\n');
    }
    if (n == 0)
        printf("No breakpoints.\n");
}
