/* test_signals.c - tests for the signal table (signals.c) and the
 * prologue analysis of the backtrace (frames.c). */
#include "frames.h"
#include "signals.h"
#include "test.h"
#include "tests.h"

void test_sig_parse(void)
{
    CHECK_INT(sig_parse("SIGSEGV"), SIGSEGV);
    CHECK_INT(sig_parse("SEGV"), SIGSEGV);
    CHECK_INT(sig_parse("14"), SIGALRM);
    CHECK_INT(sig_parse("SIGNOPE"), -1);
    CHECK_INT(sig_parse("0"), -1);
    CHECK_INT(sig_parse("14x"), -1);
    CHECK_STR(sig_name(SIGALRM), "SIGALRM");
}

void test_sig_handle(void)
{
    /* gdb's defaults. */
    CHECK(!sig_action(SIGALRM).stop && !sig_action(SIGALRM).print && sig_action(SIGALRM).pass);
    CHECK(sig_action(SIGSEGV).stop && sig_action(SIGSEGV).pass);
    CHECK(!sig_action(SIGINT).pass);
    /* "stop" implies "print"; "noprint" implies "nostop". */
    CHECK_INT(sig_handle(SIGALRM, "stop"), 0);
    CHECK(sig_action(SIGALRM).stop && sig_action(SIGALRM).print);
    CHECK_INT(sig_handle(SIGALRM, "noprint"), 0);
    CHECK(!sig_action(SIGALRM).stop && !sig_action(SIGALRM).print);
    CHECK_INT(sig_handle(SIGUSR1, "nopass"), 0);
    CHECK(sig_action(SIGUSR1).stop && !sig_action(SIGUSR1).pass);
    CHECK_INT(sig_handle(SIGUSR1, "maybe"), -1);
    sig_handle(SIGUSR1, "pass");
}

void test_sig_code_text(void)
{
    CHECK_STR(sig_code_text(SIGSEGV, SEGV_MAPERR), "SEGV_MAPERR (address not mapped to object)");
    /* Code 1 means something else for SIGFPE. */
    CHECK_STR(sig_code_text(SIGFPE, 1), "FPE_INTDIV (integer divide by zero)");
    CHECK_STR(sig_code_text(SIGUSR1, SI_TKILL), "SI_TKILL (sent by tkill or tgkill)");
    CHECK_STR(sig_code_text(SIGUSR1, 42), "code 42");
    CHECK(sig_is_fault(SIGSEGV, SEGV_MAPERR));
    CHECK(!sig_is_fault(SIGSEGV, SI_KERNEL));
    CHECK(!sig_is_fault(SIGSEGV, SI_USER));
    CHECK(!sig_is_fault(SIGCHLD, 1));
}

void test_frame_state(void)
{
    /* endbr64; push %rbp; mov %rsp,%rbp; ... */
    static const unsigned char gcc[] = { 0xf3, 0x0f, 0x1e, 0xfa, 0x55, 0x48, 0x89, 0xe5 };
    /* push %rbp; mov %rsp,%rbp (no endbr64) */
    static const unsigned char old[] = { 0x55, 0x48, 0x89, 0xe5, 0x89, 0x7d, 0xfc, 0x90 };
    /* sub $0x8,%rsp: no frame pointer */
    static const unsigned char leaf[] = { 0x48, 0x83, 0xec, 0x08, 0xc3, 0x90, 0x90, 0x90 };

    CHECK_INT(frame_state(gcc, 8, 0), FRAME_AT_ENTRY);
    CHECK_INT(frame_state(gcc, 8, 4), FRAME_AT_ENTRY);  /* on the push */
    CHECK_INT(frame_state(gcc, 8, 5), FRAME_PUSHED);    /* on the mov */
    CHECK_INT(frame_state(gcc, 8, 8), FRAME_BODY);
    CHECK_INT(frame_state(old, 8, 0), FRAME_AT_ENTRY);
    CHECK_INT(frame_state(old, 8, 1), FRAME_PUSHED);
    CHECK_INT(frame_state(old, 8, 4), FRAME_BODY);
    CHECK_INT(frame_state(leaf, 8, 0), FRAME_BODY);
}
