/* proc.c - start, resume and wait for the process being debugged. */
#include "proc.h"

#include "elf.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

bool proc_verbose;

const char *sig_name(int sig)
{
    static char buf[32];
    const char *abbrev = sigabbrev_np(sig);     /* "SEGV", glibc 2.32+ */

    if (abbrev == NULL)
        snprintf(buf, sizeof buf, "signal %d", sig);
    else
        snprintf(buf, sizeof buf, "SIG%s", abbrev);
    return buf;
}

int proc_wait(struct proc *p)
{
    int status;

    while (waitpid(p->pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("tdb: waitpid");
            return -1;
        }
    }
    if (proc_verbose)
        printf("[waitpid: status 0x%04x]\n", (unsigned)status);
    p->status = status;
    if (WIFSTOPPED(status)) {
        p->state = PROC_STOPPED;
    } else {
        p->state = PROC_EXITED;   /* WIFEXITED or WIFSIGNALED */
        p->pid = 0;
    }
    return 0;
}

int proc_launch(struct proc *p, char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("tdb: fork");
        return -1;
    }
    if (pid == 0) {
        /* The child: from here on, every signal it receives (and the exec
         * below) stops it and wakes its parent's waitpid(). */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
            perror("tdb: ptrace(TRACEME)");
            _exit(127);
        }
        /* Same addresses every run, as gdb does by default. */
        personality(ADDR_NO_RANDOMIZE);
        execv(argv[0], argv);
        fprintf(stderr, "tdb: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    p->pid = pid;
    p->pending_sig = 0;
    p->in_syscall = false;
    if (proc_wait(p) < 0)
        return -1;
    if (p->state != PROC_STOPPED || WSTOPSIG(p->status) != SIGTRAP) {
        /* exec failed: the child exited with 127 */
        p->state = PROC_NONE;
        return -1;
    }
    /* EXITKILL: if tdb dies, the kernel kills the tracee too.
     * TRACESYSGOOD: system call stops say SIGTRAP | 0x80, so they cannot be
     * mistaken for a breakpoint's SIGTRAP. */
    ptrace(PTRACE_SETOPTIONS, pid, NULL,
           (void *)(long)(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD));
    if (elf_load_base(argv[0], pid, &p->load_base) < 0) {
        fprintf(stderr, "tdb: %s: cannot find the load address\n", argv[0]);
        p->load_base = 0;
    }
    return 0;
}

/* Resume with PTRACE_CONT or PTRACE_SYSCALL and wait. */
static int resume(struct proc *p, enum __ptrace_request request)
{
    int sig = p->pending_sig;

    p->pending_sig = 0;
    /* The last argument is the signal to deliver. */
    if (ptrace(request, p->pid, NULL, (void *)(long)sig) < 0) {
        perror("tdb: ptrace");
        return -1;
    }
    return proc_wait(p);
}

int proc_continue(struct proc *p)
{
    /* Inside a system call, PTRACE_CONT lets it finish without an exit
     * stop: we are no longer between entry and exit. */
    p->in_syscall = false;
    return resume(p, PTRACE_CONT);
}

int proc_syscall(struct proc *p)
{
    return resume(p, PTRACE_SYSCALL);
}

int proc_step(struct proc *p)
{
    int sig = p->pending_sig;

    p->in_syscall = false;      /* as in proc_continue */
    p->pending_sig = 0;
    if (ptrace(PTRACE_SINGLESTEP, p->pid, NULL, (void *)(long)sig) < 0) {
        perror("tdb: ptrace(SINGLESTEP)");
        return -1;
    }
    return proc_wait(p);
}

int proc_get_regs(struct proc *p, struct user_regs_struct *regs)
{
    if (ptrace(PTRACE_GETREGS, p->pid, NULL, regs) < 0) {
        perror("tdb: ptrace(GETREGS)");
        return -1;
    }
    return 0;
}

int proc_set_regs(struct proc *p, const struct user_regs_struct *regs)
{
    if (ptrace(PTRACE_SETREGS, p->pid, NULL, regs) < 0) {
        perror("tdb: ptrace(SETREGS)");
        return -1;
    }
    return 0;
}

int proc_peek(struct proc *p, uint64_t addr, uint64_t *word)
{
    /* PEEKDATA returns the word itself, so -1 may be data: only errno
     * tells an error apart. */
    errno = 0;
    long w = ptrace(PTRACE_PEEKDATA, p->pid, (void *)addr, NULL);
    if (errno != 0)
        return -1;
    *word = (uint64_t)w;
    return 0;
}

int proc_poke(struct proc *p, uint64_t addr, uint64_t word)
{
    if (ptrace(PTRACE_POKEDATA, p->pid, (void *)addr, (void *)word) < 0)
        return -1;
    return 0;
}

int proc_read(struct proc *p, uint64_t addr, void *buf, size_t len)
{
    unsigned char *out = buf;
    uint64_t start = addr & ~(uint64_t)7;     /* the first aligned word */

    for (uint64_t w = start; w < addr + len; w += 8) {
        uint64_t word;
        if (proc_peek(p, w, &word) < 0)
            return -1;
        /* Copy the bytes of this word that lie inside [addr, addr + len). */
        for (int i = 0; i < 8; i++) {
            uint64_t a = w + i;
            if (a >= addr && a < addr + len)
                out[a - addr] = (unsigned char)(word >> (8 * i));
        }
    }
    return 0;
}

int proc_write(struct proc *p, uint64_t addr, const void *buf, size_t len)
{
    const unsigned char *in = buf;
    uint64_t start = addr & ~(uint64_t)7;

    /* Read each word, change the bytes inside [addr, addr + len), write it
     * back: ptrace has no way to write less than a word. */
    for (uint64_t w = start; w < addr + len; w += 8) {
        uint64_t word;
        if (proc_peek(p, w, &word) < 0)
            return -1;
        for (int i = 0; i < 8; i++) {
            uint64_t a = w + i;
            if (a >= addr && a < addr + len) {
                word &= ~((uint64_t)0xff << (8 * i));
                word |= (uint64_t)in[a - addr] << (8 * i);
            }
        }
        if (proc_poke(p, w, word) < 0)
            return -1;
    }
    return 0;
}

void proc_kill(struct proc *p)
{
    if (p->state != PROC_STOPPED)
        return;
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
    p->pid = 0;
    p->state = PROC_NONE;
}
