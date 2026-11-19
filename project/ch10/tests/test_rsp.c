/* test_rsp.c - tests for rsp.c. The packets are ones gdb 12.1 sent to
 * gdbserver (set debug remote 1). */
#include <signal.h>
#include <string.h>

#include "rsp.h"
#include "test.h"
#include "tests.h"

void test_rsp_frame(void)
{
    char out[32];

    CHECK_INT(rsp_checksum("g", 1), 0x67);
    CHECK_INT(rsp_frame(out, sizeof out, "?", 1), 5);
    CHECK_STR(out, "$?#3f");
    CHECK_INT(rsp_frame(out, sizeof out, "vMustReplyEmpty", 15), 19);
    CHECK_STR(out, "$vMustReplyEmpty#3a");
    CHECK_INT(rsp_frame(out, sizeof out, "", 0), 4);
    CHECK_STR(out, "$#00");
    CHECK_INT(rsp_frame(out, 8, "qOffsets", 8), -1);
}

/* Feed text to r; the last event. */
static enum rsp_event feed(struct rsp_reader *r, const char *text)
{
    enum rsp_event e = RSP_NONE;
    for (; *text != '\0'; text++)
        e = rsp_feed(r, *text);
    return e;
}

void test_rsp_feed(void)
{
    struct rsp_reader r = { 0 };

    CHECK_INT(feed(&r, "+"), RSP_ACK);
    CHECK_INT(feed(&r, "$qOffsets#4"), RSP_NONE);
    CHECK_INT(feed(&r, "b"), RSP_PACKET);
    CHECK_STR(r.data, "qOffsets");
    CHECK_INT(r.len, 8);
    CHECK_INT(feed(&r, "$m555555554040,2d8#d7"), RSP_PACKET);
    CHECK_STR(r.data, "m555555554040,2d8");
    CHECK_INT(feed(&r, "$m555555554040,2d8#d8"), RSP_BAD);
    CHECK_INT(feed(&r, "$g#6z"), RSP_BAD);
    CHECK_INT(feed(&r, "-"), RSP_NAK);
    CHECK_INT(feed(&r, "\x03"), RSP_INTERRUPT);
    CHECK_INT(feed(&r, "noise$#00"), RSP_PACKET);
    CHECK_STR(r.data, "");
}

void test_rsp_hex(void)
{
    const uint8_t bytes[] = { 0xf3, 0x0f, 0x1e, 0xfa, 0x55 };
    uint8_t back[5];
    char hex[11];
    uint64_t v;
    const char *end;

    rsp_hex_encode(bytes, 5, hex);
    CHECK_STR(hex, "f30f1efa55");
    CHECK(rsp_hex_decode("F30F1EFA55", 5, back));
    CHECK(memcmp(back, bytes, 5) == 0);
    CHECK(!rsp_hex_decode("f30g", 2, back));
    CHECK(rsp_hex_u64("555555554040,2d8", &v, &end));
    CHECK_INT(v, 0x555555554040);
    CHECK_STR(end, ",2d8");
    CHECK(!rsp_hex_u64(",2d8", &v, &end));
    CHECK(rsp_hex_u64("ffffffffffffffff", &v, &end));
    CHECK(v == UINT64_MAX);
    CHECK(!rsp_hex_u64("10000000000000000", &v, &end));
}

void test_rsp_escape(void)
{
    const uint8_t in[] = { 'a', '#', '$', '}', '*', 0 };
    char out[16];
    size_t used;

    CHECK_INT(rsp_escape(in, 6, out, sizeof out, &used), 10);
    CHECK_INT(used, 6);
    CHECK(memcmp(out, "a}\x03}\x04}]}\x0a\0", 10) == 0);
    /* Room for 'a' and half of an escape: stop before the escape. */
    CHECK_INT(rsp_escape(in, 6, out, 2, &used), 1);
    CHECK_INT(used, 1);
}

void test_rsp_regs(void)
{
    struct user_regs_struct regs = { 0 };
    char hex[2 * RSP_REGS_BYTES + 1];
    const uint8_t rip[8] = { 0x75, 0x51, 0x55, 0x55, 0x55, 0x55, 0, 0 };
    const uint8_t eflags[4] = { 0x46, 0x02, 0, 0 };

    regs.rax = 0x1122334455667788;
    regs.rip = 0x555555555175;
    regs.eflags = 0x246;
    regs.gs = 0x2b;
    rsp_regs_encode(&regs, hex);
    CHECK_INT(strlen(hex), 2 * RSP_REGS_BYTES);
    /* Little-endian: the low byte of rax first. */
    CHECK(strncmp(hex, "8877665544332211", 16) == 0);
    CHECK(strncmp(hex + 16 * 16, "7551555555550000" "46020000", 24) == 0);
    CHECK_STR(hex + 2 * RSP_REGS_BYTES - 8, "2b000000");

    regs.rip = 0;
    regs.eflags = 0;
    CHECK(rsp_reg_set(&regs, 16, rip, 8));
    CHECK(regs.rip == 0x555555555175);
    CHECK(rsp_reg_set(&regs, 17, eflags, 4));
    CHECK(regs.eflags == 0x246);
    CHECK(!rsp_reg_set(&regs, 17, rip, 8));     /* eflags has 4 bytes */
    CHECK(!rsp_reg_set(&regs, 24, rip, 8));     /* st0: not ours */
}

void test_rsp_signals(void)
{
    CHECK_INT(rsp_signal_to_gdb(SIGSEGV), 11);
    CHECK_INT(rsp_signal_to_gdb(SIGBUS), 10);
    CHECK_INT(rsp_signal_to_gdb(SIGUSR1), 30);
    CHECK_INT(rsp_signal_to_gdb(SIGCHLD), 20);
    CHECK_INT(rsp_signal_to_gdb(SIGSTKFLT), 0);
    CHECK_INT(rsp_signal_from_gdb(10), SIGBUS);
    CHECK_INT(rsp_signal_from_gdb(7), 0);       /* SIGEMT: not on Linux */
    CHECK_INT(rsp_signal_from_gdb(0), 0);
    for (int sig = 1; sig < 32; sig++)
        if (sig != SIGSTKFLT)
            CHECK_INT(rsp_signal_from_gdb(rsp_signal_to_gdb(sig)), sig);
}
