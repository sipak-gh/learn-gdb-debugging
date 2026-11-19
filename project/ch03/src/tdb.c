/* tdb.c - a tiny debugger: the command loop.
 *
 *     tdb [-v] PROGRAM [ARGS...]
 *
 * Addresses are shown and typed the way gdb does it: before the program
 * runs they are addresses in the file (as nm shows them); while it runs
 * they are run-time addresses (the file address plus the load address).
 *
 * Commands are read from stdin, one per line. When stdin is not a terminal
 * each command is echoed after the prompt, so a scripted session reads
 * like an interactive one:
 *
 *     printf 'break square\nrun\nregs\n' | tdb ./loop
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
#include "mem.h"
#include "proc.h"
#include "regs.h"
#include "run.h"
#include "symbols.h"

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

/* What to add to a file address to get the address the user sees. */
static uint64_t base(void)
{
    return proc.state == PROC_STOPPED ? proc.load_base : 0;
}

/* " in main+4 ()" for the run-time address addr, or " in ?? ()". */
static const char *where(uint64_t addr)
{
    static char buf[300];
    char name[256];

    if (sym_describe(addr - base(), name, sizeof name) == NULL)
        return " in ?? ()";
    snprintf(buf, sizeof buf, " in %s ()", name);
    return buf;
}

/* Parse a location (number, symbol, symbol+offset, $register) or say why
 * not. */
static bool parse_location(const char *arg, uint64_t *addr)
{
    if (arg == NULL) {
        printf("Argument required (an address or a symbol).\n");
        return false;
    }
    if (arg[0] == '$') {
        /* A register's value: "x $rsp" */
        const struct reg_info *r = reg_find(arg);
        struct user_regs_struct regs;
        if (r == NULL) {
            printf("Invalid register \"%s\".\n", arg);
            return false;
        }
        if (!running() || proc_get_regs(&proc, &regs) < 0)
            return false;
        *addr = reg_get(&regs, r);
        return true;
    }
    if (sym_parse(arg, base(), addr) < 0) {
        printf("No symbol \"%s\" in the program.\n", arg);
        return false;
    }
    return true;
}

/* Say why the process stopped or ended. */
static void report(const struct stop *st)
{
    int status = proc.status;
    struct user_regs_struct regs;

    switch (st->why) {
    case STOP_BREAKPOINT:
        proc_get_regs(&proc, &regs);
        printf("%s %d, 0x%016llx%s\n", st->temporary ? "Temporary breakpoint" : "Breakpoint",
               st->bp_id, regs.rip, where(regs.rip));
        break;
    case STOP_STEP:
        proc_get_regs(&proc, &regs);
        printf("0x%016llx%s\n", regs.rip, where(regs.rip));
        break;
    case STOP_SIGNAL:
        printf("Program received signal %s (%s).\n", sig_name(st->sig), strsignal(st->sig));
        proc_get_regs(&proc, &regs);
        printf("0x%016llx%s\n", regs.rip, where(regs.rip));
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

    if (arg == NULL) {
        printf("Usage: %s ADDRESS|SYMBOL[+OFFSET]\n", temporary ? "tbreak" : "break");
        return;
    }
    if (!parse_location(arg, &addr))
        return;
    /* The table keeps file addresses: they stay right when the program is
     * started again at another load address. */
    struct breakpoint *bp = bp_add(&proc, addr - base(), temporary);
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

/* regs: every register, the way gdb's "info registers" shows them. */
static void cmd_regs(void)
{
    struct user_regs_struct regs;
    char name[256], flags[80];

    if (!running() || proc_get_regs(&proc, &regs) < 0)
        return;
    for (int i = 0; i < reg_count; i++) {
        const struct reg_info *r = &reg_table[i];
        uint64_t v = reg_get(&regs, r);

        printf("%-15s0x%-18" PRIx64, r->name, v);
        if (strcmp(r->name, "rip") == 0)
            printf("0x%" PRIx64 " <%s>", v,
                   sym_describe(v - base(), name, sizeof name) ? name : "??");
        else if (strcmp(r->name, "eflags") == 0)
            printf("%s", reg_flags(v, flags, sizeof flags));
        else if (strcmp(r->name, "rsp") == 0 || strcmp(r->name, "rbp") == 0)
            printf("0x%" PRIx64, v);
        else
            printf("%" PRId64, (int64_t)v);
        putchar('\n');
    }
}

/* reg NAME [VALUE]: show or change one register. */
static void cmd_reg(const char *arg1, const char *arg2)
{
    struct user_regs_struct regs;
    const struct reg_info *r;
    uint64_t value;

    if (arg1 == NULL) {
        printf("Usage: reg NAME [VALUE]\n");
        return;
    }
    if ((r = reg_find(arg1)) == NULL) {
        printf("Invalid register \"%s\".\n", arg1);
        return;
    }
    if (!running() || proc_get_regs(&proc, &regs) < 0)
        return;
    if (arg2 != NULL) {
        if (!parse_location(arg2, &value))
            return;
        reg_set(&regs, r, value);
        if (proc_set_regs(&proc, &regs) < 0)
            return;
    }
    value = reg_get(&regs, r);
    printf("%s = 0x%" PRIx64 " (%" PRId64 ")\n", r->name, value, (int64_t)value);
}

/* x LOCATION [COUNT]: a hex dump of COUNT bytes (64 if not given). */
static void cmd_x(const char *arg1, const char *arg2)
{
    uint64_t addr, count = 64;
    unsigned char buf[1024];
    char line[160], name[256];

    if (!parse_location(arg1, &addr))
        return;
    if (arg2 != NULL && (!parse_u64(arg2, &count) || count == 0 || count > sizeof buf)) {
        printf("COUNT must be 1 to %zu.\n", sizeof buf);
        return;
    }
    if (!running())
        return;
    if (proc_read(&proc, addr, buf, count) < 0) {
        printf("Cannot access memory at address 0x%" PRIx64 "\n", addr);
        return;
    }
    for (uint64_t off = 0; off < count; off += MEM_LINE) {
        size_t n = count - off < MEM_LINE ? count - off : MEM_LINE;
        const char *label = sym_describe(addr + off - base(), name, sizeof name);
        puts(mem_format_line(line, sizeof line, addr + off, buf + off, n, label));
    }
}

/* A data symbol of 1, 2, 4 or 8 bytes: the ones print and set handle. */
static const struct symbol *scalar_symbol(const char *arg)
{
    const struct symbol *s;

    if (arg == NULL) {
        printf("Argument required (a symbol).\n");
        return NULL;
    }
    if ((s = sym_by_name(arg)) == NULL) {
        printf("No symbol \"%s\" in the program.\n", arg);
        return NULL;
    }
    if (s->kind != SYM_OBJECT || (s->size != 1 && s->size != 2 && s->size != 4 && s->size != 8)) {
        printf("%s is %s of %" PRIu64 " bytes: use x %s %" PRIu64 ".\n", s->name,
               s->kind == SYM_FUNC ? "a function" : "an object", s->size, s->name,
               s->size ? s->size : 16);
        return NULL;
    }
    return s;
}

/* print SYMBOL: the value of a global variable. The symbol table knows its
 * address and size but not its type, so it is shown as a signed integer of
 * that size, and in hex. */
static void cmd_print(const char *arg)
{
    const struct symbol *s = scalar_symbol(arg);
    unsigned char buf[8];
    uint64_t addr;

    if (s == NULL || !running())
        return;
    addr = s->addr + base();
    if (proc_read(&proc, addr, buf, s->size) < 0) {
        printf("Cannot access memory at address 0x%" PRIx64 "\n", addr);
        return;
    }
    int64_t v;
    switch (s->size) {
    case 1: { int8_t x; memcpy(&x, buf, 1); v = x; break; }
    case 2: { int16_t x; memcpy(&x, buf, 2); v = x; break; }
    case 4: { int32_t x; memcpy(&x, buf, 4); v = x; break; }
    default: memcpy(&v, buf, 8); break;
    }
    printf("%s = %" PRId64 " (0x%" PRIx64 "), %" PRIu64 " bytes at 0x%" PRIx64 "\n", s->name,
           v, s->size == 8 ? (uint64_t)v : (uint64_t)v & ((UINT64_C(1) << (8 * s->size)) - 1),
           s->size, addr);
}

/* set SYMBOL VALUE: store a number in a global variable. */
static void cmd_set(const char *arg1, const char *arg2)
{
    const struct symbol *s = scalar_symbol(arg1);
    char *end;

    if (s == NULL)
        return;
    if (arg2 == NULL) {
        printf("Usage: set SYMBOL VALUE\n");
        return;
    }
    errno = 0;
    long long v = strtoll(arg2, &end, 0);
    if (errno != 0 || end == arg2 || *end != '\0') {
        printf("Invalid number \"%s\".\n", arg2);
        return;
    }
    if (!running())
        return;
    /* Little-endian: the low s->size bytes of v are the value. */
    if (proc_write(&proc, s->addr + base(), &v, s->size) < 0)
        printf("Cannot access memory at address 0x%" PRIx64 "\n", s->addr + base());
    else
        cmd_print(arg1);
}

/* info symbol ADDR: which symbol an address belongs to. */
static void cmd_info_symbol(const char *arg)
{
    uint64_t addr;
    char name[256];

    if (!parse_location(arg, &addr))
        return;
    if (sym_describe(addr - base(), name, sizeof name) == NULL)
        printf("No symbol matches %s.\n", arg);
    else
        printf("%s\n", name);
}

static void cmd_help(void)
{
    puts("run              start the program (again) (r)\n"
         "continue         resume it (c)\n"
         "break LOC        stop at LOC: ADDR, SYMBOL or SYMBOL+OFFSET (b)\n"
         "tbreak LOC       stop at LOC once\n"
         "delete [N]       delete breakpoint N, or all (d)\n"
         "ignore N COUNT   do not stop at breakpoint N for the next COUNT hits\n"
         "info breakpoints list the breakpoints (info b)\n"
         "info symbol LOC  the symbol LOC is in\n"
         "regs             show the registers (info registers)\n"
         "reg NAME [VALUE] show or set one register\n"
         "x LOC [COUNT]    hex dump of COUNT bytes (default 64)\n"
         "print SYMBOL     the value of a 1, 2, 4 or 8-byte variable (p)\n"
         "set SYMBOL VALUE change it\n"
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
    else if (is(cmd, "info", "i") && arg1 != NULL && is(arg1, "symbol", NULL))
        cmd_info_symbol(arg2);
    else if (is(cmd, "info", "i") && arg1 != NULL && is(arg1, "registers", "r"))
        cmd_regs();
    else if (is(cmd, "regs", NULL))
        cmd_regs();
    else if (is(cmd, "reg", NULL))
        cmd_reg(arg1, arg2);
    else if (is(cmd, "x", NULL))
        cmd_x(arg1, arg2);
    else if (is(cmd, "print", "p"))
        cmd_print(arg1);
    else if (is(cmd, "set", NULL))
        cmd_set(arg1, arg2);
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

    int n = sym_load(prog_argv[0]);
    if (n < 0)
        fprintf(stderr, "tdb: %s: not a 64-bit ELF file\n", prog_argv[0]);
    else
        printf("Reading symbols from %s: %d symbols.\n", prog_argv[0], n);

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
    sym_free();
    return 0;
}
