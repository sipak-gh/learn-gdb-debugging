/* forker: the parent and a child both call greet(). A breakpoint on greet()
 * is copied into the child by fork(); a debugger must take it out. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void greet(const char *who)
{
    printf("hello from the %s\n", who);
    fflush(stdout);
}

int main(void)
{
    int status;
    pid_t pid = fork();

    if (pid == 0) {
        greet("child");
        return 0;
    }
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
        printf("child killed by signal %d (%s)\n", WTERMSIG(status), strsignal(WTERMSIG(status)));
    greet("parent");
    return 0;
}
