# tdb — Chapter 7 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 7 it debugs programs with threads and programs that
fork. It traces every thread (`PTRACE_O_TRACECLONE`) and stops them all
when one stops ("all-stop", as gdb does), so `info threads`, `thread N` and
`thread apply all bt` show the program frozen at one moment. Watchpoints
are set in every thread, new ones too. A child made by `fork` gets the
breakpoints taken out of its copy of the code and is let go. Ctrl-C stops
the program, and `attach` takes all the threads of a running process.

## Build

    make            # build/tdb and the demo targets in build/targets/
    make test       # unit tests (build/unit), then integration tests (tests/itest.sh)
    make clean

Needs gcc, GNU make and glibc 2.32 or newer (for `sigabbrev_np` and
`strerrorname_np`) on Linux x86-64. `src/sysnames.h` is generated from the
kernel headers by `tools/sysnames.sh`; it is checked in, so the build does
not need the script. The targets are built with `-g -O0`; gcc 11 writes
DWARF 5, the only version tdb reads (use `-gdwarf-5` with older compilers).

## Run

    tdb [-v] PROGRAM [ARGS...]
    tdb [-v] -p PID

Which worker thread gets to `work` first changes from run to run; this is
one run:

    $ printf 'break work\nrun\ninfo threads\nthread apply all bt\ncontinue\ncontinue\ncontinue\n' | build/tdb build/targets/workers
    Reading symbols from build/targets/workers: 22 symbols, 33 line table rows.
    (tdb) break work
    Breakpoint 1 at 0x1258: file targets/workers.c, line 14.
    (tdb) run
    Started process 870411: build/targets/workers
    [New LWP 870412]
    [New LWP 870413]
    [Switching to LWP 870412]
    Thread 2 hit Breakpoint 1, work () at targets/workers.c:14
    14	    long share = 100 * id;
    (tdb) info threads
      Id   Target Id               Frame
      1    LWP 870411 "workers"    0x00007ffff7d268cd in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    * 2    LWP 870412 "worker-1"   work () at targets/workers.c:14
      3    LWP 870413 "workers"    0x00007ffff7d268cd in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    (tdb) thread apply all bt

    Thread 3 (LWP 870413 "workers"):
    #0  0x00007ffff7d268cd in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6

    Thread 2 (LWP 870412 "worker-1"):
    #0  work () at targets/workers.c:14
    #1  0x0000555555555303 in worker () at targets/workers.c:28
    #2  0x00007ffff7c94a83 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6

    Thread 1 (LWP 870411 "workers"):
    #0  0x00007ffff7d268cd in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    #1  0x0000555555555374 in main () at targets/workers.c:37
    (tdb) continue
    [LWP 870412 exited]
    [New LWP 870414]
    [Switching to LWP 870413]
    Thread 3 hit Breakpoint 1, work () at targets/workers.c:14
    14	    long share = 100 * id;
    (tdb) continue
    [LWP 870413 exited]
    [Switching to LWP 870414]
    Thread 4 hit Breakpoint 1, work () at targets/workers.c:14
    14	    long share = 100 * id;
    (tdb) continue
    [LWP 870414 exited]
    total: 600
    [process exited with status 0]
    (tdb)

Thread numbers are tdb's own: 1, 2, 3, ... in the order tdb saw the
threads start, never reused. LWP numbers are the kernel's thread ids. The
name comes from `/proc/PID/task/TID/comm`: worker 1 has already called
`pthread_setname_np`, thread 3 has not yet. Thread 3 had not reached the
breakpoint, but it stopped too: when one thread stops, tdb stops the others
with `tgkill(pid, tid, SIGSTOP)`. tdb has no symbols for libc, so it names
the file instead (`from .../libc.so.6`).

A program that hangs: attach to it and look at every thread.

    $ build/targets/deadlock &
    deadlock: pid 870417
    $ printf 'info threads\nthread apply all bt\ndetach\n' | build/tdb -p 870417
    Reading symbols from /home/tbangma/learn-gdb-debugging/project/ch07/build/targets/deadlock: 24 symbols, 31 line table rows.
    Attached to process 870417.
    [New LWP 870418]
    [New LWP 870419]
    0x00007d797b6910d7 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    (tdb) info threads
      Id   Target Id               Frame
    * 1    LWP 870417 "deadlock"   0x00007d797b6910d7 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
      2    LWP 870418 "deadlock"   0x00007d797b691280 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
      3    LWP 870419 "deadlock"   0x00007d797b691280 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    (tdb) thread apply all bt

    Thread 3 (LWP 870419 "deadlock"):
    #0  0x00007d797b691280 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    #1  0x00005e7b74e892e5 in take_both () at targets/deadlock.c:24
    #2  0x00005e7b74e8936d in two () at targets/deadlock.c:39
    #3  0x00007d797b694a83 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6

    Thread 2 (LWP 870418 "deadlock"):
    #0  0x00007d797b691280 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    #1  0x00005e7b74e892e5 in take_both () at targets/deadlock.c:24
    #2  0x00005e7b74e8933d in one () at targets/deadlock.c:32
    #3  0x00007d797b694a83 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6

    Thread 1 (LWP 870417 "deadlock"):
    #0  0x00007d797b6910d7 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    #1  0x00005e7b74e8942a in main () at targets/deadlock.c:52
    (tdb) detach
    Detached from process 870417.
    (tdb)

Both workers wait in `pthread_mutex_lock` on line 24 of `take_both`, one
called from `one`, the other from `two`: each holds the lock the other
wants. libc keeps no frame pointer, so for a frame in libc tdb scans the
stack for the first word that points just after a `call` in the program,
and goes on from there with `%rbp`. gdb reads the call frame information
in `.eh_frame` instead, and finds every frame. The scan also finds `inner`
in the `deep` target, which the Chapter 6 tdb missed.

With the program running under tdb, Ctrl-C stops it (`Thread 1 received
signal SIGINT (Interrupt).`); at the prompt it prints `Quit`. The program
is in tdb's process group, so it gets that `SIGINT` too, and reports it at
the next `continue` (gdb gives the program the terminal while it runs, and
takes it back at the prompt).

    $ printf 'break greet\nrun\ncontinue\n' | build/tdb build/targets/forker
    Reading symbols from build/targets/forker: 20 symbols, 19 line table rows.
    (tdb) break greet
    Breakpoint 1 at 0x11f9: file targets/forker.c, line 11.
    (tdb) run
    Started process 864041: build/targets/forker
    [Detaching after fork from child process 864042]
    hello from the child
    Breakpoint 1, greet () at targets/forker.c:11
    11	    printf("hello from the %s\n", who);
    (tdb) continue
    hello from the parent
    [process exited with status 0]
    (tdb)

The child is a copy of the parent, `int3` bytes included. The Chapter 6 tdb
let it run with them, and the child died of `SIGTRAP` ("child killed by
signal 5 (Trace/breakpoint trap)"). tdb now hears of the fork
(`PTRACE_O_TRACEFORK`), puts the original bytes back in the child, and
detaches from it.

With more than one thread, stops say which thread (`Thread 2 hit
Breakpoint 1`, `Thread 3 received signal SIGSEGV`), and `[Switching to LWP
N]` when it is not the thread that was current.

Commands: `run` (`r`), `continue` (`c`), `break LOC` (`b`), `tbreak LOC`,
`step` (`s`), `next` (`n`), `stepi` (`si`), `finish` (`fin`),
`list [LOC]` (`l`),
`watch LOC [LEN]`, `rwatch LOC [LEN]`, `catch syscall [NAME|NUMBER]`,
`delete [N]` (`d`), `ignore N COUNT`, `bt [N]` (`backtrace`),
`handle SIG [stop|nostop] [print|noprint] [pass|nopass]`,
`info signals [SIG]`, `siginfo`, `attach PID`, `detach`,
`info threads`, `thread [N]` (`t`), `thread apply all|N... CMD`,
`info breakpoints` (`info b`), `info symbol LOC`, `regs` (`info registers`),
`reg NAME [VALUE]`, `x LOC [COUNT]`, `print SYMBOL` (`p`),
`set SYMBOL VALUE`, `kill`, `help`, `quit` (`q`).

`continue`, `finish`, and `next` or `step` over a call let all threads run;
`stepi`, and `step` or `next` within a line, run only the current thread.
A breakpoint that another thread hits during `next` ends the `next` there,
as in gdb.

Limits: at most 64 threads. Children made by `vfork` (and so by
`posix_spawn`) are not traced: a breakpoint in code such a child runs before
its `exec` kills it. A `stepi` over a system call that blocks (a `futex`
waiting for a lock that another, stopped, thread holds) hangs.

## Files

| File | What it does |
|---|---|
| `src/tdb.c` | `main`, the command loop, reporting stops (and which thread), the commands |
| `src/proc.c` | `fork` + `PTRACE_TRACEME` + `execv`, or `PTRACE_ATTACH` to every thread in `/proc/PID/task`; the thread table; `waitpid(-1, __WALL)` and the events it brings (new thread, thread exit, fork); all-stop: stop the other threads with `tgkill(SIGSTOP)`; resume one thread or all; registers, memory, siginfo of the current thread; detach; Ctrl-C; which file is mapped at an address |
| `src/elf.c` | reading the file; finding a section by name; the load address: ELF header `e_entry` vs `AT_ENTRY` from `/proc/PID/auxv` |
| `src/lines.c` | the DWARF 5 line table: decodes the `.debug_line` program into rows (address, file, line, is_stmt); address → line, line → address, end of the prologue |
| `src/source.c` | prints lines of a source file |
| `src/step.c` | `step`, `next`, `finish`: single steps, calls run to their return address, prologues skipped |
| `src/symbols.c` | reads `.symtab` from the ELF file; name ↔ address; parses `main+4` |
| `src/regs.c` | register names → offsets in `struct user_regs_struct`; the flags in `eflags` |
| `src/mem.c` | one line of a hex dump |
| `src/debugreg.c` | DR7 encoding; reading and writing DR0–DR7 with `PTRACE_PEEKUSER`/`POKEUSER`, in every thread, and in each new one |
| `src/syscalls.c`, `src/sysnames.h` | system call number ↔ name |
| `src/signals.c` | signal names; the stop/print/pass table; `si_code` texts |
| `src/frames.c` | the backtrace: prologue check, then the chain of saved `%rbp`; in a library, a scan of the stack for the first return address into the program; the return address of frame 0 |
| `src/breakpoint.c` | one table for breakpoints, watchpoints and catchpoints (and internal breakpoints for `next`/`finish`); inserting and removing each kind; taking the `int3` bytes out of a forked child |
| `src/run.c` | continue: resume all threads, stop them all at the first event; breakpoints (rewind `rip`, step over, in every thread that sits on one), watchpoints (DR6), syscall stops (entry/exit), signals (stop or pass on); stops left pending in other threads; forked children detached; `stepi` |
| `tools/sysnames.sh` | generates `src/sysnames.h` from `<asm/unistd_64.h>` |
| `targets/` | programs to debug: `hello`, `exit3`, `crash`, `loop`, `spin`, `data`, `watched`, `files`, `deep`, `ticker`, `waiter`, `fact`, `workers`, `deadlock`, `forker` |
| `tests/unit.c`, `tests/test_*.c` | unit tests for symbols, registers, the dump format, DR7, syscall names, signals, prologues and the line table |
| `tests/itest.sh` | runs scripted sessions and checks the output |

## What changed since Chapter 6

- `proc.c`, `proc.h`: a table of threads (`struct thread`: tid, tdb's
  number, running or stopped, a stop not yet reported). Options
  `PTRACE_O_TRACECLONE` and `PTRACE_O_TRACEFORK`; `waitpid(-1, ..., __WALL)`
  (`__WALL`: threads too; kernels before 4.7 report no thread without
  it, newer ones add it for traced children). `proc_stop_all`,
  `proc_resume`, `proc_resume_all`, `proc_select`; `attach` attaches to each
  thread in `/proc/PID/task`; a `SIGINT` handler for Ctrl-C;
  `proc_map_name`. `proc_continue` and `proc_syscall` are gone.
- `run.c`: all-stop; pending stops; forked children are detached with the
  breakpoints taken out (`bp_remove_copy` in `breakpoint.c`).
- `debugreg.c`: watchpoints are written into every thread, and into each
  new thread (`dr_setup_thread`): the kernel does not copy debug registers
  into a thread made by `clone`.
- `frames.c`: the stack scan for frames in libc.
- `tdb.c`: `info threads`, `thread`, `thread apply`; which thread stopped;
  `?? () from FILE`; Ctrl-C at the prompt prints `Quit`.
- Targets `workers`, `deadlock`, `forker`; the targets are built with
  `-pthread`. The `-v` trace says which thread:
  `[waitpid: LWP N, status 0x....]`.
