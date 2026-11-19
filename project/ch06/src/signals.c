/* signals.c - signal names, the "handle" table, and siginfo codes. */
#include "signals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct sig_action actions[NSIG];
static bool initialised;

const char *sig_name(int sig)
{
    static char buf[32];
    const char *abbrev = sigabbrev_np(sig);     /* "SEGV", glibc 2.32+ */

    if (abbrev == NULL)
        snprintf(buf, sizeof buf, "signal %d", sig);
    else
        snprintf(buf, sizeof buf, "SIG%s", abbrev);
    return buf;
}

int sig_parse(const char *s)
{
    char *end;

    if (s[0] >= '0' && s[0] <= '9') {
        long n = strtol(s, &end, 10);
        return *end == '\0' && n > 0 && n < NSIG ? (int)n : -1;
    }
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (int sig = 1; sig < NSIG; sig++) {
        const char *abbrev = sigabbrev_np(sig);
        if (abbrev != NULL && strcmp(abbrev, s) == 0)
            return sig;
    }
    return -1;
}

static void init(void)
{
    static const int quiet[] = {
        SIGALRM, SIGCHLD, SIGURG, SIGIO, SIGVTALRM, SIGPROF, SIGWINCH,
    };

    for (int sig = 1; sig < NSIG; sig++)
        actions[sig] = (struct sig_action){ .stop = true, .print = true, .pass = true };
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++)
        actions[quiet[i]] = (struct sig_action){ .stop = false, .print = false, .pass = true };
    /* Ctrl-C and breakpoint traps are meant for the debugger. */
    actions[SIGINT].pass = false;
    actions[SIGTRAP].pass = false;
    initialised = true;
}

struct sig_action sig_action(int sig)
{
    if (!initialised)
        init();
    if (sig <= 0 || sig >= NSIG)
        return (struct sig_action){ .stop = true, .print = true, .pass = true };
    return actions[sig];
}

int sig_handle(int sig, const char *word)
{
    if (!initialised)
        init();
    struct sig_action *a = &actions[sig];

    if (strcmp(word, "stop") == 0)
        a->stop = a->print = true;
    else if (strcmp(word, "nostop") == 0)
        a->stop = false;
    else if (strcmp(word, "print") == 0)
        a->print = true;
    else if (strcmp(word, "noprint") == 0)
        a->print = a->stop = false;
    else if (strcmp(word, "pass") == 0)
        a->pass = true;
    else if (strcmp(word, "nopass") == 0)
        a->pass = false;
    else
        return -1;
    return 0;
}

void sig_print_header(void)
{
    printf("Signal     Stop  Print  Pass  Description\n");
}

void sig_print_row(int sig)
{
    struct sig_action a = sig_action(sig);

    printf("%-10s %-5s %-6s %-5s %s\n", sig_name(sig), a.stop ? "Yes" : "No",
           a.print ? "Yes" : "No", a.pass ? "Yes" : "No", strsignal(sig));
}

struct code_text {
    int sig;        /* 0: any signal */
    int code;
    const char *text;
};

static const struct code_text codes[] = {
    { SIGSEGV, SEGV_MAPERR, "SEGV_MAPERR (address not mapped to object)" },
    { SIGSEGV, SEGV_ACCERR, "SEGV_ACCERR (invalid permissions for mapped object)" },
    { SIGBUS, BUS_ADRALN, "BUS_ADRALN (invalid address alignment)" },
    { SIGBUS, BUS_ADRERR, "BUS_ADRERR (nonexistent physical address)" },
    { SIGFPE, FPE_INTDIV, "FPE_INTDIV (integer divide by zero)" },
    { SIGFPE, FPE_INTOVF, "FPE_INTOVF (integer overflow)" },
    { SIGILL, ILL_ILLOPN, "ILL_ILLOPN (illegal operand)" },
    { SIGILL, ILL_ILLOPC, "ILL_ILLOPC (illegal opcode)" },
    { SIGTRAP, TRAP_BRKPT, "TRAP_BRKPT (process breakpoint)" },
    { SIGTRAP, TRAP_TRACE, "TRAP_TRACE (process trace trap)" },
    { SIGTRAP, TRAP_HWBKPT, "TRAP_HWBKPT (hardware breakpoint/watchpoint)" },
    { 0, SI_USER, "SI_USER (sent by kill)" },
    { 0, SI_KERNEL, "SI_KERNEL (sent by the kernel)" },
    { 0, SI_QUEUE, "SI_QUEUE (sent by sigqueue)" },
    { 0, SI_TIMER, "SI_TIMER (POSIX timer expired)" },
    { 0, SI_TKILL, "SI_TKILL (sent by tkill or tgkill)" },
};

const char *sig_code_text(int sig, int code)
{
    static char buf[32];

    /* Codes above 0 mean different things for different signals; codes
     * 0 and below (SI_USER, SI_TKILL, ...) and 0x80 mean the same for all. */
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
        if ((codes[i].sig == sig || codes[i].sig == 0) && codes[i].code == code)
            return codes[i].text;
    snprintf(buf, sizeof buf, "code %d", code);
    return buf;
}

bool sig_is_fault(int sig, int code)
{
    bool cpu = sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL;
    return cpu && code > 0 && code != SI_KERNEL;
}
