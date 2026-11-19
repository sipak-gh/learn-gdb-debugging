/* run.c - resuming the process and finding out why it stopped. */
#include "run.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include "breakpoint.h"
#include "debugreg.h"
#include "signals.h"

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
        p->cur->pending_sig = st->sig;
}

/* The process stopped with a signal (not SIGTRAP). Apply the "handle"
 * table: keep the signal to deliver at the next resume if it passes, and
 * return true (st filled in) if we should stop, false to go on. */
static bool signal_stops(struct proc *p, struct stop *st)
{
    int sig = WSTOPSIG(p->status);
    struct sig_action a = sig_action(sig);

    p->cur->pending_sig = a.pass ? sig : 0;
    if (!a.stop) {
        if (a.print)
            printf("Program received signal %s (%s).\n", sig_name(sig), strsignal(sig));
        return false;
    }
    st->why = STOP_SIGNAL;
    st->sig = sig;
    return true;
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

    p->cur->in_syscall = !p->cur->in_syscall;
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
    st->entry = p->cur->in_syscall;
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

/* Execute one instruction. If the process sits on inserted breakpoints,
 * put the saved byte back first and insert the 0xCC again after.
 * Returns 0 if it went fine (the process is stopped after the instruction),
 * 1 if the step stopped for another reason (filled into st), 2 if a signal
 * we do not stop for came instead (try again), -1 on error. */
static int step_one(struct proc *p, struct stop *st)
{
    struct user_regs_struct regs;
    struct breakpoint *here[BP_MAX], *bp;
    int n = 0;

    if (proc_get_regs(p, &regs) < 0)
        return -1;
    while (n < BP_MAX && (bp = bp_at(p, regs.rip)) != NULL) {
        if (bp_remove(p, bp) < 0)
            return -1;
        here[n++] = bp;
    }
    if (proc_step(p) < 0)
        return -1;
    for (int i = 0; i < n && p->state == PROC_STOPPED; i++)
        bp_insert(p, here[i]);
    if (p->state == PROC_STOPPED && WSTOPSIG(p->status) == SIGTRAP) {
        /* The instruction we stepped may itself write a watched variable. */
        int w = check_watch(p, st);
        return w == 1 ? 1 : w < 0 ? -1 : 0;
    }
    if (p->state == PROC_STOPPED)
        return signal_stops(p, st) ? 1 : 2;
    classify(p, st);
    return 1;
}

/* Before continuing from a breakpoint, its instruction must run without
 * the 0xCC: step it. Returns what step_one() does; 0 if there is no
 * breakpoint here. */
static int step_over_breakpoint(struct proc *p, struct stop *st)
{
    struct user_regs_struct regs;
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    return bp_at(p, regs.rip) == NULL ? 0 : step_one(p, st);
}

/* Every thread sitting on a breakpoint (where it stopped, or where the
 * user put one) must step over it, not only the current one. Returns 0,
 * or 1 if a step stopped for another reason (st filled in, that thread
 * is current), -1 on error. */
static int step_all_over_breakpoints(struct proc *p, struct stop *st)
{
    struct thread *cur = p->cur;

    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        struct thread *t = &p->threads[i];
        if (t->tid == 0 || t->running || t->pending != 0)
            continue;
        proc_select(p, t);
        int r;
        while ((r = step_over_breakpoint(p, st)) == 2)
            ;
        if (r != 0)
            return r < 0 ? -1 : 1;
    }
    proc_select(p, cur);
    return 0;
}

/* The child of a fork() is a copy of the process: with our 0xCC bytes in
 * it. Put the original bytes back and let it go (gdb's default,
 * "detach-on-fork on"); else it dies of SIGTRAP at the first one. */
static void detach_child(struct proc *p, pid_t child)
{
    static struct proc c;

    memset(&c, 0, sizeof c);
    c.pid = child;
    c.state = PROC_STOPPED;
    c.load_base = p->load_base;
    c.threads[0].tid = child;
    c.cur = &c.threads[0];
    bp_remove_copy(&c);
    printf("[Detaching after fork from child process %d]\n", (int)child);
    proc_detach(&c);
}

/* Resume all threads and wait until one stops for something to look at;
 * then stop the others too. 0 or -1. */
static int resume_and_wait(struct proc *p)
{
    int request = bp_catching_syscalls() ? PTRACE_SYSCALL : PTRACE_CONT;
    pid_t child;

    if (proc_resume_all(p, request) < 0)
        return -1;
    for (;;) {
        if (proc_wait(p) < 0)
            return -1;
        if (p->state != PROC_STOPPED)
            return 0;
        if (!proc_fork_event(p, &child))
            return proc_stop_all(p);
        detach_child(p, child);
        if (proc_resume(p, p->cur, request) < 0)
            return -1;
    }
}

/* A thread with a stop not yet reported (it came while we were stopping
 * all threads): make it current and report that first. */
static bool take_pending(struct proc *p)
{
    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        struct thread *t = &p->threads[i];
        if (t->tid != 0 && t->pending != 0) {
            t->status = t->pending;
            t->pending = 0;
            proc_select(p, t);
            return true;
        }
    }
    return false;
}

/* A stop at a breakpoint's address (rip is at it): count the hit and fill
 * in st. false if it is ignored. */
static bool breakpoint_stops(struct proc *p, struct breakpoint *bp, struct stop *st)
{
    if (!count_hit(bp))
        return false;
    st->why = STOP_BREAKPOINT;
    st->bp_id = bp->id;
    st->temporary = bp->temporary;
    if (bp->temporary)
        bp_delete(p, bp->id);
    return true;
}

/* As in gdb, "handle SIG nopass" typed at the stop decides too. */
static void drop_nopass_signals(struct proc *p)
{
    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        struct thread *t = &p->threads[i];
        if (t->tid != 0 && t->pending_sig != 0 && !sig_action(t->pending_sig).pass)
            t->pending_sig = 0;
    }
}

int run_stepi(struct proc *p, struct stop *st)
{
    struct user_regs_struct regs;
    int r;

    drop_nopass_signals(p);
    while ((r = step_one(p, st)) == 2)
        ;
    if (r != 0)
        return r < 0 ? -1 : 0;
    /* Landing on a breakpoint counts as hitting it, as in gdb. */
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    struct breakpoint *bp = bp_at(p, regs.rip);
    if (bp == NULL || !breakpoint_stops(p, bp, st))
        st->why = STOP_STEP;
    return 0;
}

int run_continue(struct proc *p, struct stop *st)
{
    drop_nopass_signals(p);
    for (;;) {
        bool pending = take_pending(p);
        if (!pending) {
            int r = step_all_over_breakpoints(p, st);
            if (r != 0)
                return r < 0 ? -1 : 0;
            if (resume_and_wait(p) < 0)
                return -1;
        }
        if (p->state != PROC_STOPPED) {
            classify(p, st);
            return 0;
        }
        if (WSTOPSIG(p->status) == (SIGTRAP | 0x80)) {
            int r = check_syscall(p, st);
            if (r != 0)
                return r < 0 ? -1 : 0;
            continue;
        }
        if (WSTOPSIG(p->status) != SIGTRAP) {
            if (signal_stops(p, st))
                return 0;
            continue;
        }

        /* A SIGTRAP: a watchpoint (DR6 says which), or one of our int3
         * bytes (then rip is one past it)? */
        int r = check_watch(p, st);
        if (r == 1 || r < 0)
            return r < 0 ? -1 : 0;
        if (r == 2)
            continue;
        struct user_regs_struct regs;
        if (proc_get_regs(p, &regs) < 0)
            return -1;
        struct breakpoint *bp = bp_at(p, regs.rip - 1);
        siginfo_t si;
        if (bp == NULL && pending && proc_siginfo(p, &si) == 0 && si.si_code == SI_KERNEL) {
            /* It hit a breakpoint that has been deleted since: back up
             * and run the original instruction. */
            regs.rip -= 1;
            if (proc_set_regs(p, &regs) < 0)
                return -1;
            continue;
        }
        if (bp == NULL) {
            classify(p, st);
            return 0;
        }
        /* Back up so the original instruction runs when we go on. */
        regs.rip -= 1;
        if (proc_set_regs(p, &regs) < 0)
            return -1;
        if (breakpoint_stops(p, bp, st))
            return 0;
    }
}
