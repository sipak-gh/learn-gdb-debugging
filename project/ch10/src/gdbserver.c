/* gdbserver.c - be a gdbserver: gdb sends packets ("g": the registers,
 * "m": memory, "Z0": a breakpoint, "c": continue...) and we answer each
 * with tdb's own functions. See rsp.c for the packet format. */
#include "gdbserver.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "breakpoint.h"
#include "record.h"
#include "rsp.h"
#include "run.h"

/* The breakpoints and watchpoints gdb asked for ("Z0", "Z2", "Z4"), and
 * the tdb breakpoint that implements each. */
struct gdb_bp {
    char type;           /* '0', '2' or '4'; 0: a free slot */
    uint64_t addr;       /* run-time address, as gdb sends it */
    int id;              /* tdb's breakpoint id */
};

struct server {
    int fd;              /* the connection to gdb */
    struct proc *p;
    bool debug;          /* print every packet */
    bool multi;          /* multiprocess: thread ids are "pPID.TID" */
    pid_t pid;           /* the pid gdb knows: the process's when gdb came */
    struct rsp_reader in;
    char buf[4096];      /* bytes read from gdb, not taken yet */
    size_t pos, len;
    char stop[64];       /* the last stop reply, for "?" */
    struct gdb_bp bps[BP_MAX];
    int files[8];        /* files gdb opened with vFile:open; -1: none */
};

/* The packet log: "<-" from gdb, "->" to gdb. */
static void log_packet(const char *dir, const char *data, size_t len)
{
    printf("[remote] %s ", dir);
    for (size_t i = 0; i < len && i < 100; i++)
        putchar(data[i] >= ' ' && data[i] <= '~' ? data[i] : '.');
    printf("%s\n", len > 100 ? "..." : "");
}

/* The next byte from gdb, or -1 when the connection is gone. */
static int get_byte(struct server *s)
{
    if (s->pos == s->len) {
        ssize_t n;
        do
            n = read(s->fd, s->buf, sizeof s->buf);
        while (n < 0 && errno == EINTR);
        if (n <= 0)
            return -1;
        s->pos = 0;
        s->len = (size_t)n;
    }
    return (unsigned char)s->buf[s->pos++];
}

static bool write_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        /* MSG_NOSIGNAL: if gdb has gone, an error, not a SIGPIPE that
         * would kill tdb. */
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

/* Wait for the next packet from gdb and acknowledge it ('+'). NULL when
 * the connection is gone. */
static const char *get_packet(struct server *s)
{
    for (;;) {
        int c = get_byte(s);
        if (c < 0)
            return NULL;
        switch (rsp_feed(&s->in, (char)c)) {
        case RSP_PACKET:
            if (s->debug)
                log_packet("<-", s->in.data, s->in.len);
            if (!write_all(s->fd, "+", 1))
                return NULL;
            return s->in.data;
        case RSP_BAD:
            if (!write_all(s->fd, "-", 1))
                return NULL;
            break;
        default:                /* acks, and a Ctrl-C while already stopped */
            break;
        }
    }
}

/* Send a packet of len bytes and wait for gdb's '+' (again on '-').
 * false if the connection is gone. */
static bool put_packet_len(struct server *s, const char *data, size_t len)
{
    static char frame[RSP_MAX + 8];
    int n = rsp_frame(frame, sizeof frame, data, len);

    if (n < 0)
        return false;
    if (s->debug)
        log_packet("->", data, len);
    for (;;) {
        if (!write_all(s->fd, frame, (size_t)n))
            return false;
        int c;
        while ((c = get_byte(s)) != '+' && c != '-')
            if (c < 0)
                return false;
        if (c == '+')
            return true;
    }
}

/* The same for text. */
static bool put_packet(struct server *s, const char *data)
{
    return put_packet_len(s, data, strlen(data));
}

/* A thread id as gdb wants it: "pPID.TID" in multiprocess mode, else "TID".
 * Going back in tdb's record replaces the process with a copy (a new pid);
 * gdb goes on seeing the pid it first saw. */
static const char *thread_id(const struct server *s, pid_t tid)
{
    static char id[40];

    if (tid == s->p->pid)
        tid = s->pid;
    if (s->multi)
        snprintf(id, sizeof id, "p%x.%x", (unsigned)s->pid, (unsigned)tid);
    else
        snprintf(id, sizeof id, "%x", (unsigned)tid);
    return id;
}

/* Parse a thread id ("TID", "pPID.TID", "pPID", -1, 0). *tid: the thread,
 * or 0 for "any" or "all". */
static bool parse_thread_id(const struct server *srv, const char *s, pid_t *tid)
{
    uint64_t v;
    const char *end;

    *tid = 0;
    if (s[0] == '-' || strcmp(s, "0") == 0)
        return true;
    if (s[0] == 'p') {
        if (!rsp_hex_u64(s + 1, &v, &end))
            return false;
        if (*end != '.')
            return true;
        s = end + 1;
        if (s[0] == '-')
            return true;
    }
    if (!rsp_hex_u64(s, &v, &end))
        return false;
    *tid = (pid_t)v == srv->pid ? srv->p->pid : (pid_t)v;
    return true;
}

/* "addr,len" (both hex); *end after them. */
static bool parse_addr_len(const char *s, uint64_t *addr, uint64_t *len, const char **end)
{
    return rsp_hex_u64(s, addr, &s) && *s == ',' && rsp_hex_u64(s + 1, len, end);
}

/* m addr,len: read memory, without our breakpoints' 0xCC bytes. As much
 * as can be read; an error only if nothing can. */
static void read_memory(struct server *s, const char *args, char *out)
{
    static uint8_t bytes[RSP_MAX / 2];
    uint64_t addr, len;
    const char *end;

    if (!parse_addr_len(args, &addr, &len, &end) || *end != '\0') {
        strcpy(out, "E01");
        return;
    }
    if (len > sizeof bytes)
        len = sizeof bytes;
    size_t got = len;
    if (bp_read_code(s->p, addr, bytes, len) < 0)
        for (got = 0; got < len && bp_read_code(s->p, addr + got, bytes + got, 1) == 0; got++)
            ;
    if (got == 0)
        strcpy(out, "E01");
    else
        rsp_hex_encode(bytes, got, out);
}

/* M addr,len:XX...: write memory. */
static void write_memory(struct server *s, const char *args, char *out)
{
    static uint8_t bytes[RSP_MAX / 2];
    uint64_t addr, len;
    const char *end;

    if (!parse_addr_len(args, &addr, &len, &end) || *end != ':' || len > sizeof bytes ||
        strlen(end + 1) != 2 * len || !rsp_hex_decode(end + 1, len, bytes))
        strcpy(out, "E01");
    else
        strcpy(out, proc_write(s->p, addr, bytes, len) == 0 ? "OK" : "E01");
}

/* P n=XX...: write register n. */
static void write_register(struct server *s, const char *args, char *out)
{
    uint64_t n;
    const char *end;
    uint8_t val[8];
    struct user_regs_struct regs;
    size_t len = 0;

    if (rsp_hex_u64(args, &n, &end) && *end == '=')
        len = strlen(end + 1) / 2;
    if (len == 0 || len > sizeof val || !rsp_hex_decode(end + 1, len, val) ||
        proc_get_regs(s->p, &regs) < 0 || !rsp_reg_set(&regs, (int)n, val, len) ||
        proc_set_regs(s->p, &regs) < 0)
        strcpy(out, "E01");
    else
        strcpy(out, "OK");
}

/* Z type,addr,kind (insert) or z (remove), for type 0 (a software
 * breakpoint), 2 (a write watchpoint) and 4 (read or write; gdb uses it
 * for rwatch and awatch). Other types: "" (not supported). */
static void breakpoint_packet(struct server *s, bool insert, const char *args, char *out)
{
    struct proc *p = s->p;
    char type = args[0];
    uint64_t addr, kind;
    const char *end;

    out[0] = '\0';
    if (type != '0' && type != '2' && type != '4')
        return;
    if (args[1] != ',' || !parse_addr_len(args + 2, &addr, &kind, &end)) {
        strcpy(out, "E01");
        return;
    }
    strcpy(out, "E01");
    if (!insert) {
        for (int i = 0; i < BP_MAX; i++) {
            struct gdb_bp *g = &s->bps[i];
            if (g->type == type && g->addr == addr) {
                bp_delete(p, g->id);
                g->type = 0;
                strcpy(out, "OK");
                return;
            }
        }
        return;
    }
    struct gdb_bp *g = NULL;
    for (int i = 0; i < BP_MAX && g == NULL; i++)
        if (s->bps[i].type == 0)
            g = &s->bps[i];
    if (g == NULL)
        return;
    /* tdb's table holds addresses in the file: run-time minus load_base. */
    struct breakpoint *bp;
    if (type == '0') {
        bp = bp_add_internal(p, addr - p->load_base);
    } else {
        char what[32];
        snprintf(what, sizeof what, "*0x%" PRIx64, addr);
        bp = bp_add_watch(p, type == '2' ? BP_WATCH : BP_RWATCH, addr - p->load_base,
                          (int)kind, what);
    }
    if (bp == NULL || (type == '0' && !bp->inserted)) {
        if (bp != NULL)
            bp_delete(p, bp->id);
        return;
    }
    *g = (struct gdb_bp){ .type = type, .addr = addr, .id = bp->id };
    strcpy(out, "OK");
}

/* Take out whatever gdb left in (it removes its breakpoints itself
 * before it detaches, but not if the connection just closes). */
static void remove_gdb_breakpoints(struct server *s)
{
    for (int i = 0; i < BP_MAX; i++) {
        if (s->bps[i].type != 0) {
            bp_delete(s->p, s->bps[i].id);
            s->bps[i].type = 0;
        }
    }
}

/* A qXfer reply: the part of an object of n bytes that "offset,length"
 * asks for, 'm' before it if there is more, 'l' if it is the last part.
 * The data is binary (auxv has '\0' bytes): returns the reply's length. */
static size_t xfer_reply(const char *args, const void *data, size_t n, char *out)
{
    uint64_t off, len;
    const char *end;
    size_t used;

    if (!parse_addr_len(args, &off, &len, &end)) {
        strcpy(out, "E01");
        return 3;
    }
    if (off > n)
        off = n;
    if (len > RSP_MAX / 2)
        len = RSP_MAX / 2;
    if (len > n - off)
        len = n - off;
    size_t o = rsp_escape((const uint8_t *)data + off, len, out + 1, RSP_MAX - 1, &used);
    out[0] = off + used < n ? 'm' : 'l';
    return 1 + o;
}

/* The target description: just the architecture. Without it, and without
 * a program file, gdb would take the registers for i386 ones. */
static const char target_xml[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target>\n"
    "  <architecture>i386:x86-64</architecture>\n"
    "  <osabi>GNU/Linux</osabi>\n"
    "</target>\n";

/* qXfer:OBJECT:read:ANNEX:offset,length: read an object. auxv: the
 * auxiliary vector (gdb needs AT_ENTRY to know where a PIE is loaded, and
 * AT_BASE to find the dynamic loader and through it the libraries);
 * features: target.xml; exec-file: the program's file name. */
static size_t xfer(struct server *s, const char *what, char *out)
{
    static char data[4096];
    char path[64];
    ssize_t n = -1;
    const char *args = strrchr(what, ':');        /* "offset,length" after the last ':' */

    if (args == NULL)
        return 0;
    args++;
    if (strncmp(what, "auxv:read::", 11) == 0) {
        snprintf(path, sizeof path, "/proc/%d/auxv", (int)s->p->pid);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            n = read(fd, data, sizeof data);
            close(fd);
        }
    } else if (strncmp(what, "features:read:target.xml:", 25) == 0) {
        n = (ssize_t)strlen(target_xml);
        memcpy(data, target_xml, (size_t)n);
    } else if (strncmp(what, "exec-file:read:", 15) == 0) {
        snprintf(path, sizeof path, "/proc/%d/exe", (int)s->p->pid);
        n = readlink(path, data, sizeof data);
    } else {
        return 0;               /* "": we do not know that object */
    }
    if (n < 0) {
        strcpy(out, "E01");
        return 3;
    }
    return xfer_reply(args, data, (size_t)n, out);
}

/* An errno as gdb's File-I/O counts it: the same numbers as Linux for
 * the usual ones, 9999 for the rest. */
static int fileio_errno(int e)
{
    switch (e) {
    case EPERM: case ENOENT: case EINTR: case EBADF: case EACCES: case EFAULT:
    case EBUSY: case EEXIST: case ENODEV: case ENOTDIR: case EISDIR: case EINVAL:
    case ENFILE: case EMFILE: case EFBIG: case ENOSPC: case ESPIPE: case EROFS:
        return e;
    case ENAMETOOLONG:
        return 91;
    default:
        return 9999;
    }
}

/* vFile:setfs, open, pread, close: gdb reads files through us (the
 * program, its libraries, /proc/PID/task/TID/maps). Read-only. Replies
 * are "F" and a result, "F-1,ERRNO" on failure; pread adds ";" and the
 * bytes. Returns the length of the reply. */
static size_t vfile(struct server *s, const char *what, char *out)
{
    uint64_t a, b, c;
    const char *end;
    int slot = -1, fd;

    if (strncmp(what, "setfs:", 6) == 0)
        return (size_t)sprintf(out, "F0");      /* tdb sees the same files */
    if (strncmp(what, "open:", 5) == 0) {
        static char path[PATH_MAX];
        const char *comma = strchr(what + 5, ',');
        size_t n = comma == NULL ? 0 : (size_t)(comma - (what + 5)) / 2;

        if (n == 0 || n >= sizeof path || !rsp_hex_decode(what + 5, n, (uint8_t *)path) ||
            !rsp_hex_u64(comma + 1, &a, &end))
            return (size_t)sprintf(out, "F-1,%x", fileio_errno(EINVAL));
        path[n] = '\0';
        if (a != 0)                             /* not O_RDONLY */
            return (size_t)sprintf(out, "F-1,%x", fileio_errno(EACCES));
        for (int i = 0; i < 8 && slot < 0; i++)
            if (s->files[i] < 0)
                slot = i;
        if (slot < 0)
            return (size_t)sprintf(out, "F-1,%x", fileio_errno(EMFILE));
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return (size_t)sprintf(out, "F-1,%x", fileio_errno(errno));
        s->files[slot] = fd;
        return (size_t)sprintf(out, "F%x", slot);
    }
    if (strncmp(what, "pread:", 6) == 0) {
        static uint8_t bytes[RSP_MAX];
        ssize_t got;
        size_t used, o;

        if (!rsp_hex_u64(what + 6, &a, &end) || *end != ',' || !rsp_hex_u64(end + 1, &b, &end) ||
            *end != ',' || !rsp_hex_u64(end + 1, &c, &end) || a >= 8 || s->files[a] < 0)
            return (size_t)sprintf(out, "F-1,%x", fileio_errno(EBADF));
        if (b > RSP_MAX / 2)
            b = RSP_MAX / 2;
        got = pread(s->files[a], bytes, b, (off_t)c);
        if (got < 0)
            return (size_t)sprintf(out, "F-1,%x", fileio_errno(errno));
        /* Escaped, the bytes may not all fit: say how many did. */
        o = rsp_escape(bytes, (size_t)got, out + 20, RSP_MAX - 20, &used);
        int head = sprintf(out, "F%zx;", used);
        memmove(out + head, out + 20, o);
        return (size_t)head + o;
    }
    if (strncmp(what, "close:", 6) == 0) {
        if (!rsp_hex_u64(what + 6, &a, &end) || a >= 8 || s->files[a] < 0)
            return (size_t)sprintf(out, "F-1,%x", fileio_errno(EBADF));
        close(s->files[a]);
        s->files[a] = -1;
        return (size_t)sprintf(out, "F0");
    }
    out[0] = '\0';                              /* unlink, readlink, fstat...: not supported */
    return 0;
}

/* Tell gdb why the process stopped. continuing: it was c (or bc), not s;
 * reverse: it went backwards. */
static void stop_reply(struct server *s, const struct stop *st, bool continuing, bool reverse)
{
    struct proc *p = s->p;
    const struct breakpoint *bp;

    switch (st->why) {
    case STOP_EXITED:
        if (WIFEXITED(p->status))
            snprintf(s->stop, sizeof s->stop, "W%02x", WEXITSTATUS(p->status));
        else
            snprintf(s->stop, sizeof s->stop, "X%02x", rsp_signal_to_gdb(WTERMSIG(p->status)));
        break;
    case STOP_SIGNAL:
        snprintf(s->stop, sizeof s->stop, "T%02xthread:%s;",
                 rsp_signal_to_gdb(st->sig), thread_id(s, p->cur->tid));
        break;
    case STOP_WATCH:
        bp = bp_find(st->bp_id);
        snprintf(s->stop, sizeof s->stop, "T05thread:%s;%s:%" PRIx64 ";", thread_id(s, p->cur->tid),
                 bp->type == BP_WATCH ? "watch" : "awatch", bp->addr + p->load_base);
        break;
    case STOP_BREAKPOINT:
        /* "swbreak": an int3 was hit, and rip is already back at it. */
        snprintf(s->stop, sizeof s->stop, "T05thread:%s;%s", thread_id(s, p->cur->tid),
                 continuing ? "swbreak:;" : "");
        break;
    case STOP_RECORD:
        /* The end of the record's log (or its start, going backwards). */
        snprintf(s->stop, sizeof s->stop, "T05thread:%s;replaylog:%s;", thread_id(s, p->cur->tid),
                 reverse ? "begin" : "end");
        break;
    default:                    /* a step, a catchpoint */
        snprintf(s->stop, sizeof s->stop, "T05thread:%s;", thread_id(s, p->cur->tid));
        break;
    }
}

/* qfThreadInfo: "m" and the thread ids, in hex. One answer holds them all,
 * so qsThreadInfo (the next ones) gets "l": the end of the list. */
static void thread_list(struct server *s, char *out)
{
    char *o = out;

    *o++ = 'm';
    for (int i = 0; i < PROC_MAX_THREADS; i++)
        if (s->p->threads[i].tid != 0)
            o += sprintf(o, "%s%s", o == out + 1 ? "" : ",", thread_id(s, s->p->threads[i].tid));
    *o = '\0';
}

/* Hg TID: the thread for g, P, m and M (and for s). H c TID: the threads c
 * resumes; tdb's continue always resumes them all. 0 or -1: any. */
static void select_thread(struct server *s, const char *args, char *out)
{
    pid_t tid;
    struct thread *t;

    strcpy(out, "OK");
    if (args[0] != 'g' || !parse_thread_id(s, args + 1, &tid) || tid == 0)
        return;
    t = proc_thread(s->p, tid);
    if (t == NULL)
        strcpy(out, "E01");
    else
        proc_select(s->p, t);
}

/* c, C sig, s, S sig, bc, bs: resume, wait for the stop, reply. */
static bool resume(struct server *s, char how, const char *args)
{
    struct proc *p = s->p;
    struct stop st;
    uint64_t sig = 0;
    const char *end;
    int r;

    /* The signal the process stopped with goes on only if gdb says so:
     * C and S name it, c and s drop it. */
    if ((how == 'C' || how == 'S') && !rsp_hex_u64(args, &sig, &end))
        sig = 0;
    p->cur->pending_sig = rsp_signal_from_gdb((int)sig);
    switch (how) {
    case 'c': case 'C': r = run_continue(p, &st); break;
    case 's': case 'S': r = run_stepi(p, &st); break;
    case 'b': r = args[0] == 'c' ? rec_reverse_continue(p, &st) : rec_reverse_stepi(p, &st); break;
    default: r = -1;
    }
    if (r < 0)
        return put_packet(s, "E01");
    stop_reply(s, &st, how == 'c' || how == 'C' || (how == 'b' && args[0] == 'c'), how == 'b');
    return put_packet(s, s->stop);
}

/* Answer one packet. Returns -1 to go on, or how the session ends. */
static int serve_packet(struct server *s, const char *pkt)
{
    static char out[RSP_MAX + 1];
    struct user_regs_struct regs;

    out[0] = '\0';              /* "": we do not know this packet */
    switch (pkt[0]) {
    case '?':                   /* why it stopped; "" until it has run */
        if (s->stop[0] == '\0')
            snprintf(out, sizeof out, "T05thread:%s;", thread_id(s, s->p->cur->tid));
        else
            strcpy(out, s->stop);
        break;
    case 'g':
        if (proc_get_regs(s->p, &regs) < 0)
            strcpy(out, "E01");
        else
            rsp_regs_encode(&regs, out);
        break;
    case 'P': write_register(s, pkt + 1, out); break;
    case 'm': read_memory(s, pkt + 1, out); break;
    case 'M': write_memory(s, pkt + 1, out); break;
    case 'Z': breakpoint_packet(s, true, pkt + 1, out); break;
    case 'z': breakpoint_packet(s, false, pkt + 1, out); break;
    case 'H': select_thread(s, pkt + 1, out); break;
    case 'T': {                 /* T TID: is that thread alive? */
        pid_t tid;

        strcpy(out, parse_thread_id(s, pkt + 1, &tid) && proc_thread(s->p, tid) != NULL
                    ? "OK" : "E01");
        break;
    }
    case 'c': case 'C': case 's': case 'S':
        if (!resume(s, pkt[0], pkt + 1))
            return GS_DISCONNECTED;
        return s->p->state == PROC_STOPPED ? -1 : GS_EXITED;
    case 'b':
        if ((pkt[1] != 'c' && pkt[1] != 's') || !rec_on())
            break;
        if (!resume(s, 'b', pkt + 1))
            return GS_DISCONNECTED;
        return -1;
    case 'k':                   /* no reply */
        return GS_KILL;
    case 'v':                   /* vKill;PID: k, in multiprocess mode */
        if (strncmp(pkt, "vFile:", 6) == 0)
            return put_packet_len(s, out, vfile(s, pkt + 6, out)) ? -1 : GS_DISCONNECTED;
        if (strncmp(pkt, "vKill;", 6) == 0) {
            put_packet(s, "OK");
            return GS_KILL;
        }
        break;
    case 'D':
        put_packet(s, "OK");
        return GS_DETACHED;
    case 'q':
        if (strncmp(pkt, "qSupported", 10) == 0) {
            s->multi = strstr(pkt, "multiprocess+") != NULL;
            snprintf(out, sizeof out,
                     "PacketSize=%x;qXfer:auxv:read+;qXfer:features:read+;"
                     "qXfer:exec-file:read+;swbreak+%s%s",
                     RSP_MAX, s->multi ? ";multiprocess+" : "",
                     rec_on() ? ";ReverseContinue+;ReverseStep+" : "");
        }
        else if (strncmp(pkt, "qXfer:", 6) == 0)
            return put_packet_len(s, out, xfer(s, pkt + 6, out)) ? -1 : GS_DISCONNECTED;
        else if (strcmp(pkt, "qfThreadInfo") == 0)
            thread_list(s, out);
        else if (strcmp(pkt, "qsThreadInfo") == 0)
            strcpy(out, "l");
        else if (strcmp(pkt, "qC") == 0)
            sprintf(out, "QC%s", thread_id(s, s->p->cur->tid));
        else if (strncmp(pkt, "qAttached", 9) == 0)
            strcpy(out, s->p->attached ? "1" : "0");
        break;
    default:                    /* X (binary write: gdb falls back to M) */
        break;
    }
    return put_packet(s, out) ? -1 : GS_DISCONNECTED;
}

/* Listen on 127.0.0.1:port and wait for one connection. The socket, or
 * -1 (after saying why). */
static int wait_for_gdb(int port)
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    socklen_t alen = sizeof addr;
    int one = 1, fd;

    /* Only this machine: whoever connects can make the process do
     * anything (M writes memory, P sets rip). */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 1) < 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &alen) < 0) {
        printf("Cannot listen on port %d: %s.\n", port, strerror(errno));
        if (lfd >= 0)
            close(lfd);
        return -1;
    }
    printf("Listening on port %d\n", ntohs(addr.sin_port));
    fflush(stdout);
    alen = sizeof addr;
    fd = accept(lfd, (struct sockaddr *)&addr, &alen);
    if (fd < 0)                 /* EINTR: Ctrl-C */
        printf("No connection: %s.\n", strerror(errno));
    else {
        printf("Remote debugging from host %s, port %d\n", inet_ntoa(addr.sin_addr),
               ntohs(addr.sin_port));
        /* Send each small packet at once: without this, TCP holds an ack
         * back until the answer to the last one arrives (Nagle), 40 ms a time. */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    close(lfd);
    return fd;
}

enum gs_end gs_serve(struct proc *p, int port, bool debug)
{
    static struct server s;
    int end = -1;

    memset(&s, 0, sizeof s);
    s.p = p;
    s.debug = debug;
    s.pid = p->pid;
    for (int i = 0; i < 8; i++)
        s.files[i] = -1;
    s.fd = wait_for_gdb(port);
    if (s.fd < 0)
        return GS_ERROR;
    while (end < 0) {
        const char *pkt = get_packet(&s);
        end = pkt == NULL ? GS_DISCONNECTED : serve_packet(&s, pkt);
    }
    if (p->state == PROC_STOPPED)
        remove_gdb_breakpoints(&s);
    for (int i = 0; i < 8; i++)
        if (s.files[i] >= 0)
            close(s.files[i]);
    close(s.fd);
    return (enum gs_end)end;
}
