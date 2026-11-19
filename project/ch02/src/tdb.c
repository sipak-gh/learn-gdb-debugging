/* tdb.c - a tiny debugger: the command loop.
 *
 *     tdb [-v] PROGRAM [ARGS...]
 *
 * Commands are read from stdin, one per line. When stdin is not a terminal
 * each command is echoed after the prompt, so a scripted session reads
 * like an interactive one:
 *
 *     printf 'break 0x1149\nrun\ncontinue\n' | tdb ./loop
 */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "breakpoint.h"
#include "proc.h"
#include "run.h"

static struct proc proc;
static char **prog_argv;

/* Parse a number: decimal, or hex with 0x. */
static bool parse_u64(const char *s, uint64_t *out)
{
    char *end;

    if (s == NULL)
        return false;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0')
        return false;
    *out = v;
    return true;
}

static bool running(void)
{
    if (proc.state == PROC_STOPPED)
        return true;
    printf("The program is not being run.\n");
    return false;
}

/* Say why the process stopped or ended. */
static void report(const struct stop *st)
{
    int status = proc.status;
    struct user_regs_struct regs;

    switch (st->why) {
    case STOP_BREAKPOINT:
        proc_get_regs(&proc, &regs);
        printf("%s %d, 0x%016llx\n", st->temporary ? "Temporary breakpoint" : "Breakpoint",
               st->bp_id, regs.rip);
        break;
    case STOP_STEP:
        proc_get_regs(&proc, &regs);
        printf("0x%016llx\n", regs.rip);
        break;
    case STOP_SIGNAL:
        printf("Program received signal %s (%s).\n", sig_name(st->sig), strsignal(st->sig));
        break;
    case STOP_EXITED:
        if (WIFEXITED(status))
            printf("[process exited with status %d]\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf("[process killed by %s (%s)]\n", sig_name(WTERMSIG(status)),
                   strsignal(WTERMSIG(status)));
        break;
    }
}

static void cmd_run(void)
{
    struct stop st;

    if (proc.state == PROC_STOPPED) {
        printf("Killing the running process and starting again.\n");
        proc_kill(&proc);
        bp_forget_inserted();
    }
    if (proc_launch(&proc, prog_argv) < 0) {
        printf("Could not start %s.\n", prog_argv[0]);
        return;
    }
    printf("Started process %d: %s\n", (int)proc.pid, prog_argv[0]);
    /* Now that the program is in memory, write the breakpoints into it. */
    bp_insert_all(&proc);
    if (run_continue(&proc, &st) == 0)
        report(&st);
}

static void cmd_continue(void)
{
    struct stop st;

    if (running() && run_continue(&proc, &st) == 0)
        report(&st);
}

static void cmd_kill(void)
{
    if (!running())
        return;
    proc_kill(&proc);
    bp_forget_inserted();
    printf("[process killed]\n");
}

static void cmd_break(const char *arg, bool temporary)
{
    uint64_t addr;

    if (!parse_u64(arg, &addr)) {
        printf("Usage: %s ADDRESS (as nm shows it, e.g. 0x1149)\n",
               temporary ? "tbreak" : "break");
        return;
    }
    struct breakpoint *bp = bp_add(&proc, addr, temporary);
    if (bp == NULL) {
        printf("Too many breakpoints.\n");
        return;
    }
    printf("%s %d at 0x%" PRIx64 "\n", temporary ? "Temporary breakpoint" : "Breakpoint",
           bp->id, addr);
}

static void cmd_delete(const char *arg)
{
    uint64_t id;

    if (arg == NULL) {
        for (int i = 1; i < BP_MAX * 64; i++)
            bp_delete(&proc, i);
        return;
    }
    if (!parse_u64(arg, &id) || bp_delete(&proc, (int)id) < 0)
        printf("No breakpoint number %s.\n", arg);
}

static void cmd_ignore(const char *arg1, const char *arg2)
{
    uint64_t id, count;
    struct breakpoint *bp;

    if (!parse_u64(arg1, &id) || !parse_u64(arg2, &count)) {
        printf("Usage: ignore N COUNT\n");
        return;
    }
    if ((bp = bp_find((int)id)) == NULL) {
        printf("No breakpoint number %s.\n", arg1);
        return;
    }
    bp->ignore = (unsigned)count;
    printf("Will ignore next %u crossings of breakpoint %d.\n", bp->ignore, bp->id);
}

static void cmd_help(void)
{
    puts("run              start the program (again) (r)\n"
         "continue         resume it (c)\n"
         "break ADDR       stop at ADDR, an address as nm shows it (b)\n"
         "tbreak ADDR      stop at ADDR once\n"
         "delete [N]       delete breakpoint N, or all (d)\n"
         "ignore N COUNT   do not stop at breakpoint N for the next COUNT hits\n"
         "info breakpoints list the breakpoints (info b)\n"
         "kill             kill the program\n"
         "quit             leave tdb (q)");
}

static bool is(const char *cmd, const char *name, const char *alias)
{
    return strcmp(cmd, name) == 0 || (alias != NULL && strcmp(cmd, alias) == 0);
}

/* Run one command line. Returns 0 to go on, 1 to quit. */
static int execute(char *line)
{
    char *cmd = strtok(line, " \t");
    char *arg1 = strtok(NULL, " \t");
    char *arg2 = strtok(NULL, " \t");

    if (cmd == NULL)
        return 0;
    if (is(cmd, "run", "r"))
        cmd_run();
    else if (is(cmd, "continue", "c"))
        cmd_continue();
    else if (is(cmd, "break", "b"))
        cmd_break(arg1, false);
    else if (is(cmd, "tbreak", NULL))
        cmd_break(arg1, true);
    else if (is(cmd, "delete", "d"))
        cmd_delete(arg1);
    else if (is(cmd, "ignore", NULL))
        cmd_ignore(arg1, arg2);
    else if (is(cmd, "info", "i") && arg1 != NULL && is(arg1, "breakpoints", "b"))
        bp_list(&proc);
    else if (is(cmd, "kill", NULL))
        cmd_kill();
    else if (is(cmd, "help", NULL))
        cmd_help();
    else if (is(cmd, "quit", "q"))
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
