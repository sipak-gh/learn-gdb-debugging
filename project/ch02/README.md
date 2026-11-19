# tdb — Chapter 2 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 2 it has software breakpoints: it writes an `int3`
(`0xCC`) over the first byte of an instruction, recognises the `SIGTRAP` when
the program executes it, moves `rip` back, and steps over the original
instruction when the program goes on.

## Build

    make            # build/tdb and the demo targets in build/targets/
    make test       # integration tests (tests/itest.sh)
    make clean

Needs gcc, GNU make and glibc 2.32 or newer (for `sigabbrev_np`) on Linux x86-64.

## Run

    tdb [-v] PROGRAM [ARGS...]

Breakpoint addresses are addresses in the file, as `nm` or `objdump -d`
print them; tdb adds the load address of a PIE.

    $ nm build/targets/loop | grep square
    0000000000001149 T square
    $ printf 'break 0x1149\nrun\ncontinue\ninfo breakpoints\n' | build/tdb build/targets/loop
    (tdb) break 0x1149
    Breakpoint 1 at 0x1149
    (tdb) run
    Started process 416715: build/targets/loop
    Breakpoint 1, 0x0000555555555149
    (tdb) continue
    Breakpoint 1, 0x0000555555555149
    (tdb) info breakpoints
    Num  Disp  Address  In memory           Hits
    1    keep  0x1149   0x0000555555555149  2
    (tdb)

Commands: `run` (`r`), `continue` (`c`), `break ADDR` (`b`), `tbreak ADDR`,
`delete [N]` (`d`), `ignore N COUNT`, `info breakpoints` (`info b`), `kill`,
`help`, `quit` (`q`).

## Files

| File | What it does |
|---|---|
| `src/tdb.c` | `main`, the command loop, reporting stops |
| `src/proc.c` | `fork` + `PTRACE_TRACEME` + `execv`; continue, single-step, registers, memory words |
| `src/elf.c` | the load address: ELF header `e_entry` vs `AT_ENTRY` from `/proc/PID/auxv` |
| `src/breakpoint.c` | the breakpoint table; writing and removing `0xCC` |
| `src/run.c` | continue with breakpoints: rewind `rip`, step over, `ignore` counts |
| `targets/` | programs to debug: `hello`, `exit3`, `crash`, `loop`, `spin` |
| `tests/itest.sh` | runs scripted sessions and checks the output |

## What changed since Chapter 1

- New: `src/breakpoint.c`, `src/run.c`, `src/elf.c`; targets `loop` and `spin`.
- `proc.c`: `proc_step`, `proc_get_regs`/`proc_set_regs`, `proc_peek`/`proc_poke`,
  and the load address after launch.
- `tdb.c`: `break`, `tbreak`, `delete`, `ignore`, `info breakpoints`; `run`
  inserts the breakpoints once the program is in memory.
