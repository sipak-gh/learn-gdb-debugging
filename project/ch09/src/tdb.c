/* tdb.c - a tiny debugger: the commands.
 *
 *     tdb [OPTIONS] PROGRAM [ARGS...]
 *     tdb [OPTIONS] -p PID
 *
 * Addresses are shown and typed the way gdb does it: before the program
 * runs they are addresses in the file (as nm shows them); while it runs
 * they are run-time addresses (the file address plus the load address).
 *
 * Commands are read from stdin, one per line, and may be abbreviated.
 * When stdin is not a terminal each command is echoed after the prompt,
 * so a scripted session reads like an interactive one:
 *
 *     printf 'break square\nrun\nregs\n' | tdb ./loop
 *
 * They can also come from files (source, -x, ~/.tdbinit) and -ex; with
 * -batch, tdb exits when those are done. User commands (define) and
 * breakpoint commands (commands) are kept as lines of text and run with
 * execute(), like the lines typed at the prompt.
 */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "breakpoint.h"
#include "checkpoint.h"
#include "cli.h"
#include "debugreg.h"
#include "frames.h"
#include "input.h"
#include "lines.h"
#include "mem.h"
#include "proc.h"
#include "record.h"
#include "regs.h"
#include "run.h"
#include "signals.h"
#include "source.h"
#include "step.h"
#include "symbols.h"
#include "syscalls.h"

static struct proc proc;
/* What run starts: the program and its arguments; and the arguments as
 * one line, for show args. */
static char **prog_argv;
static char args_text[INPUT_LINE_MAX];
/* Where list goes on: the file and the first line it shows next. */
static int list_file = -1, list_next = 1;

static bool batch;              /* tdb -batch */
static bool quitting;           /* quit was run (perhaps in a file) */
static bool failed;             /* the command failed: see error() */
static unsigned resumes;        /* how many times the program was resumed */
static int last_bp;             /* the last breakpoint set: commands changes it */
/* The commands of the breakpoint the program stopped at, still to run. */
static char *pending_commands;

/* Say what went wrong and mark the command as failed: a file being
 * sourced, a user command or a breakpoint's commands stop there. In a
 * file, say where first, as gdb does. */
__attribute__((format(printf, 1, 2)))
static void error(const char *fmt, ...)
{
    const char *where = input_where();
    va_list ap;

    if (where != NULL && !failed)
        printf("%s: Error in sourced command file:\n", where);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    failed = true;
}

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
    error("The program is not being run.\n");
    return false;
}

/* What to add to a file address to get the address the user sees. */
static uint64_t base(void)
{
    return proc.state == PROC_STOPPED ? proc.load_base : 0;
}

/* " in main+4 ()" for the run-time address addr; " in ?? () from FILE"
 * if it is in a library tdb has no symbols for; else " in ?? ()". */
static const char *where(uint64_t addr)
{
    static char buf[600];
    char name[256];
    const char *file;

    if (sym_describe(addr - base(), name, sizeof name) != NULL)
        snprintf(buf, sizeof buf, " in %s ()", name);
    else if (proc.state == PROC_STOPPED && (file = proc_map_name(&proc, addr)) != NULL)
        snprintf(buf, sizeof buf, " in ?? () from %s", file);
    else
        return " in ?? ()";
    return buf;
}

/* Print line row's source line, "12\t    return strlen(s);". */
static void show_source_line(const struct line_row *row)
{
    if (src_print(line_file_path(row->file), row->line, row->line) < 0)
        printf("%d\t%s: %s.\n", row->line, line_file_name(row->file), strerror(errno));
    /* list now shows the lines around this one. */
    list_file = row->file;
    list_next = row->line > 5 ? row->line - 5 : 1;
}

/* How much show_location() prints. */
enum show { SHOW_BOTH, SHOW_SOURCE, SHOW_FRAME };

/* Show where the program is (pc, a run-time address), as gdb does. With
 * line information:
 *     PREFIX inner () at targets/deep.c:12          the frame line
 *     12	    return strlen(s);                    the source line
 * with "0x... in " before the function if pc is not at the start of a
 * statement. SHOW_SOURCE: only the source line (after a step that stayed
 * in the same function); SHOW_FRAME: only the frame line. Without line
 * information: "PREFIX 0x... in main+4 ()". */
static void show_location(const char *prefix, uint64_t pc, enum show what)
{
    const struct line_row *row = line_at(pc - base());
    if (row == NULL) {
        printf("%s0x%016" PRIx64 "%s\n", prefix, pc, where(pc));
        return;
    }
    bool start = line_is_start(pc - base());
    if (what != SHOW_SOURCE) {
        const struct symbol *s = sym_by_addr(pc - base());
        printf("%s", prefix);
        if (!start)
            printf("0x%016" PRIx64 " in ", pc);
        printf("%s () at %s:%d\n", s ? s->name : "??", line_file_name(row->file), row->line);
    } else if (!start) {
        printf("0x%016" PRIx64 "\t", pc);
    }
    if (what != SHOW_FRAME)
        show_source_line(row);
}

/* Parse a location (number, symbol, symbol+offset, $register) or say why
 * not. */
static bool parse_location(const char *arg, uint64_t *addr)
{
    if (arg == NULL) {
        error("Argument required (an address or a symbol).\n");
        return false;
    }
    if (arg[0] == '$') {
        /* A register's value: "x $rsp" */
        const struct reg_info *r = reg_find(arg);
        struct user_regs_struct regs;
        if (r == NULL) {
            error("Invalid register \"%s\".\n", arg);
            return false;
        }
        if (!running() || proc_get_regs(&proc, &regs) < 0)
            return false;
        *addr = reg_get(&regs, r);
        return true;
    }
    if (sym_parse(arg, base(), addr) < 0) {
        error("No symbol \"%s\" in the program.\n", arg);
        return false;
    }
    return true;
}

/* "42 (0x2a)": the low size bytes of raw as a signed integer, and in hex. */
static const char *int_text(uint64_t raw, uint64_t size)
{
    static char buf[64];
    uint64_t mask = size >= 8 ? UINT64_MAX : (UINT64_C(1) << (8 * size)) - 1;
    uint64_t sign = UINT64_C(1) << (8 * size - 1);
    int64_t v;

    raw &= mask;
    /* Sign-extend: if the top bit of the value is set, fill the bits above. */
    v = (int64_t)(raw & sign ? raw | ~mask : raw);
    snprintf(buf, sizeof buf, "%" PRId64 " (0x%" PRIx64 ")", v, raw);
    return buf;
}

/* "write(1, 0x5555555592a0, 23)": a system call and its first arguments. */
static void print_syscall(const struct stop *st)
{
    const char *name = sys_name(st->sysno);

    if (st->entry) {
        printf("    %s(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")\n",
               name ? name : "?", st->args[0], st->args[1], st->args[2]);
    } else if (st->ret < 0 && st->ret >= -4095) {
        /* A result from -4095 to -1 is -errno. */
        printf("    = %" PRId64 " (%s: %s)\n", st->ret, strerrorname_np((int)-st->ret),
               strerror((int)-st->ret));
    } else {
        printf("    = %" PRId64 " (0x%" PRIx64 ")\n", st->ret, (uint64_t)st->ret);
    }
}

/* The function the last step, next or stepi started in: a stop in the
 * same function is shown briefly. */
static const struct symbol *step_start;
/* The thread that was current when the program was resumed. */
static pid_t resumed_tid;

/* Has the program ever had more than one thread? Then stops say which
 * thread stopped, as in gdb. */
static bool threaded(void)
{
    return proc.next_num > 2;
}

/* "Thread 2 hit " if threaded, else "". */
static const char *thread_hit(void)
{
    static char buf[32];
    if (!threaded())
        return "";
    snprintf(buf, sizeof buf, "Thread %d hit ", proc.cur->num);
    return buf;
}

/* Remember the current thread before resuming the program. */
static void resuming(void)
{
    resumed_tid = proc.cur->tid;
    resumes++;
}

/* Say why the process stopped or ended. */
static void report(const struct stop *st)
{
    int status = proc.status;
    struct user_regs_struct regs;
    char prefix[64];
    const struct breakpoint *stopped_at = NULL;

    /* A stop at a breakpoint with commands: run them when the command that
     * resumed the program is done (see run_pending_commands()). */
    free(pending_commands);
    pending_commands = NULL;
    if (st->why == STOP_BREAKPOINT || st->why == STOP_WATCH || st->why == STOP_SYSCALL)
        stopped_at = bp_find(st->bp_id);
    if (stopped_at != NULL && stopped_at->commands != NULL)
        pending_commands = strdup(stopped_at->commands);
    if (stopped_at != NULL && stopped_at->silent)
        return;
    if (st->why != STOP_EXITED && threaded() && proc.cur->tid != resumed_tid)
        printf("[Switching to LWP %d]\n", (int)proc.cur->tid);
    switch (st->why) {
    case STOP_BREAKPOINT:
        proc_get_regs(&proc, &regs);
        snprintf(prefix, sizeof prefix, "%s%s %d, ", thread_hit(),
                 st->temporary ? "Temporary breakpoint" : "Breakpoint", st->bp_id);
        show_location(prefix, regs.rip, SHOW_BOTH);
        break;
    case STOP_WATCH: {
        const struct breakpoint *bp = bp_find(st->bp_id);
        proc_get_regs(&proc, &regs);
        if (bp->type == BP_WATCH) {
            printf("%sHardware watchpoint %d: %s\n", thread_hit(), bp->id, bp->what);
            printf("Old value = %s\n", int_text(st->old, (uint64_t)bp->len));
            printf("New value = %s\n", int_text(st->value, (uint64_t)bp->len));
        } else {
            printf("%sHardware read watchpoint %d: %s\n", thread_hit(), bp->id, bp->what);
            printf("Value = %s\n", int_text(st->value, (uint64_t)bp->len));
        }
        /* The CPU stops after the instruction that touched the memory. */
        show_location("", regs.rip, SHOW_BOTH);
        break;
    }
    case STOP_SYSCALL: {
        const char *name = sys_name(st->sysno);
        proc_get_regs(&proc, &regs);
        printf("%sCatchpoint %d (%s syscall %s), 0x%016llx%s\n", thread_hit(), st->bp_id,
               st->entry ? "call to" : "returned from", name ? name : "?", regs.rip,
               where(regs.rip));
        print_syscall(st);
        break;
    }
    case STOP_STEP:
        proc_get_regs(&proc, &regs);
        show_location("", regs.rip,
                      step_start != NULL && sym_by_addr(regs.rip - base()) == step_start
                          ? SHOW_SOURCE : SHOW_BOTH);
        break;
    case STOP_SIGNAL: {
        siginfo_t si;
        if (threaded())
            printf("Thread %d received", proc.cur->num);
        else
            printf("Program received");
        printf(" signal %s (%s).\n", sig_name(st->sig), strsignal(st->sig));
        if (proc_siginfo(&proc, &si) == 0 && sig_is_fault(si.si_signo, si.si_code))
            printf("Fault address 0x%" PRIxPTR ": %s.\n", (uintptr_t)si.si_addr,
                   sig_code_text(si.si_signo, si.si_code));
        proc_get_regs(&proc, &regs);
        show_location("", regs.rip, SHOW_BOTH);
        break;
    }
    case STOP_RECORD:
        printf("%s\n", st->note);
        proc_get_regs(&proc, &regs);
        show_location("", regs.rip, SHOW_BOTH);
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

/* The process is going away (run, kill, detach, quit): so do its record
 * and its checkpoints. */
static void forget_history(void)
{
    rec_discard(&proc);
    ckpt_delete_all();
}

/* set args TEXT, run TEXT: the program's arguments from now on. They are
 * split at spaces; gdb passes them to a shell, which also handles quotes,
 * $VARIABLES and < > redirections. */
static void set_args(const char *text)
{
    static char words[INPUT_LINE_MAX];
    static char *argv[64];
    int n = 0;

    if (prog_argv == NULL) {
        error("No program to run.\n");
        return;
    }
    argv[n++] = prog_argv[0];
    snprintf(args_text, sizeof args_text, "%s", text);
    snprintf(words, sizeof words, "%s", text);
    for (char *w = strtok(words, " \t"); w != NULL && n < 63; w = strtok(NULL, " \t"))
        argv[n++] = w;
    argv[n] = NULL;
    prog_argv = argv;
}

/* run [ARGS]: start the program, with ARGS if given (and from then on). */
static void cmd_run(const char *args)
{
    struct stop st;

    if (prog_argv == NULL) {
        error("No program to run.\n");
        return;
    }
    if (args[0] != '\0')
        set_args(args);
    forget_history();
    if (proc.state == PROC_STOPPED) {
        printf("Killing the running process and starting again.\n");
        proc_kill(&proc);
        bp_forget_inserted();
    }
    if (proc_launch(&proc, prog_argv) < 0) {
        error("Could not start %s.\n", prog_argv[0]);
        return;
    }
    printf("Started process %d: %s\n", (int)proc.pid, prog_argv[0]);
    resuming();
    /* Now that the program is in memory, write the breakpoints into it. */
    bp_insert_all(&proc);
    if (run_continue(&proc, &st) == 0)
        report(&st);
}

static void cmd_continue(void)
{
    struct stop st;

    if (!running())
        return;
    resuming();
    if (run_continue(&proc, &st) == 0)
        report(&st);
}

/* stepi, step, next: one instruction, or one line into or over calls. */
static void cmd_step(int how)
{
    struct stop st;
    struct user_regs_struct regs;

    if (!running() || proc_get_regs(&proc, &regs) < 0)
        return;
    step_start = sym_by_addr(regs.rip - base());
    resuming();
    int r = how == 'i' ? run_stepi(&proc, &st) : step_line(&proc, &st, how == 'n');
    if (r == 0)
        report(&st);
    step_start = NULL;
}

/* finish: run until the function returns, and show what it returned. */
static void cmd_finish(void)
{
    struct stop st;
    struct user_regs_struct regs;

    if (!running() || proc_get_regs(&proc, &regs) < 0)
        return;
    const struct symbol *s = sym_by_addr(regs.rip - base());
    if (s != NULL && strcmp(s->name, "main") == 0) {
        error("\"finish\" not meaningful in the outermost frame.\n");
        return;
    }
    show_location("Run till exit from #0  ", regs.rip, SHOW_FRAME);
    resuming();
    if (step_finish(&proc, &st) < 0)
        return;
    report(&st);
    /* The x86-64 calling convention returns an integer or pointer in rax.
     * tdb does not know the function's type: it may be void, or a double
     * (returned in xmm0). */
    if (st.why == STOP_STEP && proc_get_regs(&proc, &regs) == 0)
        printf("Value returned in rax: %s\n", int_text(regs.rax, 8));
}

static void cmd_kill(void)
{
    if (!running())
        return;
    if (proc.attached)
        printf("Killing process %d, which tdb did not start.\n", (int)proc.pid);
    forget_history();
    proc_kill(&proc);
    bp_forget_inserted();
    printf("[process killed]\n");
}

/* Is s a line number: digits only? */
static bool is_line_number(const char *s)
{
    return s[0] != '\0' && strspn(s, "0123456789") == strlen(s);
}

/* The file "break 12" and "list 12" mean: the one the program stopped
 * in, else main's. NULL if there is no line information. */
static const char *default_file(void)
{
    struct user_regs_struct regs;
    const struct line_row *row = NULL;
    const struct symbol *m;

    if (proc.state == PROC_STOPPED && proc_get_regs(&proc, &regs) == 0)
        row = line_at(regs.rip - base());
    if (row == NULL && (m = sym_by_name("main")) != NULL)
        row = line_at(m->addr);
    if (row == NULL && list_file >= 0)
        return line_file_name(list_file);
    return row ? line_file_name(row->file) : NULL;
}

/* Parse "FILE:LINE" or "LINE" into a file name and a line. */
static bool parse_file_line(const char *arg, char *file, size_t size, int *line)
{
    const char *colon = strrchr(arg, ':');
    const char *num = colon ? colon + 1 : arg;
    if (!is_line_number(num)) {
        error("\"%s\" is not FILE:LINE.\n", arg);
        return false;
    }
    *line = atoi(num);
    if (colon != NULL) {
        snprintf(file, size, "%.*s", (int)(colon - arg), arg);
    } else {
        const char *f = default_file();
        if (f == NULL) {
            error("No line information: was the program compiled with -g?\n");
            return false;
        }
        snprintf(file, size, "%s", f);
    }
    return true;
}

/* Where to put a breakpoint for LOC, as gdb reads it:
 *   *ADDR, *SYMBOL+OFFSET, 0x...  exactly that address
 *   LINE, FILE:LINE               the first statement of that line
 *   FUNCTION                      after its prologue, if it has lines
 * The run-time address goes into *addr. */
static bool code_location(const char *arg, uint64_t *addr)
{
    char file[256];
    int line, found;
    uint64_t a;

    if (arg[0] == '*')
        return parse_location(arg + 1, addr);
    if (is_line_number(arg) || strchr(arg, ':') != NULL) {
        if (!parse_file_line(arg, file, sizeof file, &line))
            return false;
        if (line_find(file, 1, &a, &found) < 0) {
            error("No source file named %s.\n", file);
            return false;
        }
        if (line_find(file, line, &a, &found) < 0) {
            error("No line %d in file \"%s\".\n", line, file);
            return false;
        }
        /* The line of a function's opening brace is its prologue. */
        const struct symbol *s = sym_by_addr(a);
        if (s != NULL && s->addr == a)
            a = line_after_prologue(s->addr, s->size);
        *addr = a + base();
        return true;
    }
    if (!parse_location(arg, addr))
        return false;
    const struct symbol *s = sym_by_name(arg);
    if (s != NULL && s->kind == SYM_FUNC && line_at(s->addr) != NULL)
        *addr = line_after_prologue(s->addr, s->size) + base();
    return true;
}

static void cmd_break(const char *arg, bool temporary)
{
    uint64_t addr;

    if (arg == NULL) {
        error("Usage: %s LINE|FILE:LINE|FUNCTION|*ADDRESS\n", temporary ? "tbreak" : "break");
        return;
    }
    if (!code_location(arg, &addr))
        return;
    /* The table keeps file addresses: they stay right when the program is
     * started again at another load address. */
    struct breakpoint *bp = bp_add(&proc, addr - base(), temporary);
    if (bp == NULL) {
        error("Too many breakpoints.\n");
        return;
    }
    last_bp = bp->id;
    printf("%s %d at 0x%" PRIx64, temporary ? "Temporary breakpoint" : "Breakpoint", bp->id,
           addr);
    const struct line_row *row = line_at(addr - base());
    if (row != NULL)
        printf(": file %s, line %d.", line_file_name(row->file), row->line);
    putchar('\n');
}

/* list [LINE|FILE:LINE|FUNCTION]: ten lines of source around LINE, or
 * the ten after the last ones shown. */
static void cmd_list(const char *arg)
{
    int file = list_file, center = 0;
    char name[256];
    uint64_t a;
    int line;

    if (arg != NULL && (is_line_number(arg) || strchr(arg, ':') != NULL)) {
        if (!parse_file_line(arg, name, sizeof name, &center))
            return;
        if (line_find(name, 1, &a, &line) < 0) {
            error("No source file named %s.\n", name);
            return;
        }
        file = line_at(a)->file;
    } else if (arg != NULL) {
        const struct symbol *s = sym_by_name(arg);
        const struct line_row *row = s ? line_at(s->addr) : NULL;
        if (row == NULL) {
            error("Function \"%s\" not defined%s.\n", arg, s ? " with line information" : "");
            return;
        }
        file = row->file;
        center = row->line;
    } else if (file < 0) {
        /* Nothing shown yet: around main. */
        const struct symbol *m = sym_by_name("main");
        const struct line_row *row = m ? line_at(m->addr) : NULL;
        if (row == NULL) {
            error("No line information: was the program compiled with -g?\n");
            return;
        }
        file = row->file;
        center = row->line;
    }
    int first = center == 0 ? list_next : center > 5 ? center - 5 : 1;
    int count = src_count(line_file_path(file));
    if (count < 0) {
        error("%s: %s.\n", line_file_path(file), strerror(errno));
        return;
    }
    if (first > count) {
        error("Line number %d out of range; \"%s\" has %d lines.\n", first,
              line_file_name(file), count);
        return;
    }
    list_file = file;
    list_next = first + src_print(line_file_path(file), first, first + 9);
}

/* watch LOC [LEN] / rwatch LOC [LEN]: a hardware watchpoint. LEN is the
 * symbol's size if LOC is a variable of 1, 2, 4 or 8 bytes, else 8. */
static void cmd_watch(const char *arg1, const char *arg2, enum bp_type type)
{
    uint64_t addr, len = 8;
    const struct symbol *s;

    if (arg1 == NULL) {
        error("Usage: %s LOC [LEN]\n", type == BP_WATCH ? "watch" : "rwatch");
        return;
    }
    if (!parse_location(arg1, &addr))
        return;
    if ((s = sym_by_name(arg1)) != NULL &&
        (s->size == 1 || s->size == 2 || s->size == 4 || s->size == 8))
        len = s->size;
    if (arg2 != NULL && !parse_u64(arg2, &len)) {
        error("Usage: %s LOC [LEN]\n", type == BP_WATCH ? "watch" : "rwatch");
        return;
    }
    struct breakpoint *bp = bp_add_watch(&proc, type, addr - base(), (int)len, arg1);
    if (bp == NULL) {
        failed = true;          /* bp_add_watch() said why */
        return;
    }
    last_bp = bp->id;
    printf("Hardware %swatchpoint %d: %s (%d bytes at 0x%" PRIx64 ", DR%d)\n",
           type == BP_WATCH ? "" : "read ", bp->id, arg1, bp->len, addr, bp->slot);
}

/* catch syscall [NAME|NUMBER]: stop at a system call's entry and exit. */
static void cmd_catch(const char *arg1, const char *arg2)
{
    long nr = -1;

    if (arg1 == NULL || strcmp(arg1, "syscall") != 0) {
        error("Usage: catch syscall [NAME|NUMBER]\n");
        return;
    }
    if (arg2 != NULL && (nr = sys_number(arg2)) < 0) {
        error("Unknown syscall name '%s'.\n", arg2);
        return;
    }
    struct breakpoint *bp = bp_add_syscall(nr);
    if (bp == NULL) {
        error("Too many breakpoints.\n");
        return;
    }
    last_bp = bp->id;
    if (nr < 0)
        printf("Catchpoint %d (any syscall)\n", bp->id);
    else
        printf("Catchpoint %d (syscall '%s' [%ld])\n", bp->id, sys_name(nr), nr);
}

static void cmd_delete(const char *arg, const char *arg2)
{
    uint64_t id;

    if (arg != NULL && strcmp(arg, "checkpoint") == 0) {
        if (arg2 == NULL || !parse_u64(arg2, &id) || ckpt_delete((int)id) < 0)
            error("No checkpoint number %s.\n", arg2 != NULL ? arg2 : "");
        return;
    }
    if (arg == NULL) {
        for (int i = 1; i < BP_MAX * 64; i++)
            bp_delete(&proc, i);
        return;
    }
    if (!parse_u64(arg, &id) || bp_delete(&proc, (int)id) < 0)
        error("No breakpoint number %s.\n", arg);
}

static void cmd_ignore(const char *arg1, const char *arg2)
{
    uint64_t id, count;
    struct breakpoint *bp;

    if (!parse_u64(arg1, &id) || !parse_u64(arg2, &count)) {
        error("Usage: ignore N COUNT\n");
        return;
    }
    if ((bp = bp_find((int)id)) == NULL) {
        error("No breakpoint number %s.\n", arg1);
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
        error("Usage: reg NAME [VALUE]\n");
        return;
    }
    if ((r = reg_find(arg1)) == NULL) {
        error("Invalid register \"%s\".\n", arg1);
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
        error("COUNT must be 1 to %zu.\n", sizeof buf);
        return;
    }
    if (!running())
        return;
    if (proc_read(&proc, addr, buf, count) < 0) {
        error("Cannot access memory at address 0x%" PRIx64 "\n", addr);
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
        error("Argument required (a symbol).\n");
        return NULL;
    }
    if ((s = sym_by_name(arg)) == NULL) {
        error("No symbol \"%s\" in the program.\n", arg);
        return NULL;
    }
    if (s->kind != SYM_OBJECT || (s->size != 1 && s->size != 2 && s->size != 4 && s->size != 8)) {
        error("%s is %s of %" PRIu64 " bytes: use x %s %" PRIu64 ".\n", s->name,
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
        error("Cannot access memory at address 0x%" PRIx64 "\n", addr);
        return;
    }
    uint64_t raw = 0;
    memcpy(&raw, buf, s->size);     /* little-endian: the low bytes */
    printf("%s = %s, %" PRIu64 " bytes at 0x%" PRIx64 "\n", s->name, int_text(raw, s->size),
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
        error("Usage: set SYMBOL VALUE\n");
        return;
    }
    errno = 0;
    long long v = strtoll(arg2, &end, 0);
    if (errno != 0 || end == arg2 || *end != '\0') {
        error("Invalid number \"%s\".\n", arg2);
        return;
    }
    if (!running())
        return;
    /* Little-endian: the low s->size bytes of v are the value. */
    if (proc_write(&proc, s->addr + base(), &v, s->size) < 0)
        error("Cannot access memory at address 0x%" PRIx64 "\n", s->addr + base());
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
        error("No symbol matches %s.\n", arg);
    else
        printf("%s\n", name);
}

/* bt [N]: the frames on the stack, innermost first. */
static void cmd_backtrace(const char *arg)
{
    uint64_t pcs[64], max = 64;

    if (!running())
        return;
    if (arg != NULL && (!parse_u64(arg, &max) || max == 0)) {
        error("Usage: bt [N]\n");
        return;
    }
    int n = frame_backtrace(&proc, pcs, max < 64 ? (int)max : 64);
    for (int i = 0; i < n; i++) {
        /* A return address is just past the call: the call's line is the
         * line of the byte before it. */
        uint64_t at = (i == 0 ? pcs[i] : pcs[i] - 1) - base();
        const struct line_row *row = line_at(at);
        const struct symbol *s = sym_by_addr(at);
        printf("#%-2d ", i);
        if (row == NULL) {
            printf("0x%016" PRIx64 "%s\n", pcs[i], where(pcs[i]));
            continue;
        }
        if (i > 0 || !line_is_start(at))
            printf("0x%016" PRIx64 " in ", pcs[i]);
        printf("%s () at %s:%d\n", s ? s->name : "??", line_file_name(row->file), row->line);
    }
}

/* A thread's name, from /proc/PID/task/TID/comm: the program's name, or
 * what the thread set with pthread_setname_np(). */
static const char *thread_name(pid_t tid)
{
    static char name[32];
    char path[64];
    FILE *f;

    name[0] = '\0';
    snprintf(path, sizeof path, "/proc/%d/task/%d/comm", (int)proc.pid, (int)tid);
    if ((f = fopen(path, "r")) != NULL) {
        if (fgets(name, sizeof name, f) == NULL)
            name[0] = '\0';
        fclose(f);
    }
    name[strcspn(name, "\n")] = '\0';
    return name;
}

/* info threads: every thread and where it is; * marks the current one. */
static void cmd_info_threads(void)
{
    struct thread *cur = proc.cur;
    struct user_regs_struct regs;
    char target[64], prefix[128];

    if (!running())
        return;
    printf("  Id   Target Id               Frame\n");
    for (int num = 1; num < proc.next_num; num++) {
        struct thread *t = proc_thread_num(&proc, num);
        if (t == NULL)
            continue;           /* it has ended */
        snprintf(target, sizeof target, "LWP %d \"%s\"", (int)t->tid, thread_name(t->tid));
        snprintf(prefix, sizeof prefix, "%c %-4d %-23s ", t == cur ? '*' : ' ', num, target);
        proc_select(&proc, t);
        if (t->running)
            printf("%s(running)\n", prefix);
        else if (proc_get_regs(&proc, &regs) == 0)
            show_location(prefix, regs.rip, SHOW_FRAME);
    }
    proc_select(&proc, cur);
}

static void execute(const char *line);

/* thread apply all|N... COMMAND: run COMMAND in each of the threads,
 * highest number first (as gdb does). line is the whole command line. */
static void thread_apply(const char *line)
{
    int nums[PROC_MAX_THREADS], n = 0;
    bool all = false;
    const char *p = line;
    char *end;

    /* Skip "thread" and "apply", then read "all" or thread numbers. */
    for (int i = 0; i < 2; i++) {
        p += strspn(p, " \t");
        p += strcspn(p, " \t");
    }
    for (;;) {
        p += strspn(p, " \t");
        if (strncmp(p, "all", 3) == 0 && (p[3] == ' ' || p[3] == '\t' || p[3] == '\0')) {
            all = true;
            p += 3;
            continue;
        }
        long num = strtol(p, &end, 10);
        if (end == p || (*end != ' ' && *end != '\t' && *end != '\0'))
            break;
        if (n < PROC_MAX_THREADS)
            nums[n++] = (int)num;
        p = end;
    }
    if ((!all && n == 0) || *p == '\0') {
        error("Usage: thread apply all|N... COMMAND\n");
        return;
    }
    if (all) {
        n = 0;
        for (int num = proc.next_num - 1; num >= 1; num--)
            if (proc_thread_num(&proc, num) != NULL)
                nums[n++] = num;
    }

    struct thread *cur = proc.cur;
    for (int i = 0; i < n && proc.state == PROC_STOPPED; i++) {
        struct thread *t = proc_thread_num(&proc, nums[i]);
        char cmd[256];
        if (t == NULL) {
            printf("Unknown thread %d.\n", nums[i]);
            continue;
        }
        proc_select(&proc, t);
        printf("\nThread %d (LWP %d \"%s\"):\n", t->num, (int)t->tid, thread_name(t->tid));
        snprintf(cmd, sizeof cmd, "%s", p);
        execute(cmd);
    }
    /* Back to the thread that was current, if it is still there. */
    if (proc.state == PROC_STOPPED && cur->tid != 0)
        proc_select(&proc, cur);
}

/* thread: which thread is current; thread N: make thread N current;
 * thread apply ...: see thread_apply(). */
static void cmd_thread(const char *arg, const char *line)
{
    struct user_regs_struct regs;
    struct thread *t;
    uint64_t num;

    if (!running())
        return;
    if (arg == NULL) {
        printf("[Current thread is %d (LWP %d)]\n", proc.cur->num, (int)proc.cur->tid);
        return;
    }
    if (strcmp(arg, "apply") == 0) {
        thread_apply(line);
        return;
    }
    if (!parse_u64(arg, &num) || num > INT_MAX ||
        (t = proc_thread_num(&proc, (int)num)) == NULL) {
        error("Invalid thread ID: %s\n", arg);
        return;
    }
    proc_select(&proc, t);
    printf("[Switching to thread %d (LWP %d)]\n", t->num, (int)t->tid);
    if (proc_get_regs(&proc, &regs) == 0)
        show_location("#0  ", regs.rip, SHOW_BOTH);
}

/* handle SIGNAL [WORD...]: show or change what tdb does when the program
 * gets SIGNAL. */
static void cmd_handle(const char *sig_text, char *const words[], int nwords)
{
    int sig;

    if (sig_text == NULL) {
        error("Usage: handle SIGNAL [stop|nostop] [print|noprint] [pass|nopass]\n");
        return;
    }
    if ((sig = sig_parse(sig_text)) < 0) {
        error("Unknown signal \"%s\".\n", sig_text);
        return;
    }
    for (int i = 0; i < nwords; i++) {
        if (sig_handle(sig, words[i]) < 0) {
            error("Unknown word \"%s\": use stop, nostop, print, noprint, pass or nopass.\n",
                  words[i]);
            return;
        }
    }
    sig_print_header();
    sig_print_row(sig);
}

static void cmd_info_signals(const char *arg)
{
    int sig = 0;

    if (arg != NULL && (sig = sig_parse(arg)) < 0) {
        error("Unknown signal \"%s\".\n", arg);
        return;
    }
    sig_print_header();
    for (int s = 1; s < SIGRTMIN; s++)
        if (sig == 0 || s == sig)
            sig_print_row(s);
}

/* siginfo: what the kernel says about the signal the program stopped with. */
static void cmd_siginfo(void)
{
    siginfo_t si;

    if (!running())
        return;
    if (!WIFSTOPPED(proc.status) || WSTOPSIG(proc.status) == SIGTRAP) {
        error("The program did not stop because of a signal.\n");
        return;
    }
    if (proc_siginfo(&proc, &si) < 0)
        return;
    printf("si_signo = %d (%s)\n", si.si_signo, sig_name(si.si_signo));
    printf("si_code  = %d: %s\n", si.si_code, sig_code_text(si.si_signo, si.si_code));
    if (sig_is_fault(si.si_signo, si.si_code))
        printf("si_addr  = 0x%" PRIxPTR "\n", (uintptr_t)si.si_addr);
    else if (si.si_code <= 0)
        printf("si_pid   = %d, si_uid = %d (who sent it)\n", (int)si.si_pid, (int)si.si_uid);
}

/* Read the program's symbol table and line table. */
static void load_symbols(const char *path)
{
    int n = sym_load(path);
    if (n < 0) {
        fprintf(stderr, "tdb: %s: not a 64-bit ELF file\n", path);
        return;
    }
    int rows = lines_load(path);
    if (!batch)
        printf("Reading symbols from %s: %d symbols, %d line table rows.\n", path, n,
               rows < 0 ? 0 : rows);
}

/* Read /proc/PID/exe: the program file of a running process. */
static bool exe_of(pid_t pid, char *buf, size_t size)
{
    char link[64];
    snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(link, buf, size - 1);
    if (n < 0)
        return false;
    buf[n] = '\0';
    return true;
}

/* attach PID: start debugging a process that is already running. */
static void cmd_attach(const char *arg)
{
    static char exe[PATH_MAX];
    static char *exe_argv[2];
    uint64_t pid;
    struct user_regs_struct regs;

    if (arg == NULL || !parse_u64(arg, &pid) || pid == 0 || pid > INT_MAX) {
        error("Usage: attach PID\n");
        return;
    }
    if (proc.state == PROC_STOPPED) {
        error("Already debugging process %d: detach or kill it first.\n", (int)proc.pid);
        return;
    }
    if (!exe_of((pid_t)pid, exe, sizeof exe)) {
        error("No process %d (or not ours to look at).\n", (int)pid);
        return;
    }
    load_symbols(exe);
    exe_argv[0] = exe;
    prog_argv = exe_argv;
    if (proc_attach(&proc, (pid_t)pid, exe) < 0) {
        error("Could not attach to process %d: %s.\n", (int)pid, strerror(errno));
        if (errno == EPERM)
            printf("With /proc/sys/kernel/yama/ptrace_scope = 1, only a parent may trace a\n"
                   "process, unless it allows it with prctl(PR_SET_PTRACER, ...).\n");
        return;
    }
    printf("Attached to process %d.\n", (int)pid);
    for (int num = 2; num < proc.next_num; num++) {
        const struct thread *t = proc_thread_num(&proc, num);
        if (t != NULL)
            printf("[New LWP %d]\n", (int)t->tid);
    }
    bp_insert_all(&proc);
    if (proc_get_regs(&proc, &regs) == 0)
        show_location("", regs.rip, SHOW_BOTH);
}

/* detach: let the process run on without tdb. */
static void cmd_detach(void)
{
    if (!running())
        return;
    forget_history();
    pid_t pid = proc.pid;
    bp_remove_all(&proc);
    if (proc_detach(&proc) == 0)
        printf("Detached from process %d.\n", (int)pid);
}

/* record [full], record stop, record goto begin|end|N */
static void cmd_record(const char *arg1, const char *arg2)
{
    struct user_regs_struct regs;

    if (arg1 == NULL || strcmp(arg1, "full") == 0) {
        if (running())
            rec_start(&proc);
    } else if (strcmp(arg1, "stop") == 0) {
        rec_stop(&proc);
    } else if (strcmp(arg1, "goto") == 0) {
        if (arg2 == NULL)
            error("Command requires an argument (insn number to go to).\n");
        else if (running() && rec_goto(&proc, arg2) == 0 && proc_get_regs(&proc, &regs) == 0)
            show_location("#0  ", regs.rip, SHOW_BOTH);
    } else {
        error("Undefined record command: \"%s\".  Try \"help\".\n", arg1);
    }
}

/* reverse-stepi, reverse-next, reverse-continue, reverse-finish ('i',
 * 'n', 'c', 'f'): back in the record. */
static void cmd_reverse(int how)
{
    struct stop st;
    struct user_regs_struct regs;
    int r = -1;

    if (!running() || proc_get_regs(&proc, &regs) < 0)
        return;
    const struct symbol *s = sym_by_addr(regs.rip - base());
    if (how == 'f' && rec_on()) {
        if (s != NULL && strcmp(s->name, "main") == 0) {
            error("\"finish\" not meaningful in the outermost frame.\n");
            return;
        }
        show_location("Run back to call of #0  ", regs.rip, SHOW_FRAME);
    }
    step_start = s;
    resuming();
    switch (how) {
    case 'i': r = rec_reverse_stepi(&proc, &st); break;
    case 'n': r = rec_reverse_next(&proc, &st); break;
    case 'c': r = rec_reverse_continue(&proc, &st); break;
    case 'f': r = rec_reverse_finish(&proc, &st); break;
    }
    if (r == 0)
        report(&st);
    else
        failed = true;
    step_start = NULL;
}

static void cmd_checkpoint(void)
{
    if (!running())
        return;
    if (rec_replaying()) {
        error("Cannot make a checkpoint in the past: \"record goto end\" first.\n");
        return;
    }
    if (ckpt_add(&proc) < 0)
        failed = true;
}

/* restart N: back to checkpoint N. */
static void cmd_restart(const char *arg)
{
    uint64_t n;
    struct user_regs_struct regs;

    if (arg == NULL || !parse_u64(arg, &n) || n > INT_MAX) {
        error("Usage: restart N\n");
        return;
    }
    if (!ckpt_exists((int)n)) {
        error("Not found: checkpoint id %d\n", (int)n);
        return;
    }
    rec_discard(&proc);
    if (ckpt_restart(&proc, (int)n) < 0) {
        failed = true;
        return;
    }
    if (proc_get_regs(&proc, &regs) == 0)
        show_location("#0  ", regs.rip, SHOW_BOTH);
}

static void cmd_help(void);

/* The commands, for lookup by name or abbreviation (see cli.h). */
enum {
    CMD_ATTACH, CMD_BT, CMD_BREAK, CMD_CATCH, CMD_COMMANDS, CMD_CONTINUE, CMD_DEFINE,
    CMD_DELETE, CMD_DETACH, CMD_ECHO, CMD_FINISH, CMD_HANDLE, CMD_HELP, CMD_IGNORE,
    CMD_INFO, CMD_KILL, CMD_LIST, CMD_NEXT, CMD_PRINT, CMD_QUIT, CMD_RECORD, CMD_REG,
    CMD_REGS, CMD_RESTART, CMD_RCONTINUE, CMD_RFINISH, CMD_RNEXT, CMD_RSTEPI, CMD_RUN,
    CMD_RWATCH, CMD_CHECKPOINT, CMD_SET, CMD_SHOW, CMD_SIGINFO, CMD_SOURCE, CMD_STEP, CMD_STEPI,
    CMD_TBREAK, CMD_THREAD, CMD_WATCH, CMD_X,
    CMD_USER            /* CMD_USER + i: user command i (see define) */
};

static const struct cli_cmd builtins[] = {
    { "attach", CMD_ATTACH, 0 },
    { "backtrace", CMD_BT, 0 },
    { "bt", CMD_BT, CLI_ALIAS },
    { "break", CMD_BREAK, 0 },
    { "b", CMD_BREAK, CLI_ALIAS },
    { "catch", CMD_CATCH, 0 },
    { "checkpoint", CMD_CHECKPOINT, 0 },
    { "commands", CMD_COMMANDS, 0 },
    { "continue", CMD_CONTINUE, CLI_REPEAT },
    { "c", CMD_CONTINUE, CLI_ALIAS | CLI_REPEAT },
    { "define", CMD_DEFINE, 0 },
    { "delete", CMD_DELETE, 0 },
    { "d", CMD_DELETE, CLI_ALIAS },
    { "detach", CMD_DETACH, 0 },
    { "echo", CMD_ECHO, 0 },
    { "finish", CMD_FINISH, CLI_REPEAT },
    { "handle", CMD_HANDLE, 0 },
    { "help", CMD_HELP, 0 },
    { "ignore", CMD_IGNORE, 0 },
    { "info", CMD_INFO, 0 },
    { "i", CMD_INFO, CLI_ALIAS },
    { "kill", CMD_KILL, 0 },
    { "list", CMD_LIST, CLI_REPEAT_BARE },
    { "next", CMD_NEXT, CLI_REPEAT },
    { "print", CMD_PRINT, 0 },
    { "quit", CMD_QUIT, 0 },
    { "rc", CMD_RCONTINUE, CLI_ALIAS | CLI_REPEAT },
    { "record", CMD_RECORD, 0 },
    { "rec", CMD_RECORD, CLI_ALIAS },
    { "reg", CMD_REG, 0 },
    { "regs", CMD_REGS, 0 },
    { "restart", CMD_RESTART, 0 },
    { "reverse-continue", CMD_RCONTINUE, CLI_REPEAT },
    { "reverse-finish", CMD_RFINISH, CLI_REPEAT },
    { "reverse-next", CMD_RNEXT, CLI_REPEAT },
    { "reverse-stepi", CMD_RSTEPI, CLI_REPEAT },
    { "rn", CMD_RNEXT, CLI_ALIAS | CLI_REPEAT },
    { "rsi", CMD_RSTEPI, CLI_ALIAS | CLI_REPEAT },
    { "run", CMD_RUN, 0 },
    { "r", CMD_RUN, CLI_ALIAS },
    { "rwatch", CMD_RWATCH, 0 },
    { "set", CMD_SET, 0 },
    { "show", CMD_SHOW, 0 },
    { "siginfo", CMD_SIGINFO, 0 },
    { "source", CMD_SOURCE, 0 },
    { "step", CMD_STEP, CLI_REPEAT },
    { "s", CMD_STEP, CLI_ALIAS | CLI_REPEAT },
    { "stepi", CMD_STEPI, CLI_REPEAT },
    { "si", CMD_STEPI, CLI_ALIAS | CLI_REPEAT },
    { "tbreak", CMD_TBREAK, 0 },
    { "thread", CMD_THREAD, 0 },
    { "t", CMD_THREAD, CLI_ALIAS },
    { "watch", CMD_WATCH, 0 },
    { "x", CMD_X, 0 },
};

#define NBUILTINS ((int)(sizeof builtins / sizeof builtins[0]))
#define USER_MAX 32
#define USER_DEPTH_MAX 64

/* The command table: the built-in commands, then the user commands. */
static struct cli_cmd commands[NBUILTINS + USER_MAX];
static int ncommands;

/* User commands (define): a name and the lines of the body, each ending
 * in '\n'. */
static struct user_cmd {
    char name[32];
    char *body;
} users[USER_MAX];
static int nusers;

/* The user command being run: the next line of its body (NULL if none is
 * running) and its arguments, for $arg0... and $argc. */
static struct user_run {
    char *next;
    char *const *args;
    int nargs;
} user;

/* Look word up in table; if there is no such command, say so. what is
 * "info " for the subcommands of info, and so on. */
static const struct cli_cmd *find(const struct cli_cmd *table, int n, const char *word,
                                  const char *what)
{
    const struct cli_cmd *c;
    char names[512];

    switch (cli_lookup(table, n, word, &c)) {
    case CLI_FOUND:
        return c;
    case CLI_AMBIGUOUS:
        error("Ambiguous %scommand \"%s\": %s.\n", what, word,
              cli_matches(table, n, word, names, sizeof names));
        return NULL;
    case CLI_UNDEFINED:
        break;
    }
    if (what[0] == '\0')
        error("Undefined command: \"%s\".  Try \"help\".\n", word);
    else
        error("Undefined %scommand: \"%s\".  Try \"help %.*s\".\n", what, word,
              (int)strlen(what) - 1, what);
    return NULL;
}

/* A subcommand of info or show (prefix is "info" or "show"): its id, or
 * -1 after saying what is wrong. */
static int subcommand(const char *prefix, const struct cli_cmd *table, int n, const char *word)
{
    char what[16], names[256];

    if (word == NULL) {
        error("\"%s\" must be followed by the name of a subcommand: %s.\n", prefix,
              cli_matches(table, n, "", names, sizeof names));
        return -1;
    }
    snprintf(what, sizeof what, "%s ", prefix);
    const struct cli_cmd *c = find(table, n, word, what);
    return c != NULL ? c->id : -1;
}

static void cmd_info(const char *arg1, const char *arg2)
{
    enum { BREAKPOINTS, CHECKPOINTS, RECORD, REGISTERS, SIGNALS, SYMBOL, THREADS };
    static const struct cli_cmd subs[] = {
        { "breakpoints", BREAKPOINTS, 0 }, { "checkpoints", CHECKPOINTS, 0 },
        { "record", RECORD, 0 },           { "registers", REGISTERS, 0 },
        { "signals", SIGNALS, 0 },         { "symbol", SYMBOL, 0 },
        { "threads", THREADS, 0 },
    };

    switch (subcommand("info", subs, (int)(sizeof subs / sizeof subs[0]), arg1)) {
    case BREAKPOINTS:
        bp_list(&proc);
        break;
    case CHECKPOINTS:
        ckpt_list();
        break;
    case RECORD:
        rec_info();
        break;
    case REGISTERS:
        cmd_regs();
        break;
    case SIGNALS:
        cmd_info_signals(arg2);
        break;
    case SYMBOL:
        cmd_info_symbol(arg2);
        break;
    case THREADS:
        cmd_info_threads();
        break;
    }
}

/* show user [NAME]: the lines of one user command, or of all of them. */
static void show_user(const char *name)
{
    int shown = 0;

    for (int i = 0; i < nusers; i++) {
        if (name != NULL && strcmp(users[i].name, name) != 0)
            continue;
        printf("User command \"%s\":\n", users[i].name);
        for (const char *p = users[i].body; *p != '\0';) {
            int len = (int)strcspn(p, "\n");
            printf("  %.*s\n", len, p);
            p += len + 1;
        }
        shown++;
    }
    if (name != NULL && shown == 0)
        error("Not a user command: \"%s\".\n", name);
}

static void cmd_show(const char *arg1, const char *arg2)
{
    enum { ARGS, USER };
    static const struct cli_cmd subs[] = { { "args", ARGS, 0 }, { "user", USER, 0 } };

    switch (subcommand("show", subs, 2, arg1)) {
    case ARGS:
        printf("Argument list to give program being debugged when it is started is \"%s\".\n",
               args_text);
        break;
    case USER:
        show_user(arg2);
        break;
    }
}

/* The next line of the user command being run, with its arguments put in
 * (in line, size bytes). NULL at its end, or after an error. */
static char *user_line(char *line, size_t size)
{
    int missing;

    if (user.next == NULL || *user.next == '\0')
        return NULL;
    char *p = user.next, *nl = strchr(p, '\n');
    *nl = '\0';
    user.next = nl + 1;
    switch (cli_expand(p, user.args, user.nargs, line, size, &missing)) {
    case -1:
        error("Missing argument %d in user function.\n", missing);
        return NULL;
    case -2:
        error("Line too long.\n");
        return NULL;
    }
    return line;
}

/* Does line start a body that ends with "end" (commands, define)? */
static bool starts_body(const char *line)
{
    char word[64];
    const struct cli_cmd *c;

    snprintf(word, sizeof word, "%.*s", (int)strcspn(line, " \t"), line);
    return cli_lookup(commands, ncommands, word, &c) == CLI_FOUND &&
           (c->id == CMD_COMMANDS || c->id == CMD_DEFINE);
}

/* Read lines up to "end", for commands and define: from the user command
 * being run if there is one, else from the input. Returns them as one
 * malloc'd string, each line ending in '\n'. A body may hold another
 * one (a define that sets commands): its "end" does not end this one. */
static char *read_body(void)
{
    char buf[INPUT_LINE_MAX], *line;
    size_t len = 0, size = 256;
    char *body = malloc(size);
    int nested = 0;

    body[0] = '\0';
    while ((line = user.next != NULL ? user_line(buf, sizeof buf) : input_read(">")) != NULL) {
        line += strspn(line, " \t");
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t'))
            line[--n] = '\0';
        if (n == 0)
            continue;
        if (strcmp(line, "end") == 0 && nested-- == 0)
            break;
        if (starts_body(line))
            nested++;
        while (len + n + 2 > size)
            body = realloc(body, size *= 2);
        memcpy(body + len, line, n);
        len += n;
        body[len++] = '\n';
        body[len] = '\0';
    }
    return body;
}

/* commands [N]: the commands to run when the program stops at breakpoint
 * N (or the last one set). */
static void cmd_commands(const char *arg)
{
    uint64_t id = (uint64_t)last_bp;
    struct breakpoint *bp;

    if (arg != NULL && !parse_u64(arg, &id)) {
        error("Usage: commands [N]\n");
        return;
    }
    if (id == 0 || id > INT_MAX || (bp = bp_find((int)id)) == NULL) {
        if (arg == NULL)
            error("No breakpoints specified.\n");
        else
            error("No breakpoint number %s.\n", arg);
        return;
    }
    if (input_from_user() && user.next == NULL)
        printf("Type commands for breakpoint(s) %d, one per line.\n"
               "End with a line saying just \"end\".\n", bp->id);
    bp_set_commands(bp, read_body());
}

/* define NAME: a new command, made of the lines up to "end". */
static void cmd_define(const char *name)
{
    const struct cli_cmd *c;
    int i;

    if (name == NULL) {
        error("Argument required (name of command to define).\n");
        return;
    }
    if (strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            strlen(name) || strlen(name) >= sizeof users[0].name) {
        error("Invalid command name \"%s\".\n", name);
        return;
    }
    if (cli_lookup(commands, ncommands, name, &c) == CLI_FOUND && c->id < CMD_USER &&
        strcmp(c->name, name) == 0) {
        error("\"%s\" is a tdb command: it cannot be redefined.\n", name);
        return;
    }
    for (i = 0; i < nusers && strcmp(users[i].name, name) != 0; i++)
        ;
    if (i == USER_MAX) {
        error("Too many user commands.\n");
        return;
    }
    if (input_from_user() && user.next == NULL)
        printf("Type commands for definition of \"%s\".\n"
               "End with a line saying just \"end\".\n", name);
    char *body = read_body();
    if (i == nusers) {
        snprintf(users[i].name, sizeof users[i].name, "%s", name);
        commands[ncommands++] = (struct cli_cmd){ users[i].name, CMD_USER + i, 0 };
        nusers++;
    }
    free(users[i].body);
    users[i].body = body;
}

/* Run user command i with these arguments: each line of its body, until
 * one fails. */
static void call_user(int i, char *const args[], int nargs)
{
    static int depth;
    char line[INPUT_LINE_MAX];

    if (depth == USER_DEPTH_MAX) {
        error("Max user call depth exceeded -- command aborted.\n");
        return;
    }
    char *body = strdup(users[i].body);     /* it may be redefined while it runs */
    struct user_run saved = user;
    user.next = body;
    user.args = args;
    user.nargs = nargs;
    depth++;
    while (!failed && !quitting && user_line(line, sizeof line) != NULL)
        execute(line);
    depth--;
    user = saved;
    free(body);
}

/* Run the commands of the breakpoint the program stopped at. One that
 * resumes the program ends the list (gdb does the same): the next stop
 * may have commands of its own, and they run next. */
static void run_pending_commands(void)
{
    static bool active;

    if (active)
        return;             /* the loop below will see the new ones */
    active = true;
    while (pending_commands != NULL && !failed && !quitting) {
        char *list = pending_commands, *p = list;
        unsigned resumed = resumes;

        pending_commands = NULL;
        if (strncmp(p, "silent\n", 7) == 0)
            p += 7;
        while (*p != '\0' && resumes == resumed && !failed && !quitting) {
            char *nl = strchr(p, '\n');
            *nl = '\0';
            execute(p);
            p = nl + 1;
        }
        free(list);
    }
    free(pending_commands);         /* after an error: forget them */
    pending_commands = NULL;
    active = false;
}

/* source FILE: run the commands in FILE, up to the first that fails. */
static void cmd_source(const char *path)
{
    char *line;

    if (path[0] == '\0') {
        error("source command requires file name of file to source.\n");
        return;
    }
    if (input_push(path) < 0) {
        error("%s: %s.\n", path, strerror(errno));
        return;
    }
    /* The file's lines are not part of a user command that sources it. */
    char *saved = user.next;
    user.next = NULL;
    while (!failed && !quitting && (line = input_read("")) != NULL) {
        execute(line);
        run_pending_commands();
    }
    user.next = saved;
    input_pop();
}

static void cmd_echo(const char *text)
{
    char buf[INPUT_LINE_MAX];

    fputs(cli_unescape(text, buf, sizeof buf), stdout);
    fflush(stdout);
}

#define WORDS_MAX 8

/* Run one command line. */
static void execute(const char *line)
{
    char copy[INPUT_LINE_MAX], word[64];
    char *words[WORDS_MAX + 1] = { NULL };
    const char *p = line + strspn(line, " \t");
    const struct cli_cmd *c;
    int nwords = 0;

    if (*p == '\0' || *p == '#')
        return;             /* a comment */
    size_t len = strcspn(p, " \t");
    snprintf(word, sizeof word, "%.*s", (int)len, p);
    /* rest: the arguments as typed; words: the same, split at spaces. */
    const char *rest = p + len + strspn(p + len, " \t");
    snprintf(copy, sizeof copy, "%s", rest);
    for (char *w = strtok(copy, " \t"); w != NULL && nwords < WORDS_MAX; w = strtok(NULL, " \t"))
        words[nwords++] = w;
    char *arg1 = words[0], *arg2 = words[1];

    if ((c = find(commands, ncommands, word, "")) == NULL)
        return;
    switch (c->id) {
    case CMD_ATTACH:   cmd_attach(arg1); break;
    case CMD_BT:       cmd_backtrace(arg1); break;
    case CMD_BREAK:    cmd_break(arg1, false); break;
    case CMD_CATCH:    cmd_catch(arg1, arg2); break;
    case CMD_CHECKPOINT: cmd_checkpoint(); break;
    case CMD_COMMANDS: cmd_commands(arg1); break;
    case CMD_CONTINUE: cmd_continue(); break;
    case CMD_DEFINE:   cmd_define(arg1); break;
    case CMD_DELETE:   cmd_delete(arg1, arg2); break;
    case CMD_DETACH:   cmd_detach(); break;
    case CMD_ECHO:     cmd_echo(rest); break;
    case CMD_FINISH:   cmd_finish(); break;
    case CMD_HANDLE:   cmd_handle(arg1, words + 1, nwords > 1 ? nwords - 1 : 0); break;
    case CMD_HELP:     cmd_help(); break;
    case CMD_IGNORE:   cmd_ignore(arg1, arg2); break;
    case CMD_INFO:     cmd_info(arg1, arg2); break;
    case CMD_KILL:     cmd_kill(); break;
    case CMD_LIST:     cmd_list(arg1); break;
    case CMD_NEXT:     cmd_step('n'); break;
    case CMD_PRINT:    cmd_print(arg1); break;
    case CMD_QUIT:     quitting = true; break;
    case CMD_RCONTINUE: cmd_reverse('c'); break;
    case CMD_RECORD:   cmd_record(arg1, arg2); break;
    case CMD_RESTART:  cmd_restart(arg1); break;
    case CMD_RFINISH:  cmd_reverse('f'); break;
    case CMD_RNEXT:    cmd_reverse('n'); break;
    case CMD_RSTEPI:   cmd_reverse('i'); break;
    case CMD_REG:      cmd_reg(arg1, arg2); break;
    case CMD_REGS:     cmd_regs(); break;
    case CMD_RUN:      cmd_run(rest); break;
    case CMD_RWATCH:   cmd_watch(arg1, arg2, BP_RWATCH); break;
    case CMD_SET:
        if (arg1 != NULL && strcmp(arg1, "args") == 0)
            set_args(rest + 4 + strspn(rest + 4, " \t"));
        else
            cmd_set(arg1, arg2);
        break;
    case CMD_SHOW:     cmd_show(arg1, arg2); break;
    case CMD_SIGINFO:  cmd_siginfo(); break;
    case CMD_SOURCE:   cmd_source(rest); break;
    case CMD_STEP:     cmd_step('s'); break;
    case CMD_STEPI:    cmd_step('i'); break;
    case CMD_TBREAK:   cmd_break(arg1, true); break;
    case CMD_THREAD:   cmd_thread(arg1, line); break;
    case CMD_WATCH:    cmd_watch(arg1, arg2, BP_WATCH); break;
    case CMD_X:        cmd_x(arg1, arg2); break;
    default:           call_user(c->id - CMD_USER, words, nwords); break;
    }
}

static void cmd_help(void)
{
    puts("run [ARGS]       start the program (again), with ARGS from now on (r)\n"
         "continue         resume it (c)\n"
         "break LOC        stop at LOC: LINE, FILE:LINE, FUNCTION, *ADDR or\n"
         "                 *SYMBOL+OFFSET (b)\n"
         "tbreak LOC       stop at LOC once\n"
         "step             run to the next source line, into calls (s)\n"
         "next             run to the next source line, over calls (n)\n"
         "stepi            execute one machine instruction (si)\n"
         "finish           run until the current function returns (fin)\n"
         "list [LOC]       show source lines around LOC, or the next ten (l)\n"
         "delete [N]       delete breakpoint N, or all (d)\n"
         "watch LOC [LEN]  stop when LEN bytes at LOC change (hardware)\n"
         "rwatch LOC [LEN] stop when they are read or written\n"
         "catch syscall [NAME|NR]  stop at entry to and exit from a system call\n"
         "ignore N COUNT   do not stop at breakpoint N for the next COUNT hits\n"
         "commands [N]     commands to run when breakpoint N (or the last one)\n"
         "                 stops the program, up to \"end\"; \"silent\" first: no report\n"
         "bt [N]           backtrace: the frames on the stack (backtrace)\n"
         "handle SIG [stop|nostop] [print|noprint] [pass|nopass]\n"
         "                 what to do when the program gets signal SIG\n"
         "info signals [SIG]  the table of what to do for each signal\n"
         "siginfo          details of the signal the program stopped with\n"
         "attach PID       debug a running process; detach: let it go\n"
         "info threads     list the threads; * marks the current one\n"
         "thread [N]       show the current thread, or switch to thread N (t)\n"
         "thread apply all|N... CMD\n"
         "                 run CMD in each thread (thread apply all bt)\n"
         "info breakpoints list the breakpoints (info b)\n"
         "record           start recording every instruction (rec); record stop\n"
         "reverse-stepi    go back one instruction in the record (rsi)\n"
         "reverse-next     back to the start of the previous line (rn)\n"
         "reverse-continue back to the last breakpoint or change of a watched\n"
         "                 variable (rc)\n"
         "reverse-finish   back to the call of the current function\n"
         "record goto begin|end|N  go to instruction N of the record; info record\n"
         "checkpoint       keep a copy of the process as it is now\n"
         "restart N        go back to checkpoint N; info checkpoints;\n"
         "                 delete checkpoint N\n"
         "info symbol LOC  the symbol LOC is in\n"
         "regs             show the registers (info registers)\n"
         "reg NAME [VALUE] show or set one register\n"
         "x LOC [COUNT]    hex dump of COUNT bytes (default 64)\n"
         "print SYMBOL     the value of a 1, 2, 4 or 8-byte variable (p)\n"
         "set SYMBOL VALUE change it\n"
         "set args ARGS    the program's arguments for the next run; show args\n"
         "define NAME      a new command: its lines, up to \"end\"; in them $arg0,\n"
         "                 $arg1... are its arguments and $argc how many; show user\n"
         "source FILE      run the commands in FILE\n"
         "echo TEXT        print TEXT; \\n is a newline\n"
         "kill             kill the program\n"
         "quit             leave tdb (q)\n"
         "A command may be shortened to any unique start (\"cont\"). An empty line\n"
         "repeats step, next, stepi, continue, finish and list.");
    if (nusers > 0) {
        printf("User commands:");
        for (int i = 0; i < nusers; i++)
            printf(" %s", users[i].name);
        printf(".\n");
    }
}

/* What an empty line runs after line: line itself, its command alone
 * ("list"), or nothing. */
static void set_repeat(const char *line, char *repeat, size_t size)
{
    char word[64];
    const struct cli_cmd *c;
    const char *p = line + strspn(line, " \t");

    snprintf(word, sizeof word, "%.*s", (int)strcspn(p, " \t"), p);
    repeat[0] = '\0';
    if (cli_lookup(commands, ncommands, word, &c) != CLI_FOUND)
        return;
    if (c->flags & CLI_REPEAT)
        snprintf(repeat, size, "%s", p);
    else if (c->flags & CLI_REPEAT_BARE)
        snprintf(repeat, size, "%s", word);
}

/* Read commands from stdin and run them, until quit or the end. */
static void command_loop(void)
{
    char repeat[INPUT_LINE_MAX] = "", line[INPUT_LINE_MAX];
    char *typed;

    while (!quitting && (typed = input_read("(tdb) ")) != NULL) {
        if (typed[strspn(typed, " \t")] == '\0') {
            if (repeat[0] == '\0')
                continue;
            snprintf(line, sizeof line, "%s", repeat);
        } else {
            snprintf(line, sizeof line, "%s", typed);
            set_repeat(line, repeat, sizeof repeat);
        }
        failed = false;
        execute(line);
        run_pending_commands();
    }
}

/* ~/.tdbinit: commands to run each time tdb starts, if the file exists.
 * tdb does not read one in the current directory: a .gdbinit in a
 * directory you were given could run any command, so gdb only reads one
 * from directories you list in its "auto-load safe-path". */
static void source_tdbinit(void)
{
    const char *home = getenv("HOME");
    char path[PATH_MAX];

    if (home == NULL)
        return;
    snprintf(path, sizeof path, "%s/.tdbinit", home);
    if (access(path, F_OK) == 0) {
        failed = false;
        cmd_source(path);
    }
}

static int usage(void)
{
    fprintf(stderr, "usage: tdb [OPTIONS] PROGRAM [ARGS...]\n"
                    "       tdb [OPTIONS] -p PID\n"
                    "options:\n"
                    "  -x FILE     run the commands in FILE\n"
                    "  -ex COMMAND run COMMAND (-x and -ex run in the order given)\n"
                    "  -batch      exit after them, with status 1 if the last one failed\n"
                    "  -nx         do not read ~/.tdbinit\n"
                    "  -v          show every waitpid() status\n");
    return 2;
}

int main(int argc, char **argv)
{
    /* -x and -ex, in order: is_file[i] says which. */
    const char *todo[32];
    bool is_file[32];
    int ntodo = 0, i;
    const char *pid = NULL;
    bool nx = false, last_failed = false;

    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        const char *opt = argv[i];
        if (strcmp(opt, "-v") == 0)
            proc_verbose = true;
        else if (strcmp(opt, "-nx") == 0)
            nx = true;
        else if (strcmp(opt, "-batch") == 0)
            batch = true;
        else if ((strcmp(opt, "-x") == 0 || strcmp(opt, "-ex") == 0) && i + 1 < argc &&
                 ntodo < 32) {
            is_file[ntodo] = opt[1] == 'x';
            todo[ntodo++] = argv[++i];
        } else if (strcmp(opt, "-p") == 0 && i + 1 < argc)
            pid = argv[++i];
        else
            return usage();
    }
    if (pid != NULL ? i != argc : i >= argc)
        return usage();

    /* Our output and the target's share the terminal (or pipe): flush ours
     * line by line so the two come out in order. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    proc_catch_ctrl_c();
    input_init(batch);
    memcpy(commands, builtins, sizeof builtins);
    ncommands = NBUILTINS;

    if (!nx)
        source_tdbinit();
    if (pid != NULL) {
        cmd_attach(pid);
    } else {
        prog_argv = &argv[i];
        load_symbols(prog_argv[0]);
        /* show args: the arguments as one line. */
        for (int a = i + 1, len = 0; a < argc && len < (int)sizeof args_text; a++)
            len += snprintf(args_text + len, sizeof args_text - (size_t)len, "%s%s",
                            a > i + 1 ? " " : "", argv[a]);
    }
    for (int t = 0; t < ntodo && !quitting; t++) {
        failed = false;
        if (is_file[t]) {
            cmd_source(todo[t]);
        } else {
            execute(todo[t]);
            run_pending_commands();
        }
        last_failed = failed;
    }
    if (!batch)
        command_loop();

    /* A process we attached to goes on; one we started dies with us. */
    if (proc.attached) {
        cmd_detach();
    } else {
        forget_history();
        proc_kill(&proc);
    }
    sym_free();
    lines_free();
    return batch && last_failed ? 1 : 0;
}
