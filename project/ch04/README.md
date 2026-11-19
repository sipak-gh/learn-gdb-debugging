# tdb — Chapter 4 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 4 it has hardware watchpoints (`watch`, `rwatch`), which
use the CPU's debug registers DR0–DR3, DR6 and DR7, and system call
catchpoints (`catch syscall`), which use `PTRACE_SYSCALL`.

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

A location is a number, a symbol, a symbol plus an offset (`main+4`) or a
register (`$rsp`). As in gdb, numbers are addresses in the file before the
program runs and run-time addresses while it runs.

    $ printf 'watch counter\nrwatch limit\nrun\ncontinue\ncontinue\ncontinue\n' | build/tdb build/targets/watched
    Reading symbols from build/targets/watched: 21 symbols.
    (tdb) watch counter
    Hardware watchpoint 1: counter (4 bytes at 0x4018, DR0)
    (tdb) rwatch limit
    Hardware read watchpoint 2: limit (4 bytes at 0x4010, DR1)
    (tdb) run
    Started process 515230: build/targets/watched
    Hardware watchpoint 1: counter
    Old value = 0 (0x0)
    New value = 2 (0x2)
    0x000055555555516f in step+38 ()
    (tdb) continue
    Hardware watchpoint 1: counter
    Old value = 2 (0x2)
    New value = 6 (0x6)
    0x000055555555516f in step+38 ()
    (tdb) continue
    Hardware read watchpoint 2: limit
    Value = 3 (0x3)
    0x00005555555551a1 in main+47 ()
    (tdb) continue
    counter=6 limit=3
    [process exited with status 0]
    (tdb)

`step(0)` writes `counter` without changing it, so tdb, like gdb, does not
stop there. The reported address is the instruction *after* the store: the
CPU reports a data breakpoint once the instruction has finished.

    $ printf 'catch syscall write\nrun\ncontinue\ncontinue\n' | build/tdb build/targets/files
    Reading symbols from build/targets/files: 18 symbols.
    (tdb) catch syscall write
    Catchpoint 1 (syscall 'write' [1])
    (tdb) run
    Started process 515234: build/targets/files
    Catchpoint 1 (call to syscall write), 0x00007ffff7d149d7 in ?? ()
        write(0x1, 0x7fffffffe5d0, 0x1c)
    (tdb) continue
    no settings, using defaults
    Catchpoint 1 (returned from syscall write), 0x00007ffff7d149d7 in ?? ()
        = 28 (0x1c)
    (tdb) continue
    [process exited with status 0]
    (tdb)

The stops are inside libc, which has no `.symtab`, hence `?? ()`.

Commands: `run` (`r`), `continue` (`c`), `break LOC` (`b`), `tbreak LOC`,
`watch LOC [LEN]`, `rwatch LOC [LEN]`, `catch syscall [NAME|NUMBER]`,
`delete [N]` (`d`), `ignore N COUNT`, `info breakpoints` (`info b`),
`info symbol LOC`, `regs` (`info registers`), `reg NAME [VALUE]`,
`x LOC [COUNT]`, `print SYMBOL` (`p`), `set SYMBOL VALUE`, `kill`, `help`,
`quit` (`q`).

## Files

| File | What it does |
|---|---|
| `src/tdb.c` | `main`, the command loop, reporting stops, the commands |
| `src/proc.c` | `fork` + `PTRACE_TRACEME` + `execv`; continue, `PTRACE_SYSCALL`, single-step, registers, memory |
| `src/elf.c` | the load address: ELF header `e_entry` vs `AT_ENTRY` from `/proc/PID/auxv` |
| `src/symbols.c` | reads `.symtab` from the ELF file; name ↔ address; parses `main+4` |
| `src/regs.c` | register names → offsets in `struct user_regs_struct`; the flags in `eflags` |
| `src/mem.c` | one line of a hex dump |
| `src/debugreg.c` | DR7 encoding; reading and writing DR0–DR7 with `PTRACE_PEEKUSER`/`POKEUSER` |
| `src/syscalls.c`, `src/sysnames.h` | system call number ↔ name |
| `src/breakpoint.c` | one table for breakpoints, watchpoints and catchpoints; inserting and removing each kind |
| `src/run.c` | continue: breakpoints (rewind `rip`, step over), watchpoints (DR6), syscall stops (entry/exit) |
| `tools/sysnames.sh` | generates `src/sysnames.h` from `<asm/unistd_64.h>` |
| `targets/` | programs to debug: `hello`, `exit3`, `crash`, `loop`, `spin`, `data`, `watched`, `files` |
| `tests/unit.c`, `tests/test_*.c` | unit tests for symbols, registers, the dump format, DR7 and syscall names |
| `tests/itest.sh` | runs scripted sessions and checks the output |

## What changed since Chapter 3

- New: `src/debugreg.c`, `src/syscalls.c`, `src/sysnames.h`,
  `tools/sysnames.sh`; targets `watched` and `files`; `tests/test_debugreg.c`.
- `breakpoint.c`: one table with a type (`BP_BREAK`, `BP_WATCH`, `BP_RWATCH`,
  `BP_SYSCALL`); `info breakpoints` has a Type column and lists by number.
- `proc.c`: `proc_syscall`; `PTRACE_O_TRACESYSGOOD` so syscall stops show as
  `SIGTRAP | 0x80`; `in_syscall` tells entry from exit.
- `run.c`: new stop kinds `STOP_WATCH` and `STOP_SYSCALL`; continue resumes
  with `PTRACE_SYSCALL` while any catchpoint exists.
- `tdb.c`: `watch`, `rwatch`, `catch syscall`; their stop reports.
