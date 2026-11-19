/* run.h - resuming the process and finding out why it stopped. */
#ifndef TDB_RUN_H
#define TDB_RUN_H

#include <stdbool.h>

#include "proc.h"

enum stop_reason {
    STOP_BREAKPOINT,     /* hit breakpoint stop.bp_id; rip is at its address */
    STOP_SIGNAL,         /* got signal stop.sig (passed on at the next resume) */
    STOP_STEP,           /* finished a single step */
    STOP_EXITED,         /* exited or was killed: see proc.status */
};

struct stop {
    enum stop_reason why;
    int bp_id;
    bool temporary;      /* the breakpoint was a tbreak (now deleted) */
    int sig;
};

/* Continue until a breakpoint, a signal or the end. 0 or -1. */
int run_continue(struct proc *p, struct stop *st);

#endif
