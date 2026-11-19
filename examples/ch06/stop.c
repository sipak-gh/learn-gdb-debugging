/* stop.c - count as fast as possible for one second, then say how far we
 * got. An alarm signal ends the loop.
 *
 *     ./stop
 *     ./stop-O2
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static int stop;    /* set by the signal handler */

static void on_alarm(int sig)
{
    (void)sig;
    stop = 1;
}

int main(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm(1);

    unsigned long count = 0;
    while (!stop)
        count++;
    printf("counted to %lu in one second\n", count);
    return 0;
}
