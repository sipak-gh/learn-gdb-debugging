# tdb — Chapter 5 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 5 it can explain a crash: a backtrace (`bt`) that
follows the chain of saved frame pointers, the details of the signal
(`siginfo`, from `PTRACE_GETSIGINFO`), a gdb-style signal table (`handle`,
`info signals`), and it can attach to a running process (`tdb -p PID`,
`attach`, `detach`).

## Build

    make            # build/tdb and the demo targets in build/targets/
    make test       # unit tests (build/unit), then integration tests (tests/itest.sh)
    make clean

Needs gcc, GNU make and glibc 2.32 or newer (for `sigabbrev_np` and
`strerrorname_np`) on Linux x86-64. `src/sysnames.h` is generated from the
kernel headers by `tools/sysnames.sh`; it is checked in, so the build does
not need the script.

## Run

    tdb [-v] PROGRAM [ARGS...]
    tdb [-v] -p PID

A location is a number, a symbol, a symbol plus an offset (`main+4`) or a
register (`$rsp`). As in gdb, numbers are addresses in the file before the
program runs and run-time addresses while it runs.

    $ printf 'run\nbt\nsiginfo\n' | build/tdb build/targets/deep
    Reading symbols from build/targets/deep: 21 symbols.
    (tdb) run
    Started process 542810: build/targets/deep
    Program received signal SIGSEGV (Segmentation fault).
    Fault address 0x0: SEGV_MAPERR (address not mapped to object).
    0x00007ffff7d9d8bd in ?? ()
    (tdb) bt
    #0  0x00007ffff7d9d8bd in ?? ()
    #1  0x00005555555551a3 in middle+28 ()
    #2  0x00005555555551c5 in outer+28 ()
    #3  0x0000555555555202 in main+56 ()
    (tdb) siginfo
    si_signo = 11 (SIGSEGV)
    si_code  = 1: SEGV_MAPERR (address not mapped to object)
    si_addr  = 0x0
    (tdb)

`deep` calls `main → outer → middle → inner → strlen(NULL)`. Frame #0 is
`strlen` inside libc, which has no `.symtab`, hence `?? ()`. `inner` is
missing: `strlen` does not save `%rbp`, so the walk goes from `strlen`
straight to the frame of `inner`'s caller. gdb uses the unwind tables in
`.eh_frame` and shows every frame; that is an exercise.

    $ printf 'handle SIGALRM print\ninfo signals SIGUSR1\nrun\ncontinue\n' | build/tdb build/targets/ticker
    Reading symbols from build/targets/ticker: 21 symbols.
    (tdb) handle SIGALRM print
    Signal     Stop  Print  Pass  Description
    SIGALRM    No    Yes    Yes   Alarm clock
    (tdb) info signals SIGUSR1
    Signal     Stop  Print  Pass  Description
    SIGUSR1    Yes   Yes    Yes   User defined signal 1
    (tdb) run
    Started process 542814: build/targets/ticker
    Program received signal SIGALRM (Alarm clock).
    Program received signal SIGALRM (Alarm clock).
    Program received signal SIGALRM (Alarm clock).
    Program received signal SIGALRM (Alarm clock).
    Program received signal SIGALRM (Alarm clock).
    Program received signal SIGUSR1 (User defined signal 1).
    0x00007ffff7c969bc in ?? ()
    (tdb) continue
    got SIGUSR1
    ticks=5
    [process exited with status 0]
    (tdb)

The defaults are gdb's: `SIGALRM`, `SIGCHLD`, `SIGWINCH` and a few others
pass to the program without stopping; most signals stop, print and pass;
`SIGINT` and `SIGTRAP` are not passed. `handle SIGUSR1 nopass` typed at the
stop means the program never sees the signal.

To attach, the process must let you. With
`/proc/sys/kernel/yama/ptrace_scope` set to 1 (Ubuntu's default) only a
parent may trace a process, unless the process calls
`prctl(PR_SET_PTRACER, ...)`; `targets/waiter` does that:

    $ build/targets/waiter > /dev/null &
    $ printf 'break work\ncontinue\nbt\nprint rounds\ndetach\nquit\n' | build/tdb -p $!
    Reading symbols from /home/you/learn-gdb-debugging/project/ch05/build/targets/waiter: 21 symbols.
    Attached to process 542906.
    0x000079ff0aae574a in ?? ()
    (tdb) break work
    Breakpoint 1 at 0x55b34b8a61e9
    (tdb) continue
    Breakpoint 1, 0x000055b34b8a61e9 in work ()
    (tdb) bt
    #0  0x000055b34b8a61e9 in work ()
    #1  0x000055b34b8a62ab in main+93 ()
    (tdb) print rounds
    rounds = 267 (0x10b), 8 bytes at 0x55b34b8a9020
    (tdb) detach
    Detached from process 542906.
    (tdb) quit

The process was started outside tdb, with address randomization on, so the
addresses are not the `0x5555...` ones of a program that tdb runs. tdb finds
the program file through `/proc/PID/exe`.

`detach` removes the breakpoints and lets the process run on; `quit` detaches
too (it kills only a program that tdb started).

Commands: `run` (`r`), `continue` (`c`), `break LOC` (`b`), `tbreak LOC`,
`watch LOC [LEN]`, `rwatch LOC [LEN]`, `catch syscall [NAME|NUMBER]`,
`delete [N]` (`d`), `ignore N COUNT`, `bt [N]` (`backtrace`),
`handle SIG [stop|nostop] [print|noprint] [pass|nopass]`,
`info signals [SIG]`, `siginfo`, `attach PID`, `detach`,
`info breakpoints` (`info b`), `info symbol LOC`, `regs` (`info registers`),
`reg NAME [VALUE]`, `x LOC [COUNT]`, `print SYMBOL` (`p`),
`set SYMBOL VALUE`, `kill`, `help`, `quit` (`q`).

## Files

| File | What it does |
|---|---|
| `src/tdb.c` | `main`, the command loop, reporting stops, the commands |
| `src/proc.c` | `fork` + `PTRACE_TRACEME` + `execv`, or `PTRACE_ATTACH`; continue, `PTRACE_SYSCALL`, single-step, registers, memory, siginfo, detach |
| `src/elf.c` | the load address: ELF header `e_entry` vs `AT_ENTRY` from `/proc/PID/auxv` |
| `src/symbols.c` | reads `.symtab` from the ELF file; name ↔ address; parses `main+4` |
| `src/regs.c` | register names → offsets in `struct user_regs_struct`; the flags in `eflags` |
| `src/mem.c` | one line of a hex dump |
| `src/debugreg.c` | DR7 encoding; reading and writing DR0–DR7 with `PTRACE_PEEKUSER`/`POKEUSER` |
| `src/syscalls.c`, `src/sysnames.h` | system call number ↔ name |
| `src/signals.c` | signal names; the stop/print/pass table; `si_code` texts |
| `src/frames.c` | the backtrace: prologue check, then the chain of saved `%rbp` |
| `src/breakpoint.c` | one table for breakpoints, watchpoints and catchpoints; inserting and removing each kind |
| `src/run.c` | continue: breakpoints (rewind `rip`, step over), watchpoints (DR6), syscall stops (entry/exit), signals (stop or pass on) |
| `tools/sysnames.sh` | generates `src/sysnames.h` from `<asm/unistd_64.h>` |
| `targets/` | programs to debug: `hello`, `exit3`, `crash`, `loop`, `spin`, `data`, `watched`, `files`, `deep`, `ticker`, `waiter` |
| `tests/unit.c`, `tests/test_*.c` | unit tests for symbols, registers, the dump format, DR7, syscall names, signals and prologues |
| `tests/itest.sh` | runs scripted sessions and checks the output |

## What changed since Chapter 4

- New: `src/signals.c` (with `sig_name`, moved from `proc.c`),
  `src/frames.c`; targets `deep`, `ticker`, `waiter`; `tests/test_signals.c`.
- `proc.c`: `proc_attach` (`PTRACE_ATTACH`, wait for its `SIGSTOP`, no
  `PTRACE_O_EXITKILL`), `proc_detach`, `proc_siginfo`.
- `run.c`: a signal stop follows the `handle` table; a signal that does not
  stop is printed (or not) and passed (or not) without returning to the
  prompt. The `pass` setting is checked again when the program resumes.
- `breakpoint.c`: `bp_remove_all`, for `detach`.
- `tdb.c`: `bt`, `handle`, `info signals`, `siginfo`, `attach`, `detach`,
  `tdb -p PID`; a `SIGSEGV`/`SIGBUS`/`SIGFPE`/`SIGILL` stop shows the
  fault address; `quit` detaches from a process it attached to.
- `Makefile`: `LIB_OBJS` lists the objects shared by `tdb` and the unit
  tests.
