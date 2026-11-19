/* debugreg.c - the x86 debug registers: hardware watchpoints.
 *
 * A tracer cannot write DR0-DR7 with an instruction (they are privileged);
 * it asks the kernel with PTRACE_POKEUSER at the offset of u_debugreg[n] in
 * struct user (<sys/user.h>). The kernel checks the values and loads them
 * when the tracee runs.
 */
#include "debugreg.h"

#include <errno.h>
#include <stddef.h>
#include <sys/ptrace.h>
#include <sys/user.h>

/* Where debug register n is in struct user. */
#define DR_OFFSET(n) (offsetof(struct user, u_debugreg) + (n) * sizeof(unsigned long long))

/* The LEN field of DR7 for a length in bytes (Intel SDM, "Debug Registers").
 * Note 8 is 0b10, out of order: it was added last, for 64-bit mode. */
static uint64_t len_bits(int len)
{
    switch (len) {
    case 1: return 0;
    case 2: return 1;
    case 8: return 2;
    default: return 3;     /* 4 */
    }
}

uint64_t dr7_enable(uint64_t dr7, int slot, enum dr_kind kind, int len)
{
    dr7 = dr7_disable(dr7, slot);
    dr7 |= UINT64_C(1) << (2 * slot);                       /* L0..L3: local enable */
    dr7 |= (uint64_t)kind << (16 + 4 * slot);               /* R/W0..R/W3 */
    dr7 |= len_bits(len) << (18 + 4 * slot);                /* LEN0..LEN3 */
    return dr7;
}

uint64_t dr7_disable(uint64_t dr7, int slot)
{
    dr7 &= ~(UINT64_C(3) << (2 * slot));                    /* L and G bits */
    dr7 &= ~(UINT64_C(0xf) << (16 + 4 * slot));             /* R/W and LEN */
    return dr7;
}

int dr_read(struct proc *p, int n, uint64_t *value)
{
    errno = 0;
    long v = ptrace(PTRACE_PEEKUSER, p->cur->tid, (void *)DR_OFFSET(n), NULL);
    if (errno != 0)
        return -1;
    *value = (uint64_t)v;
    return 0;
}

int dr_write(struct proc *p, int n, uint64_t value)
{
    if (ptrace(PTRACE_POKEUSER, p->cur->tid, (void *)DR_OFFSET(n), (void *)value) < 0)
        return -1;
    return 0;
}

/* Every thread has its own debug registers, and a watchpoint must catch
 * all of them: write register n in each stopped thread, and remember the
 * value for threads that have not stopped yet or start later. */
static int write_all(struct proc *p, int n, uint64_t value)
{
    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        const struct thread *t = &p->threads[i];
        if (t->tid != 0 && !t->running &&
            ptrace(PTRACE_POKEUSER, t->tid, (void *)DR_OFFSET(n), (void *)value) < 0)
            return -1;
    }
    p->dr[n] = value;
    return 0;
}

int dr_set(struct proc *p, int slot, uint64_t addr, enum dr_kind kind, int len)
{
    /* The address first, then DR7: the slot is never enabled while it
     * still holds an old address (the kernel accepts either order). */
    if (write_all(p, slot, addr) < 0)
        return -1;
    return write_all(p, 7, dr7_enable(p->dr[7], slot, kind, len));
}

int dr_clear(struct proc *p, int slot)
{
    return write_all(p, 7, dr7_disable(p->dr[7], slot));
}

int dr_setup_thread(struct proc *p, pid_t tid)
{
    if (p->dr[7] == 0)
        return 0;
    for (int n = 0; n < DR_SLOTS; n++)
        if (ptrace(PTRACE_POKEUSER, tid, (void *)DR_OFFSET(n), (void *)p->dr[n]) < 0)
            return -1;
    if (ptrace(PTRACE_POKEUSER, tid, (void *)DR_OFFSET(7), (void *)p->dr[7]) < 0)
        return -1;
    return 0;
}

int dr_hit(struct proc *p)
{
    uint64_t dr6;

    if (dr_read(p, 6, &dr6) < 0)
        return -1;
    /* The CPU never clears DR6 itself: do it, or the next stop looks like
     * this one again. */
    dr_write(p, 6, 0);
    for (int slot = 0; slot < DR_SLOTS; slot++)
        if (dr6 & (UINT64_C(1) << slot))
            return slot;
    return -1;
}
