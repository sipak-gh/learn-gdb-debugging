/* gdbserver.h - be a gdbserver: let gdb drive the process tdb is
 * debugging, with the Remote Serial Protocol over TCP ("target remote"). */
#ifndef TDB_GDBSERVER_H
#define TDB_GDBSERVER_H

#include <stdbool.h>

#include "proc.h"

/* How the session ended. */
enum gs_end {
    GS_DETACHED,         /* gdb detached ("D"): the process is still stopped, ours again */
    GS_DISCONNECTED,     /* the connection closed without a word: the same */
    GS_KILL,             /* gdb asked to kill the process ("k"): the caller does it */
    GS_EXITED,           /* the process ended (gdb was told: "W" or "X") */
    GS_ERROR,            /* no session: could not listen, or interrupted (said why) */
};

/* Listen on 127.0.0.1:port (0: any free port; the port is printed), wait
 * for gdb to connect and serve its packets until the session ends. The
 * process p must be stopped. debug: print every packet. */
enum gs_end gs_serve(struct proc *p, int port, bool debug);

#endif
