/* proc.c - start, resume and wait for the process being debugged. */
#include "proc.h"

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
    if (proc_wait(p) < 0)
        return -1;
    if (p->state != PROC_STOPPED || WSTOPSIG(p->status) != SIGTRAP) {
        /* exec failed: the child exited with 127 */
        p->state = PROC_NONE;
        return -1;
    }
    /* If tdb dies, the kernel kills the tracee too. */
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(long)PTRACE_O_EXITKILL);
    return 0;
}

int proc_continue(struct proc *p)
{
    int sig = p->pending_sig;

    p->pending_sig = 0;
    /* The last argument of PTRACE_CONT is the signal to deliver. */
    if (ptrace(PTRACE_CONT, p->pid, NULL, (void *)(long)sig) < 0) {
        perror("tdb: ptrace(CONT)");
        return -1;
    }
    return proc_wait(p);
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
