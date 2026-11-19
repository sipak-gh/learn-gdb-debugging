/* step.h - stepping by source lines (step, next) and out of a function
 * (finish), built from single instructions and breakpoints. */
#ifndef TDB_STEP_H
#define TDB_STEP_H

#include <stdbool.h>

#include "proc.h"
#include "run.h"

/* Run until the program reaches the start of a statement on another
 * source line. A call to a function with line information is entered
 * (step) and the stop is after that function's prologue; with over (next),
 * or if the function has no line information, the call is run to its end.
 * Stops early for a breakpoint, a signal or the end of the program, as
 * continue would. STOP_STEP if it got there. 0 (st filled in) or -1. */
int step_line(struct proc *p, struct stop *st, bool over);

/* Run until the current function returns to its caller (finish). The
 * stop is just after the call, usually in the middle of a line.
 * STOP_STEP if it got there. 0 (st filled in) or -1. */
int step_finish(struct proc *p, struct stop *st);

#endif
