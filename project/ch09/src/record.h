/* record.h - record and replay: log every instruction the program
 * executes, then go back in time by replaying the log in a copy of the
 * process made when the record started (reverse-stepi, reverse-continue,
 * reverse-next, reverse-finish). */
#ifndef TDB_RECORD_H
#define TDB_RECORD_H

#include <stdbool.h>

#include "proc.h"
#include "run.h"

#define REC_MAX 200000   /* instructions in the log: gdb's default limit */

/* Is the record on? Are we in the past (replaying the log)? */
bool rec_on(void);
bool rec_replaying(void);

/* record: start recording p, from now. 0, or -1 (after saying why). */
int rec_start(struct proc *p);

/* record stop: back to the present and forget the log. */
void rec_stop(struct proc *p);

/* The same without a message: before run, kill, detach, restart. */
void rec_discard(struct proc *p);

/* info record */
void rec_info(void);

/* Execute one instruction, recording it (in the present) or replaying it
 * from the log (in the past); step does the work (run.c's step_one())
 * and rec_exec() returns what it returns: 0, 1 (a stop, st filled in), 2
 * (a signal: try again) or -1. Also 1 with STOP_RECORD when the record
 * cannot go on: the end of the log, or the program is about to exit. */
int rec_exec(struct proc *p, struct stop *st, int (*step)(struct proc *, struct stop *));

/* Go back: one instruction; to the last breakpoint or change of a
 * watched value; to the start of the previous line in this function; to
 * the call of this function. 0 (st filled in: STOP_STEP, STOP_BREAKPOINT,
 * STOP_WATCH, or STOP_RECORD at the start of the log) or -1. */
int rec_reverse_stepi(struct proc *p, struct stop *st);
int rec_reverse_continue(struct proc *p, struct stop *st);
int rec_reverse_next(struct proc *p, struct stop *st);
int rec_reverse_finish(struct proc *p, struct stop *st);

/* record goto begin|end|N: go to instruction N of the log. 0 or -1. */
int rec_goto(struct proc *p, const char *where);

#endif
