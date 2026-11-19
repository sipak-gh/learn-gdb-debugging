/* frames.c - a backtrace by following the frame pointer (rbp) chain. */
#include "frames.h"

#include <string.h>

#include "breakpoint.h"
#include "symbols.h"

enum frame_state frame_state(const unsigned char *code, size_t len, uint64_t offset)
{
    static const unsigned char endbr64[] = { 0xf3, 0x0f, 0x1e, 0xfa };
    static const unsigned char mov_rsp_rbp[] = { 0x48, 0x89, 0xe5 };
    size_t i = 0;

    if (len >= 4 && memcmp(code, endbr64, 4) == 0)
        i = 4;
    if (i >= len || code[i] != 0x55)
        return FRAME_BODY;          /* no push %rbp: no frame of its own */
    if (offset <= i)
        return FRAME_AT_ENTRY;
    i++;
    if (i + 3 <= len && memcmp(code + i, mov_rsp_rbp, 3) == 0 && offset < i + 3)
        return FRAME_PUSHED;
    return FRAME_BODY;
}

static bool is_main(const struct symbol *s)
{
    return s != NULL && strcmp(s->name, "main") == 0;
}

/* How far up the stack scan_for_return() looks, in 8-byte words. */
#define SCAN_WORDS 1024

/* The program is stopped in a library (libc) built without frame pointers,
 * so neither rsp nor rbp says where the return address into the program
 * is. Guess: the first word above rsp that points just past a "call rel32"
 * (e8 xx xx xx xx) in one of the program's functions. Sets *slot to where
 * that word is. 0, or -1 if there is none. */
static int scan_for_return(struct proc *p, uint64_t rsp, uint64_t *slot)
{
    uint64_t base = p->load_base, word;
    unsigned char op;

    for (int i = 0; i < SCAN_WORDS; i++) {
        if (proc_peek(p, rsp + 8 * i, &word) < 0)
            return -1;
        if (word < base + 5 || sym_by_addr(word - 1 - base) == NULL)
            continue;
        if (bp_read_code(p, word - 5, &op, 1) == 0 && op == 0xe8) {
            *slot = rsp + 8 * i;
            return 0;
        }
    }
    return -1;
}

/* Frame 0 is the one function that may be in the middle of setting up or
 * tearing down its frame. Find where its return address is stored (*slot)
 * and the caller's rbp. 0, or -1 if memory cannot be read. */
static int frame0(struct proc *p, const struct user_regs_struct *regs, uint64_t *slot,
                  uint64_t *rbp)
{
    uint64_t base = p->load_base;
    const struct symbol *s = sym_by_addr(regs->rip - base);
    enum frame_state state = FRAME_BODY;
    unsigned char code[8];

    /* Outside the program: rbp is still that of the program function
     * that called into the library (the library does not use it). */
    if (s == NULL && scan_for_return(p, regs->rsp, slot) == 0) {
        *rbp = regs->rbp;
        return 0;
    }
    if (bp_read_code(p, regs->rip, code, 1) == 0 && code[0] == 0xc3)
        state = FRAME_AT_ENTRY;     /* at "ret": the frame is already gone */
    else if (s != NULL && bp_read_code(p, s->addr + base, code, sizeof code) == 0)
        state = frame_state(code, sizeof code, regs->rip - base - s->addr);

    switch (state) {
    case FRAME_AT_ENTRY:
        *rbp = regs->rbp;
        *slot = regs->rsp;
        return 0;
    case FRAME_PUSHED:
        *slot = regs->rsp + 8;
        return proc_peek(p, regs->rsp, rbp);
    default:
        *slot = regs->rbp + 8;
        return proc_peek(p, regs->rbp, rbp);
    }
}

int frame_return(struct proc *p, uint64_t *ret, uint64_t *sp)
{
    struct user_regs_struct regs;
    uint64_t slot, rbp;

    if (proc_get_regs(p, &regs) < 0 || frame0(p, &regs, &slot, &rbp) < 0 ||
        proc_peek(p, slot, ret) < 0)
        return -1;
    *sp = slot + 8;
    return 0;
}

int frame_backtrace(struct proc *p, uint64_t *pcs, int max)
{
    struct user_regs_struct regs;
    uint64_t base = p->load_base;
    uint64_t ret, rbp, slot;
    int n = 0;

    if (max < 1 || proc_get_regs(p, &regs) < 0)
        return -1;
    pcs[n++] = regs.rip;

    const struct symbol *s = sym_by_addr(regs.rip - base);
    if (frame0(p, &regs, &slot, &rbp) < 0 || proc_peek(p, slot, &ret) < 0)
        return n;

    /* Each frame: [rbp] = the caller's rbp, [rbp + 8] = the return address. */
    while (n < max && !is_main(s) && ret != 0) {
        pcs[n++] = ret;
        /* A return address is just past a call; the call is in ret - 1. */
        s = sym_by_addr(ret - 1 - base);
        if (is_main(s) || rbp == 0)
            break;
        uint64_t next;
        if (proc_peek(p, rbp, &next) < 0 || proc_peek(p, rbp + 8, &ret) < 0)
            break;
        /* Callers' frames are higher up the stack: anything else is garbage. */
        if (next != 0 && next <= rbp)
            break;
        rbp = next;
    }
    return n;
}
