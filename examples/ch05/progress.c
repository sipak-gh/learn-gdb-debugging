/* progress.c - a long computation that prints a progress line ten times a
 * second, driven by a SIGALRM timer.
 *
 *     ./progress
 *
 * Bug: it prints the first progress line and then dies with "Alarm clock".
 * The handler is installed; why does the second SIGALRM kill it?
 */
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>

#define STEPS 400000000L

static volatile sig_atomic_t report_due;

static void on_alarm(int sig)
{
    (void)sig;
    report_due = 1;
}

int main(void)
{
    struct itimerval tenth = { { 0, 100000 }, { 0, 100000 } };
    unsigned long sum = 0;

    signal(SIGALRM, on_alarm);
    setitimer(ITIMER_REAL, &tenth, NULL);
    for (long i = 0; i < STEPS; i++) {
        sum += (unsigned long)i * i % 7;
        if (report_due) {
            report_due = 0;
            printf("%3ld%% done\n", i * 100 / STEPS);
            fflush(stdout);
        }
    }
    printf("sum = %lu\n", sum);
    return 0;
}
