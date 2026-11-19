/* rsp.c - the GDB Remote Serial Protocol, the pure parts. */
#include "rsp.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

uint8_t rsp_checksum(const char *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += (uint8_t)data[i];
    return sum;
}

int rsp_frame(char *out, size_t size, const char *data, size_t len)
{
    if (len + 5 > size)
        return -1;
    out[0] = '$';
    memcpy(out + 1, data, len);
    snprintf(out + 1 + len, 4, "#%02x", rsp_checksum(data, len));
    return (int)len + 4;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

enum rsp_event rsp_feed(struct rsp_reader *r, char c)
{
    int d;

    switch (r->state) {
    case 0:                     /* between packets */
        if (c == '$') {
            r->state = 1;
            r->len = 0;
            r->sum = 0;
            r->overflow = false;
        } else if (c == '+') {
            return RSP_ACK;
        } else if (c == '-') {
            return RSP_NAK;
        } else if (c == 0x03) {
            return RSP_INTERRUPT;
        }
        return RSP_NONE;        /* anything else is noise */
    case 1:                     /* the data, up to '#' */
        if (c == '#') {
            r->state = 2;
            return RSP_NONE;
        }
        r->sum += (uint8_t)c;
        if (r->len < RSP_MAX)
            r->data[r->len++] = c;
        else
            r->overflow = true;
        return RSP_NONE;
    case 2:                     /* first checksum digit */
        d = hex_digit(c);
        r->got = (uint8_t)((d < 0 ? 0 : d) << 4);
        r->overflow |= d < 0;
        r->state = 3;
        return RSP_NONE;
    default:                    /* second checksum digit: the packet is complete */
        d = hex_digit(c);
        r->got |= (uint8_t)(d < 0 ? 0 : d);
        r->overflow |= d < 0;
        r->state = 0;
        r->data[r->len] = '\0';
        return r->overflow || r->got != r->sum ? RSP_BAD : RSP_PACKET;
    }
}

void rsp_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xf];
    }
    out[2 * n] = '\0';
}

bool rsp_hex_decode(const char *hex, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hex_digit(hex[2 * i]), lo = hi < 0 ? -1 : hex_digit(hex[2 * i + 1]);
        if (lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

bool rsp_hex_u64(const char *s, uint64_t *v, const char **end)
{
    uint64_t x = 0;
    int d, n = 0;

    for (; (d = hex_digit(s[n])) >= 0; n++) {
        if (x >> 60)
            return false;
        x = x << 4 | (uint64_t)d;
    }
    *end = s + n;
    *v = x;
    return n > 0;
}

size_t rsp_escape(const uint8_t *in, size_t n, char *out, size_t size, size_t *used)
{
    size_t o = 0, i;

    for (i = 0; i < n; i++) {
        bool special = in[i] == '#' || in[i] == '$' || in[i] == '}' || in[i] == '*';
        if (o + (special ? 2 : 1) > size)
            break;
        if (special) {
            out[o++] = '}';
            out[o++] = (char)(in[i] ^ 0x20);
        } else {
            out[o++] = (char)in[i];
        }
    }
    *used = i;
    return o;
}

/* Where each register of the "g" packet is in struct user_regs_struct,
 * and how many bytes gdb wants of it. */
static const struct {
    size_t offset;
    size_t size;
} layout[RSP_NREGS] = {
#define R(name, size) { offsetof(struct user_regs_struct, name), size }
    R(rax, 8), R(rbx, 8), R(rcx, 8), R(rdx, 8), R(rsi, 8), R(rdi, 8), R(rbp, 8), R(rsp, 8),
    R(r8, 8),  R(r9, 8),  R(r10, 8), R(r11, 8), R(r12, 8), R(r13, 8), R(r14, 8), R(r15, 8),
    R(rip, 8), R(eflags, 4), R(cs, 4), R(ss, 4), R(ds, 4), R(es, 4), R(fs, 4), R(gs, 4),
#undef R
};

void rsp_regs_encode(const struct user_regs_struct *regs, char *out)
{
    for (int i = 0; i < RSP_NREGS; i++) {
        /* x86-64 is little-endian, and so is the packet: the low bytes
         * of each field come first. */
        rsp_hex_encode((const uint8_t *)regs + layout[i].offset, layout[i].size, out);
        out += 2 * layout[i].size;
    }
}

bool rsp_reg_set(struct user_regs_struct *regs, int regno, const uint8_t *val, size_t len)
{
    if (regno < 0 || regno >= RSP_NREGS || len != layout[regno].size)
        return false;
    unsigned long long v = 0;   /* the fields are unsigned long long */
    memcpy(&v, val, len);
    memcpy((uint8_t *)regs + layout[regno].offset, &v, sizeof v);
    return true;
}

/* Linux signal number -> gdb's, from gdb's include/gdb/signals.def. */
static const int to_gdb[32] = {
    [SIGHUP] = 1,     [SIGINT] = 2,     [SIGQUIT] = 3,    [SIGILL] = 4,
    [SIGTRAP] = 5,    [SIGABRT] = 6,    [SIGBUS] = 10,    [SIGFPE] = 8,
    [SIGKILL] = 9,    [SIGUSR1] = 30,   [SIGSEGV] = 11,   [SIGUSR2] = 31,
    [SIGPIPE] = 13,   [SIGALRM] = 14,   [SIGTERM] = 15,   [SIGCHLD] = 20,
    [SIGCONT] = 19,   [SIGSTOP] = 17,   [SIGTSTP] = 18,   [SIGTTIN] = 21,
    [SIGTTOU] = 22,   [SIGURG] = 16,    [SIGXCPU] = 24,   [SIGXFSZ] = 25,
    [SIGVTALRM] = 26, [SIGPROF] = 27,   [SIGWINCH] = 28,  [SIGIO] = 23,
    [SIGPWR] = 32,    [SIGSYS] = 12,
};

int rsp_signal_to_gdb(int sig)
{
    return sig > 0 && sig < 32 ? to_gdb[sig] : 0;
}

int rsp_signal_from_gdb(int gdb_sig)
{
    for (int sig = 1; sig < 32; sig++)
        if (gdb_sig != 0 && to_gdb[sig] == gdb_sig)
            return sig;
    return 0;
}
