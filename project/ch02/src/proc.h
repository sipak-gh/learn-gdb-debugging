/* proc.h - start, resume and wait for the process being debugged. */
#ifndef TDB_PROC_H
#define TDB_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/user.h>

enum proc_state { PROC_NONE, PROC_STOPPED, PROC_EXITED };

struct proc {
    pid_t pid;
    enum proc_state state;
    int status;          /* the last status from waitpid() */
    int pending_sig;     /* signal to pass on at the next resume, or 0 */
    uint64_t load_base;  /* add to addresses in the file to get run-time ones */
};

/* Print every raw waitpid() status (tdb -v). */
extern bool proc_verbose;

/* Fork; in the child ask to be traced, turn off address randomisation and
 * exec argv[0]. Returns once the child has stopped at its exec: 0, or -1
 * if it could not be started. */
int proc_launch(struct proc *p, char *const argv[]);

/* Resume a stopped process (passing on pending_sig) and wait until it stops
 * again or ends. 0 or -1. */
int proc_continue(struct proc *p);

/* Execute one machine instruction (passing on pending_sig) and wait. */
int proc_step(struct proc *p);

/* Read or write all the general-purpose registers. 0 or -1. */
int proc_get_regs(struct proc *p, struct user_regs_struct *regs);
int proc_set_regs(struct proc *p, const struct user_regs_struct *regs);

/* Read or write the 8-byte word of memory at addr. 0 or -1. */
int proc_peek(struct proc *p, uint64_t addr, uint64_t *word);
int proc_poke(struct proc *p, uint64_t addr, uint64_t word);

/* Wait for the next change of state and record it in p. 0 or -1. */
int proc_wait(struct proc *p);

/* Kill the process (if there is one) and reap it. */
void proc_kill(struct proc *p);

/* "SIGSEGV": the name of a signal number. */
const char *sig_name(int sig);

#endif
