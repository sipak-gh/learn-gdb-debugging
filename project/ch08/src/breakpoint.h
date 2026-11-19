/* breakpoint.h - the places where the program should stop: software
 * breakpoints (an int3 byte in the code), hardware watchpoints (a debug
 * register) and system call catchpoints. They share one table and one
 * numbering, as in gdb. */
#ifndef TDB_BREAKPOINT_H
#define TDB_BREAKPOINT_H

#include <stdbool.h>
#include <stddef.h>
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
    int id;              /* 1, 2, ... as shown to the user; 0: a free slot;
                          * negative: an internal breakpoint */
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
    char *commands;      /* tdb commands to run when it stops here, one per
                          * line (malloc'd), or NULL */
    bool silent;         /* the first of them is "silent": do not report the stop */
};

/* Add a breakpoint; insert it now if the process is stopped. Returns NULL
 * if the table is full. */
struct breakpoint *bp_add(struct proc *p, uint64_t addr, bool temporary);

/* Add an internal breakpoint, one tdb uses itself (to stop when a call
 * returns, for next and finish). It has a negative id and is not listed;
 * delete it with bp_delete(). NULL if the table is full. */
struct breakpoint *bp_add_internal(struct proc *p, uint64_t addr);
/* Add a watchpoint of type BP_WATCH or BP_RWATCH on len bytes at the file
 * address addr. Returns NULL (and says why) if it cannot be set. */
struct breakpoint *bp_add_watch(struct proc *p, enum bp_type type, uint64_t addr, int len,
                                const char *what);

/* Add a catchpoint for system call sysno (-1: all). NULL if full. */
struct breakpoint *bp_add_syscall(long sysno);

/* Remove breakpoint id (from memory too). 0, or -1 if there is none. */
int bp_delete(struct proc *p, int id);

/* Give breakpoint bp these commands (malloc'd, lines separated by '\n';
 * bp frees them), or none (NULL or ""). */
void bp_set_commands(struct breakpoint *bp, char *commands);

/* The breakpoint with this id, or NULL. */
struct breakpoint *bp_find(int id);

/* The inserted software breakpoint at this run-time address, or NULL.
 * There may be more than one: ask again after removing it. */
struct breakpoint *bp_at(const struct proc *p, uint64_t addr);

/* The inserted watchpoint using debug register slot, or NULL. */
struct breakpoint *bp_by_slot(int slot);

/* The catchpoint for system call nr, or NULL. Any catchpoints at all? */
struct breakpoint *bp_syscall(long nr);
bool bp_catching_syscalls(void);

/* Read the current value of a watchpoint's bytes. 0 or -1. */
int bp_watch_value(struct proc *p, const struct breakpoint *bp, uint64_t *value);

/* Read len bytes of code at the run-time address addr as they are in the
 * file: where a breakpoint's 0xCC is, put back the byte it replaced.
 * 0 or -1. */
int bp_read_code(struct proc *p, uint64_t addr, unsigned char *buf, size_t len);
/* Put one or all breakpoints into the process / take them out. 0 or -1. */
int bp_insert(struct proc *p, struct breakpoint *bp);
int bp_remove(struct proc *p, struct breakpoint *bp);
int bp_insert_all(struct proc *p);

/* The process is gone: nothing is inserted any more. */
void bp_forget_inserted(void);

/* Take every breakpoint and watchpoint out of the process (before
 * detaching: it must go on without our 0xCC bytes). */
void bp_remove_all(struct proc *p);

/* Put the original bytes back in c, a copy of the process made by
 * fork(), without changing the table: the breakpoints stay in p. */
void bp_remove_copy(struct proc *c);

/* Print the table (info breakpoints). */
void bp_list(const struct proc *p);

#endif
