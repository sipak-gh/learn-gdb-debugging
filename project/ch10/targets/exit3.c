/* exit3.c - exits with status 3, and sends itself SIGUSR1 first. */
#include <signal.h>
#include <stdio.h>

static void on_usr1(int sig)
{
    (void)sig;
    puts("target: got SIGUSR1");
}

int main(void)
{
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    return 3;
}
