# Appendix B — ptrace, Wait Status and Signals Reference

The system interface under tdb, in one place: every `ptrace` request and option tdb 1.0 uses
and the chapter that introduced it, the other system calls around them, the wait status words
the course decodes, the registers at a system call stop, the DR7 layout, the `si_code` values
tdb prints, the signal numbers (Linux's and the remote protocol's) and gdb's default for each,
and where each concept lives in tdb's source. Everything here is checked against
`project/ch10/src` and the chapters; the authority is [ptrace(2)](https://man7.org/linux/man-pages/man2/ptrace.2.html).

Previous: [Appendix A](appendix-a-cheatsheet.md). Next: [REFERENCES.md](REFERENCES.md).

## ptrace requests

The call is `long ptrace(enum __ptrace_request request, pid_t pid, void *addr, void *data)`.
Unused arguments are `NULL` (0). Every request below works on one thread: `pid` is a thread ID
(tid), which for the main thread equals the process ID. The functions named are tdb 1.0's; the
chapter is the one that first used the request.

| Request | `addr` | `data` | What it does | tdb (chapter) |
|---------|--------|--------|--------------|---------------|
| `PTRACE_TRACEME` | — | — | called by the child after `fork`: "my parent traces me"; the next `execv` stops it with `SIGTRAP` | `proc_launch` (1) |
| `PTRACE_CONT` | — | signal or 0 | resume; deliver `data` as a signal, or none | `proc_resume` (1) |
| `PTRACE_SETOPTIONS` | — | `PTRACE_O_*` bits | set tracing options (below) | `proc_launch`, `attach_thread` (1) |
| `PTRACE_PEEKDATA` / `PEEKTEXT` | address | — | read one 8-byte word of the tracee's memory; the word is the return value, so clear `errno` first and check it after | `proc_peek` (2); `ckpt_fork` (9) |
| `PTRACE_POKEDATA` / `POKETEXT` | address | the word | write one word: how an `int3` (`0xCC`) goes in and comes out | `proc_poke` (2); `ckpt_fork` (9) |
| `PTRACE_GETREGS` | — | `struct user_regs_struct *` | read all general registers (`<sys/user.h>`) | `proc_get_regs` (2) |
| `PTRACE_SETREGS` | — | `struct user_regs_struct *` | write them: `rip - 1` after an `int3`, `reg NAME VALUE` | `proc_set_regs` (2) |
| `PTRACE_SINGLESTEP` | — | signal or 0 | execute one instruction, then stop with `SIGTRAP` (the CPU's trap flag) | `proc_step` (2) |
| `PTRACE_PEEKUSER` | offset in `struct user` | — | read a word of the kernel's `struct user`: here the debug registers | `dr_read` (4) |
| `PTRACE_POKEUSER` | offset in `struct user` | the value | write DR0–DR3 (addresses) and DR7 (control); the kernel checks them | `dr_set`, `dr_setup_thread` (4) |
| `PTRACE_SYSCALL` | — | signal or 0 | like `PTRACE_CONT`, but also stop at the next system call entry or exit | `run_continue` with a catchpoint (4) |
| `PTRACE_ATTACH` | — | — | trace a running thread; the kernel sends it `SIGSTOP` | `proc_attach`, `attach_thread` (5) |
| `PTRACE_DETACH` | — | signal or 0 | stop tracing and resume, delivering `data` | `proc_detach`, `detach_child` (5) |
| `PTRACE_GETSIGINFO` | — | `siginfo_t *` | the `siginfo_t` of the signal the thread stopped with (gdb's `$_siginfo`) | `proc_siginfo` (5) |
| `PTRACE_GETEVENTMSG` | — | `unsigned long *` | after an event stop: the new thread's tid (clone) or the child's pid (fork) | `handle`, `proc_fork_event` (7); `ckpt_fork` (9) |

Used only in exercise solutions: `PTRACE_GETFPREGS` (a `struct user_fpregs_struct` with the SSE
registers, Chapter 6) and `PTRACE_GET_SYSCALL_INFO` (entry or exit, Linux 5.3, Chapter 4).

### Options (`PTRACE_SETOPTIONS`)

| Option | Effect | tdb (chapter) |
|--------|--------|---------------|
| `PTRACE_O_EXITKILL` | if the tracer dies, the kernel kills the tracee; only for programs tdb started | `proc_launch` (1) |
| `PTRACE_O_TRACESYSGOOD` | system call stops report `SIGTRAP \| 0x80` instead of plain `SIGTRAP` | (4) |
| `PTRACE_O_TRACECLONE` | a new thread is traced from its first instruction; the creator stops with `PTRACE_EVENT_CLONE` | (7) |
| `PTRACE_O_TRACEFORK` | a forked child is traced too; the parent stops with `PTRACE_EVENT_FORK` | (7); checkpoints (9) |

tdb sets `PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK` (`TRACE_OPTIONS` in
`proc.c`) on a launched program, with `PTRACE_O_EXITKILL`, and on every thread it attaches to.
Tracees that the clone and fork options bring in inherit the options.

### Other system calls

| Call | Why | tdb (chapter) |
|------|-----|---------------|
| [`fork`](https://man7.org/linux/man-pages/man2/fork.2.html), `execv` | start the program: the child calls `PTRACE_TRACEME`, then `execv` | `proc_launch` (1) |
| [`waitpid`](https://man7.org/linux/man-pages/man2/wait.2.html)`(pid, &status, __WALL)` | wait for a stop or an exit; `__WALL` also reports threads (clone children); `-1` waits for any | `proc_launch` (1); `wait_tid` with `__WALL` (7) |
| [`personality`](https://man7.org/linux/man-pages/man2/personality.2.html)`(ADDR_NO_RANDOMIZE)` | in the child, before `execv`: the same addresses every run, as gdb's `disable-randomization` | `proc_launch` (1) |
| [`tgkill`](https://man7.org/linux/man-pages/man2/tgkill.2.html)`(pid, tid, SIGSTOP)` | stop one thread (`kill` would pick any): all-stop | `proc_stop_all` (7) |
| `syscall` (`0f 05`) injected with `rax = SYS_clone`, `rdi = CLONE_PARENT \| SIGCHLD` | make the *tracee* fork a copy of itself: a checkpoint ([clone(2)](https://man7.org/linux/man-pages/man2/clone.2.html)) | `ckpt_fork` (9) |
| [`process_vm_readv`](https://man7.org/linux/man-pages/man2/process_vm_readv.2.html) | read a block of the tracee's memory in one call | Chapter 3 exercise |
| [`memfd_create`](https://man7.org/linux/man-pages/man2/memfd_create.2.html) | an in-memory file to capture a command's output | Chapter 10 exercise |
| [`prctl`](https://man7.org/linux/man-pages/man2/prctl.2.html)`(PR_SET_PTRACER, ...)` | in a target: allow a debugger to attach under `ptrace_scope` 1 | the `waiter` target (5) |

## Wait status words

`waitpid` fills an `int` status. `tdb -v` prints it in hex, as `[waitpid: status 0x057f]`.

| Status | Low byte | Next byte | Decoded with | Meaning | Seen in |
|--------|----------|-----------|--------------|---------|---------|
| `0x057f` | `0x7f` | `0x05` | `WIFSTOPPED`, `WSTOPSIG` = 5 | stopped by `SIGTRAP`: the exec, an `int3`, a single step, a watchpoint | Chapter 1 |
| `0x0a7f` | `0x7f` | `0x0a` | `WSTOPSIG` = 10 | stopped by `SIGUSR1` (signal-delivery-stop) | Chapter 1 |
| `0x0b7f` | `0x7f` | `0x0b` | `WSTOPSIG` = 11 | stopped by `SIGSEGV` | Chapters 1, 2 |
| `0x137f` | `0x7f` | `0x13` | `WSTOPSIG` = 19 | stopped by `SIGSTOP`: a new thread, an attach, tdb's `tgkill` | Chapter 7 |
| `0x3057f` | `0x7f` | `0x05` | `WSTOPSIG` = 5, `status >> 16` = 3 | `PTRACE_EVENT_CLONE`: the thread called `clone` | Chapter 7 |
| `0x857f` | `0x7f` | `0x85` | `WSTOPSIG` = `SIGTRAP \| 0x80` | a system call stop with `PTRACE_O_TRACESYSGOOD` (computed, not printed in a chapter) | Chapter 4 |
| `0x0300` | `0x00` | `0x03` | `WIFEXITED`, `WEXITSTATUS` = 3 | exited with code 3 | Chapter 1 |
| `0x0000` | `0x00` | `0x00` | `WIFEXITED`, `WEXITSTATUS` = 0 | exited with code 0 (also a thread that ended) | Chapter 7 |
| `0x008b` | `0x8b` | `0x00` | `WIFSIGNALED`, `WTERMSIG` = 11, `WCOREDUMP` | killed by `SIGSEGV`, core dumped (`0x80` is the core flag) | Chapter 1 |

The rule: low byte `0x7f` means *stopped*, by the signal in the next byte. Low byte 0 means
*exited*, with the code in the next byte. Anything else: *killed*, by the signal in the low 7
bits, with `0x80` set if a core was written. For a `PTRACE_EVENT_*` stop the event number is in
bits 16 and up: `PTRACE_EVENT_FORK` 1, `PTRACE_EVENT_CLONE` 3.

## System call stops (Chapter 4)

| What | Where |
|------|-------|
| the system call number | `orig_rax` (at entry and exit) |
| arguments 1–6 | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` (`r10`, not `rcx`: `syscall` overwrites `rcx`) |
| at entry | `rax` = `-ENOSYS` (-38), set by the kernel in case no one runs the call |
| at exit | `rax` = the result; -4095 to -1 is `-errno` |
| entry or exit? | the stops alternate, so count them (or `PTRACE_GET_SYSCALL_INFO`, Linux 5.3) |
| not in a system call | `orig_rax` = -1 (checked by `ckpt_fork` before it injects one) |

## Debug registers (Chapter 4)

DR0–DR3 hold up to four addresses; DR6 says which one fired (bits B0–B3; the CPU sets them and
never clears them, so the debugger must); DR7 enables and configures them. A tracer writes them
with `PTRACE_POKEUSER` at `offsetof(struct user, u_debugreg) + 8 * n`.

| DR7 bits | Field | Values |
|----------|-------|--------|
| 2n | Ln, local enable for DRn | 1 = on |
| 2n + 1 | Gn, global enable | tdb never sets it |
| 16 + 4n, 17 + 4n | R/Wn: when | `00` execute, `01` write, `11` read or write |
| 18 + 4n, 19 + 4n | LENn: how many bytes | `00` 1, `01` 2, `11` 4, `10` 8 |

The address must be aligned to the length. A write watchpoint stops the thread just *after* the
write, with `SIGTRAP`. Debug registers are per thread: tdb copies them into every new thread
(`dr_setup_thread`, Chapter 7).

## si_code values tdb prints (Chapter 5)

`siginfo` (gdb: `print $_siginfo`) shows `si_signo`, `si_code` and, for faults, `si_addr`.
Codes above 0 mean different things for different signals; 0 and below mean the same for all.

| Signal | `si_code` | Meaning |
|--------|-----------|---------|
| `SIGSEGV` | `SEGV_MAPERR` | address not mapped to object |
| `SIGSEGV` | `SEGV_ACCERR` | invalid permissions for mapped object |
| `SIGBUS` | `BUS_ADRALN`, `BUS_ADRERR` | invalid address alignment; nonexistent physical address |
| `SIGFPE` | `FPE_INTDIV`, `FPE_INTOVF` | integer divide by zero; integer overflow |
| `SIGILL` | `ILL_ILLOPN`, `ILL_ILLOPC` | illegal operand; illegal opcode |
| `SIGTRAP` | `TRAP_BRKPT`, `TRAP_TRACE`, `TRAP_HWBKPT` | breakpoint; trace trap (single step); hardware breakpoint or watchpoint |
| any | `SI_KERNEL` (0x80) | sent by the kernel: on x86-64 this is what an `int3` gives (Chapter 7) |
| any | `SI_USER`, `SI_TKILL`, `SI_QUEUE`, `SI_TIMER` | sent by `kill`; by `tkill`/`tgkill`; by `sigqueue`; by a POSIX timer |

## Signals

Linux x86-64 numbers ([signal(7)](https://man7.org/linux/man-pages/man7/signal.7.html)); gdb
12.1's default `handle` setting (tdb 1.0's is the same, Chapter 5); and the number gdb's remote
protocol uses, which is gdb's own numbering, not Linux's (tdb converts in `rsp_signal_to_gdb`
and `rsp_signal_from_gdb`, Chapter 10).

| Linux | Signal | Stop | Print | Pass | RSP |
|------:|--------|------|-------|------|----:|
| 1 | `SIGHUP` | Yes | Yes | Yes | 1 |
| 2 | `SIGINT` | Yes | Yes | No | 2 |
| 3 | `SIGQUIT` | Yes | Yes | Yes | 3 |
| 4 | `SIGILL` | Yes | Yes | Yes | 4 |
| 5 | `SIGTRAP` | Yes | Yes | No | 5 |
| 6 | `SIGABRT` | Yes | Yes | Yes | 6 |
| 7 | `SIGBUS` | Yes | Yes | Yes | 10 |
| 8 | `SIGFPE` | Yes | Yes | Yes | 8 |
| 9 | `SIGKILL` | Yes | Yes | Yes | 9 |
| 10 | `SIGUSR1` | Yes | Yes | Yes | 30 |
| 11 | `SIGSEGV` | Yes | Yes | Yes | 11 |
| 12 | `SIGUSR2` | Yes | Yes | Yes | 31 |
| 13 | `SIGPIPE` | Yes | Yes | Yes | 13 |
| 14 | `SIGALRM` | No | No | Yes | 14 |
| 15 | `SIGTERM` | Yes | Yes | Yes | 15 |
| 16 | `SIGSTKFLT` | Yes | Yes | Yes | — |
| 17 | `SIGCHLD` | No | No | Yes | 20 |
| 18 | `SIGCONT` | Yes | Yes | Yes | 19 |
| 19 | `SIGSTOP` | Yes | Yes | Yes | 17 |
| 20 | `SIGTSTP` | Yes | Yes | Yes | 18 |
| 21 | `SIGTTIN` | Yes | Yes | Yes | 21 |
| 22 | `SIGTTOU` | Yes | Yes | Yes | 22 |
| 23 | `SIGURG` | No | No | Yes | 16 |
| 24 | `SIGXCPU` | Yes | Yes | Yes | 24 |
| 25 | `SIGXFSZ` | Yes | Yes | Yes | 25 |
| 26 | `SIGVTALRM` | No | No | Yes | 26 |
| 27 | `SIGPROF` | No | No | Yes | 27 |
| 28 | `SIGWINCH` | No | No | Yes | 28 |
| 29 | `SIGIO` (`SIGPOLL`) | No | No | Yes | 23 |
| 30 | `SIGPWR` | Yes | Yes | Yes | 32 |
| 31 | `SIGSYS` | Yes | Yes | Yes | 12 |

`SIGKILL` never reaches the tracer: the process just dies. `SIGINT` and `SIGTRAP` are not passed
by default because the debugger usually caused them (Ctrl-C, an `int3`). `SIGSTKFLT` has no
number in gdb's numbering; tdb sends 0 for it.

## Where things are in tdb 1.0

| Concept | File (chapter added) | Key functions |
|---------|----------------------|---------------|
| launch, wait, threads, fork events, all-stop | `src/proc.c` (1, threads in 7) | `proc_launch`, `proc_wait`, `proc_stop_all`, `proc_fork_event`, `proc_attach`, `proc_detach`, `proc_siginfo` |
| command loop, all handlers | `src/tdb.c` (1) | `main`, `execute`, `report`, the `builtins[]` table (8) |
| software breakpoints, watch and catch entries | `src/breakpoint.c` (2) | `bp_add`, `bp_add_internal`, `bp_at`, `bp_add_watch`, `bp_add_syscall` |
| continue and stepping over an `int3` | `src/run.c` (2) | `run_continue`, `run_stepi` |
| ELF reading, load base (PIE) | `src/elf.c` (2) | `elf_read_file`, `elf_section`, `elf_load_base` |
| symbols (`.symtab`) | `src/symbols.c` (3) | `sym_load`, `sym_by_name`, `sym_by_addr`, `sym_describe` |
| registers by name, eflags | `src/regs.c` (3) | `reg_find`, `reg_get`, `reg_set`, `reg_flags` |
| `x` output format | `src/mem.c` (3) | `mem_format_line` |
| hardware watchpoints (DR7) | `src/debugreg.c` (4) | `dr7_enable`, `dr_set`, `dr_hit`, `dr_setup_thread` |
| system call names | `src/syscalls.c`, `sysnames.h` (4) | `sys_name`, `sys_number` |
| backtrace (the `rbp` chain) | `src/frames.c` (5) | `frame_state`, `frame_backtrace`, `frame_return` |
| signals, `handle`, `siginfo` text | `src/signals.c` (5) | `sig_handle`, `sig_action`, `sig_code_text`, `sig_is_fault` |
| DWARF 5 line table | `src/lines.c` (6) | `lines_load`, `line_at`, `line_find`, `line_after_prologue` |
| `step`, `next`, `finish` | `src/step.c` (6) | `step_line`, `step_finish`, `is_call` |
| source listing | `src/source.c` (6) | `src_print` |
| input sources (`-x`, `source`, `~/.tdbinit`, readline) | `src/input.c` (8) | `input_push`, `input_pop`, `input_read` |
| abbreviations, ambiguity | `src/cli.c` (8) | `cli_lookup`, `cli_matches`, `cli_expand` |
| checkpoints (a fork in the tracee) | `src/checkpoint.c` (9) | `ckpt_fork`, `ckpt_snapshot`, `ckpt_restart` |
| record and replay | `src/record.c` (9) | `rec_start`, `rec_exec`, `rec_goto`, `rec_reverse_stepi`, `rec_reverse_continue` |
| RSP framing, hex, checksums (pure, unit-tested) | `src/rsp.c` (10) | `rsp_frame`, `rsp_feed`, `rsp_checksum`, `rsp_regs_encode` |
| the gdbserver stub | `src/gdbserver.c` (10) | `gs_serve` |

Sources for this appendix: [REFERENCES.md](REFERENCES.md#appendix-b--ptrace-wait-status-and-signals-reference).
