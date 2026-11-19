/* frames.h - a backtrace by following the frame pointer (rbp) chain. */
#ifndef TDB_FRAMES_H
#define TDB_FRAMES_H

#include <stddef.h>
#include <stdint.h>

#include "proc.h"

/* Where a function is in setting up its frame, which tells where its
 * return address is. A function compiled with frame pointers starts
 *     endbr64             (f3 0f 1e fa, may be missing)
 *     push %rbp           (55)
 *     mov  %rsp,%rbp      (48 89 e5)                                    */
enum frame_state {
    FRAME_AT_ENTRY,  /* push not done: return address at [rsp], rbp is the caller's */
    FRAME_PUSHED,    /* push done, mov not: return address at [rsp+8], caller's rbp at [rsp] */
    FRAME_BODY,      /* frame set up: return address at [rbp+8], caller's rbp at [rbp] */
};

/* The state at offset bytes into a function whose first bytes are code[0]
 * to code[len - 1]. A function without the push is taken to be in its body:
 * rbp then still belongs to some caller, and that caller's caller is the
 * next frame found (one frame is skipped). */
enum frame_state frame_state(const unsigned char *code, size_t len, uint64_t offset);

/* Fill pcs[0..] with the program counter of each frame: pcs[0] = rip, then
 * the return addresses. Stops after main (as gdb does), at a zero frame
 * pointer, at memory it cannot read, or after max frames. Returns the
 * number of frames, or -1 if the registers cannot be read. */
int frame_backtrace(struct proc *p, uint64_t *pcs, int max);

#endif
