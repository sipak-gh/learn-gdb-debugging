/* tdb.c - a tiny debugger: the command loop.
 *
 *     tdb [-v] PROGRAM [ARGS...]
 *
 * Commands are read from stdin, one per line. When stdin is not a terminal
 * each command is echoed after the prompt, so a scripted session reads
 * like an interactive one:
 *
 *     printf 'run\n' | tdb ./hello
 */
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proc.h"

static struct proc proc;
static char **prog_argv;

/* Say why the process stopped or ended. */
static void report(void)
{
    int st = proc.status;

    if (proc.state == PROC_STOPPED) {
        int sig = WSTOPSIG(st);
        printf("Program received signal %s (%s).\n", sig_name(sig), strsignal(sig));
        /* Like gdb, pass the signal on when the user continues. */
        proc.pending_sig = sig;
    } else if (WIFEXITED(st)) {
        printf("[process exited with status %d]\n", WEXITSTATUS(st));
    } else if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);
        printf("[process killed by %s (%s)]\n", sig_name(sig), strsignal(sig));
    }
}

static void cmd_run(void)
{
    if (proc.state == PROC_STOPPED) {
        printf("Killing the running process and starting again.\n");
        proc_kill(&proc);
    }
    if (proc_launch(&proc, prog_argv) < 0) {
        printf("Could not start %s.\n", prog_argv[0]);
        return;
    }
    printf("Started process %d: %s\n", (int)proc.pid, prog_argv[0]);
    if (proc_continue(&proc) == 0)
        report();
}

static void cmd_continue(void)
{
    if (proc.state != PROC_STOPPED) {
        printf("The program is not being run.\n");
        return;
    }
    if (proc_continue(&proc) == 0)
        report();
}

static void cmd_kill(void)
{
    if (proc.state != PROC_STOPPED) {
        printf("The program is not being run.\n");
        return;
    }
    proc_kill(&proc);
    printf("[process killed]\n");
}

static void cmd_help(void)
{
    puts("run        start the program (again)\n"
         "continue   resume it (c)\n"
         "kill       kill it\n"
         "quit       leave tdb (q)");
}

/* Run one command line. Returns 0 to go on, 1 to quit. */
static int execute(char *line)
{
    char *cmd = strtok(line, " \t");

    if (cmd == NULL)
        return 0;
    if (strcmp(cmd, "run") == 0 || strcmp(cmd, "r") == 0)
        cmd_run();
    else if (strcmp(cmd, "continue") == 0 || strcmp(cmd, "c") == 0)
        cmd_continue();
    else if (strcmp(cmd, "kill") == 0)
        cmd_kill();
    else if (strcmp(cmd, "help") == 0)
        cmd_help();
    else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0)
        return 1;
    else
        printf("Unknown command \"%s\". Try \"help\".\n", cmd);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;

    if (i < argc && strcmp(argv[i], "-v") == 0) {
        proc_verbose = true;
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: tdb [-v] PROGRAM [ARGS...]\n");
        return 2;
    }
    prog_argv = &argv[i];

    /* Our output and the target's share the terminal (or pipe): flush ours
     * line by line so the two come out in order. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    bool echo = !isatty(STDIN_FILENO);
    char line[256];
    for (;;) {
        printf("(tdb) ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) {
            if (echo)
                putchar('\n');
            break;
        }
        line[strcspn(line, "\n")] = '\0';
        if (echo)
            printf("%s\n", line);
        if (execute(line))
            break;
    }
    proc_kill(&proc);
    return 0;
}
