/* ticker.c - a timer sends SIGALRM every 10 ms; after 5 of them the
 * program sends itself SIGUSR1 and ends. A target for "handle". */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

volatile sig_atomic_t ticks;

void on_alarm(int sig)
{
    (void)sig;
    ticks++;
}

void on_usr1(int sig)
{
    (void)sig;
    write(1, "got SIGUSR1\n", 12);
}

int main(void)
{
    struct itimerval every_10ms = { { 0, 10000 }, { 0, 10000 } };
    /* sigaction, not signal(): with -std=c11, glibc's signal() resets the
     * handler after the first signal, and the second SIGALRM would kill us. */
    struct sigaction sa = { .sa_handler = on_alarm };

    sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    setitimer(ITIMER_REAL, &every_10ms, NULL);
    while (ticks < 5)
        pause();
    raise(SIGUSR1);
    printf("ticks=%d\n", (int)ticks);
    return 0;
}
