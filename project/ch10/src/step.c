/* step.c - stepping by source lines and out of functions.
 *
 * The CPU only knows how to execute one instruction at a time. A source
 * line is many instructions, so step is a loop: single-step, look the new
 * address up in the line table, stop if a new line starts there. Two
 * things need more than that:
 *
 *   - a call: next (and step, for a function without line information)
 *     must not single-step through the whole called function. Right after
 *     the call instruction the return address is at [rsp]: put an internal
 *     breakpoint there and continue.
 *   - a return to the middle of a line (the rest of "x = f() + 1" after f
 *     returns): not a new statement, so keep stepping to the next one.
 */
#include "step.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "breakpoint.h"
#include "frames.h"
#include "lines.h"
#include "symbols.h"

/* Is the instruction in code[] a call? "call rel32" is e8; an indirect
 * call (through a register or memory) is ff with 2 (or 3) in the middle
 * bits of the next byte, the ModRM byte. Prefixes may come first. */
bool is_call(const unsigned char *code, size_t len)
{
    size_t i = 0;
    while (i < len && (code[i] == 0x66 || code[i] == 0x67 || code[i] == 0xf2 ||
                       code[i] == 0xf3 || (code[i] >= 0x40 && code[i] <= 0x4f)))
        i++;                            /* operand size, bnd, REX */
    if (i < len && code[i] == 0xe8)
        return true;
    if (i + 1 < len && code[i] == 0xff) {
        int reg = (code[i + 1] >> 3) & 7;
        return reg == 2 || reg == 3;
    }
    return false;
}

/* The program counter and stack pointer now. */
static int where_now(struct proc *p, uint64_t *pc, uint64_t *sp)
{
    struct user_regs_struct regs;
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    *pc = regs.rip;
    *sp = regs.rsp;
    return 0;
}

/* Continue until the code returns to the run-time address ret with rsp ==
 * sp (the same address with another rsp is a recursive call of the same
 * function returning, not ours). 0 if it got there (st->why = STOP_STEP),
 * 1 if something else stopped it first (st filled in), -1 on error. */
static int return_to(struct proc *p, struct stop *st, uint64_t ret, uint64_t sp)
{
    struct breakpoint *bp = bp_add_internal(p, ret - p->load_base);
    if (bp == NULL)
        return -1;
    int id = bp->id, r;
    uint64_t pc, now_sp;

    for (;;) {
        if (run_continue(p, st) < 0) {
            r = -1;
            break;
        }
        if (st->why == STOP_BREAKPOINT && st->bp_id == id) {
            if (where_now(p, &pc, &now_sp) < 0) {
                r = -1;
                break;
            }
            if (now_sp != sp)
                continue;
            st->why = STOP_STEP;
            r = 0;
            break;
        }
        r = 1;
        break;
    }
    bp_delete(p, id);
    return r;
}

/* Step from the first instruction of a function into its body. */
static int skip_prologue(struct proc *p, struct stop *st, uint64_t pc)
{
    uint64_t base = p->load_base, sp;
    const struct symbol *s = sym_by_addr(pc - base);
    if (s == NULL || s->addr != pc - base)
        return 0;
    uint64_t body = line_after_prologue(s->addr, s->size) + base;
    /* A prologue is a few instructions; give up after many. */
    for (int i = 0; i < 64 && pc != body; i++) {
        if (run_stepi(p, st) < 0)
            return -1;
        if (st->why != STOP_STEP || where_now(p, &pc, &sp) < 0)
            return 0;
    }
    return 0;
}

int step_line(struct proc *p, struct stop *st, bool over)
{
    uint64_t base = p->load_base, pc, sp;
    unsigned char code[16];

    if (where_now(p, &pc, &sp) < 0)
        return -1;
    const struct line_row *start = line_at(pc - base);
    if (start == NULL) {
        /* No line here (a library, the PLT): get out of the function. */
        const struct symbol *s = sym_by_addr(pc - base);
        if (s == NULL) {
            printf("Cannot find bounds of current function\n");
            return -1;
        }
        printf("Single stepping until exit from function %s,\n"
               "which has no line number information.\n", s->name);
        if (step_finish(p, st) < 0)
            return -1;
        if (st->why != STOP_STEP)
            return 0;
        if (where_now(p, &pc, &sp) < 0)
            return -1;
        if (line_at(pc - base) == NULL || line_is_start(pc - base))
            return 0;
        start = line_at(pc - base);   /* back in the middle of a line: finish it */
    }
    int line = start->line, file = start->file;

    for (;;) {
        bool call = bp_read_code(p, pc, code, sizeof code) == 0 && is_call(code, sizeof code);
        if (run_stepi(p, st) < 0)
            return -1;
        if (st->why != STOP_STEP)
            return 0;
        if (where_now(p, &pc, &sp) < 0)
            return -1;

        if (call) {
            /* At the first instruction of the called function; the return
             * address is at [rsp]. */
            if (!over && line_at(pc - base) != NULL)
                return skip_prologue(p, st, pc);
            uint64_t ret;
            if (proc_peek(p, sp, &ret) < 0)
                return -1;
            int r = return_to(p, st, ret, sp + 8);
            if (r != 0)
                return r < 0 ? -1 : 0;
            pc = ret;       /* back after the call: a new line may start here */
        }

        const struct line_row *row = line_at(pc - base);
        if (row == NULL)
            return 0;       /* returned into code without lines (main's caller) */
        if (line_is_start(pc - base) && (row->line != line || row->file != file))
            return 0;
    }
}

int step_finish(struct proc *p, struct stop *st)
{
    uint64_t ret, sp;

    if (frame_return(p, &ret, &sp) < 0)
        return -1;
    return return_to(p, st, ret, sp) < 0 ? -1 : 0;
}
