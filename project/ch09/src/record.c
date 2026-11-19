/* record.c - record and replay.
 *
 * The CPU cannot run backwards, and an instruction does not say what it
 * overwrote. So to go back, tdb goes forward again, from a copy.
 *
 * record makes a snapshot of the program (a stopped copy made with fork(),
 * as checkpoint does) and then executes the program one instruction at a
 * time, keeping a log: the address of each instruction, how deep in
 * calls it is, and, for a system call, its result and the memory it
 * wrote. Going back to instruction i of the log means: fork the snapshot
 * again and single-step the new copy i times. The program is
 * deterministic, so the copy does exactly what the program did. The one
 * thing that is not is the outside world: a read() from a file, the time,
 * random bytes. Those system calls are not executed again in the copy:
 * tdb gives it the recorded result and writes the recorded bytes into its
 * memory (it "emulates" them). System calls that only change the process
 * itself (brk, mmap, ...) are executed again, so its memory is laid out
 * as before.
 *
 * While we are in the past the real program (the "head") is put aside,
 * stopped, and the copy is the process being debugged: print, x,
 * backtrace, breakpoints all work on it. Going forward through the log
 * steps the copy; at the end of the log we are back in the present and
 * the head becomes the process again.
 *
 * reverse-continue finds the last breakpoint before now in the log. For a
 * watchpoint it steps a copy through the log and watches the value: the
 * last change before now is where to stop.
 *
 * gdb's "record full" does it the other way: before each instruction it
 * works out which registers and memory the instruction will change and
 * saves their old values, so it can undo instructions one by one. That
 * needs a model of every instruction (gdb has none for AVX2, see the
 * chapter), but going back is quick. tdb's way needs no model, but going
 * back is slow for a long log: rr, the record and replay debugger, uses
 * snapshots along the way.
 *
 * Limits: one thread; a signal ends the record; writing to memory or
 * registers in the past makes the replay go another way (tdb notices and
 * goes back to the present); rdtsc and the vDSO (clock_gettime without a
 * system call) are not recorded.
 */
#include "record.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "breakpoint.h"
#include "checkpoint.h"
#include "lines.h"
#include "signals.h"
#include "step.h"

struct entry {
    uint64_t pc;         /* the instruction */
    int32_t depth;       /* calls made minus returns, before it */
    int32_t sys;         /* a system call: its index in sys[]; else -1 */
};

struct sysrec {
    bool emulate;        /* give the recorded result; do not execute it */
    struct user_regs_struct after;   /* the registers after it */
    uint64_t addr;       /* emulate: the memory it wrote... */
    size_t len;
    unsigned char *data; /* ...and what */
};

static bool on;
static pid_t start_pid;          /* the snapshot: state 0 */
static struct entry *log_;       /* state i: before log_[i] executes */
static size_t len, cap;
static struct sysrec *sys;
static size_t nsys, syscap;
static size_t cur;               /* where we are: state cur */
static bool away;                /* in the past: the head is put aside */
static int32_t live_depth;       /* the depth now, in the present */
static uint64_t end_pc;          /* the pc now, in the present */
static struct proc head;         /* the real program, while we are in the past */

static const char NO_HISTORY[] = "\nNo more reverse-execution history.";

bool rec_on(void)
{
    return on;
}

bool rec_replaying(void)
{
    return on && away;
}

static uint64_t pc_at(size_t i)
{
    return i < len ? log_[i].pc : end_pc;
}

static int32_t depth_at(size_t i)
{
    return i < len ? log_[i].depth : live_depth;
}

/* ---- Moving between the present and the past ---- */

/* Make p a new copy of the snapshot: state 0. */
static int from_start(struct proc *p)
{
    pid_t pid = ckpt_fork(start_pid);
    if (pid < 0)
        return -1;
    bp_forget_inserted();
    proc_adopt(p, pid);
    cur = 0;
    return 0;
}

static void back_to_head(struct proc *p)
{
    if (!away)
        return;
    ckpt_kill(p->pid);
    bp_forget_inserted();
    proc_move(p, &head);
    away = false;
    cur = len;
    bp_insert_all(p);
}

static void diverged(struct proc *p, const char *why)
{
    printf("tdb: the replay went another way at instruction %zu (%s); back to the present.\n",
           cur, why);
    back_to_head(p);
}

/* After a step in the past: is the copy where the log says? 0 or -1. */
static int verify(struct proc *p, const struct sysrec *s)
{
    struct user_regs_struct regs;

    if (p->state != PROC_STOPPED) {
        diverged(p, "it ended");
        return -1;
    }
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    if (regs.rip != pc_at(cur)) {
        diverged(p, "another address");
        return -1;
    }
    if (s != NULL && regs.rax != s->after.rax) {
        diverged(p, "another system call result");
        return -1;
    }
    return 0;
}

/* Give the copy the result of a system call instead of making it. */
static int emulate(struct proc *p, const struct sysrec *s)
{
    struct user_regs_struct regs = s->after;

    regs.orig_rax = (unsigned long long)-1;    /* not in a system call */
    if (s->len > 0 && proc_write(p, s->addr, s->data, s->len) < 0)
        return -1;
    return proc_set_regs(p, &regs);
}

/* Execute instruction cur in the copy p, which has no breakpoints in it.
 * 0, or -1 if it went another way (then we are back in the present). */
static int replay_raw(struct proc *p)
{
    const struct entry *e = &log_[cur];
    const struct sysrec *s = e->sys >= 0 ? &sys[e->sys] : NULL;

    if (s != NULL && s->emulate) {
        if (emulate(p, s) < 0)
            return -1;
        s = NULL;
    } else {
        p->cur->pending_sig = 0;
        if (proc_step(p) < 0)
            return -1;
        if (p->state == PROC_STOPPED && WSTOPSIG(p->status) != SIGTRAP) {
            diverged(p, "a signal");
            return -1;
        }
    }
    cur++;
    return verify(p, s);
}

/* Go to state target: forward from here, or again from the start. The
 * breakpoints are in the process again after. 0 or -1. */
static int go_to(struct proc *p, size_t target)
{
    if (target == len) {
        back_to_head(p);
        return 0;
    }
    bp_remove_all(p);
    if (!away || target < cur) {
        if (away)
            ckpt_kill(p->pid);
        else
            proc_move(&head, p);        /* put the present aside */
        away = true;
        if (from_start(p) < 0) {
            proc_move(p, &head);
            away = false;
            cur = len;
            bp_insert_all(p);
            return -1;
        }
    }
    while (cur < target)
        if (replay_raw(p) < 0)
            return -1;
    bp_insert_all(p);
    return 0;
}

/* ---- Recording ---- */

/* System calls that only change the process itself: execute them again
 * in the replay, so the copy's memory and signal handlers are as they
 * were. Every other one is emulated. */
static bool re_execute(long nr)
{
    switch (nr) {
    case SYS_brk: case SYS_mmap: case SYS_mprotect: case SYS_munmap:
    case SYS_mremap: case SYS_madvise: case SYS_rt_sigaction:
    case SYS_rt_sigprocmask: case SYS_sigaltstack: case SYS_arch_prctl:
    case SYS_set_tid_address: case SYS_set_robust_list: case SYS_prlimit64:
    case SYS_rseq:
        return true;
    }
    return false;
}

/* The memory a system call wrote, from its arguments (before) and result:
 * the buffer to restore in the replay. */
static void written(long nr, const uint64_t arg[3], long ret, uint64_t *addr, size_t *n)
{
    *addr = 0;
    *n = 0;
    if (ret < 0)
        return;
    switch (nr) {
    case SYS_read: case SYS_pread64:
        *addr = arg[1], *n = (size_t)ret;
        break;
    case SYS_getrandom:
        *addr = arg[0], *n = (size_t)ret;
        break;
    case SYS_fstat:
        *addr = arg[1], *n = 144;           /* sizeof(struct stat) */
        break;
    case SYS_newfstatat:
        *addr = arg[2], *n = 144;           /* sizeof(struct stat) */
        break;
    case SYS_clock_gettime:
        *addr = arg[1], *n = 16;            /* sizeof(struct timespec) */
        break;
    case SYS_ioctl:
        if (arg[1] == 0x5401)               /* TCGETS: isatty() */
            *addr = arg[2], *n = 36;        /* sizeof(struct termios), the kernel's */
        break;
    }
}

static bool is_ret(const unsigned char *code, size_t n)
{
    size_t i = 0;
    while (i < n && (code[i] == 0xf2 || code[i] == 0xf3 || code[i] == 0x66 ||
                     (code[i] >= 0x40 && code[i] <= 0x4f)))
        i++;                            /* bnd (f2), rep (f3), REX */
    return i < n && (code[i] == 0xc3 || code[i] == 0xc2);
}

static int add_entry(uint64_t pc, int32_t sysidx)
{
    if (len == cap) {
        size_t ncap = cap ? cap * 2 : 4096;
        struct entry *n = realloc(log_, ncap * sizeof *n);
        if (n == NULL)
            return -1;
        log_ = n;
        cap = ncap;
    }
    log_[len++] = (struct entry){ pc, live_depth, sysidx };
    return 0;
}

static int add_sysrec(struct proc *p, long nr, const uint64_t arg[3])
{
    struct sysrec s = { .emulate = !re_execute(nr) };

    if (proc_get_regs(p, &s.after) < 0)
        return -1;
    if (s.emulate) {
        written(nr, arg, (long)s.after.rax, &s.addr, &s.len);
        if (s.len > 0) {
            s.data = malloc(s.len);
            if (s.data == NULL || proc_read(p, s.addr, s.data, s.len) < 0) {
                free(s.data);
                return -1;
            }
        }
    }
    if (nsys == syscap) {
        size_t ncap = syscap ? syscap * 2 : 64;
        struct sysrec *n = realloc(sys, ncap * sizeof *n);
        if (n == NULL) {
            free(s.data);
            return -1;
        }
        sys = n;
        syscap = ncap;
    }
    sys[nsys++] = s;
    return (int)(nsys - 1);
}

static int stop_record(struct stop *st, const char *note)
{
    st->why = STOP_RECORD;
    st->note = note;
    return 1;
}

/* Read the instruction at pc: up to 16 bytes, fewer at the end of a page. */
static size_t read_code(struct proc *p, uint64_t pc, unsigned char *code)
{
    size_t n = 16;
    while (n > 0 && bp_read_code(p, pc, code, n) < 0)
        n--;
    return n;
}

/* In the present: execute one instruction and log it. */
static int record_one(struct proc *p, struct stop *st, int (*step)(struct proc *, struct stop *))
{
    struct user_regs_struct regs;
    unsigned char code[16];

    if (len == REC_MAX)
        return stop_record(st, "tdb: the log is full (200000 instructions); \"record stop\" to go on\n"
                               "without it.");
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    size_t n = read_code(p, regs.rip, code);
    bool syscall = n >= 2 && code[0] == 0x0f && code[1] == 0x05;
    long nr = (long)regs.rax;
    if (syscall && (nr == SYS_exit_group || nr == SYS_exit))
        return stop_record(st, "The next instruction is syscall exit_group.  It will make the program exit.\n"
                               "Process record: the program stopped; \"record stop\" to let it exit.");
    if (syscall && (nr == SYS_clone || nr == SYS_fork || nr == SYS_vfork || nr == SYS_clone3))
        return stop_record(st, "The next instruction makes a new thread or process: tdb cannot record\n"
                               "that; \"record stop\" to go on.");
    if (p->cur->pending_sig != 0) {
        printf("tdb: the program is to take signal %s: tdb cannot record that; the\n"
               "record is deleted.\n", sig_name(p->cur->pending_sig));
        rec_discard(p);
        return step(p, st);
    }

    uint64_t arg[3] = { regs.rdi, regs.rsi, regs.rdx };
    int r = step(p, st);
    if (r < 0)
        return -1;
    if (p->state != PROC_STOPPED) {
        rec_discard(p);
        return r;
    }
    struct user_regs_struct after;
    if (proc_get_regs(p, &after) < 0)
        return -1;
    /* A stop for a signal comes before the instruction executes: it did
     * not (unless it was a jump to itself; then it does not matter). */
    bool executed = r == 0 || (r == 1 && st->why == STOP_WATCH) || after.rip != regs.rip;
    if (!executed)
        return r;
    int sysidx = -1;
    if (syscall && (sysidx = add_sysrec(p, nr, arg)) < 0)
        return -1;
    if (add_entry(regs.rip, sysidx) < 0)
        return -1;
    if (is_call(code, n))
        live_depth++;
    else if (is_ret(code, n))
        live_depth--;
    end_pc = after.rip;
    cur = len;
    return r;
}

int rec_start(struct proc *p)
{
    struct user_regs_struct regs;

    if (on) {
        printf("The process is already being recorded.  Use \"record stop\" to stop recording first.\n");
        return -1;
    }
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    pid_t pid = ckpt_snapshot(p);
    if (pid < 0)
        return -1;
    on = true;
    start_pid = pid;
    len = cur = 0;
    nsys = 0;
    live_depth = 0;
    end_pc = regs.rip;
    return 0;
}

void rec_discard(struct proc *p)
{
    if (!on)
        return;
    back_to_head(p);
    ckpt_kill(start_pid);
    for (size_t i = 0; i < nsys; i++)
        free(sys[i].data);
    free(log_);
    free(sys);
    log_ = NULL;
    sys = NULL;
    len = cap = cur = nsys = syscap = 0;
    on = false;
}

void rec_stop(struct proc *p)
{
    if (!on) {
        printf("No recording is currently active.\n");
        return;
    }
    rec_discard(p);
    printf("Process record is stopped and all execution logs are deleted.\n");
}

void rec_info(void)
{
    if (!on) {
        printf("No recording is currently active.\n");
        return;
    }
    printf("Active record target: tdb\n");
    printf("%s mode:\n", rec_replaying() ? "Replay" : "Record");
    if (len == 0) {
        printf("No instructions have been logged.\n");
        return;
    }
    size_t emulated = 0;
    for (size_t i = 0; i < nsys; i++)
        emulated += sys[i].emulate;
    printf("Lowest recorded instruction number is 1.\n");
    if (rec_replaying())
        printf("Current instruction number is %zu.\n", cur);
    printf("Highest recorded instruction number is %zu.\n", len);
    printf("Log contains %zu instructions.\n", len);
    printf("Max logged instructions is %d.\n", REC_MAX);
    printf("System calls: %zu (%zu emulated in the replay, %zu executed again).\n",
           nsys, emulated, nsys - emulated);
}

/* ---- Forward ---- */

int rec_exec(struct proc *p, struct stop *st, int (*step)(struct proc *, struct stop *))
{
    if (!rec_replaying())
        return record_one(p, st, step);

    const struct entry *e = &log_[cur];
    const struct sysrec *s = e->sys >= 0 ? &sys[e->sys] : NULL;
    int r = 0;
    if (s != NULL && s->emulate) {
        if (emulate(p, s) < 0)
            return -1;
        s = NULL;
    } else {
        p->cur->pending_sig = 0;        /* no signals in the past */
        r = step(p, st);
        if (r < 0)
            return -1;
        if (r == 2 || (r == 1 && st->why != STOP_WATCH)) {
            p->cur->pending_sig = 0;
            diverged(p, "a signal");
            return -1;
        }
    }
    cur++;
    if (verify(p, s) < 0)
        return -1;
    if (cur == len) {
        back_to_head(p);
        if (r == 0)
            return stop_record(st, NO_HISTORY);
    }
    return r;
}

/* ---- Backward ---- */

static bool check_on(void)
{
    if (!on)
        printf("Target native does not support this command.\n");
    return on;
}

/* Stop at state i, where breakpoint bp is. */
static void bp_stop(struct proc *p, struct breakpoint *bp, struct stop *st)
{
    bp->hits++;
    st->why = STOP_BREAKPOINT;
    st->bp_id = bp->id;
    st->temporary = bp->temporary;
    if (bp->temporary)
        bp_delete(p, bp->id);
}

/* The last state in [from, to) at a breakpoint, or (size_t)-1. */
static size_t last_bp(const struct proc *p, size_t from, size_t to)
{
    for (size_t j = to; j-- > from;)
        if (bp_at(p, log_[j].pc) != NULL)
            return j;
    return (size_t)-1;
}

/* Go to target; there, st is a stop at the breakpoint (if one is there
 * and at_bp), else what. At state 0 the history ends: say so instead. */
static int arrive(struct proc *p, size_t target, bool at_bp, enum stop_reason why,
                  struct stop *st)
{
    if (go_to(p, target) < 0)
        return -1;
    struct breakpoint *bp = at_bp ? bp_at(p, pc_at(target)) : NULL;
    if (target == 0)
        stop_record(st, NO_HISTORY);
    else if (bp != NULL)
        bp_stop(p, bp, st);
    else
        st->why = why;
    return 0;
}

int rec_reverse_stepi(struct proc *p, struct stop *st)
{
    if (!check_on())
        return -1;
    if (cur == 0) {
        stop_record(st, NO_HISTORY);
        return 0;
    }
    return arrive(p, cur - 1, true, STOP_STEP, st);
}

/* The last change of a watched value before state upto: replay from the
 * start, reading the values after every instruction. *at is the state
 * just before the instruction that changed it ((size_t)-1: none). */
static int last_change(struct proc *p, size_t upto, size_t *at, struct breakpoint **which,
                       uint64_t *before, uint64_t *after)
{
    struct breakpoint *w[BP_MAX];
    uint64_t val[BP_MAX];
    int nw = bp_watches(w, BP_MAX);

    *at = (size_t)-1;
    if (go_to(p, 0) < 0)
        return -1;
    bp_remove_all(p);
    for (int k = 0; k < nw; k++)
        bp_watch_value(p, w[k], &val[k]);
    while (cur < upto) {
        if (replay_raw(p) < 0)
            return -1;
        for (int k = 0; k < nw; k++) {
            uint64_t v;
            bp_watch_value(p, w[k], &v);
            if (v != val[k]) {
                *at = cur - 1;
                *which = w[k];
                *before = val[k];
                *after = v;
                val[k] = v;
            }
        }
    }
    bp_insert_all(p);
    return 0;
}

int rec_reverse_continue(struct proc *p, struct stop *st)
{
    struct breakpoint *w[BP_MAX];

    if (!check_on())
        return -1;
    size_t from = cur;
    size_t b = last_bp(p, 0, from);
    size_t c = (size_t)-1;
    struct breakpoint *wbp = NULL;
    uint64_t before = 0, after = 0;
    if (bp_watches(w, BP_MAX) > 0 && last_change(p, from, &c, &wbp, &before, &after) < 0)
        return -1;
    if (c != (size_t)-1 && (b == (size_t)-1 || c >= b)) {
        if (go_to(p, c) < 0)
            return -1;
        /* Backwards, as gdb shows it: "old" is the value after the
         * write, "new" the value before it. */
        wbp->hits++;
        wbp->old = before;
        st->why = STOP_WATCH;
        st->bp_id = wbp->id;
        st->temporary = false;
        st->old = after;
        st->value = before;
        return 0;
    }
    if (b != (size_t)-1)
        return arrive(p, b, true, STOP_BREAKPOINT, st);
    return arrive(p, 0, false, STOP_STEP, st);
}

int rec_reverse_finish(struct proc *p, struct stop *st)
{
    if (!check_on())
        return -1;
    int32_t d = depth_at(cur);
    size_t j = cur;
    while (j > 0 && log_[j - 1].depth >= d)
        j--;
    /* j > 0: log_[j - 1] is the call of this function. */
    size_t target = j > 0 ? j - 1 : 0;
    size_t b = last_bp(p, target + 1, cur);
    if (b != (size_t)-1)
        return arrive(p, b, true, STOP_BREAKPOINT, st);
    return arrive(p, target, false, STOP_STEP, st);
}

/* The source line of state i: (file, line), or line 0 for none. */
static bool same_line(size_t i, size_t k, uint64_t base)
{
    const struct line_row *a = line_at(pc_at(i) - base), *b = line_at(pc_at(k) - base);
    if (a == NULL || b == NULL)
        return a == b;
    return a->file == b->file && a->line == b->line;
}

/* The state before i in the same function (calls from it skipped), or
 * the call of the function if i is its first; (size_t)-1 if none. */
static size_t prev_in_frame(size_t i)
{
    int32_t d = depth_at(i);
    while (i > 0) {
        i--;
        if (log_[i].depth <= d)
            return i;
    }
    return (size_t)-1;
}

/* The first state of the run of states on the line of state i, in its
 * function. */
static size_t run_start(size_t i, uint64_t base)
{
    for (;;) {
        size_t k = prev_in_frame(i);
        if (k == (size_t)-1 || log_[k].depth < depth_at(i) || !same_line(k, i, base))
            return i;
        i = k;
    }
}

int rec_reverse_next(struct proc *p, struct stop *st)
{
    if (!check_on())
        return -1;
    if (cur == 0) {
        stop_record(st, NO_HISTORY);
        return 0;
    }
    uint64_t base = p->load_base;
    size_t k = prev_in_frame(cur);
    /* In the middle of a line, k is on it too: go to its start. Else k
     * is on the line before, or the call of this function. */
    size_t target = k == (size_t)-1 ? 0 : run_start(k, base);
    /* Back at the very start of the function: on to the line that called it. */
    size_t c = target > 0 ? prev_in_frame(target) : (size_t)-1;
    if (c != (size_t)-1 && log_[c].depth < log_[target].depth)
        target = run_start(c, base);
    size_t b = last_bp(p, target + 1, cur);
    if (b != (size_t)-1)
        return arrive(p, b, true, STOP_BREAKPOINT, st);
    return arrive(p, target, true, STOP_STEP, st);
}

int rec_goto(struct proc *p, const char *where)
{
    size_t target;

    if (!on) {
        printf("No recording is currently active.\n");
        return -1;
    }
    if (strcmp(where, "begin") == 0 || strcmp(where, "start") == 0) {
        target = 0;
    } else if (strcmp(where, "end") == 0) {
        target = len;
    } else {
        char *end;
        unsigned long long n = strtoull(where, &end, 10);
        if (*where == '\0' || *end != '\0' || n > len) {
            printf("Target insn '%s' not found.\n", where);
            return -1;
        }
        target = (size_t)n;
    }
    if (target != cur)
        printf("Go %s to insn number %zu\n", target < cur ? "backward" : "forward", target);
    return go_to(p, target);
}
