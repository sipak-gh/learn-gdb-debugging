/* checkpoint.c - snapshots of the process.
 *
 * fork() copies a process: its memory, its registers, its open files. A
 * copy that we keep stopped is a snapshot of the program at that moment;
 * to go back to it, fork it again and debug the new copy. gdb's
 * checkpoint command works this way, and so does the record in record.c.
 *
 * The process must call fork() itself: no system call forks someone
 * else. So we make it, for one instruction. Save its registers and the
 * code at rip; write the two bytes of a syscall instruction (0f 05) there;
 * set rax to the number of the system call we want and its arguments in
 * rdi, rsi...; single-step. The kernel sees an ordinary system call. Then
 * put the code and the registers back, in the process and in the copy.
 *
 * We ask for clone(CLONE_PARENT | SIGCHLD) rather than fork(). It is
 * fork() with one difference: the copy's parent is the process's parent,
 * which is tdb (we started the program). So tdb, not the stopped
 * program, gets the copy's exit status when we kill it; otherwise every
 * copy we kill would stay a zombie until the program reaped it.
 *
 * glibc's fork() does more than the system call: it updates the copy's
 * idea of its own thread id. Ours does not, so in a copy, raise() and
 * abort() (which use that id) do not work. Programs rarely notice.
 */
#include "checkpoint.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>

#include "breakpoint.h"
#include "lines.h"
#include "symbols.h"

#define CKPT_MAX 16

static struct checkpoint {
    int num;             /* 1, 2, ... as shown; 0: a free slot */
    pid_t pid;           /* the stopped copy */
    uint64_t pc;         /* where it is */
    uint64_t load_base;
} ckpts[CKPT_MAX];
static int next_num = 1;

static pid_t fail(const char *what)
{
    printf("tdb: checkpoint: %s: %s\n", what, strerror(errno));
    return -1;
}

/* Wait for pid's next stop or end. 0, or -1 if it is gone. */
static int wait_stop(pid_t pid, int *status)
{
    while (waitpid(pid, status, __WALL) < 0)
        if (errno != EINTR)
            return -1;
    return WIFSTOPPED(*status) ? 0 : -1;
}

pid_t ckpt_fork(pid_t pid)
{
    struct user_regs_struct saved, regs;
    int status;
    unsigned long child = 0;

    if (ptrace(PTRACE_GETREGS, pid, NULL, &saved) < 0)
        return fail("GETREGS");
    /* Stopped inside a system call (Ctrl-C in read(), say): the kernel
     * would restart it when the process goes on, and our syscall would
     * get in the way. */
    if ((long long)saved.orig_rax != -1) {
        printf("tdb: the program is in a system call; stepi first.\n");
        return -1;
    }
    errno = 0;
    long code = ptrace(PTRACE_PEEKTEXT, pid, (void *)saved.rip, NULL);
    if (errno != 0)
        return fail("PEEKTEXT");
    /* 0f 05: syscall. Little-endian: the low bytes of the word come first. */
    long inject = (long)(((unsigned long)code & ~0xffffUL) | 0x050f);
    regs = saved;
    regs.rax = SYS_clone;
    regs.rdi = CLONE_PARENT | SIGCHLD;  /* flags; the exit signal is in the low byte */
    regs.rsi = 0;                       /* the stack: the same (copied) one */
    regs.rdx = regs.r10 = regs.r8 = 0;
    if (ptrace(PTRACE_POKETEXT, pid, (void *)saved.rip, (void *)inject) < 0)
        return fail("POKETEXT");
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) < 0)
        return fail("SETREGS");

    /* With PTRACE_O_TRACEFORK the kernel stops the process at the fork
     * (PTRACE_EVENT_FORK) and makes the copy ours too; it starts with a
     * SIGSTOP. A signal that comes meanwhile (Ctrl-C) is dropped. */
    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) < 0 || wait_stop(pid, &status) < 0)
            return fail("SINGLESTEP");
        if (status >> 16 == PTRACE_EVENT_FORK) {
            ptrace(PTRACE_GETEVENTMSG, pid, NULL, &child);
            continue;
        }
        if (WSTOPSIG(status) == SIGTRAP)
            break;
    }
    int ok = child != 0 && wait_stop((pid_t)child, &status) == 0;

    ptrace(PTRACE_POKETEXT, pid, (void *)saved.rip, (void *)code);
    ptrace(PTRACE_SETREGS, pid, NULL, &saved);
    if (!ok) {
        printf("tdb: checkpoint: fork failed\n");
        return -1;
    }
    /* The copy has the syscall bytes too, and rax = 0 (fork's return
     * value in the child). */
    ptrace(PTRACE_POKETEXT, (pid_t)child, (void *)saved.rip, (void *)code);
    ptrace(PTRACE_SETREGS, (pid_t)child, NULL, &saved);
    return (pid_t)child;
}

pid_t ckpt_snapshot(struct proc *p)
{
    static struct proc copy;
    int threads = 0;

    for (int i = 0; i < PROC_MAX_THREADS; i++)
        threads += p->threads[i].tid != 0;
    if (threads > 1) {
        printf("tdb: only a program with one thread can be copied.\n");
        return -1;
    }
    pid_t pid = ckpt_fork(p->cur->tid);
    if (pid < 0)
        return -1;
    copy.load_base = p->load_base;
    proc_adopt(&copy, pid);
    bp_remove_copy(&copy);
    return pid;
}

void ckpt_kill(pid_t pid)
{
    int status;

    kill(pid, SIGKILL);
    while (waitpid(pid, &status, __WALL) == pid || errno == EINTR)
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
}

static struct checkpoint *find(int n)
{
    for (int i = 0; i < CKPT_MAX; i++)
        if (ckpts[i].num == n && n != 0)
            return &ckpts[i];
    return NULL;
}

int ckpt_add(struct proc *p)
{
    struct checkpoint *c = NULL;
    struct user_regs_struct regs;

    for (int i = 0; c == NULL && i < CKPT_MAX; i++)
        if (ckpts[i].num == 0)
            c = &ckpts[i];
    if (c == NULL) {
        printf("tdb: too many checkpoints.\n");
        return -1;
    }
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    pid_t pid = ckpt_snapshot(p);
    if (pid < 0)
        return -1;
    c->num = next_num++;
    c->pid = pid;
    c->pc = regs.rip;
    c->load_base = p->load_base;
    printf("checkpoint %d: fork returned pid %d.\n", c->num, (int)pid);
    return c->num;
}

int ckpt_restart(struct proc *p, int n)
{
    struct checkpoint *c = find(n);

    if (c == NULL) {
        printf("Not found: checkpoint id %d\n", n);
        return -1;
    }
    pid_t pid = ckpt_fork(c->pid);
    if (pid < 0)
        return -1;
    proc_kill(p);
    bp_forget_inserted();
    p->load_base = c->load_base;
    proc_adopt(p, pid);
    bp_insert_all(p);
    printf("Switching to process %d\n", (int)pid);
    return 0;
}

bool ckpt_exists(int n)
{
    return find(n) != NULL;
}

int ckpt_delete(int n)
{
    struct checkpoint *c = find(n);

    if (c == NULL)
        return -1;
    ckpt_kill(c->pid);
    c->num = 0;
    return 0;
}

void ckpt_delete_all(void)
{
    for (int i = 0; i < CKPT_MAX; i++)
        if (ckpts[i].num != 0)
            ckpt_delete(ckpts[i].num);
}

void ckpt_list(void)
{
    int n = 0;

    for (int num = 1; num < next_num; num++) {
        const struct checkpoint *c = find(num);
        if (c == NULL)
            continue;
        n++;
        printf("  %d process %d at 0x%" PRIx64, c->num, (int)c->pid, c->pc);
        const struct line_row *row = line_at(c->pc - c->load_base);
        const struct symbol *s = sym_by_addr(c->pc - c->load_base);
        if (row != NULL)
            printf(", file %s, line %d", line_file_name(row->file), row->line);
        else if (s != NULL)
            printf(", in %s", s->name);
        printf("\n");
    }
    if (n == 0)
        printf("No checkpoints.\n");
}
