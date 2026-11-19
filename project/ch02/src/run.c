/* run.c - resuming the process and finding out why it stopped. */
#include "run.h"

#include <signal.h>
#include <sys/wait.h>

#include "breakpoint.h"

/* Fill in st for a stop that is not one of ours. */
static void classify(struct proc *p, struct stop *st)
{
    if (p->state != PROC_STOPPED) {
        st->why = STOP_EXITED;
        bp_forget_inserted();
        return;
    }
    st->why = STOP_SIGNAL;
    st->sig = WSTOPSIG(p->status);
    /* SIGTRAP is the debugger's own signal: don't pass it on. */
    if (st->sig != SIGTRAP)
        p->pending_sig = st->sig;
}

/* If the process sits on an inserted breakpoint, execute the original
 * instruction: put the saved byte back, single-step, insert the 0xCC again.
 * Returns 0 if it went fine (the process is stopped after the instruction),
 * 1 if the step stopped for another reason (filled into st), -1 on error. */
static int step_over_breakpoint(struct proc *p, struct stop *st)
{
    struct user_regs_struct regs;
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    struct breakpoint *bp = bp_at(p, regs.rip);
    if (bp == NULL)
        return 0;

    if (bp_remove(p, bp) < 0 || proc_step(p) < 0)
        return -1;
    if (p->state == PROC_STOPPED)
        bp_insert(p, bp);
    if (p->state == PROC_STOPPED && WSTOPSIG(p->status) == SIGTRAP)
        return 0;
    classify(p, st);
    return 1;
}

int run_continue(struct proc *p, struct stop *st)
{
    for (;;) {
        int r = step_over_breakpoint(p, st);
        if (r != 0)
            return r < 0 ? -1 : 0;
        if (proc_continue(p) < 0)
            return -1;
        if (p->state != PROC_STOPPED || WSTOPSIG(p->status) != SIGTRAP) {
            classify(p, st);
            return 0;
        }

        /* A SIGTRAP: did we just execute one of our int3 bytes? Then rip is
         * one past it. */
        struct user_regs_struct regs;
        if (proc_get_regs(p, &regs) < 0)
            return -1;
        struct breakpoint *bp = bp_at(p, regs.rip - 1);
        if (bp == NULL) {
            classify(p, st);
            return 0;
        }
        /* Back up so the original instruction runs when we go on. */
        regs.rip -= 1;
        if (proc_set_regs(p, &regs) < 0)
            return -1;
        bp->hits++;
        if (bp->ignore > 0) {
            bp->ignore--;
            continue;
        }
        st->why = STOP_BREAKPOINT;
        st->bp_id = bp->id;
        st->temporary = bp->temporary;
        if (bp->temporary)
            bp_delete(p, bp->id);
        return 0;
    }
}
