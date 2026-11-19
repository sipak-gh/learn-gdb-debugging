/* proc.h - start, resume and wait for the process being debugged. */
#ifndef TDB_PROC_H
#define TDB_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/user.h>

enum proc_state { PROC_NONE, PROC_STOPPED, PROC_EXITED };

#define PROC_MAX_THREADS 64

/* One thread of the process. Linux calls it a task or LWP (light-weight
 * process); each has its own registers, its own tid, and is traced on its
 * own: ptrace() and waitpid() take a tid. */
struct thread {
    pid_t tid;           /* 0: a free slot */
    int num;             /* 1, 2, ... in the order tdb saw them, as in gdb */
    bool running;        /* resumed, and not seen stopping since */
    bool stop_sent;      /* a SIGSTOP from tdb is on its way to it */
    int status;          /* waitpid() status of its last stop */
    int pending;         /* a stop tdb has not reported yet (a status), or 0 */
    int pending_sig;     /* signal to deliver when it resumes, or 0 */
    bool in_syscall;     /* between a syscall-entry stop and its exit stop */
    bool fresh;          /* new, and not stopped yet: give it the debug registers */
};

struct proc {
    pid_t pid;           /* the process: also the tid of its main thread */
    enum proc_state state;
    int status;          /* the current thread's last stop, or how the process ended */
    struct thread *cur;  /* the current thread: registers, steps, backtraces */
    struct thread threads[PROC_MAX_THREADS];
    int next_num;        /* the number the next new thread gets */
    int request;         /* how running threads were resumed: PTRACE_CONT or
                          * PTRACE_SYSCALL; 0 while only the current one steps */
    uint64_t dr[8];      /* the debug registers every thread gets (DR0-DR7) */
    uint64_t load_base;  /* add to addresses in the file to get run-time ones */
    bool attached;       /* attached to a running process, not started by us */
};

/* Print every raw waitpid() status (tdb -v). */
extern bool proc_verbose;

/* Survive Ctrl-C: the program gets it and stops (or, attached to a
 * process that is not in our terminal, we send it SIGINT). */
void proc_catch_ctrl_c(void);
/* Was Ctrl-C pressed since the last call? (At the prompt: forget the
 * line typed so far.) */
bool proc_take_ctrl_c(void);

/* Fork; in the child ask to be traced, turn off address randomisation and
 * exec argv[0]. Returns once the child has stopped at its exec: 0, or -1
 * if it could not be started. */
int proc_launch(struct proc *p, char *const argv[]);

/* Attach to the running process pid (PTRACE_ATTACH) and wait until it
 * has stopped. exe is its program file. 0, or -1 with errno set (EPERM:
 * not allowed, see /proc/sys/kernel/yama/ptrace_scope). */
int proc_attach(struct proc *p, pid_t pid, const char *exe);

/* Let the process go on without us, delivering each thread's pending_sig.
 * 0 or -1. */
int proc_detach(struct proc *p);

/* The kernel's details on the signal that stopped the current thread: who
 * sent it, or the faulting address. 0 or -1. */
int proc_siginfo(struct proc *p, siginfo_t *si);

/* The thread with this tid or number, or NULL. */
struct thread *proc_thread(struct proc *p, pid_t tid);
struct thread *proc_thread_num(struct proc *p, int num);

/* Make t the current thread. */
void proc_select(struct proc *p, struct thread *t);

/* Resume thread t with request (PTRACE_CONT, PTRACE_SYSCALL), passing on
 * its pending_sig. 0 or -1. */
int proc_resume(struct proc *p, struct thread *t, int request);

/* Resume every stopped thread that has no unreported stop. 0 or -1. */
int proc_resume_all(struct proc *p, int request);

/* Wait until some thread stops (or the process ends) and make it the
 * current thread. New threads, threads that end and our own SIGSTOPs are
 * dealt with on the way. A fork() is a stop too: see proc_fork_event().
 * 0 or -1. */
int proc_wait(struct proc *p);

/* Did the current thread stop because it called fork()? Then *child is
 * the new process, traced by us (because of PTRACE_O_TRACEFORK) and
 * stopped. true or false. */
bool proc_fork_event(struct proc *p, pid_t *child);

/* All-stop: stop every thread that is still running and wait until it
 * has. A thread that stops for another reason first keeps that stop in
 * pending, to be reported later. 0 or -1. */
int proc_stop_all(struct proc *p);

/* Execute one machine instruction in the current thread only (passing on
 * its pending_sig) and wait for it. 0 or -1. */
int proc_step(struct proc *p);

/* Read or write all the general-purpose registers. 0 or -1. */
int proc_get_regs(struct proc *p, struct user_regs_struct *regs);
int proc_set_regs(struct proc *p, const struct user_regs_struct *regs);

/* Read or write the 8-byte word of memory at addr. 0 or -1. */
int proc_peek(struct proc *p, uint64_t addr, uint64_t *word);
int proc_poke(struct proc *p, uint64_t addr, uint64_t word);

/* Read or write len bytes at addr, any alignment, a word at a time.
 * 0, or -1 if some word could not be read or written (a write may then
 * be left half done). */
int proc_read(struct proc *p, uint64_t addr, void *buf, size_t len);
int proc_write(struct proc *p, uint64_t addr, const void *buf, size_t len);

/* The file mapped at run-time address addr ("/usr/lib/.../libc.so.6"),
 * from /proc/PID/maps; NULL if none. The string is overwritten by the
 * next call. */
const char *proc_map_name(const struct proc *p, uint64_t addr);

/* Kill the process (if there is one) and reap all its threads. */
void proc_kill(struct proc *p);

#endif
