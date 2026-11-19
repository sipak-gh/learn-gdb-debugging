# Appendix A — gdb and tdb Cheat Sheet

Every gdb 12.1 command, setting and option this course uses, grouped by task, next to the tdb
command that does the same job and the chapter that added it. The command-line tools around gdb
(gcc flags, binutils, strace, gdbserver) come after the tables. learn-c's Chapter 10 basics are
assumed and only listed once: `run`, `bt`, `print`, `next`, `step`, `finish`, `watch`,
`display`. gdb accepts any unique prefix of a command (`cont`), and several one-letter
abbreviations: `r`, `c`, `n`, `s`, `b`, `p`, `si`, `ni`, `i` (`info`). tdb has the short names its
`help` shows in parentheses, and unique prefixes from Chapter 8.

Previous: [Chapter 10 — Remote Debugging](10-remote-debugging.md). Next: [Appendix B](appendix-b-ptrace-reference.md).

## Starting and stopping the program

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| start the debugger | `gdb -q PROG`, `gdb -q --args PROG ARGS...` | `tdb PROG ARGS...` (1) |
| run | `run ARGS`, `set args ARGS`, `show args` | `run` (1); `run ARGS`, `set args`, `show args` (8) |
| stop at `main` / at the first instruction | `start`, `starti` | — |
| go on | `continue`, `continue N` (ignore this breakpoint N-1 more times) | `continue` (1) |
| interrupt | Ctrl-C | Ctrl-C (7) |
| how it ended | `print $_exitcode` | printed at the stop (1) |
| the process | `info proc`, `info proc mappings`, `info inferiors`, `info sharedlibrary` | — |
| no ASLR | on by default: `show disable-randomization` | `personality(ADDR_NO_RANDOMIZE)` (1) |
| the shell that starts it | `show startup-with-shell` | none: tdb calls `execv` (1) |
| run a shell command | `shell CMD`, `pipe CMD \| SHELL-CMD` | — |
| end | `kill`, `quit` | `kill`, `quit` (1) |
| show every wait status | — | `tdb -v` (1) |

## Breakpoints (Chapter 2)

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| on a function, line, file:line, address | `break FUNC`, `break 55`, `break ledger.c:55`, `break *ADDR`, `break *main+13` | `break ADDR` (2); `SYMBOL`, `SYMBOL+OFFSET` (3); `LINE`, `FILE:LINE`, `*ADDR`, `*SYMBOL+OFFSET` (6) |
| on every function matching a regex | `rbreak ledger.c:.` | — |
| once | `tbreak LOC` | `tbreak` (2) |
| list, delete, disable | `info breakpoints`, `delete`, `delete 2 4`, `disable N`, `enable N` | `info breakpoints`, `delete [N]` (2) |
| skip hits | `ignore N COUNT` (the hit count is in `info breakpoints`) | `ignore N COUNT` (2) |
| conditions | `break fee if balance < 0`, `condition N EXPR`, `condition N` (removes it), `break *add_size if $rsi == 200000000` | — |
| leave a loop | `until`, `until LINE`, `advance LOC` | — (Chapter 6 exercise) |
| commands at a hit | `commands [N]` ... `silent` ... `continue` ... `end` | `commands [N]`, `silent` (8) |
| print without stopping | `dprintf LOC,"fmt",args` | — |
| save to a file | `save breakpoints FILE`, then `source FILE` | — |
| keep the `int3` bytes in memory | `set breakpoint always-inserted on` | tdb always does |

## Looking at data (Chapter 3)

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| print | `print EXPR`, `print/x`, `/t`, `/c`, `/f`, `/r` (no pretty printer); `set print pretty on`; `set output-radix 16` | `print SYMBOL` (1, 2, 4 or 8 bytes) (3) |
| types | `ptype EXPR`, `ptype/o struct student` (offsets and holes), `whatis EXPR` | — |
| value history | `$`, `$$2`, `$4`; `print $ * 4` | — |
| convenience variables | `set $p = head`, then `while $p` ... `end` | — |
| artificial arrays | `print *p@n`, `print best[0]@n` | — |
| string length in a condition | `$_strlen(head->name)` (not `strlen`, which needs the program) | — |
| change data | `set var x = value`, `print $rax = 42` | `set SYMBOL VALUE` (3) |
| memory | `x/NFU ADDR`: `x/16xb s`, `x/4dw $rdi`, `x/2gx $rbp`, `x/s $rdi`, `x/3i $pc` | `x LOC [COUNT]` (hex dump, 64 bytes by default) (3) |
| registers | `info registers rip rsp`, `info registers eflags`, `info all-registers`, `print $pc`, `print $xmm0.v2_double[0]` | `regs`, `reg NAME [VALUE]` (3) |
| which symbol an address is in | `info symbol ADDR`, `info symbol *(long *)$rsp` | `info symbol LOC` (3) |
| the current frame | `info frame`, `info args`, `info locals` | — |

## Watchpoints and catchpoints (Chapter 4)

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| stop when a value changes | `watch total`, `watch f.window[0]` | `watch LOC [LEN]` (4) |
| the same address, whatever the scope | `watch -l f->alarm_level`, `watch -l *(unsigned long *)($rbp - 8)` | addresses only (4) |
| reads, reads and writes | `rwatch EXPR`, `awatch EXPR` | `rwatch LOC [LEN]` (4) |
| list | `info watchpoints` | `info breakpoints` (4) |
| no hardware watchpoints (slow) | `set can-use-hw-watchpoints 0` | — |
| see the debug registers | `maint set show-debug-regs on` | `info breakpoints` shows the slot (`DR0`) (4) |
| system calls | `catch syscall openat`, `catch syscall group:file` | `catch syscall [NAME\|NR]` (4) |
| other events | `catch fork`, `catch exec`, `catch signal SIGALRM`, `catch throw` | — |

Hardware watchpoints on x86-64: four debug registers, each covering 1, 2, 4 or 8 bytes aligned to
that size. gdb accepts a fifth `watch`, then fails at `continue` with "Could not insert hardware
watchpoint". An expression that can't be watched by address, or `set can-use-hw-watchpoints 0`,
gives a *software* watchpoint: gdb single-steps and is far slower.

## Crashes, signals, attach, cores (Chapter 5)

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| backtrace | `bt`, `bt N`, `backtrace` | `bt [N]` (5) |
| move between frames | `frame N`, `frame function drain`, `up`, `down` | — |
| the signal's details | `print $_siginfo`, `print $_siginfo.si_code`, `print $_siginfo._sifields._sigfault.si_addr` | `siginfo` (5) |
| what to do with a signal | `handle SIGALRM nostop noprint`, `handle SIG stop print`, `info signals [SIG]` | `handle SIG [stop\|nostop] [print\|noprint] [pass\|nopass]`, `info signals [SIG]` (5) |
| go on without the signal / with another | `signal 0`, `signal SIGINT` | `continue` passes it unless `handle SIG nopass` (5) |
| attach | `gdb -q -p PID`, `attach PID`, `detach` | `tdb -p PID`, `attach PID`, `detach` (5) |
| a core file | `gcore FILE`, `gdb -q PROG CORE` | — |
| a sanitizer report | `break __asan_report_store4` (or `_load8`...), then `up` | — |

Cores: `ulimit -c unlimited`; `/proc/sys/kernel/core_pattern` says where they go;
`coredumpctl list` and `coredumpctl gdb` where systemd-coredump collects them. Attaching to a
process you did not start needs `/proc/sys/kernel/yama/ptrace_scope` 0, or the target calling
`prctl(PR_SET_PTRACER, ...)`.

## The machine level (Chapter 6)

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| disassemble | `disassemble /s FUNC` (with source), `disassemble /r START,END` (raw bytes), `x/i $pc` | — |
| syntax | `set disassembly-flavor intel` | — |
| one instruction | `stepi` (`si`), `nexti` (`ni`) | `stepi` (`si`) (6) |
| by line | `step`, `next`, `finish` | `step`, `next`, `finish` (6) |
| show each instruction as you step | `display/i $pc`, `undisplay N` | — |
| source | `list`, `list LOC` | `list [LOC]` (6) |
| where a line's code is | `info line 13`, `info line *$pc`, `info scope FUNC` | — |
| the TUI | `layout src`, `layout asm`, `layout split`, `layout regs`, `focus cmd`/`src`/`asm`, `refresh`, `tui disable`; C-x a toggles, C-x 2 cycles two-window layouts | — |

## Threads and processes (Chapter 7)

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| list, switch | `info threads`, `thread N` | `info threads`, `thread [N]` (7) |
| a command in every thread | `thread apply all bt` | `thread apply all\|N... CMD` (7) |
| only the current thread runs | `set scheduler-locking on` | — |
| other threads keep running | `set non-stop on` (before `run`) | all-stop only |
| forks | `set follow-fork-mode child`, `set detach-on-fork off`, `info inferiors`, `inferior N`, `catch fork` | follows the parent; the child is detached (7) |

Races: `gcc -g -O0 -pthread -fsanitize=thread bank.c -o bank-tsan`, run as `setarch -R
./bank-tsan`, which recent kernels need (Chapter 7).

## Automating gdb (Chapter 8)

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| a new command | `define NAME` ... `end` with `$arg0`..`$arg9`, `$argc`; `document NAME`; `show user NAME` | `define NAME` with `$arg0`.., `$argc`; `show user` (8) |
| run at every stop | `define hook-stop` | — (Chapter 8 exercise) |
| a script | `source FILE` (gdb commands or `.py`) | `source FILE` (8) |
| print text | `echo TEXT\n`, `printf "fmt", args`, `output EXPR` | `echo TEXT` (8) |
| batch runs | `gdb -q -batch -x FILE -ex CMD --args PROG ARGS`; `-return-child-result`; `-nx`; `-iex CMD` (before the program loads) | `tdb -batch -x FILE -ex CMD`, `-nx` (8) |
| startup files | `~/.gdbinit`; a local `.gdbinit` needs `add-auto-load-safe-path DIR` | `~/.tdbinit` (8) |
| logging | `set logging file trace.txt`, `set logging redirect on`, `set logging enabled on` | — |
| history | `set history save on` | GNU readline at the prompt (8) |
| repeat | RET repeats the last command | RET repeats `step`, `next`, `stepi`, `continue`, `finish`, `list` (8) |

Python in gdb (`source inventory.py`): pretty printers are classes with `to_string()` and
`children()`, registered with `gdb.printing.RegexpCollectionPrettyPrinter` and
`gdb.printing.register_pretty_printer(gdb.current_objfile(), ...)`; `info pretty-printer` lists
them and `print/r` bypasses them. `gdb.Command` subclasses add commands (`invoke(arg,
from_tty)`), `gdb.Function` subclasses add convenience functions such as `$misplaced(TABLE)`
(usable in `condition`), `gdb.Breakpoint` subclasses decide in `stop()` whether to stop.
`gdb.parse_and_eval(EXPR)` returns a `gdb.Value`; `gdb.execute(CMD)` runs a command.

## Time travel (Chapter 9)

| Task | gdb | tdb (chapter) |
|------|-----|---------------|
| start / stop recording | `record` (= `record full`), `record stop` | `record` (`rec`), `record stop` (9) |
| back | `reverse-continue`, `reverse-step`, `reverse-next`, `reverse-stepi`, `reverse-finish`; or `set exec-direction reverse` | `reverse-continue` (`rc`), `reverse-next` (`rn`), `reverse-stepi` (`rsi`), `reverse-finish` (9) |
| move in the record | `record goto begin`, `record goto end`, `record goto N`, `info record` | `record goto begin\|end\|N`, `info record` (9) |
| a longer record | `set record full insn-number-max N` (default 200000) | — |
| bookmarks | `checkpoint`, `info checkpoints`, `restart N` | `checkpoint`, `info checkpoints`, `restart N`, `delete checkpoint N` (9) |

`record full` in gdb 12.1 stops with `Process record does not support instruction 0xc5` at the
AVX instructions of glibc's string functions. `set environment GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-AVX_Fast_Unaligned_Load`
before `run` makes glibc pick SSE2 versions (Chapter 9).

## Remote debugging (Chapter 10)

| Task | gdb / gdbserver | tdb (chapter) |
|------|-----------------|---------------|
| serve a program | `gdbserver :2345 ./meter meter.log` | `gdbserver [--remote-debug] PORT` (10) |
| serve a running process | `gdbserver --attach :2345 PID` | — |
| stay up, start programs from gdb | `gdbserver --multi :2345`, then `target extended-remote :2345`, `set remote exec-file PATH`, `run` | — |
| connect | `target remote :2345`; over a pipe: `target remote \| gdbserver - PROG ARGS` | gdb's `target remote :PORT` to tdb (10) |
| libraries and debug info on the host | `set sysroot /`, `set debug-file-directory ./debug:/usr/lib/debug`, `file PROG` | — |
| commands to the server | `monitor help`, `monitor exit` | — (Chapter 10 exercise) |
| leave | `disconnect` (the server keeps the program), `detach`, `kill` | — |
| see the packets | `set debug remote 1` | `--remote-debug` (10) |
| another binary than the server's | `set exec-file-mismatch ask\|warn\|off` | — |

## Build flags (gcc 11.4)

| Flags | What you get in gdb |
|-------|---------------------|
| `-g -O0` | everything: every variable, every line, in order (the course default) |
| `-g -Og` | almost everything; some dead values `<optimized out>` |
| `-g -O2` | inlining and reordering: `<optimized out>`, lines jump around |
| no `-g` | function names only (from the ELF symbol table): `info locals` says `No symbol table info available.`; `x` and registers still work |
| `-fsanitize=address` | AddressSanitizer: out-of-bounds and use-after-free reports (Chapter 5) |
| `-fsanitize=undefined` | UBSan: signed overflow and other undefined behaviour at run time (Chapters 2, 6) |
| `-fsanitize=thread -pthread` | ThreadSanitizer: data races (Chapter 7) |
| `-fwrapv` | signed overflow wraps, so `-O2` keeps overflow checks (Chapter 6) |
| `-flto` | link-time checks across files: a `mean` declared differently in two files (Chapter 6) |

tdb's test targets are built with `-std=c11 -g -O0 -fno-omit-frame-pointer -pthread`, so `bt` can
follow `rbp` (Chapter 5); tdb itself with `-O2 -g -std=c11 -Wall -Wextra -pedantic -Werror`.

## Tools around gdb

| Task | Command |
|------|---------|
| system calls | `strace ./prog`, `strace -e trace=openat ./prog`, `strace -e trace=%file ./prog`, `-o FILE`, `-f` (children) |
| sections, symbols | `readelf -S -W PROG`, `readelf -s -W PROG`, `readelf -h PROG` |
| the line table | `readelf --debug-dump=decodedline PROG`, `--debug-dump=rawline`, `readelf -x .debug_line PROG` |
| call frame information | `readelf --debug-dump=frames-interp PROG` |
| the build ID, the debug link | `readelf -n PROG`, `readelf --string-dump=.gnu_debuglink PROG` |
| address to line | `addr2line -e PROG ADDR` |
| split off debug info | `objcopy --only-keep-debug prog prog.debug`; `objcopy --strip-all --add-gnu-debuglink=prog.debug prog stripped` |
| build-ID tree | `debug/.build-id/NN/REST.debug`, found with `set debug-file-directory ./debug:/usr/lib/debug` |
| may I attach? | `cat /proc/sys/kernel/yama/ptrace_scope` (0 any, 1 descendants only) |
| who traces a process | `TracerPid` in `/proc/PID/status` (in gdb: `pipe info proc status \| grep TracerPid`) |

Sources for this appendix: [REFERENCES.md](REFERENCES.md#appendix-a--gdb-and-tdb-cheat-sheet).
