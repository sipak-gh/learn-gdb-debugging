/* linecount.c - count the lines of several files at once: one child
 * process per file.
 *
 *     ./linecount bank.c hits.c linecount.c
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

long count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    long lines = 0;
    int c;

    while ((c = getc(f)) != EOF)
        if (c == '\n')
            lines++;
    fclose(f);
    return lines;
}

int main(int argc, char *argv[])
{
    printf("counting lines in %d files\n", argc - 1);
    for (int i = 1; i < argc; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            printf("%8ld %s\n", count_lines(argv[i]), argv[i]);
            exit(0);
        }
    }
    int status, failed = 0;
    pid_t pid;
    while ((pid = wait(&status)) > 0) {
        if (WIFSIGNALED(status)) {
            printf("worker %d killed by signal %d (%s)\n", (int)pid, WTERMSIG(status),
                   strsignal(WTERMSIG(status)));
            failed++;
        }
    }
    return failed ? 1 : 0;
}
