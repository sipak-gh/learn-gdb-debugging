/* run.c - resuming the process and finding out why it stopped. */
#include "run.h"

#include <signal.h>
#include <sys/wait.h>

#include "breakpoint.h"
#include "debugreg.h"

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

/* Count a hit of bp. true if we should stop, false if it is ignored. */
static bool count_hit(struct breakpoint *bp)
{
    bp->hits++;
    if (bp->ignore > 0) {
        bp->ignore--;
        return false;
    }
    return true;
}

/* After a SIGTRAP: did a watchpoint trigger? Returns 1 if st now describes
 * a watchpoint stop, 2 if one triggered but we should go on (no change, or
 * ignored), 0 if it was not a watchpoint, -1 on error. */
static int check_watch(struct proc *p, struct stop *st)
{
    int slot = dr_hit(p);
    if (slot < 0)
        return 0;
    struct breakpoint *bp = bp_by_slot(slot);
    if (bp == NULL)
        return 0;

    uint64_t value;
    if (bp_watch_value(p, bp, &value) < 0)
        return -1;
    uint64_t old = bp->old;
    bp->old = value;
    /* Like gdb, a write watchpoint only stops if the value changed. */
    if (bp->type == BP_WATCH && value == old)
        return 2;
    if (!count_hit(bp))
        return 2;
    st->why = STOP_WATCH;
    st->bp_id = bp->id;
    st->temporary = false;
    st->old = old;
    st->value = value;
    return 1;
}

/* After a SIGTRAP | 0x80: a system call entry or exit. Returns 1 if a
 * catchpoint wants to stop (st filled in), 0 to go on, -1 on error. */
static int check_syscall(struct proc *p, struct stop *st)
{
    struct user_regs_struct regs;

    p->in_syscall = !p->in_syscall;
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    /* rax is overwritten by the result; the kernel keeps the number in
     * orig_rax. */
    struct breakpoint *bp = bp_syscall((long)regs.orig_rax);
    if (bp == NULL || !count_hit(bp))
        return 0;
    st->why = STOP_SYSCALL;
    st->bp_id = bp->id;
    st->temporary = false;
    st->entry = p->in_syscall;
    st->sysno = (long)regs.orig_rax;
    /* The system call convention: rdi, rsi, rdx, r10, r8, r9. */
    st->args[0] = regs.rdi;
    st->args[1] = regs.rsi;
    st->args[2] = regs.rdx;
    st->args[3] = regs.r10;
    st->args[4] = regs.r8;
    st->args[5] = regs.r9;
    st->ret = (int64_t)regs.rax;
    return 1;
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
    if (p->state == PROC_STOPPED && WSTOPSIG(p->status) == SIGTRAP) {
        /* The instruction we stepped may itself write a watched variable. */
        int w = check_watch(p, st);
        return w == 1 ? 1 : w < 0 ? -1 : 0;
    }
    classify(p, st);
    return 1;
}

int run_continue(struct proc *p, struct stop *st)
{
    for (;;) {
        int r = step_over_breakpoint(p, st);
        if (r != 0)
            return r < 0 ? -1 : 0;
        if ((bp_catching_syscalls() ? proc_syscall(p) : proc_continue(p)) < 0)
            return -1;
        if (p->state != PROC_STOPPED) {
            classify(p, st);
            return 0;
        }
        if (WSTOPSIG(p->status) == (SIGTRAP | 0x80)) {
            r = check_syscall(p, st);
            if (r != 0)
                return r < 0 ? -1 : 0;
            continue;
        }
        if (WSTOPSIG(p->status) != SIGTRAP) {
            classify(p, st);
            return 0;
        }

        /* A SIGTRAP: a watchpoint (DR6 says which), or one of our int3
         * bytes (then rip is one past it)? */
        r = check_watch(p, st);
        if (r == 1 || r < 0)
            return r < 0 ? -1 : 0;
        if (r == 2)
            continue;
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
        if (!count_hit(bp))
            continue;
        st->why = STOP_BREAKPOINT;
        st->bp_id = bp->id;
        st->temporary = bp->temporary;
        if (bp->temporary)
            bp_delete(p, bp->id);
        return 0;
    }
}
