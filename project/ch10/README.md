# tdb 1.0 — Chapter 10 snapshot

`tdb` is a tiny debugger for Linux x86-64, written in C11 on top of `ptrace(2)`.
At the end of Chapter 10 it can also be a gdbserver. `gdbserver PORT`
waits for gdb on 127.0.0.1:PORT; there, `target remote :PORT` connects,
and from then on gdb drives the program through tdb. gdb sends packets of
the GDB Remote Serial Protocol ("g": send me the registers, "m": read
memory, "Z0": put a breakpoint here, "c": continue), and tdb answers each
with the functions it already has: `proc_get_regs`, `bp_read_code`,
`bp_add_internal`, `run_continue`. gdb's watchpoints use tdb's debug
registers; if tdb was recording, gdb's `reverse-continue` and
`reverse-stepi` use tdb's record. When gdb detaches, the program is
tdb's again.

This is the last snapshot: tdb 1.0. It runs programs, stops them at
breakpoints, watchpoints, catchpoints and signals, steps by line and by
instruction, reads DWARF line tables and ELF symbols, walks the stack,
debugs threads, takes checkpoints, records and replays, and serves gdb.

## Build

    make            # build/tdb and the demo targets in build/targets/
    make test       # unit tests (build/unit), then integration tests (tests/itest.sh;
                    # the gdbserver ones need gdb, and are skipped without it)
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

Start the program under tdb and let gdb in (two terminals, or tdb in
the background):

    $ build/tdb build/targets/sum 10 20
    Reading symbols from build/targets/sum: 21 symbols, 15 line table rows.
    (tdb) gdbserver 1234
    Process build/targets/sum created; pid = 1294803
    Listening on port 1234

    $ gdb -q build/targets/sum
    (gdb) target remote :1234
    Reading /lib64/ld-linux-x86-64.so.2 from remote target...
    warning: File transfers from remote targets can be slow. Use "set sysroot" to access files locally instead.
    Reading /lib64/ld-linux-x86-64.so.2 from remote target...
    Reading /usr/lib/debug/.build-id/db/469fb3d3d1f916c08281b31108a5038f87d15b.debug from remote target...
    0x00007ffff7fe3290 in _start () from target:/lib64/ld-linux-x86-64.so.2
    (gdb) break add
    Breakpoint 1 at 0x555555555175: file targets/sum.c, line 14.
    (gdb) continue
    Reading /lib/x86_64-linux-gnu/libc.so.6 from remote target...
    Breakpoint 1, add (x=10) at targets/sum.c:14
    14	    total += x;
    (gdb) bt
    #0  add (x=10) at targets/sum.c:14
    #1  0x00005555555551df in main (argc=3, argv=0x7fffffffe708) at targets/sum.c:21
    (gdb) watch total
    Hardware watchpoint 2: total
    (gdb) continue
    Hardware watchpoint 2: total

    Old value = 0
    New value = 10
    add (x=10) at targets/sum.c:15
    15	    calls++;
    (gdb) finish
    main (argc=3, argv=0x7fffffffe708) at targets/sum.c:20
    20	    for (int i = 1; i < argc; i++)
    (gdb) set var total = 1000
    (gdb) print $rax = 42
    $1 = 42
    (gdb) detach
    [Inferior 1 (process 1294803) detached]

Back in tdb, the program is where gdb left it, with gdb's changes:

    Remote debugging from host 127.0.0.1, port 35458
    gdb is gone; process 1294803 is tdb's again.
    main () at targets/sum.c:20
    20	    for (int i = 1; i < argc; i++)
    (tdb) reg rax
    rax = 0x2a (42)
    (tdb) print total
    total = 1000 (0x3e8), 8 bytes at 0x555555558018
    (tdb) continue
    sum = 1020
    [process exited with status 0]

`gdbserver --remote-debug PORT` shows the packets (`<-` from gdb, `->`
tdb's answer; long ones cut at 100 characters, bytes that are not text
shown as `.`):

    (tdb) gdbserver --remote-debug 0
    Process build/targets/hello created; pid = 1294896
    Listening on port 41589
    Remote debugging from host 127.0.0.1, port 51636
    [remote] <- qSupported:multiprocess+;swbreak+;hwbreak+;qRelocInsn+;fork-events+;vfork-events+;exec-events+;vCont...
    [remote] -> PacketSize=1000;qXfer:auxv:read+;qXfer:features:read+;qXfer:exec-file:read+;swbreak+;multiprocess+
    [remote] <- vMustReplyEmpty
    [remote] -> 
    [remote] <- Hgp0.0
    [remote] -> OK
    [remote] <- qXfer:features:read:target.xml:0,ffb
    [remote] -> l<?xml version="1.0"?>.<!DOCTYPE target SYSTEM "gdb-target.dtd">.<target>.  <architecture>i386:x86-6...
    ...
    [remote] <- ?
    [remote] -> T05thread:p13c230.13c230;
    [remote] <- qfThreadInfo
    [remote] -> mp13c230.13c230
    [remote] <- qsThreadInfo
    [remote] -> l
    ...
    [remote] <- vFile:pread:0,1000,0
    [remote] -> F800;.ELF..............>.....`.......@........9..........@.8...@.%.}..........@.......@.......@........
    ...
    [remote] <- Hcp13c230.0
    [remote] -> OK
    [remote] <- c
    [remote] -> T05thread:p13c230.13c230;swbreak:;
    [remote] <- g
    [remote] -> 887e55555555000018e1fff7ff7f00007032fcf7ff7f0000487e55555555000000000000000000000030fcf7ff7f000090...
    ...
    [remote] <- c
    hello from the target
    [remote] -> W00
    [process exited with status 0]

`tdb -p PID`, then `gdbserver PORT`, lends gdb a running process the
same way (`gdb -q` with no program file works: tdb tells gdb which file
it is). `PORT` 0 means any free port; tdb prints the one it got.

Chapter 9's time machine is still here. `ledger` reads deposits from a
file, but one account's balance ends up wrong. Where did erin's 182 (`accounts[4].balance`, 8 bytes at
`accounts+64`) come from? Record, run to the end, watch it, and go back:

    $ printf 'break 65\nrun targets/deposits.txt\ndelete\nrecord\nbreak 72\ncontinue\nwatch accounts+64 8\nreverse-continue\nreverse-continue\nreverse-continue\nbt\nreverse-finish\ninfo record\nrecord goto end\n' | build/tdb build/targets/ledger
    Reading symbols from build/targets/ledger: 24 symbols, 74 line table rows.
    (tdb) break 65
    Breakpoint 1 at 0x1467: file targets/ledger.c, line 65.
    (tdb) run targets/deposits.txt
    Started process 1070701: build/targets/ledger
    Breakpoint 1, main () at targets/ledger.c:65
    65	    while (fscanf(f, "%63s %ld", name, &amount) == 2) {
    (tdb) delete
    (tdb) record
    (tdb) break 72
    Breakpoint 2 at 0x55555555548c: file targets/ledger.c, line 72.
    (tdb) continue
    Breakpoint 2, main () at targets/ledger.c:72
    72	    fclose(f);
    (tdb) watch accounts+64 8
    Hardware watchpoint 3: accounts+64 (8 bytes at 0x555555558080, DR0)
    (tdb) reverse-continue
    Hardware watchpoint 3: accounts+64
    Old value = 182 (0xb6)
    New value = 167 (0xa7)
    0x000055555555545c in main () at targets/ledger.c:68
    68	            a->balance += amount;
    (tdb) reverse-continue
    Hardware watchpoint 3: accounts+64
    Old value = 167 (0xa7)
    New value = 107 (0x6b)
    0x000055555555545c in main () at targets/ledger.c:68
    68	            a->balance += amount;
    (tdb) reverse-continue
    Hardware watchpoint 3: accounts+64
    Old value = 107 (0x6b)
    New value = 0 (0x0)
    0x000055555555523b in set_name () at targets/ledger.c:25
    25	        a->name[i] = name[i];
    (tdb) bt
    #0  0x000055555555523b in set_name () at targets/ledger.c:25
    #1  0x0000555555555371 in find () at targets/ledger.c:47
    #2  0x000055555555543f in main () at targets/ledger.c:66
    (tdb) reverse-finish
    Run back to call of #0  0x000055555555523b in set_name () at targets/ledger.c:25
    0x000055555555536c in find () at targets/ledger.c:47
    47	    set_name(a, name);
    (tdb) info record
    Active record target: tdb
    Replay mode:
    Lowest recorded instruction number is 1.
    Current instruction number is 6732.
    Highest recorded instruction number is 13635.
    Log contains 13635 instructions.
    Max logged instructions is 200000.
    System calls: 3 (3 emulated in the replay, 0 executed again).
    (tdb) record goto end
    Go forward to insn number 13635
    #0  main () at targets/ledger.c:72
    72	    fclose(f);
    (tdb) 

The first two changes are erin's deposits (`+=` on line 68, 60 and 15);
the third is `set_name()` copying "frederick" into the 8 bytes of
`accounts[3].name`: the `k` (107) lands in erin's balance, before erin
has an account. `info record` counts instructions the way gdb's `record full` does:
this log has the same 13635.

A checkpoint is a copy to come back to. `restart` forks it again, so the
same checkpoint can be used many times:

    $ printf 'break 72\nrun targets/deposits.txt\ncheckpoint\ncontinue\nrestart 1\nnext\ninfo checkpoints\n' | build/tdb build/targets/ledger
    Reading symbols from build/targets/ledger: 24 symbols, 74 line table rows.
    (tdb) break 72
    Breakpoint 1 at 0x148c: file targets/ledger.c, line 72.
    (tdb) run targets/deposits.txt
    Started process 1071428: build/targets/ledger
    Breakpoint 1, main () at targets/ledger.c:72
    72	    fclose(f);
    (tdb) checkpoint
    checkpoint 1: fork returned pid 1071429.
    (tdb) continue
    alice       125
    bob         200
    carol       100
    frederic    300
    erin        182
    deposited 800, held 907
    [process exited with status 1]
    (tdb) restart 1
    Switching to process 1071430
    #0  main () at targets/ledger.c:72
    72	    fclose(f);
    (tdb) next
    73	    for (int i = 0; i < naccounts; i++) {
    (tdb) info checkpoints
      1 process 1071429 at 0x55555555548c, file targets/ledger.c, line 72
    (tdb) 

`todo` crashes in the C library. Record from `main`, let it crash, then
ask who wrote the bad pointer (`cmds[4].arg`, 8 bytes at `cmds+72`):

    $ printf 'break main\nrun targets/plan.txt\nrecord\ncontinue\nwatch cmds+72 8\nreverse-continue\nbt\nreverse-next\nreverse-stepi\nrecord goto begin\n' | build/tdb build/targets/todo
    Reading symbols from build/targets/todo: 26 symbols, 87 line table rows.
    (tdb) break main
    Breakpoint 1 at 0x1539: file targets/todo.c, line 63.
    (tdb) run targets/plan.txt
    Started process 1071433: build/targets/todo
    Breakpoint 1, main () at targets/todo.c:63
    63	    if (argc != 2) {
    (tdb) record
    (tdb) continue
    Program received signal SIGSEGV (Segmentation fault).
    Fault address 0x1: SEGV_MAPERR (address not mapped to object).
    0x00007ffff7d9d8bd in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
    (tdb) watch cmds+72 8
    Hardware watchpoint 2: cmds+72 (8 bytes at 0x555555558888, DR0)
    (tdb) reverse-continue
    Hardware watchpoint 2: cmds+72
    Old value = 1 (0x1)
    New value = 0 (0x0)
    0x00005555555552fe in parse () at targets/todo.c:39
    39	    c->arg = space + 1;
    (tdb) bt
    #0  0x00005555555552fe in parse () at targets/todo.c:39
    #1  0x00005555555555ee in main () at targets/todo.c:73
    (tdb) reverse-next
    39	    c->arg = space + 1;
    (tdb) reverse-stepi
    0x00005555555552ee	38	    char *space = strchr(line, ' ');
    (tdb) record goto begin
    Go backward to insn number 0
    #0  main () at targets/todo.c:63
    63	    if (argc != 2) {
    (tdb) 

Changing memory or registers in the past changes what the program does
next, and the log no longer fits it. tdb notices at the next step and
goes back to the present:

    (tdb) set naccounts 0
    naccounts = 0 (0x0), 4 bytes at 0x5555555580c0
    (tdb) next
    tdb: the replay went another way at instruction 1205 (another address); back to the present.

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
`define NAME`, `show user [NAME]`, `source FILE`, `echo TEXT`,
`checkpoint`, `restart N`, `info checkpoints`, `delete checkpoint N`,
`record` (`rec`, `record full`), `record stop`,
`record goto begin|end|N`, `info record`, `reverse-stepi` (`rsi`),
`reverse-next` (`rn`), `reverse-continue` (`rc`), `reverse-finish`,
`gdbserver [--remote-debug] PORT`, `kill`,
`help`, `quit` (`q`). Any unique start of a command works too (`cont`,
`wa`); `ste` is ambiguous (`step, stepi`). An empty line repeats `step`,
`next`, `stepi`, `continue`, `finish`, `list` and the `reverse-`
commands. A line starting with `#`
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

Limits of the record: one thread (it stops before `clone` or `fork`:
`record stop` to go on without it); at most 200000 instructions; about
20000 instructions a second, recording or replaying, and every step back
replays from the start of the record, so going back in a long log takes
seconds. A signal is not recorded: the program stops when it gets one
(look back from there), and if it then takes the signal the record is
deleted. Changing memory or registers in the past ends the replay (above).
`rdtsc` and `clock_gettime` through the vDSO (no system call) are not in
the log. There is no `reverse-step`. Checkpoints share open files with
the program, file offsets included: a copy goes on reading where the
program got to, not where the copy was. Take a checkpoint of `ledger` at
line 65, let the program finish, `restart 1`, `continue`: the copy reads
nothing ("deposited 0, held 0"). gdb saves the offsets and puts them back.

gdb reads the program and its libraries through tdb (`vFile:open`,
`vFile:pread`), as it does through gdbserver: that is the "Reading ...
from remote target" (`set sysroot /` in gdb before `target remote` reads
them from gdb's own disk instead). tdb opens files for reading only.

Limits of the gdbserver: one gdb, once (after it is gone, `gdbserver`
again for the next one); 127.0.0.1 only. Ctrl-C in gdb does not stop a
running program (tdb does not read the socket while the program runs);
Ctrl-C in tdb stops `gdbserver` while it waits for gdb. No `G` (gdb
writes registers one at a time with `P`), no hardware breakpoints (`Z1`)
or read watchpoints (`Z3`), no `$_siginfo` (`qXfer:siginfo:read`), no
`monitor` commands (`qRcmd`), no `vCont` (gdb uses `Hc` and `c`/`s`), no
`QStartNoAckMode`, no fork or exec events. gdb's `reverse-continue` and
`reverse-stepi` go to tdb's record, and only if tdb was recording when
gdb connected; otherwise gdb's own `record` works, by single steps,
through tdb. tdb's own breakpoints stay in while gdb is in charge: the
program still stops at each, tdb reports a breakpoint stop, and gdb,
which has no breakpoint there, quietly continues (the hit counts in
`info breakpoints` go up all the same).

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
| `src/run.c` | continue: resume all threads, stop them all at the first event; breakpoints (rewind `rip`, step over, in every thread that sits on one), watchpoints (DR6), syscall stops (entry/exit), signals (stop or pass on); stops left pending in other threads; forked children detached; `stepi`; with the record on, one instruction at a time through `record.c` |
| `src/checkpoint.c` | snapshots: make the program call `clone()` for us (two bytes of `syscall` at `rip`), keep the copy stopped; fork a copy again to restart; switch the debugger to another process |
| `src/record.c` | record and replay: the log (address and call depth of each instruction, results and written memory of system calls); going to instruction N by stepping a fresh copy of the snapshot; `reverse-*` and `record goto`; checking that the replay goes the way the log says |
| `src/rsp.c` | the GDB Remote Serial Protocol, without sockets or ptrace: packets (`$data#checksum`), a reader fed one byte at a time, hex, escapes, the register block of `g`, signal numbers as gdb counts them |
| `src/gdbserver.c` | `gdbserver`: listen on 127.0.0.1, accept gdb, answer its packets with tdb's functions; stop replies; the breakpoints gdb asked for |
| `tools/sysnames.sh` | generates `src/sysnames.h` from `<asm/unistd_64.h>` |
| `targets/` | programs to debug: `hello`, `exit3`, `crash`, `loop`, `spin`, `data`, `watched`, `files`, `deep`, `ticker`, `waiter`, `fact`, `workers`, `deadlock`, `forker`, `sum`, `ledger` (with `deposits.txt`), `todo` (with `plan.txt`) |
| `scripts/trace-sum.tdb` | a command file for `tdb -batch -x` |
| `tests/unit.c`, `tests/test_*.c` | unit tests for symbols, registers, the dump format, DR7, syscall names, signals, prologues, the line table, command lookup and `$arg` expansion, and the packet layer of the remote protocol |
| `tests/itest.sh` | runs scripted sessions and checks the output; for `gdbserver`, real gdb connects to tdb |

## What changed since Chapter 9

- `rsp.c`, `rsp.h` (new): `rsp_checksum`, `rsp_frame`, `rsp_feed` (the
  reader: a packet, an ack `+`, a nak `-`, Ctrl-C `\x03`),
  `rsp_hex_encode`, `rsp_hex_decode`, `rsp_hex_u64`, `rsp_escape`,
  `rsp_regs_encode`, `rsp_reg_set`, `rsp_signal_to_gdb`,
  `rsp_signal_from_gdb`.
- `gdbserver.c`, `gdbserver.h` (new): `gs_serve`. Packets: `?`, `g`, `P`,
  `m`, `M`, `Z0`/`z0`, `Z2`/`z2`, `Z4`/`z4`, `c`, `C`, `s`, `S`, `bc`,
  `bs`, `H`, `T`, `k`, `vKill`, `D`, `qSupported` (with `multiprocess`:
  thread ids `pPID.TID`), `qfThreadInfo`, `qsThreadInfo`, `qC`, `qXfer`
  (`features`, `auxv`, `exec-file`), `qAttached`, `vFile` (`setfs`,
  `open`, `pread`, `close`); the empty answer to all others ("not
  supported"). `TCP_NODELAY` on the socket.
- `tdb.c`: the `gdbserver` command; after gdb is gone, the program is
  tdb's again (detach), or gone (kill, exit).
- `Makefile`: `rsp.o`, `gdbserver.o`, `test_rsp.o`.
- `tests/test_rsp.c` (new); `tests/itest.sh`: `remote` runs tdb's
  `gdbserver 0` and real gdb against it: registers, memory, breakpoints,
  steps, the exit; a watchpoint, changes, detach back to tdb; a crash;
  the packet log; threads; gdb's own `record`; `reverse-continue`
  through tdb's record.

## Earlier: what changed in Chapter 9

- `checkpoint.c`, `checkpoint.h` (new): `ckpt_fork` (inject `clone`),
  `ckpt_snapshot`, `ckpt_kill`; the checkpoint table: `ckpt_add`,
  `ckpt_restart`, `ckpt_delete`, `ckpt_list`.
- `record.c`, `record.h` (new): `rec_start`, `rec_stop`, `rec_exec` (one
  instruction: record it, or replay it in the past), `rec_reverse_stepi`,
  `rec_reverse_next`, `rec_reverse_continue`, `rec_reverse_finish`,
  `rec_goto`, `rec_info`.
- `run.c`: `continue` and `stepi` go through `rec_exec` while the record
  is on (so `next`, `step`, `finish` do too).
- `run.h`: stop reason `STOP_RECORD`, with a `note` to print ("No more
  reverse-execution history.").
- `proc.c`, `proc.h`: `proc_adopt` (debug another, already stopped
  process: a copy), `proc_move`.
- `breakpoint.c`, `breakpoint.h`: `bp_watches` (the watchpoints, for
  `reverse-continue`).
- `step.c`, `step.h`: `is_call` is exported (call depth in the log).
- `tdb.c`: the new commands; `run`, `kill`, `detach` and quitting delete
  the record and the checkpoints; `[Switching to LWP ...]` only for
  programs with threads.
- `Makefile`: `checkpoint.o`, `record.o`; targets `ledger` and `todo`.
- `tests/itest.sh`: checkpoints, reverse watch, a tour of the reverse
  commands, the `todo` crash, the stops at `exit_group` and `clone`,
  divergence, error messages.
