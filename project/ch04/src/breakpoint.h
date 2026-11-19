/* breakpoint.h - the places where the program should stop: software
 * breakpoints (an int3 byte in the code), hardware watchpoints (a debug
 * register) and system call catchpoints. They share one table and one
 * numbering, as in gdb. */
#ifndef TDB_BREAKPOINT_H
#define TDB_BREAKPOINT_H

#include <stdbool.h>
#include <stdint.h>

#include "proc.h"

#define BP_MAX 32

enum bp_type {
    BP_BREAK,            /* int3 at addr */
    BP_WATCH,            /* stop after a write to [addr, addr + len) */
    BP_RWATCH,           /* stop after a read or write */
    BP_SYSCALL,          /* stop at entry to and exit from a system call */
};

struct breakpoint {
    int id;              /* 1, 2, ... as shown to the user; 0: a free slot */
    enum bp_type type;
    uint64_t addr;       /* address in the file, as nm and objdump show it */
    bool temporary;      /* tbreak: delete after the first hit */
    bool inserted;       /* in the process now (0xCC written, debug register set) */
    uint8_t saved;       /* BP_BREAK: the byte the 0xCC replaced */
    unsigned hits;
    unsigned ignore;     /* hits still to skip without stopping */
    int len;             /* watchpoints: 1, 2, 4 or 8 bytes */
    int slot;            /* watchpoints: the debug register, 0-3 */
    uint64_t old;        /* watchpoints: the value at the last stop */
    long sysno;          /* BP_SYSCALL: the system call, or -1 for every one */
    char what[64];       /* watchpoints: what the user typed ("counter") */
};

/* Add a breakpoint; insert it now if the process is stopped. Returns NULL
 * if the table is full. */
struct breakpoint *bp_add(struct proc *p, uint64_t addr, bool temporary);

/* Add a watchpoint of type BP_WATCH or BP_RWATCH on len bytes at the file
 * address addr. Returns NULL (and says why) if it cannot be set. */
struct breakpoint *bp_add_watch(struct proc *p, enum bp_type type, uint64_t addr, int len,
                                const char *what);

/* Add a catchpoint for system call sysno (-1: all). NULL if full. */
struct breakpoint *bp_add_syscall(long sysno);

/* Remove breakpoint id (from memory too). 0, or -1 if there is none. */
int bp_delete(struct proc *p, int id);

/* The breakpoint with this id, or NULL. */
struct breakpoint *bp_find(int id);

/* The inserted software breakpoint at this run-time address, or NULL. */
struct breakpoint *bp_at(const struct proc *p, uint64_t addr);

/* The inserted watchpoint using debug register slot, or NULL. */
struct breakpoint *bp_by_slot(int slot);

/* The catchpoint for system call nr, or NULL. Any catchpoints at all? */
struct breakpoint *bp_syscall(long nr);
bool bp_catching_syscalls(void);

/* Read the current value of a watchpoint's bytes. 0 or -1. */
int bp_watch_value(struct proc *p, const struct breakpoint *bp, uint64_t *value);

/* Put one or all breakpoints into the process / take them out. 0 or -1. */
int bp_insert(struct proc *p, struct breakpoint *bp);
int bp_remove(struct proc *p, struct breakpoint *bp);
int bp_insert_all(struct proc *p);

/* The process is gone: nothing is inserted any more. */
void bp_forget_inserted(void);

/* Print the table (info breakpoints). */
void bp_list(const struct proc *p);

#endif
