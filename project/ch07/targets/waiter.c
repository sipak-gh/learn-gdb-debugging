/* waiter.c - runs until killed, counting in a global, and allows any
 * process of the same user to attach a debugger to it. A target for
 * "attach".
 *
 *     waiter &
 *     tdb -p $!
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

long rounds;

void work(void)
{
    struct timespec ms = { 0, 1000000 };
    rounds++;
    nanosleep(&ms, NULL);
}

int main(void)
{
    /* With /proc/sys/kernel/yama/ptrace_scope = 1, only a parent may trace
     * a process, unless the process names a tracer: here, anyone. */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    printf("waiter: pid %d\n", (int)getpid());
    fflush(stdout);
    for (;;)
        work();
}
