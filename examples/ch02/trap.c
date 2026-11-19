/* trap.c - executes an int3 instruction itself, with no debugger involved.
 *
 *     ./trap        the kernel turns the int3 into SIGTRAP and kills us
 */
#include <stdio.h>

int main(void)
{
    puts("before the int3");
    fflush(stdout);
    __asm__ volatile("int3");       /* one byte: 0xCC */
    puts("after the int3");
    return 0;
}
