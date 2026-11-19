/* tdb.c - a tiny debugger: the command loop.
 *
 *     tdb [-v] PROGRAM [ARGS...]
 *     tdb [-v] -p PID
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
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "breakpoint.h"
#include "debugreg.h"
#include "frames.h"
#include "lines.h"
#include "mem.h"
#include "proc.h"
#include "regs.h"
#include "run.h"
#include "signals.h"
#include "source.h"
#include "step.h"
#include "symbols.h"
#include "syscalls.h"

static struct proc proc;
static char **prog_argv;
/* Where list goes on: the file and the first line it shows next. */
static int list_file = -1, list_next = 1;

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
}

/* Say why the process stopped or ended. */
static void report(const struct stop *st)
{
    int status = proc.status;
    struct user_regs_struct regs;
    char prefix[64];

    if (st->why != STOP_EXITED && proc.cur->tid != resumed_tid)
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
    case STOP_EXITED:
        if (WIFEXITED(status))
            printf("[process exited with status %d]\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf("[process killed by %s (%s)]\n", sig_name(WTERMSIG(status)),
                   strsignal(WTERMSIG(status)));
        break;
    }
}

static void cmd_run(const char *arg)
{
    struct stop st;

    if (arg != NULL) {
        printf("tdb takes the program's arguments on its own command line:\n"
               "    tdb PROGRAM ARGS...\n");
        return;
    }
    if (prog_argv == NULL) {
        printf("No program to run.\n");
        return;
    }
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
        printf("\"finish\" not meaningful in the outermost frame.\n");
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
        printf("\"%s\" is not FILE:LINE.\n", arg);
        return false;
    }
    *line = atoi(num);
    if (colon != NULL) {
        snprintf(file, size, "%.*s", (int)(colon - arg), arg);
    } else {
        const char *f = default_file();
        if (f == NULL) {
            printf("No line information: was the program compiled with -g?\n");
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
            printf("No source file named %s.\n", file);
            return false;
        }
        if (line_find(file, line, &a, &found) < 0) {
            printf("No line %d in file \"%s\".\n", line, file);
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
        printf("Usage: %s LINE|FILE:LINE|FUNCTION|*ADDRESS\n", temporary ? "tbreak" : "break");
        return;
    }
    if (!code_location(arg, &addr))
        return;
    /* The table keeps file addresses: they stay right when the program is
     * started again at another load address. */
    struct breakpoint *bp = bp_add(&proc, addr - base(), temporary);
    if (bp == NULL) {
        printf("Too many breakpoints.\n");
        return;
    }
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
            printf("No source file named %s.\n", name);
            return;
        }
        file = line_at(a)->file;
    } else if (arg != NULL) {
        const struct symbol *s = sym_by_name(arg);
        const struct line_row *row = s ? line_at(s->addr) : NULL;
        if (row == NULL) {
            printf("Function \"%s\" not defined%s.\n", arg, s ? " with line information" : "");
            return;
        }
        file = row->file;
        center = row->line;
    } else if (file < 0) {
        /* Nothing shown yet: around main. */
        const struct symbol *m = sym_by_name("main");
        const struct line_row *row = m ? line_at(m->addr) : NULL;
        if (row == NULL) {
            printf("No line information: was the program compiled with -g?\n");
            return;
        }
        file = row->file;
        center = row->line;
    }
    int first = center == 0 ? list_next : center > 5 ? center - 5 : 1;
    int count = src_count(line_file_path(file));
    if (count < 0) {
        printf("%s: %s.\n", line_file_path(file), strerror(errno));
        return;
    }
    if (first > count) {
        printf("Line number %d out of range; \"%s\" has %d lines.\n", first,
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
        printf("Usage: %s LOC [LEN]\n", type == BP_WATCH ? "watch" : "rwatch");
        return;
    }
    if (!parse_location(arg1, &addr))
        return;
    if ((s = sym_by_name(arg1)) != NULL &&
        (s->size == 1 || s->size == 2 || s->size == 4 || s->size == 8))
        len = s->size;
    if (arg2 != NULL && !parse_u64(arg2, &len)) {
        printf("Usage: %s LOC [LEN]\n", type == BP_WATCH ? "watch" : "rwatch");
        return;
    }
    struct breakpoint *bp = bp_add_watch(&proc, type, addr - base(), (int)len, arg1);
    if (bp != NULL)
        printf("Hardware %swatchpoint %d: %s (%d bytes at 0x%" PRIx64 ", DR%d)\n",
               type == BP_WATCH ? "" : "read ", bp->id, arg1, bp->len, addr, bp->slot);
}

/* catch syscall [NAME|NUMBER]: stop at a system call's entry and exit. */
static void cmd_catch(const char *arg1, const char *arg2)
{
    long nr = -1;

    if (arg1 == NULL || strcmp(arg1, "syscall") != 0) {
        printf("Usage: catch syscall [NAME|NUMBER]\n");
        return;
    }
    if (arg2 != NULL && (nr = sys_number(arg2)) < 0) {
        printf("Unknown syscall name '%s'.\n", arg2);
        return;
    }
    struct breakpoint *bp = bp_add_syscall(nr);
    if (bp == NULL) {
        printf("Too many breakpoints.\n");
        return;
    }
    if (nr < 0)
        printf("Catchpoint %d (any syscall)\n", bp->id);
    else
        printf("Catchpoint %d (syscall '%s' [%ld])\n", bp->id, sys_name(nr), nr);
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

/* bt [N]: the frames on the stack, innermost first. */
static void cmd_backtrace(const char *arg)
{
    uint64_t pcs[64], max = 64;

    if (!running())
        return;
    if (arg != NULL && (!parse_u64(arg, &max) || max == 0)) {
        printf("Usage: bt [N]\n");
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

static int execute(char *line);

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
        printf("Usage: thread apply all|N... COMMAND\n");
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
        printf("Invalid thread ID: %s\n", arg);
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
        printf("Usage: handle SIGNAL [stop|nostop] [print|noprint] [pass|nopass]\n");
        return;
    }
    if ((sig = sig_parse(sig_text)) < 0) {
        printf("Unknown signal \"%s\".\n", sig_text);
        return;
    }
    for (int i = 0; i < nwords; i++) {
        if (sig_handle(sig, words[i]) < 0) {
            printf("Unknown word \"%s\": use stop, nostop, print, noprint, pass or nopass.\n",
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
        printf("Unknown signal \"%s\".\n", arg);
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
        printf("The program did not stop because of a signal.\n");
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
        printf("Usage: attach PID\n");
        return;
    }
    if (proc.state == PROC_STOPPED) {
        printf("Already debugging process %d: detach or kill it first.\n", (int)proc.pid);
        return;
    }
    if (!exe_of((pid_t)pid, exe, sizeof exe)) {
        printf("No process %d (or not ours to look at).\n", (int)pid);
        return;
    }
    load_symbols(exe);
    exe_argv[0] = exe;
    prog_argv = exe_argv;
    if (proc_attach(&proc, (pid_t)pid, exe) < 0) {
        printf("Could not attach to process %d: %s.\n", (int)pid, strerror(errno));
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
    pid_t pid = proc.pid;
    bp_remove_all(&proc);
    if (proc_detach(&proc) == 0)
        printf("Detached from process %d.\n", (int)pid);
}

static void cmd_help(void)
{
    puts("run              start the program (again) (r)\n"
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
    char whole[256];
    snprintf(whole, sizeof whole, "%s", line);  /* strtok() cuts line up */
    char *cmd = strtok(line, " \t");
    char *arg1 = strtok(NULL, " \t");
    char *arg2 = strtok(NULL, " \t");
    char *more[4];
    int nmore = 0;

    while (nmore < 4 && (more[nmore] = strtok(NULL, " \t")) != NULL)
        nmore++;
    if (cmd == NULL)
        return 0;
    if (is(cmd, "run", "r"))
        cmd_run(arg1);
    else if (is(cmd, "continue", "c"))
        cmd_continue();
    else if (is(cmd, "break", "b"))
        cmd_break(arg1, false);
    else if (is(cmd, "tbreak", NULL))
        cmd_break(arg1, true);
    else if (is(cmd, "step", "s"))
        cmd_step('s');
    else if (is(cmd, "next", "n"))
        cmd_step('n');
    else if (is(cmd, "stepi", "si"))
        cmd_step('i');
    else if (is(cmd, "finish", "fin"))
        cmd_finish();
    else if (is(cmd, "list", "l"))
        cmd_list(arg1);
    else if (is(cmd, "watch", NULL))
        cmd_watch(arg1, arg2, BP_WATCH);
    else if (is(cmd, "rwatch", NULL))
        cmd_watch(arg1, arg2, BP_RWATCH);
    else if (is(cmd, "catch", NULL))
        cmd_catch(arg1, arg2);
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
    else if (is(cmd, "bt", "backtrace"))
        cmd_backtrace(arg1);
    else if (is(cmd, "handle", NULL)) {
        char *words[5] = { arg2, more[0], more[1], more[2], more[3] };
        cmd_handle(arg1, words, arg2 == NULL ? 0 : 1 + nmore);
    } else if (is(cmd, "info", "i") && arg1 != NULL && is(arg1, "signals", NULL))
        cmd_info_signals(arg2);
    else if (is(cmd, "siginfo", NULL))
        cmd_siginfo();
    else if (is(cmd, "info", "i") && arg1 != NULL && is(arg1, "threads", NULL))
        cmd_info_threads();
    else if (is(cmd, "thread", "t"))
        cmd_thread(arg1, whole);
    else if (is(cmd, "attach", NULL))
        cmd_attach(arg1);
    else if (is(cmd, "detach", NULL))
        cmd_detach();
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
    if (i >= argc || (strcmp(argv[i], "-p") == 0 && i + 2 != argc)) {
        fprintf(stderr, "usage: tdb [-v] PROGRAM [ARGS...]\n"
                        "       tdb [-v] -p PID\n");
        return 2;
    }

    /* Our output and the target's share the terminal (or pipe): flush ours
     * line by line so the two come out in order. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    proc_catch_ctrl_c();

    if (strcmp(argv[i], "-p") == 0) {
        cmd_attach(argv[i + 1]);
    } else {
        prog_argv = &argv[i];
        load_symbols(prog_argv[0]);
    }

    bool echo = !isatty(STDIN_FILENO);
    char line[256];
    for (;;) {
        printf("(tdb) ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) {
            if (ferror(stdin) && errno == EINTR) {
                /* Ctrl-C at the prompt: forget the line, as gdb does. */
                clearerr(stdin);
                printf("Quit\n");
                continue;
            }
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
    /* A process we attached to goes on; one we started dies with us. */
    if (proc.attached)
        cmd_detach();
    else
        proc_kill(&proc);
    sym_free();
    lines_free();
    return 0;
}
