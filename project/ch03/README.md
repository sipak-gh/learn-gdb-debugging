# tdb — Chapter 3 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 3 it reads the program's ELF symbol table, so breakpoints
and stop reports use names (`break square`, `Breakpoint 1, 0x... in square ()`),
and it can show and change registers and memory.

## Build

    make            # build/tdb and the demo targets in build/targets/
    make test       # unit tests (build/unit), then integration tests (tests/itest.sh)
    make clean

Needs gcc, GNU make and glibc 2.32 or newer (for `sigabbrev_np`) on Linux x86-64.

## Run

    tdb [-v] PROGRAM [ARGS...]

A location is a number, a symbol, a symbol plus an offset (`main+4`) or a
register (`$rsp`). As in gdb, numbers are addresses in the file before the
program runs and run-time addresses while it runs.

    $ printf 'break square\nrun\nx square 16\nreg rdi 10\ndelete\ncontinue\n' | build/tdb build/targets/loop
    Reading symbols from build/targets/loop: 19 symbols.
    (tdb) break square
    Breakpoint 1 at 0x1149
    (tdb) run
    Started process 465572: build/targets/loop
    Breakpoint 1, 0x0000555555555149 in square ()
    (tdb) x square 16
    0x0000555555555149  cc 0f 1e fa 55 48 89 e5  89 7d fc 8b 45 fc 0f af  |....UH...}..E...|  <square>
    (tdb) reg rdi 10
    rdi = 0xa (10)
    (tdb) delete
    (tdb) continue
    i=0 total=100
    i=1 total=101
    i=2 total=105
    i=3 total=114
    i=4 total=130
    [process exited with status 0]
    (tdb)

Commands: `run` (`r`), `continue` (`c`), `break LOC` (`b`), `tbreak LOC`,
`delete [N]` (`d`), `ignore N COUNT`, `info breakpoints` (`info b`),
`info symbol LOC`, `regs` (`info registers`), `reg NAME [VALUE]`,
`x LOC [COUNT]`, `print SYMBOL` (`p`), `set SYMBOL VALUE`, `kill`, `help`,
`quit` (`q`).

## Files

| File | What it does |
|---|---|
| `src/tdb.c` | `main`, the command loop, reporting stops, the commands |
| `src/proc.c` | `fork` + `PTRACE_TRACEME` + `execv`; continue, single-step, registers, memory |
| `src/elf.c` | the load address: ELF header `e_entry` vs `AT_ENTRY` from `/proc/PID/auxv` |
| `src/symbols.c` | reads `.symtab` from the ELF file; name ↔ address; parses `main+4` |
| `src/regs.c` | register names → offsets in `struct user_regs_struct`; the flags in `eflags` |
| `src/mem.c` | one line of a hex dump |
| `src/breakpoint.c` | the breakpoint table; writing and removing `0xCC` |
| `src/run.c` | continue with breakpoints: rewind `rip`, step over, `ignore` counts |
| `targets/` | programs to debug: `hello`, `exit3`, `crash`, `loop`, `spin`, `data` |
| `tests/unit.c`, `tests/test_*.c` | unit tests for symbols, registers and the dump format |
| `tests/itest.sh` | runs scripted sessions and checks the output |

## What changed since Chapter 2

- New: `src/symbols.c`, `src/regs.c`, `src/mem.c`; target `data`; unit tests
  (`tests/test.h`, `tests/tests.h`, `tests/unit.c`, `tests/test_*.c`).
- `proc.c`: `proc_read`/`proc_write` for any number of bytes at any alignment.
- `tdb.c`: locations by name; `regs`, `reg`, `x`, `print`, `set`, `info symbol`;
  stop reports name the function.
- `breakpoint.c`: `info breakpoints` shows the symbol.
