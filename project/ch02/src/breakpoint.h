/* breakpoint.h - software breakpoints: an int3 (0xCC) byte in the code. */
#ifndef TDB_BREAKPOINT_H
#define TDB_BREAKPOINT_H

#include <stdbool.h>
#include <stdint.h>

#include "proc.h"

#define BP_MAX 32

struct breakpoint {
    int id;              /* 1, 2, ... as shown to the user; 0: a free slot */
    uint64_t addr;       /* address in the file, as nm and objdump show it */
    bool temporary;      /* tbreak: delete after the first hit */
    bool inserted;       /* the 0xCC is in the process's memory now */
    uint8_t saved;       /* the byte the 0xCC replaced */
    unsigned hits;
    unsigned ignore;     /* hits still to skip without stopping */
};

/* Add a breakpoint; insert it now if the process is stopped. Returns NULL
 * if the table is full. */
struct breakpoint *bp_add(struct proc *p, uint64_t addr, bool temporary);

/* Remove breakpoint id (from memory too). 0, or -1 if there is none. */
int bp_delete(struct proc *p, int id);

/* The breakpoint with this id, or NULL. */
struct breakpoint *bp_find(int id);

/* The inserted breakpoint at this run-time address, or NULL. */
struct breakpoint *bp_at(const struct proc *p, uint64_t addr);

/* Write the 0xCC of one or all breakpoints into memory / put the saved
 * bytes back. 0 or -1. */
int bp_insert(struct proc *p, struct breakpoint *bp);
int bp_remove(struct proc *p, struct breakpoint *bp);
int bp_insert_all(struct proc *p);

/* The process is gone: nothing is inserted any more. */
void bp_forget_inserted(void);

/* Print the table (info breakpoints). */
void bp_list(const struct proc *p);

#endif
