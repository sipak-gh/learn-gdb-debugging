/* checkpoint.h - snapshots of the process: stopped copies of it, made by
 * making it call fork(), to go back to later (checkpoint, restart). */
#ifndef TDB_CHECKPOINT_H
#define TDB_CHECKPOINT_H

#include <stdbool.h>
#include <sys/types.h>

#include "proc.h"

/* Make the stopped, single-threaded process pid fork (see checkpoint.c)
 * and wait until the copy has stopped. Both are left as pid was: the same
 * registers, the same memory. The copy's pid, or -1 (after saying why). */
pid_t ckpt_fork(pid_t pid);

/* A copy of p's current thread, made by ckpt_fork(), without our
 * breakpoints' 0xCC bytes in it. Its pid, or -1. */
pid_t ckpt_snapshot(struct proc *p);

/* Kill a stopped copy and reap it. */
void ckpt_kill(pid_t pid);

/* checkpoint: keep a snapshot of p as it is now. Its number, or -1. */
int ckpt_add(struct proc *p);

/* restart N: kill p and make it a new copy of checkpoint n, which stays
 * as it is (restart it again later). 0, or -1 (after saying why). */
int ckpt_restart(struct proc *p, int n);

/* Is there a checkpoint n? */
bool ckpt_exists(int n);

/* delete checkpoint N. 0, or -1 if there is none. */
int ckpt_delete(int n);

/* Kill every checkpoint (run, kill, detach, quit). */
void ckpt_delete_all(void);

/* info checkpoints */
void ckpt_list(void);

#endif
