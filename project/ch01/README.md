# tdb — Chapter 1 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 1 it can start a program under its control, let it run,
and say how it stopped or ended: a signal, an exit status, or death by a signal.

## Build

    make            # build/tdb and the demo targets in build/targets/
    make test       # integration tests (tests/itest.sh)
    make clean

Needs gcc, GNU make and glibc 2.32 or newer (for `sigabbrev_np`) on Linux x86-64.

## Run

    tdb [-v] PROGRAM [ARGS...]

Commands (one per line on stdin): `run` (`r`), `continue` (`c`), `kill`,
`help`, `quit` (`q`). `-v` prints every raw `waitpid()` status.

    $ printf 'run\ncontinue\n' | build/tdb build/targets/crash
    (tdb) run
    Started process 408560: build/targets/crash
    about to crash
    Program received signal SIGSEGV (Segmentation fault).
    (tdb) continue
    [process killed by SIGSEGV (Segmentation fault)]
    (tdb)

## Files

| File | What it does |
|---|---|
| `src/tdb.c` | `main`, the command loop, reporting stops |
| `src/proc.c` | `fork` + `PTRACE_TRACEME` + `execv`, `PTRACE_CONT`, `waitpid` |
| `targets/` | small programs to debug: `hello`, `exit3`, `crash` |
| `tests/itest.sh` | runs scripted sessions and checks the output |

## What changed

First version.
