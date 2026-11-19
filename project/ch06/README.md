# tdb — Chapter 6 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 6 it works with source lines: it reads the DWARF 5 line
table (`.debug_line`) of the program, so it can show where the program
stopped as `FILE:LINE`, put breakpoints on lines (`break fact.c:9`), list the
source (`list`), and step by lines (`step`, `next`), by instructions
(`stepi`), or out of a function (`finish`).

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

A location is a line (`9`), a file and a line (`fact.c:9`), a function
(`fact`), or an address: `*main`, `*main+4`, `*0x1166`, a register
(`$rsp`). `break FUNCTION` puts the breakpoint after the function's prologue,
at its first line, as gdb does; `break *FUNCTION` puts it at the first
instruction. A line without code (an empty line, a comment) moves to the next
line that has some.

    $ printf 'list fact\nbreak fact.c:9\nrun\nbt\ndelete\nfinish\nnext\nnext\nstep\nstep\nnext\nquit\n' | build/tdb build/targets/fact
    Reading symbols from build/targets/fact: 20 symbols, 22 line table rows.
    (tdb) list fact
    1	/* fact.c - a recursive function, a function that returns nothing, and a
    2	 * loop. A target for step, next and finish. */
    3	#include <stdio.h>
    4	
    5	long fact(int n)
    6	{
    7	    if (n <= 1)
    8	        return 1;
    9	    return n * fact(n - 1);
    10	}
    (tdb) break fact.c:9
    Breakpoint 1 at 0x1166: file targets/fact.c, line 9.
    (tdb) run
    Started process 643983: build/targets/fact
    Breakpoint 1, fact () at targets/fact.c:9
    9	    return n * fact(n - 1);
    (tdb) bt
    #0  fact () at targets/fact.c:9
    #1  0x00005555555551cc in main () at targets/fact.c:19
    (tdb) delete
    (tdb) finish
    Run till exit from #0  fact () at targets/fact.c:9
    0x00005555555551cc in main () at targets/fact.c:19
    19	    long f = fact(5);
    Value returned in rax: 120 (0x78)
    (tdb) next
    20	    show(5, f);
    (tdb) next
    21	    for (int i = 0; i < 3; i++)
    (tdb) step
    22	        f += i;
    (tdb) step
    21	    for (int i = 0; i < 3; i++)
    (tdb) next
    22	        f += i;
    (tdb) quit

`finish` ran the whole recursion, `fact(5)` down to `fact(1)` and back: it
stops when the return address is reached *and* the stack pointer is the
caller's, not the first time a deeper call returns there. It stops in the
middle of line 19 (the result still has to be stored in `f`), hence the
address in front of `main ()`. tdb has no types, so it shows `rax`: for a
`void` function that is garbage, and a `double` comes back in `xmm0`.

`step` goes into a function only if it has line information; `printf` (in
`show`) and the other functions of libc are run as one step. tdb reads only
the program's line table, not the ones of the shared libraries.

    $ printf 'run\nbt\n' | build/tdb build/targets/deep
    Reading symbols from build/targets/deep: 21 symbols, 20 line table rows.
    (tdb) run
    Started process 643173: build/targets/deep
    Program received signal SIGSEGV (Segmentation fault).
    Fault address 0x0: SEGV_MAPERR (address not mapped to object).
    0x00007ffff7d9d8bd in ?? ()
    (tdb) bt
    #0  0x00007ffff7d9d8bd in ?? ()
    #1  0x00005555555551a3 in middle () at targets/deep.c:17
    #2  0x00005555555551c5 in outer () at targets/deep.c:22
    #3  0x0000555555555202 in main () at targets/deep.c:29
    (tdb)

For the frames above #0 the line is the one of the call: tdb looks up the
return address minus 1, since the return address can already belong to the
next line. Frame `inner` is still missing (`strlen` keeps no frame pointer;
see Chapter 5).

Commands: `run` (`r`), `continue` (`c`), `break LOC` (`b`), `tbreak LOC`,
`step` (`s`), `next` (`n`), `stepi` (`si`), `finish` (`fin`),
`list [LOC]` (`l`),
`watch LOC [LEN]`, `rwatch LOC [LEN]`, `catch syscall [NAME|NUMBER]`,
`delete [N]` (`d`), `ignore N COUNT`, `bt [N]` (`backtrace`),
`handle SIG [stop|nostop] [print|noprint] [pass|nopass]`,
`info signals [SIG]`, `siginfo`, `attach PID`, `detach`,
`info breakpoints` (`info b`), `info symbol LOC`, `regs` (`info registers`),
`reg NAME [VALUE]`, `x LOC [COUNT]`, `print SYMBOL` (`p`),
`set SYMBOL VALUE`, `kill`, `help`, `quit` (`q`).

`list` with no argument shows ten lines around `main` (or around the place
where the program stopped), then the next ten each time.

## Files

| File | What it does |
|---|---|
| `src/tdb.c` | `main`, the command loop, reporting stops, the commands |
| `src/proc.c` | `fork` + `PTRACE_TRACEME` + `execv`, or `PTRACE_ATTACH`; continue, `PTRACE_SYSCALL`, single-step, registers, memory, siginfo, detach |
| `src/elf.c` | reading the file; finding a section by name; the load address: ELF header `e_entry` vs `AT_ENTRY` from `/proc/PID/auxv` |
| `src/lines.c` | the DWARF 5 line table: decodes the `.debug_line` program into rows (address, file, line, is_stmt); address → line, line → address, end of the prologue |
| `src/source.c` | prints lines of a source file |
| `src/step.c` | `step`, `next`, `finish`: single steps, calls run to their return address, prologues skipped |
| `src/symbols.c` | reads `.symtab` from the ELF file; name ↔ address; parses `main+4` |
| `src/regs.c` | register names → offsets in `struct user_regs_struct`; the flags in `eflags` |
| `src/mem.c` | one line of a hex dump |
| `src/debugreg.c` | DR7 encoding; reading and writing DR0–DR7 with `PTRACE_PEEKUSER`/`POKEUSER` |
| `src/syscalls.c`, `src/sysnames.h` | system call number ↔ name |
| `src/signals.c` | signal names; the stop/print/pass table; `si_code` texts |
| `src/frames.c` | the backtrace: prologue check, then the chain of saved `%rbp`; the return address of frame 0 |
| `src/breakpoint.c` | one table for breakpoints, watchpoints and catchpoints (and internal breakpoints for `next`/`finish`); inserting and removing each kind |
| `src/run.c` | continue: breakpoints (rewind `rip`, step over), watchpoints (DR6), syscall stops (entry/exit), signals (stop or pass on); `stepi` |
| `tools/sysnames.sh` | generates `src/sysnames.h` from `<asm/unistd_64.h>` |
| `targets/` | programs to debug: `hello`, `exit3`, `crash`, `loop`, `spin`, `data`, `watched`, `files`, `deep`, `ticker`, `waiter`, `fact` |
| `tests/unit.c`, `tests/test_*.c` | unit tests for symbols, registers, the dump format, DR7, syscall names, signals, prologues and the line table |
| `tests/itest.sh` | runs scripted sessions and checks the output |

## What changed since Chapter 5

- New: `src/lines.c` (the line table), `src/source.c`, `src/step.c`; target
  `fact`; `tests/test_lines.c`.
- `elf.c`: `elf_read_file` (moved from `symbols.c`) and `elf_section`, to
  find `.debug_line`, `.debug_line_str` and `.debug_str` by name.
- `breakpoint.c`: internal breakpoints (negative numbers, not listed);
  a bug fixed: a second breakpoint at the same address saved the `0xCC` of
  the first as its "original" byte. Deleting the first put the real byte
  back, so the second never stopped; deleting the second then wrote `0xCC`
  into the code, and the program died of a `SIGTRAP` when it got there.
  `info breakpoints` shows `in FUNCTION at FILE:LINE`.
- `run.c`: stepping over a breakpoint removes all breakpoints at that
  address; `run_stepi` (landing on a breakpoint counts as a hit).
- `frames.c`: `frame_return`, the return address of the current function.
- `tdb.c`: `step`, `next`, `stepi`, `finish`, `list`; locations
  `LINE`, `FILE:LINE`, `*ADDR`; `break FUNCTION` now skips the prologue (the
  older tests use `break *FUNCTION`); stops and `bt` show `FILE:LINE` and the
  source line.
