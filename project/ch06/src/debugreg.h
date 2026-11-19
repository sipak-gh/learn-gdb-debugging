/* debugreg.h - the x86 debug registers: hardware watchpoints.
 *
 * DR0-DR3 hold up to four addresses. DR7 says, for each, whether it is
 * enabled, what to watch for (execution, writes, reads or writes) and how
 * many bytes (1, 2, 4 or 8). When one triggers, the CPU sets its bit in
 * DR6 and raises a debug exception: the tracee stops with SIGTRAP.
 */
#ifndef TDB_DEBUGREG_H
#define TDB_DEBUGREG_H

#include <stdint.h>

#include "proc.h"

#define DR_SLOTS 4

/* The R/W field of DR7: what triggers the watchpoint. */
enum dr_kind { DR_EXEC = 0, DR_WRITE = 1, DR_ACCESS = 3 };

/* dr7 with slot enabled to watch len bytes (1, 2, 4 or 8) for kind. */
uint64_t dr7_enable(uint64_t dr7, int slot, enum dr_kind kind, int len);

/* dr7 with slot disabled and its fields cleared. */
uint64_t dr7_disable(uint64_t dr7, int slot);

/* Read or write debug register n (0-7) of the stopped process. 0 or -1. */
int dr_read(struct proc *p, int n, uint64_t *value);
int dr_write(struct proc *p, int n, uint64_t value);

/* Watch len bytes at addr in slot / stop watching. 0 or -1. */
int dr_set(struct proc *p, int slot, uint64_t addr, enum dr_kind kind, int len);
int dr_clear(struct proc *p, int slot);

/* Which slot triggered the last stop (from DR6), or -1. Clears DR6. */
int dr_hit(struct proc *p);

#endif
