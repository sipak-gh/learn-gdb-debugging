/* run.h - resuming the process and finding out why it stopped. */
#ifndef TDB_RUN_H
#define TDB_RUN_H

#include <stdbool.h>
#include <stdint.h>

#include "proc.h"

enum stop_reason {
    STOP_BREAKPOINT,     /* hit breakpoint stop.bp_id; rip is at its address */
    STOP_WATCH,          /* watchpoint stop.bp_id triggered; see old/value */
    STOP_SYSCALL,        /* catchpoint stop.bp_id: entry to or exit from sysno */
    STOP_SIGNAL,         /* got signal stop.sig (passed on at the next resume) */
    STOP_STEP,           /* finished a step (stepi, step, next, finish) */
    STOP_EXITED,         /* exited or was killed: see proc.status */
};

struct stop {
    enum stop_reason why;
    int bp_id;
    bool temporary;      /* the breakpoint was a tbreak (now deleted) */
    int sig;
    uint64_t old, value; /* STOP_WATCH: the value before and now */
    bool entry;          /* STOP_SYSCALL: entry (true) or exit (false) */
    long sysno;          /* STOP_SYSCALL: which system call */
    uint64_t args[6];    /* STOP_SYSCALL at entry: the arguments */
    int64_t ret;         /* STOP_SYSCALL at exit: the result (-errno on error) */
};

/* Continue all threads until one of them stops at a breakpoint,
 * watchpoint, catchpoint, signal or the end; then stop the others (all-
 * stop, as gdb does by default). That thread becomes the current one.
 * 0 or -1. */
int run_continue(struct proc *p, struct stop *st);

/* Execute one machine instruction in the current thread (stepi); the
 * other threads stay stopped: STOP_STEP, or STOP_BREAKPOINT
 * if it lands on a breakpoint, or whatever else stopped it. 0 or -1. */
int run_stepi(struct proc *p, struct stop *st);

#endif
