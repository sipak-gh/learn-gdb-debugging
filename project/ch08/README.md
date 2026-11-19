# tdb — Chapter 8 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 8 it can be programmed. A breakpoint can carry
`commands` that run when it stops the program (`silent` first: no report),
`define` makes new commands with arguments (`$arg0`, `$argc`), `source FILE`
runs a command file, and `tdb -batch -x FILE -ex CMD` runs scripts without
a prompt and exits with status 1 if the last command failed. Commands may be
shortened to any unique start, an empty line repeats `next` and its friends,
`run ARGS` passes arguments, and `~/.tdbinit` is read at start-up. On a
terminal the prompt uses GNU readline (line editing, history) if it was
built in.

## Build

    make            # build/tdb and the demo targets in build/targets/
    make test       # unit tests (build/unit), then integration tests (tests/itest.sh)
    make READLINE=no   # without GNU readline (default: with it, if <readline/readline.h> exists)
    make clean

Needs gcc, GNU make and glibc 2.32 or newer (for `sigabbrev_np` and
`strerrorname_np`) on Linux x86-64; GNU readline (`libreadline-dev`) is
optional. `src/sysnames.h` is generated from the kernel headers by
`tools/sysnames.sh`; it is checked in, so the build does not need the
script. The targets are built with `-g -O0`; gcc 11 writes DWARF 5, the
only version tdb reads (use `-gdwarf-5` with older compilers).

## Run

    tdb [OPTIONS] PROGRAM [ARGS...]
    tdb [OPTIONS] -p PID

    options:
      -x FILE     run the commands in FILE
      -ex COMMAND run COMMAND (-x and -ex run in the order given)
      -batch      exit after them, with status 1 if the last one failed
      -nx         do not read ~/.tdbinit
      -v          show every waitpid() status

A breakpoint that prints a register and goes on, the way `dprintf` would:

    $ printf 'break add\ncommands\nsilent\nreg rdi\ncontinue\nend\nrun 10 20 30\nshow args\ninfo b\n' | build/tdb build/targets/sum
    Reading symbols from build/targets/sum: 21 symbols, 15 line table rows.
    (tdb) break add
    Breakpoint 1 at 0x1175: file targets/sum.c, line 14.
    (tdb) commands
    Type commands for breakpoint(s) 1, one per line.
    End with a line saying just "end".
    >silent
    >reg rdi
    >continue
    >end
    (tdb) run 10 20 30
    Started process 1021640: build/targets/sum
    rdi = 0xa (10)
    rdi = 0x14 (20)
    rdi = 0x1e (30)
    sum = 60
    [process exited with status 0]
    (tdb) show args
    Argument list to give program being debugged when it is started is "10 20 30".
    (tdb) info b
    Num  Type        Disp  Address  In memory           Hits  What
    1    breakpoint  keep  0x1175   (not running)       3     in add at targets/sum.c:14
            silent
            reg rdi
            continue
    (tdb)

A command of your own, with arguments; an empty line repeats `next`:

    $ printf 'define upto\ntbreak $arg0\nrun $arg1 $arg2\nprint total\nend\nupto add 4 5\nnext\n\n\nupto\nste\n' | build/tdb build/targets/sum
    Reading symbols from build/targets/sum: 21 symbols, 15 line table rows.
    (tdb) define upto
    Type commands for definition of "upto".
    End with a line saying just "end".
    >tbreak $arg0
    >run $arg1 $arg2
    >print total
    >end
    (tdb) upto add 4 5
    Temporary breakpoint 1 at 0x1175: file targets/sum.c, line 14.
    Started process 1021644: build/targets/sum
    Temporary breakpoint 1, add () at targets/sum.c:14
    14	    total += x;
    total = 0 (0x0), 8 bytes at 0x555555558018
    (tdb) next
    15	    calls++;
    (tdb) 
    16	}
    (tdb) 
    main () at targets/sum.c:20
    20	    for (int i = 1; i < argc; i++)
    (tdb) upto
    Missing argument 0 in user function.
    (tdb) ste
    Ambiguous command "ste": step, stepi.
    (tdb)

The same trace as a script, with no prompt:

    $ cat scripts/trace-sum.tdb
    # trace-sum.tdb: show each number add() gets, then the totals.
    #
    #     tdb -batch -x scripts/trace-sum.tdb build/targets/sum 10 20 30
    break add
    commands
      silent
      reg rdi
      continue
    end
    tbreak 22
    run
    print total
    print calls
    kill

    $ build/tdb -batch -x scripts/trace-sum.tdb build/targets/sum 10 20 30
    Breakpoint 1 at 0x1175: file targets/sum.c, line 14.
    Temporary breakpoint 2 at 0x11eb: file targets/sum.c, line 22.
    Started process 1021647: build/targets/sum
    rdi = 0xa (10)
    rdi = 0x14 (20)
    rdi = 0x1e (30)
    Temporary breakpoint 2, main () at targets/sum.c:22
    22	    printf("sum = %ld\n", total);
    total = 60 (0x3c), 8 bytes at 0x555555558018
    calls = 3 (0x3), 4 bytes at 0x555555558020
    [process killed]
    $ echo $?
    0

An error in a command file stops that file (`FILE:LINE: Error in sourced
command file:` and the message), but not the `-x` and `-ex` items after
it. In a `commands` list, the lines after one that lets the program run
(`continue`, `next`, ...) are skipped, as in gdb: the program has moved on.

Commands: `run [ARGS]` (`r`), `continue` (`c`), `break LOC` (`b`),
`tbreak LOC`, `step` (`s`), `next` (`n`), `stepi` (`si`), `finish` (`fin`),
`list [LOC]` (`l`), `watch LOC [LEN]`, `rwatch LOC [LEN]`,
`catch syscall [NAME|NUMBER]`, `delete [N]` (`d`), `ignore N COUNT`,
`commands [N]`, `bt [N]` (`backtrace`),
`handle SIG [stop|nostop] [print|noprint] [pass|nopass]`,
`info signals [SIG]`, `siginfo`, `attach PID`, `detach`,
`info threads`, `thread [N]` (`t`), `thread apply all|N... CMD`,
`info breakpoints` (`info b`, `i b`), `info symbol LOC`,
`regs` (`info registers`), `reg NAME [VALUE]`, `x LOC [COUNT]`,
`print SYMBOL` (`p`), `set SYMBOL VALUE`, `set args ARGS`, `show args`,
`define NAME`, `show user [NAME]`, `source FILE`, `echo TEXT`, `kill`,
`help`, `quit` (`q`). Any unique start of a command works too (`cont`,
`wa`); `ste` is ambiguous (`step, stepi`). An empty line repeats `step`,
`next`, `stepi`, `continue`, `finish` and `list`. A line starting with `#`
is a comment.

Limits: the arguments of `run` are split at spaces, with no quotes and no
redirection (gdb hands them to a shell). There is no `if` or `while`, no
convenience variables and no expressions: `define` and `commands` run
plain lists of commands. User commands may call each other 64 deep.
Breakpoint commands run when the command that let the program run has
finished, so a `continue` inside them loops without growing tdb's stack.
Only `~/.tdbinit` is read, never one in the current directory (gdb reads
`./.gdbinit` only from directories in its auto-load safe-path). Ctrl-C at
the prompt still reaches the program too, as in Chapter 7.

## Files

| File | What it does |
|---|---|
| `src/tdb.c` | `main` and its options; the command table and the command loop (abbreviations, an empty line repeats); reporting stops (and which thread); the commands; breakpoint `commands`, `define`, `source`, `echo`, `~/.tdbinit` |
| `src/cli.c` | pure functions for the command language: look a word up in a command table (exact name, else a unique start), the list of matches for `Ambiguous command`, `$arg0`/`$argc` expansion, `echo` escapes |
| `src/input.c` | where the next command line comes from: a stack of `source` files, else stdin (with GNU readline on a terminal, if it was built in); `FILE:LINE` for error messages; Ctrl-C at the prompt |
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
| `src/breakpoint.c` | one table for breakpoints, watchpoints and catchpoints (and internal breakpoints for `next`/`finish`); inserting and removing each kind; taking the `int3` bytes out of a forked child; each one's `commands` |
| `src/run.c` | continue: resume all threads, stop them all at the first event; breakpoints (rewind `rip`, step over, in every thread that sits on one), watchpoints (DR6), syscall stops (entry/exit), signals (stop or pass on); stops left pending in other threads; forked children detached; `stepi` |
| `tools/sysnames.sh` | generates `src/sysnames.h` from `<asm/unistd_64.h>` |
| `targets/` | programs to debug: `hello`, `exit3`, `crash`, `loop`, `spin`, `data`, `watched`, `files`, `deep`, `ticker`, `waiter`, `fact`, `workers`, `deadlock`, `forker`, `sum` |
| `scripts/trace-sum.tdb` | a command file for `tdb -batch -x` |
| `tests/unit.c`, `tests/test_*.c` | unit tests for symbols, registers, the dump format, DR7, syscall names, signals, prologues, the line table, and command lookup and `$arg` expansion |
| `tests/itest.sh` | runs scripted sessions and checks the output |

## What changed since Chapter 7

- `cli.c`, `cli.h` (new): `cli_lookup` (an exact name wins; else one
  unique start, where a command and its alias count once), `cli_matches`,
  `cli_expand` (`$argc`, `$arg0`...`$arg9`), `cli_unescape`.
- `input.c`, `input.h` (new): the stack of `source` files and stdin; GNU
  readline when stdin is a terminal and `TDB_READLINE` is defined, with
  tdb, not readline, handling `SIGINT`; `input_where` for error messages.
- `tdb.c`: the command table replaces the chain of `strcmp`s; gdb's
  messages for unknown and ambiguous commands; `error()` (prefixes the
  file and line inside a command file, and marks the command failed);
  repeat on an empty line; `run ARGS`, `set args`, `show args`;
  `commands`, `define`, `show user`, `source`, `echo`; options `-x`, `-ex`,
  `-batch`, `-nx`; `~/.tdbinit`.
- `breakpoint.c`, `breakpoint.h`: `commands` and `silent` in each
  breakpoint; `info breakpoints` lists the commands.
- `proc.c`: `proc_take_ctrl_c`, for the prompt.
- `Makefile`: `READLINE=yes|no`; target `sum`.
