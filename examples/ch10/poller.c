/* poller.c - a service that starts its work when a flag file appears.
 * Its config file names the flag file:
 *
 *     cd device; ./poller poller.conf &
 *     touch ready.flag
 *
 * Bug: after the touch it goes on waiting ("still waiting for
 * ready.flag" every 5 seconds). The file is there. Attach to the running
 * service with gdbserver --attach and look at what it is waiting for.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

/* Read "wait-for FILE" from the config into flag. */
static void read_config(const char *path, char *flag, size_t size)
{
    FILE *f = fopen(path, "r");
    char line[256];

    if (f == NULL) {
        perror(path);
        exit(1);
    }
    while (fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, "wait-for ", 9) == 0)
            snprintf(flag, size, "%s", line + 9);
    fclose(f);
}

static void wait_for(const char *flag)
{
    for (int tries = 1; access(flag, F_OK) != 0; tries++) {
        if (tries % 5 == 0)
            printf("still waiting for %s\n", flag);
        sleep(1);
    }
}

int main(int argc, char *argv[])
{
    char flag[256] = "";

    if (argc != 2) {
        fprintf(stderr, "usage: poller CONFIG\n");
        return 2;
    }
    read_config(argv[1], flag, sizeof flag);
    /* Let any process of ours attach a debugger (see ptrace_scope). */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("poller: pid %d\n", (int)getpid());
    wait_for(flag);
    printf("starting work\n");
    return 0;
}
