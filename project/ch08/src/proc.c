/* proc.c - start, resume and wait for the process being debugged, and
 * keep track of its threads. */
#include "proc.h"

#include "debugreg.h"
#include "elf.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

bool proc_verbose;

/* Ctrl-C was pressed (see proc_catch_ctrl_c()). */
static volatile sig_atomic_t interrupted;

static void on_sigint(int sig)
{
    (void)sig;
    interrupted = 1;
}

void proc_catch_ctrl_c(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    /* A handler, not SIG_IGN: an ignored signal stays ignored across
     * exec, and the program must still get its Ctrl-C. No SA_RESTART:
     * waitpid() returns with EINTR. */
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
}

bool proc_take_ctrl_c(void)
{
    bool was = interrupted;
    interrupted = 0;
    return was;
}

/* SYSGOOD: system call stops are SIGTRAP | 0x80.
 * TRACECLONE: trace every new thread too, from its first instruction.
 * TRACEFORK: and the child of a fork(), so that we can clean it up. */
#define TRACE_OPTIONS (PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK)

struct thread *proc_thread(struct proc *p, pid_t tid)
{
    for (int i = 0; i < PROC_MAX_THREADS; i++)
        if (tid != 0 && p->threads[i].tid == tid)
            return &p->threads[i];
    return NULL;
}

struct thread *proc_thread_num(struct proc *p, int num)
{
    for (int i = 0; i < PROC_MAX_THREADS; i++)
        if (p->threads[i].tid != 0 && p->threads[i].num == num)
            return &p->threads[i];
    return NULL;
}

void proc_select(struct proc *p, struct thread *t)
{
    p->cur = t;
    p->status = t->status;
}

/* Put tid in the thread table with the next number. NULL if it is full. */
static struct thread *add_thread(struct proc *p, pid_t tid)
{
    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        struct thread *t = &p->threads[i];
        if (t->tid == 0) {
            memset(t, 0, sizeof *t);
            t->tid = tid;
            t->num = p->next_num++;
            return t;
        }
    }
    fprintf(stderr, "tdb: more than %d threads: LWP %d is not followed\n", PROC_MAX_THREADS,
            (int)tid);
    return NULL;
}

/* A fresh thread table holding only the main thread. */
static void reset_threads(struct proc *p, pid_t pid)
{
    memset(p->threads, 0, sizeof p->threads);
    memset(p->dr, 0, sizeof p->dr);
    p->next_num = 1;
    p->request = 0;
    p->cur = add_thread(p, pid);
}

/* Children of a fork() whose first stop came before their parent's fork
 * event (the kernel does not order the two). */
static pid_t early_children[8];

static bool is_thread_of(pid_t pid, pid_t tid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task/%d", (int)pid, (int)tid);
    return access(path, F_OK) == 0;
}

/* waitpid() for thread which (or -1: any). __WALL: threads too (by
 * default waitpid() only reports children that signal their parent with
 * SIGCHLD, and threads do not; since Linux 4.7 the kernel adds __WALL
 * itself for traced children, but say what we mean). */
static pid_t wait_tid(struct proc *p, pid_t which, int *status)
{
    pid_t tid;

    while ((tid = waitpid(which, status, __WALL)) < 0) {
        if (errno != EINTR) {
            perror("tdb: waitpid");
            return -1;
        }
        /* Ctrl-C reaches a program we started (it shares our terminal),
         * but not one we attached to: pass it on. */
        if (interrupted && p->attached)
            kill(p->pid, SIGINT);
        interrupted = 0;
    }
    if (proc_verbose)
        printf("[waitpid: LWP %d, status 0x%04x]\n", (int)tid, (unsigned)*status);
    return tid;
}

/* Resume thread t the way the threads are running now: with p->request,
 * or with another single step if only it was stepping. */
static int resume_again(struct proc *p, struct thread *t)
{
    if (p->request != 0)
        return proc_resume(p, t, p->request);
    if (t == p->cur) {
        if (ptrace(PTRACE_SINGLESTEP, t->tid, NULL, NULL) < 0)
            return -1;
        t->running = true;
    }
    return 0;      /* stepping another thread: this one waits */
}

/* Deal with one waitpid() result. stopping: we are stopping all threads,
 * do not resume any. Returns 1 if it is a stop to report, 0 if it was
 * dealt with here, 2 if the process has ended, -1 on error. */
static int handle(struct proc *p, pid_t tid, int status, bool stopping)
{
    struct thread *t = proc_thread(p, tid);

    if (!WIFSTOPPED(status)) {
        /* With __WALL the main thread's end comes last, after every
         * other thread's: the process is gone. */
        if (tid == p->pid) {
            p->status = status;
            p->state = PROC_EXITED;
            p->pid = 0;
            p->cur = NULL;
            p->attached = false;
            memset(p->threads, 0, sizeof p->threads);
            return 2;
        }
        if (t != NULL) {
            printf("[LWP %d exited]\n", (int)tid);
            t->tid = 0;
            if (p->cur == t)
                p->cur = NULL;
        }
        return 0;
    }

    if (t == NULL) {
        if (!is_thread_of(p->pid, tid)) {
            /* A fork child, before its parent's fork event. */
            for (int i = 0; i < 8; i++)
                if (early_children[i] == 0) {
                    early_children[i] = tid;
                    break;
                }
            return 0;
        }
        /* A new thread, stopped by the SIGSTOP every traced clone starts
         * with; the clone event of the thread that made it is still on its
         * way. */
        printf("[New LWP %d]\n", (int)tid);
        if ((t = add_thread(p, tid)) == NULL)
            return 0;
        t->status = status;
        dr_setup_thread(p, tid);
        return stopping || p->request == 0 ? 0 : proc_resume(p, t, p->request);
    }

    t->running = false;
    t->status = status;
    int event = status >> 16;

    if (event == PTRACE_EVENT_CLONE) {
        unsigned long new_tid;
        ptrace(PTRACE_GETEVENTMSG, tid, NULL, &new_tid);
        if (proc_thread(p, (pid_t)new_tid) == NULL) {
            printf("[New LWP %lu]\n", new_tid);
            struct thread *n = add_thread(p, (pid_t)new_tid);
            if (n != NULL) {
                /* Its first stop, a SIGSTOP, is on the way. */
                n->running = true;
                n->stop_sent = true;
                n->fresh = true;
            }
        }
        return stopping ? 0 : resume_again(p, t) < 0 ? -1 : 0;
    }
    if (event == 0 && WSTOPSIG(status) == SIGSTOP && t->stop_sent) {
        /* Our own SIGSTOP (or a new thread's first stop): nothing to report. */
        t->stop_sent = false;
        if (t->fresh) {
            t->fresh = false;
            dr_setup_thread(p, tid);
        }
        return stopping ? 0 : resume_again(p, t) < 0 ? -1 : 0;
    }
    return 1;
}

int proc_wait(struct proc *p)
{
    for (;;) {
        int status;
        pid_t tid = wait_tid(p, -1, &status);
        if (tid < 0)
            return -1;
        int r = handle(p, tid, status, false);
        if (r < 0)
            return -1;
        if (r == 2)
            return 0;
        if (r == 1) {
            proc_select(p, proc_thread(p, tid));
            return 0;
        }
    }
}

bool proc_fork_event(struct proc *p, pid_t *child)
{
    int event = p->status >> 16;
    unsigned long msg;
    int status;

    if (!WIFSTOPPED(p->status) || event != PTRACE_EVENT_FORK)
        return false;
    if (ptrace(PTRACE_GETEVENTMSG, p->cur->tid, NULL, &msg) < 0)
        return false;
    *child = (pid_t)msg;
    for (int i = 0; i < 8; i++)
        if (early_children[i] == *child) {
            early_children[i] = 0;
            return true;
        }
    /* Wait for the child's first stop. */
    while (waitpid(*child, &status, __WALL) < 0 && errno == EINTR)
        ;
    return true;
}

int proc_stop_all(struct proc *p)
{
    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        struct thread *t = &p->threads[i];
        if (t->tid != 0 && t->running && !t->stop_sent) {
            /* tgkill: a signal to one thread (kill() would pick any). */
            syscall(SYS_tgkill, p->pid, t->tid, SIGSTOP);
            t->stop_sent = true;
        }
    }
    /* Wait until none is running. Threads may start or end meanwhile.
     * Wait for any thread, not each in turn: if the program is exiting,
     * the main thread is only reported after all the others. */
    while (p->state == PROC_STOPPED) {
        bool running = false;
        for (int i = 0; i < PROC_MAX_THREADS; i++)
            running |= p->threads[i].tid != 0 && p->threads[i].running;
        if (!running)
            break;
        int status;
        pid_t tid = wait_tid(p, -1, &status);
        if (tid < 0)
            return -1;
        int r = handle(p, tid, status, true);
        if (r < 0)
            return -1;
        if (r == 1)
            proc_thread(p, tid)->pending = status;  /* our SIGSTOP comes after it */
    }
    p->request = 0;
    return 0;
}

int proc_launch(struct proc *p, char *const argv[])
{
    pid_t pid = fork();
    int status;

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

    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    if (proc_verbose)
        printf("[waitpid: LWP %d, status 0x%04x]\n", (int)pid, (unsigned)status);
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
        /* exec failed: the child exited with 127 */
        p->state = PROC_NONE;
        return -1;
    }
    p->pid = pid;
    p->state = PROC_STOPPED;
    p->attached = false;
    reset_threads(p, pid);
    p->cur->status = status;
    p->status = status;
    /* EXITKILL: if tdb dies, the kernel kills the tracee too. */
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(long)(PTRACE_O_EXITKILL | TRACE_OPTIONS));
    if (elf_load_base(argv[0], pid, &p->load_base) < 0) {
        fprintf(stderr, "tdb: %s: cannot find the load address\n", argv[0]);
        p->load_base = 0;
    }
    return 0;
}

/* Attach to thread tid and wait until it has stopped. A signal that comes
 * first is kept to pass on later. 0 or -1. */
static int attach_thread(struct proc *p, pid_t tid)
{
    struct thread *t;
    int status;

    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) < 0)
        return -1;
    if ((t = tid == p->pid ? p->cur : add_thread(p, tid)) == NULL)
        return 0;
    for (;;) {
        while (waitpid(tid, &status, __WALL) < 0)
            if (errno != EINTR)
                return -1;
        if (!WIFSTOPPED(status)) {
            t->tid = 0;     /* it ended meanwhile */
            return 0;
        }
        if (WSTOPSIG(status) == SIGSTOP)
            break;
        t->pending_sig = WSTOPSIG(status);
        if (ptrace(PTRACE_CONT, tid, NULL, NULL) < 0)
            return -1;
    }
    t->status = status;
    /* No EXITKILL: if tdb dies, a process we only visit must live on. */
    ptrace(PTRACE_SETOPTIONS, tid, NULL, (void *)(long)TRACE_OPTIONS);
    return 0;
}

int proc_attach(struct proc *p, pid_t pid, const char *exe)
{
    char path[64];
    bool more = true;

    p->pid = pid;
    p->attached = true;
    reset_threads(p, pid);
    if (attach_thread(p, pid) < 0) {
        p->pid = 0;
        p->attached = false;
        return -1;              /* errno says why; the caller reports it */
    }
    p->state = PROC_STOPPED;
    /* Then every other thread in /proc/PID/task. Threads may start while
     * we do this: read the directory again until it has no new ones. */
    snprintf(path, sizeof path, "/proc/%d/task", (int)pid);
    while (more) {
        DIR *dir = opendir(path);
        struct dirent *d;
        more = false;
        while (dir != NULL && (d = readdir(dir)) != NULL) {
            pid_t tid = (pid_t)atoi(d->d_name);
            if (tid > 0 && proc_thread(p, tid) == NULL) {
                attach_thread(p, tid);
                more = true;
            }
        }
        if (dir != NULL)
            closedir(dir);
    }
    proc_select(p, p->cur);
    if (elf_load_base(exe, pid, &p->load_base) < 0) {
        fprintf(stderr, "tdb: %s: cannot find the load address\n", exe);
        p->load_base = 0;
    }
    return 0;
}

int proc_detach(struct proc *p)
{
    bool stop_sent = false;

    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        struct thread *t = &p->threads[i];
        if (t->tid == 0)
            continue;
        stop_sent |= t->stop_sent;
        if (t->running)
            continue;       /* a new thread not stopped yet: cannot detach it */
        if (ptrace(PTRACE_DETACH, t->tid, NULL, (void *)(long)t->pending_sig) < 0) {
            perror("tdb: ptrace(DETACH)");
            return -1;
        }
    }
    /* A SIGSTOP of ours still on its way would stop the whole process
     * once we are gone; SIGCONT throws pending stop signals away. */
    if (stop_sent)
        kill(p->pid, SIGCONT);
    p->pid = 0;
    p->state = PROC_NONE;
    p->cur = NULL;
    p->attached = false;
    return 0;
}

int proc_siginfo(struct proc *p, siginfo_t *si)
{
    if (ptrace(PTRACE_GETSIGINFO, p->cur->tid, NULL, si) < 0) {
        perror("tdb: ptrace(GETSIGINFO)");
        return -1;
    }
    return 0;
}

int proc_resume(struct proc *p, struct thread *t, int request)
{
    int sig = t->pending_sig;

    (void)p;
    t->pending_sig = 0;
    /* Inside a system call, PTRACE_CONT lets it finish without an exit
     * stop: we are no longer between entry and exit. */
    if (request == PTRACE_CONT)
        t->in_syscall = false;
    /* The last argument is the signal to deliver. */
    if (ptrace((enum __ptrace_request)request, t->tid, NULL, (void *)(long)sig) < 0) {
        perror("tdb: ptrace");
        return -1;
    }
    t->running = true;
    return 0;
}

int proc_resume_all(struct proc *p, int request)
{
    p->request = request;
    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        struct thread *t = &p->threads[i];
        if (t->tid != 0 && !t->running && t->pending == 0 && proc_resume(p, t, request) < 0)
            return -1;
    }
    return 0;
}

int proc_step(struct proc *p)
{
    struct thread *t = p->cur;
    int sig = t->pending_sig;

    t->in_syscall = false;      /* as in proc_resume() */
    t->pending_sig = 0;
    p->request = 0;
    if (ptrace(PTRACE_SINGLESTEP, t->tid, NULL, (void *)(long)sig) < 0) {
        perror("tdb: ptrace(SINGLESTEP)");
        return -1;
    }
    t->running = true;
    /* Only this thread runs, but wait for any: if it ends the program,
     * the other threads end too. */
    for (;;) {
        int status;
        pid_t tid = wait_tid(p, -1, &status);
        if (tid < 0)
            return -1;
        int r = handle(p, tid, status, false);
        if (r < 0)
            return -1;
        if (r == 2)
            return 0;
        if (t->tid == 0) {
            /* The thread ended (it stepped into exit): make the main
             * thread current. */
            printf("[The current thread exited; now in thread 1.]\n");
            proc_select(p, proc_thread(p, p->pid));
            return -1;
        }
        if (r == 1 && tid == t->tid) {
            proc_select(p, t);
            return 0;
        }
        if (r == 1)
            proc_thread(p, tid)->pending = status;
    }
}

int proc_get_regs(struct proc *p, struct user_regs_struct *regs)
{
    if (ptrace(PTRACE_GETREGS, p->cur->tid, NULL, regs) < 0) {
        perror("tdb: ptrace(GETREGS)");
        return -1;
    }
    return 0;
}

int proc_set_regs(struct proc *p, const struct user_regs_struct *regs)
{
    if (ptrace(PTRACE_SETREGS, p->cur->tid, NULL, regs) < 0) {
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
    long w = ptrace(PTRACE_PEEKDATA, p->cur->tid, (void *)addr, NULL);
    if (errno != 0)
        return -1;
    *word = (uint64_t)w;
    return 0;
}

int proc_poke(struct proc *p, uint64_t addr, uint64_t word)
{
    if (ptrace(PTRACE_POKEDATA, p->cur->tid, (void *)addr, (void *)word) < 0)
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

const char *proc_map_name(const struct proc *p, uint64_t addr)
{
    static char name[512];
    char path[64], line[640];
    const char *found = NULL;
    FILE *f;

    snprintf(path, sizeof path, "/proc/%d/maps", (int)p->pid);
    if ((f = fopen(path, "r")) == NULL)
        return NULL;
    /* Each line: start-end perms offset dev inode [path] */
    while (found == NULL && fgets(line, sizeof line, f) != NULL) {
        unsigned long long start, end;
        int pos = 0;
        if (sscanf(line, "%llx-%llx %*s %*s %*s %*s %n", &start, &end, &pos) < 2 ||
            pos == 0 || addr < start || addr >= end || line[pos] != '/')
            continue;
        line[strcspn(line, "\n")] = '\0';
        snprintf(name, sizeof name, "%s", line + pos);
        found = name;
    }
    fclose(f);
    return found;
}

void proc_kill(struct proc *p)
{
    int status;
    pid_t tid;

    if (p->state != PROC_STOPPED)
        return;
    kill(p->pid, SIGKILL);
    /* Reap every thread; the main thread comes last. */
    while ((tid = waitpid(-1, &status, __WALL)) > 0 || errno == EINTR)
        if (tid == p->pid && !WIFSTOPPED(status))
            break;
    p->pid = 0;
    p->cur = NULL;
    p->state = PROC_NONE;
    p->attached = false;
}
