/* rsp.h - the GDB Remote Serial Protocol, the pure parts: packets and
 * their checksums, hex, the register layout gdb expects, and gdb's own
 * signal numbers. No sockets and no ptrace here (see gdbserver.c). */
#ifndef TDB_RSP_H
#define TDB_RSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/user.h>

/* The longest packet we take or send (data, without $ # and checksum).
 * We tell gdb in qSupported: PacketSize. */
#define RSP_MAX 4096

/* The sum of the bytes, modulo 256: the two hex digits after '#'. */
uint8_t rsp_checksum(const char *data, size_t len);

/* Frame len bytes of data as "$data#cs" into out (with a '\0' after).
 * The length, or -1 if it does not fit in size. */
int rsp_frame(char *out, size_t size, const char *data, size_t len);

/* What came in from gdb. */
enum rsp_event {
    RSP_NONE,            /* nothing yet: feed more */
    RSP_PACKET,          /* a whole packet with a good checksum: data, len */
    RSP_BAD,             /* a whole packet with a wrong checksum: ask again ('-') */
    RSP_ACK,             /* '+': our last packet arrived */
    RSP_NAK,             /* '-': send it again */
    RSP_INTERRUPT,       /* 0x03 outside a packet: Ctrl-C in gdb */
};

/* Packet reader: feed it the bytes one by one. */
struct rsp_reader {
    int state;           /* 0 between packets, 1 in the data, 2 and 3 in the checksum */
    char data[RSP_MAX + 1];
    size_t len;
    uint8_t sum;         /* checksum of the data so far */
    uint8_t got;         /* the checksum gdb sent */
    bool overflow;       /* the packet did not fit: it counts as bad */
};

/* Take byte c. On RSP_PACKET, r->data holds the packet ('\0'-terminated)
 * and r->len its length, until the next call. */
enum rsp_event rsp_feed(struct rsp_reader *r, char c);

/* Bytes to hex digits, two per byte (lower case, '\0' after). */
void rsp_hex_encode(const uint8_t *in, size_t n, char *out);

/* Hex digits to bytes: n bytes from 2n digits. false on a bad digit. */
bool rsp_hex_decode(const char *hex, size_t n, uint8_t *out);

/* Parse a hex number up to the first non-hex character; *end is set to
 * it. false if there is no digit or it overflows 64 bits. */
bool rsp_hex_u64(const char *s, uint64_t *v, const char **end);

/* Binary data in a reply (qXfer): '#', '$', '}' and '*' are escaped as
 * '}' then the byte XOR 0x20. Writes at most size bytes of out and
 * returns how many; *used is how many bytes of in fit. */
size_t rsp_escape(const uint8_t *in, size_t n, char *out, size_t size, size_t *used);

/* The registers in the order of gdb's "g" packet for x86-64: rax rbx rcx
 * rdx rsi rdi rbp rsp r8..r15 rip (8 bytes each), then eflags cs ss ds es
 * fs gs (4 bytes each). gdb knows more (x87, SSE): a shorter "g" reply
 * makes those "unavailable". */
#define RSP_NREGS 24
#define RSP_REGS_BYTES (17 * 8 + 7 * 4)

/* regs as the hex text of a "g" reply: 2 * RSP_REGS_BYTES digits + '\0'. */
void rsp_regs_encode(const struct user_regs_struct *regs, char *out);

/* Set register number regno (gdb's numbering above) from len bytes,
 * little-endian (a "P" packet). false if we do not have that register. */
bool rsp_reg_set(struct user_regs_struct *regs, int regno, const uint8_t *val, size_t len);

/* gdb numbers signals its own way, the same on every system (SIGSEGV is
 * 11, but SIGBUS is 10, SIGUSR1 30, SIGCHLD 20). Linux to gdb and back;
 * a signal with no number on the other side gives 0. */
int rsp_signal_to_gdb(int sig);
int rsp_signal_from_gdb(int gdb_sig);

#endif
