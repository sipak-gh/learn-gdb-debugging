/* signals.h - signal names, what to do when the program gets one (gdb's
 * "handle" table), and the details the kernel gives in a siginfo_t. */
#ifndef TDB_SIGNALS_H
#define TDB_SIGNALS_H

#include <signal.h>
#include <stdbool.h>

/* What to do when the program receives a signal. */
struct sig_action {
    bool stop;      /* stop and return to the (tdb) prompt */
    bool print;     /* say that it came */
    bool pass;      /* deliver it to the program when it goes on */
};

/* "SIGSEGV": the name of a signal number. */
const char *sig_name(int sig);

/* The number of "SIGSEGV", "SEGV" or "11"; -1 if there is no such signal. */
int sig_parse(const char *s);

/* The action for sig. Defaults as in gdb: stop, print and pass, except
 * SIGALRM, SIGCHLD, SIGURG, SIGIO, SIGVTALRM, SIGPROF and SIGWINCH
 * (nostop, noprint, pass) and SIGINT and SIGTRAP (not passed). */
struct sig_action sig_action(int sig);

/* Change sig's action with one of gdb's words: stop, nostop, print, noprint,
 * pass, nopass. "stop" implies "print", "noprint" implies "nostop".
 * 0, or -1 for an unknown word. */
int sig_handle(int sig, const char *word);

/* Print the table header, and one row: "SIGALRM  No    No     Yes   Alarm clock". */
void sig_print_header(void);
void sig_print_row(int sig);

/* "SEGV_MAPERR (address not mapped to object)": what si_code means for this
 * signal, or "code N" if tdb does not know it. */
const char *sig_code_text(int sig, int code);

/* true if the CPU raised sig at an address (si_addr): a SIGSEGV, SIGBUS,
 * SIGFPE or SIGILL fault, not one some process sent with kill(). */
bool sig_is_fault(int sig, int code);

#endif
