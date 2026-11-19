# Chapter 4 — Watchpoints and Catchpoints

A breakpoint answers "when does the program get *here*?". Many bugs raise a different question:
"who changed *this*?". A watchpoint answers that one: it stops the program when a piece of
memory changes, wherever the code doing it is. A catchpoint answers "when does *this event*
happen?". The event is often a system call: the moment the program asks the kernel to open a
file. This chapter covers `watch`, `rwatch`, `awatch` and `watch -l`, how a watchpoint's scope
ends, the limit of four, and software watchpoints and what they cost. Then it covers
`catch syscall` (names, numbers, groups), reading `$rax` and the arguments at a system call stop,
and `strace` as the quick alternative. tdb v0.4 gets the same two powers from the hardware and
the kernel. It programs the CPU's debug registers DR0–DR3, DR6 and DR7 through
`PTRACE_POKEUSER`, and it stops at system calls with `PTRACE_SYSCALL`.

Previous: [Chapter 3](03-looking-at-data.md). Next: [Chapter 5](05-crashes-and-post-mortem.md).

## A value nobody assigns

`examples/ch04/sensor.c`
```c
/* sensor.c - read temperatures, one per line, keep the average of the last
 * five, and raise an alarm when a reading is above the alarm level (30).
 *
 *     ./sensor < readings.txt
 *
 * Bug: the alarm goes off for 24 degrees. Nothing assigns alarm_level after
 * the start, yet it changes. Find the write with a watchpoint.
 */
#include <stdio.h>

#define WINDOW 5

struct filter {
    int window[WINDOW];
    int alarm_level;
    int pos;
    int count;
};

/* The average of the readings in the window. */
static double average(const struct filter *f)
{
    int sum = 0;
    int n = f->count < WINDOW ? f->count : WINDOW;

    for (int i = 0; i < n; i++)
        sum += f->window[i];
    return n > 0 ? (double)sum / n : 0.0;
}

/* Put reading r into the window, replacing the oldest one. */
static void filter_add(struct filter *f, int r)
{
    f->window[f->pos] = r;
    f->pos = (f->pos + 1) % (WINDOW + 1);
    f->count++;
}

int main(void)
{
    struct filter f = { .alarm_level = 30 };
    int r;

    while (scanf("%d", &r) == 1) {
        filter_add(&f, r);
        if (r > f.alarm_level)
            printf("ALARM: %d is above %d\n", r, f.alarm_level);
        printf("reading %2d  average %.1f\n", r, average(&f));
    }
    return 0;
}
```

`readings.txt` holds eight readings, one per line: 20 22 19 23 20 21 24 22.

```console
$ cd examples/ch04
$ make
cc -std=c11 -Wall -Wextra -g -O0    sensor.c   -o sensor
cc -std=c11 -Wall -Wextra -g -O0    config.c   -o config
$ ./sensor < readings.txt
reading 20  average 20.0
reading 22  average 21.0
reading 19  average 20.3
reading 23  average 21.0
reading 20  average 20.8
reading 21  average 20.8
ALARM: 24 is above 21
reading 24  average 21.6
ALARM: 22 is above 21
reading 22  average 21.6
$ grep -n alarm_level sensor.c
6: * Bug: the alarm goes off for 24 degrees. Nothing assigns alarm_level after
15:    int alarm_level;
41:    struct filter f = { .alarm_level = 30 };
46:        if (r > f.alarm_level)
47:            printf("ALARM: %d is above %d\n", r, f.alarm_level);
```

- The alarm level was 30 and is now 21, and after the sixth reading the average stops moving.
- `grep` finds one assignment, the initializer on line 41. Writes through a bad index or a stray
  pointer don't mention the variable's name, so reading the code will not find them.
- A breakpoint needs a line, and we don't know the line. We know the *memory*: `f.alarm_level`.

## watch

Stop at line 44, after the initializer has run, and watch the member:

```console
$ gdb -q ./sensor
Reading symbols from ./sensor...
(gdb) break 44
Breakpoint 1 at 0x12b0: file sensor.c, line 44.
(gdb) run < readings.txt
Starting program: .../sensor < readings.txt
...
Breakpoint 1, main () at sensor.c:44
44	    while (scanf("%d", &r) == 1) {
(gdb) print f
$1 = {window = {0, 0, 0, 0, 0}, alarm_level = 30, pos = 0, count = 0}
(gdb) watch f.alarm_level
Hardware watchpoint 2: f.alarm_level
(gdb) continue
Continuing.
reading 20  average 20.0
reading 22  average 21.0
reading 19  average 20.3
reading 23  average 21.0
reading 20  average 20.8

Hardware watchpoint 2: f.alarm_level

Old value = 30
New value = 21
filter_add (f=0x7fffffffe3e0, r=21) at sensor.c:35
35	    f->pos = (f->pos + 1) % (WINDOW + 1);
(gdb) bt
#0  filter_add (f=0x7fffffffe3e0, r=21) at sensor.c:35
#1  0x00005555555552c3 in main () at sensor.c:45
(gdb) print f->pos
$2 = 5
(gdb) print &f->window[f->pos]
$3 = (int *) 0x7fffffffe3f4
(gdb) print &f->alarm_level
$4 = (int *) 0x7fffffffe3f4
(gdb) print *f
$5 = {window = {20, 22, 19, 23, 20}, alarm_level = 21, pos = 5, count = 5}
```

- gdb ran the program at full speed and stopped once, at the one write that changed the value.
  It prints the old and new value, and where the program is now.
- "Now" is line 35, but the write happened on line 34. The CPU reports a data watchpoint
  *after* the instruction that wrote the memory has finished (a *trap*). So the program stops
  at the next instruction, which is the start of line 35. When a watchpoint stops you, look at
  the line *before* the one shown.
- `f->pos` is 5 and `window` has 5 elements, so `window[5]` is one past the end. The next
  member in memory is `alarm_level`, and both addresses are `0x7fffffffe3f4`. The sixth
  reading, 21, was stored in the alarm level. `% (WINDOW + 1)` lets `pos` reach 5. The fix is
  `% WINDOW`. With it, the program prints no alarms and the average moves on: `reading 21
  average 21.0`, `reading 24  average 21.4`, `reading 22  average 22.0`.
- The rule: set the watchpoint after the variable has its first value. If you watch `f` at
  `break main`, before line 41 runs, you get two extra stops, 32767→0 and 0→30. The 32767 is
  stack garbage; gcc zeroes the whole struct and then stores the 30.

`watch` takes any expression, not just a variable: `watch f.pos > 4`, `watch *p`, `watch
list->head`. gdb works out which memory the expression reads and watches all of it. Then it
checks the expression's value again whenever any of that memory is written.

## Scope

`f` is a local variable of `main`. Continue the session above to the end and, as `main`
returns, gdb says `Watchpoint 2 deleted because the program has left the block in which its
expression is valid.` An expression with local variables in it means something only while their
frame exists. That is a nuisance when the variable is only a pointer *to* the memory you care
about. In `filter_add`, `f` is a parameter:

```console
(gdb) break filter_add
Breakpoint 1 at 0x1212: file sensor.c, line 34.
(gdb) run < readings.txt
...
Breakpoint 1, filter_add (f=0x7fffffffe3e0, r=20) at sensor.c:34
34	    f->window[f->pos] = r;
(gdb) watch f->alarm_level
Hardware watchpoint 2: f->alarm_level
(gdb) continue
Continuing.

Watchpoint 2 deleted because the program has left the block in
which its expression is valid.
main () at sensor.c:46
46	        if (r > f.alarm_level)
(gdb) continue
Continuing.
reading 20  average 20.0

Breakpoint 1, filter_add (f=0x7fffffffe3e0, r=22) at sensor.c:34
34	    f->window[f->pos] = r;
(gdb) watch -l f->alarm_level
Hardware watchpoint 3: -location f->alarm_level
(gdb) delete 1
(gdb) continue
Continuing.
...
reading 20  average 20.8

Hardware watchpoint 3: -location f->alarm_level

Old value = 30
New value = 21
filter_add (f=0x7fffffffe3e0, r=21) at sensor.c:35
35	    f->pos = (f->pos + 1) % (WINDOW + 1);
(gdb) continue
Continuing.
...
reading 22  average 21.6

Hardware watchpoint 3: -location f->alarm_level

Old value = 21
New value = 21845
0x00007ffff7c4539a in __run_exit_handlers (status=0, listp=0x7ffff7e1a838 <__exit_funcs>, ...
```

- The first watchpoint lasted one call. It was deleted when `filter_add` returned, long before
  the sixth reading.
- `watch -l EXPR` (long form `-location`) evaluates the expression *once*, takes its address,
  and watches that address: `*(int *) 0x7fffffffe3f4`. No local variable is left in it, so it
  has no scope and survives every return. It caught the sixth reading.
- The last stop is the warning in this lesson. `main` has returned, so `f` no longer exists,
  but its stack slot does. Code in libc's exit path (`__run_exit_handlers`) reused the slot and
  wrote over it. 21845 is 0x5555, which looks like the high half of an address in this
  program. An address outlives its variable. A `-l` watchpoint on stack memory needs deleting
  once the frame is gone, or it will report writes by code that has nothing to do with yours.

## Reads too: rwatch and awatch

`watch` stops on writes that change the value. `rwatch` stops on reads, and `awatch` (access)
stops on reads and writes. In the same program, stopped at line 44:

```console
(gdb) rwatch f.alarm_level
Hardware read watchpoint 2: f.alarm_level
(gdb) awatch f.pos
Hardware access (read/write) watchpoint 3: f.pos
(gdb) continue
Continuing.

Hardware access (read/write) watchpoint 3: f.pos

Value = 0
filter_add (f=0x7fffffffe3e0, r=20) at sensor.c:34
34	    f->window[f->pos] = r;
(gdb) continue
Continuing.

Hardware access (read/write) watchpoint 3: f.pos

Value = 0
filter_add (f=0x7fffffffe3e0, r=20) at sensor.c:35
35	    f->pos = (f->pos + 1) % (WINDOW + 1);
(gdb) continue
Continuing.

Hardware access (read/write) watchpoint 3: f.pos

Old value = 0
New value = 1
filter_add (f=0x7fffffffe3e0, r=20) at sensor.c:36
36	    f->count++;
```

Line 34 reads `f->pos` to index the array: one stop. Line 35 reads it and then writes the new
value: two stops, one for each instruction. A read prints `Value =`; a write that changed the
value prints `Old value`/`New value`. As with `watch`, each stop is reported just after the
instruction that did the access, so the first read in line 34 shows as "line 34". The store
`window[0] = r` comes after it in the same line.

`rwatch` hides a hardware detail. x86 debug registers can trigger on writes, or on reads *or*
writes, but not on reads alone (see [How hardware watchpoints
work](#how-hardware-watchpoints-work)). So gdb implements `rwatch` as an access watchpoint. When the
value has changed since the last stop, gdb assumes the access was a write and does not report it.
You can see the guess at work with the course project's `watched` target. Its `step(i)` adds `i` to
the global `counter` for even `i`, so `step(0)` writes `counter` without changing it:

```console
$ cd project/ch04 && make
$ gdb -q -nx -batch -ex 'break main' -ex run -ex 'rwatch counter' \
      -ex c -ex c -ex c -ex c -ex c -ex c build/targets/watched
...
Hardware read watchpoint 2: counter
Value = 0
0x0000555555555164 in step (i=0) at targets/watched.c:12
12	        counter += i;
Hardware read watchpoint 2: counter
Value = 0
step (i=0) at targets/watched.c:13
13	}
Hardware read watchpoint 2: counter
Value = 0
0x0000555555555164 in step (i=2) at targets/watched.c:12
12	        counter += i;
Hardware read watchpoint 2: counter
Value = 2
0x0000555555555164 in step (i=4) at targets/watched.c:12
...
```

The second stop is the *store* in `step(0)`, reported as a read because the value did not
change. The stores at `i = 2` and `i = 4` changed the value and are not reported; the reads
before them are, and so is `main`'s read for the `printf` (`Value = 6`). In practice `rwatch`
answers "who reads this?" well, apart from the odd same-value write.

## Four registers

Hardware watchpoints are cheap because the CPU does the checking, but there are only four of
them. Add three more to the session above:

```console
(gdb) watch f.count
Hardware watchpoint 4: f.count
(gdb) watch f.window[0]
Hardware watchpoint 5: f.window[0]
(gdb) watch f.window[1]
Hardware watchpoint 6: f.window[1]
(gdb) continue
Continuing.
Warning:
Could not insert hardware watchpoint 3.
Could not insert hardware watchpoint 4.
Could not insert hardware breakpoints:
You may have requested too many hardware breakpoints/watchpoints.

Command aborted.
```

- gdb accepted all five at `watch` time (`info watchpoints`, which is `info breakpoints`
  without the breakpoints, lists them as `read watchpoint`,
  `acc watchpoint` and `hw watchpoint`). It writes the debug registers only when the program
  resumes, so the error comes at `continue`. It names watchpoints 3 and 4, not the fifth one
  you added last, so don't read too much into the numbers. After `delete 5 6`, `continue`
  works again and stops at `f.count`'s first change, 0 to 1.
- Each register covers 1, 2, 4 or 8 bytes, aligned to that size. A 4-byte `int` takes one
  register. gdb splits bigger regions over several registers when it can.
- `hbreak` (a *hardware breakpoint*, one that needs no `0xCC` in the code) uses the same four
  registers. It is for code that can't be patched, such as code in ROM or flash.

## Software watchpoints

When there are no registers left, or the expression can't be watched by address, gdb falls back
to a *software watchpoint*: it single-steps the program and evaluates the expression after
every instruction. You can force it to do that:

```console
$ time gdb -q -batch -ex 'set can-use-hw-watchpoints 1' -ex 'break 44' \
      -ex 'run < readings.txt' -ex 'watch f.alarm_level' -ex 'continue' ./sensor
...
filter_add (f=0x7fffffffe570, r=21) at sensor.c:35
0.25 s elapsed
$ time gdb -q -batch -ex 'set can-use-hw-watchpoints 0' -ex 'break 44' \
      -ex 'run < readings.txt' -ex 'watch f.alarm_level' -ex 'continue' ./sensor
...
Watchpoint 2: f.alarm_level

Old value = 30
New value = 21
filter_add (f=0x7fffffffe570, r=21) at sensor.c:35
5.03 s elapsed
```

- Both find the same write. The software one says `Watchpoint`, not `Hardware watchpoint`, and
  on my machine it took 20 times as long for six readings (timings vary). Each instruction
  costs a `PTRACE_SINGLESTEP`, two context switches and a memory read. A program that runs for
  a second natively could take hours. (`f` is at another address here: the stack's position
  depends on the environment, which differs between `gdb -batch` and the earlier session.)
- Registers have no address, so a watchpoint on one is always software: `watch $rax` answers
  `Watchpoint 2: $rax`, with no "Hardware".
- Use software watchpoints for short stretches. Get close with a breakpoint first, then watch.

## Catching system calls

The second program's bug leaves no trace in its memory at all.

`examples/ch04/config.c`
```c
/* config.c - read "key=value" settings from settings.conf; use defaults for
 * anything the file does not set.
 *
 *     ./config
 *
 * Bug: settings.conf says port=8080, but the program uses port 80 and says
 * nothing. Find out which file it really opens with catch syscall.
 */
#include <stdio.h>
#include <string.h>

struct config {
    int port;
    int workers;
};

static void load(struct config *c, const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[128];

    if (fp == NULL)
        return;                 /* no file: keep the defaults */
    while (fgets(line, sizeof line, fp) != NULL) {
        sscanf(line, "port=%d", &c->port);
        sscanf(line, "workers=%d", &c->workers);
    }
    fclose(fp);
}

int main(void)
{
    struct config c = { .port = 80, .workers = 1 };

    load(&c, "setings.conf");
    printf("port %d, %d worker%s\n", c.port, c.workers, c.workers == 1 ? "" : "s");
    return 0;
}
```

```console
$ cat settings.conf
port=8080
workers=4
$ ./config
port 80, 1 worker
```

Every file a program opens goes through one system call, on x86-64 Linux usually `openat`
(`fopen` → `open` → `openat`). A catchpoint on it stops the program at the kernel's door, so
you can see each name it asks for:

```console
$ gdb -q ./config
Reading symbols from ./config...
(gdb) catch syscall openat
Catchpoint 1 (syscall 'openat' [257])
(gdb) run
Starting program: .../config

Catchpoint 1 (call to syscall openat), __GI___open64_nocancel (file=file@entry=0x7ffff7ff121b
    "/etc/ld.so.cache", oflag=oflag@entry=524288)
    at ../sysdeps/unix/sysv/linux/open64_nocancel.c:39
39	../sysdeps/unix/sysv/linux/open64_nocancel.c: No such file or directory.
(gdb) x/s $rsi
0x7ffff7ff121b:	"/etc/ld.so.cache"
(gdb) continue
Continuing.

Catchpoint 1 (returned from syscall openat), __GI___open64_nocancel (...
...
Catchpoint 1 (call to syscall openat), __GI___open64_nocancel (file=file@entry=0x7ffff7fbb140
    "/lib/x86_64-linux-gnu/libc.so.6", oflag=oflag@entry=524288)
...
(gdb) info breakpoints
Num     Type           Disp Enb Address            What
1       catchpoint     keep y                      syscall "openat"
	catchpoint already hit 3 times
```

(gdb prints each `Catchpoint` report on one line; it is wrapped here.)

- `openat` is system call number 257. gdb reads the names from a table it ships
  (`/usr/share/gdb/syscalls/amd64-linux.xml`), so you can use either.
- The first opens are not the program's. Before `main`, the dynamic loader opens
  `/etc/ld.so.cache` to find libraries, then `libc.so.6`. gdb can name the loader's function
  because this machine has glibc's debug info (`libc6-dbg`); without it you would see `?? ()`.
- Every call stops twice: on the way in ("call to") and on the way out ("returned from"). Each
  stop counts as a hit, so three hits are one and a half `openat`s.
- On the way in, the arguments are in the system call registers: `rdi` is the directory
  (`AT_FDCWD`, "relative to the current directory"), `rsi` the path, `rdx` the flags. `x/s
  $rsi` reads the path, with or without debug info.

To skip the loader, stop at `main` and go on from there:

```console
(gdb) break main
Breakpoint 2 at 0x5555555552e7: file config.c, line 32.
(gdb) continue
...
Breakpoint 2, main () at config.c:32
32	{
(gdb) continue
Continuing.

Catchpoint 1 (call to syscall openat), 0x00007ffff7d1468b in __libc_open64 (file=0x555555556019
    "setings.conf", oflag=0) at ../sysdeps/unix/sysv/linux/open64.c:41
41	../sysdeps/unix/sysv/linux/open64.c: No such file or directory.
(gdb) bt
#0  0x00007ffff7d1468b in __libc_open64 (file=0x555555556019 "setings.conf", oflag=0)
    at ../sysdeps/unix/sysv/linux/open64.c:41
...
#5  0x000055555555522e in load (c=0x7fffffffe400, path=0x555555556019 "setings.conf")
    at config.c:19
#6  0x000055555555531a in main () at config.c:35
(gdb) continue
Continuing.

Catchpoint 1 (returned from syscall openat), 0x00007ffff7d1468b in __libc_open64 (...
41	in ../sysdeps/unix/sysv/linux/open64.c
(gdb) print $rax
$1 = -2
```

- The program asks for `setings.conf`, with one `t`. The backtrace leads from the kernel's
  door through four libc frames (#1–#4: `fopen` and its helpers) to `load` at `config.c:19`,
  and from there to the call in `main` on line 35 with the misspelled name.
- At the "returned" stop, `rax` holds the result. A negative value from -4095 to -1 is
  `-errno`, so -2 is `-ENOENT`: "No such file or directory". `fopen` turned it into `NULL` and
  `errno = ENOENT`, and `load` threw both away. Fixing the name gives `port 8080, 4 workers`.
- Read arguments at the entry stop (`x/s $rsi` there shows `"setings.conf"`) and results at
  the return. At the return `rsi` may still hold the pointer, but nothing promises it.

`catch syscall` also takes several names (`catch syscall openat close` gives `Catchpoint 1
(syscalls 'openat' [257] 'close' [3])`), numbers, and groups:

- A number gdb has no name for is accepted as is: `Catchpoint 2 (syscall 999)`. `catch syscall
  %process` says `Unknown syscall name '%process'.`: that is `strace` syntax (next section).
- gdb's groups are spelled `group:NAME`. `catch syscall group:file` sets one catchpoint on the 54
  system calls that take a file name: `Catchpoint 1 (syscalls 'open' [2] 'stat' [4] 'lstat' [6]
  'access' [21] 'execve' [59] ...`. On `config` it first stops in the loader's `access
  ("/etc/ld.so.preload")`. `close` and `read` are not in the group: they take a file
  descriptor, not a name.
- Other catchpoints stop at other events: `catch signal SIGSEGV`
  ([Chapter 5](05-crashes-and-post-mortem.md)), `catch fork` and `catch exec`
  ([Chapter 7](07-threads-and-processes.md)), and `catch throw` for C++ exceptions.

## strace

When the question is only "what does this program ask the kernel?", `strace` answers it in one
command. It prints every system call with decoded arguments and its result:

```console
$ strace -e trace=openat ./config
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
openat(AT_FDCWD, "/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
openat(AT_FDCWD, "setings.conf", O_RDONLY) = -1 ENOENT (No such file or directory)
port 80, 1 worker
+++ exited with 0 +++
$ strace -e trace=%file ./config 2>&1 | head -3
execve("./config", ["./config"], 0x7ffc57c0ee18 /* 44 vars */) = 0
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
```

The third line of the first run is the whole bug. `strace` writes to stderr, so its lines mix
with the program's own output on the terminal (`-o FILE` keeps them apart). `%file` is strace's
spelling of a group. Use `strace` to see what a program does with the OS, then `catch syscall`
when you need to stop at that moment and look around: the backtrace, the variables, the caller.
strace is built on the same kernel mechanism as gdb's catchpoint, `PTRACE_SYSCALL`, described
two sections down.

## How hardware watchpoints work

x86 has eight debug registers. Four matter here: DR0–DR3 hold addresses, and DR7 says what to
do with each one. DR6 reports which one fired.

![DR7 as gdb set it for `watch counter` and `rwatch limit`: 0xdf0105](images/ch04-dr7.png)

| DR7 bits | field | meaning |
|---|---|---|
| 0, 2, 4, 6 | L0–L3 | enable DR0–DR3 (local: for this task; the kernel switches them per thread) |
| 1, 3, 5, 7 | G0–G3 | global enable (left to the kernel) |
| 8 | LE | "exact" data breakpoints; no effect since the P6 family, Intel suggests setting it |
| 16+4n, 17+4n | R/W*n* | 00 execute, 01 write, 10 I/O ports, 11 read or write |
| 18+4n, 19+4n | LEN*n* | 00 = 1 byte, 01 = 2, 11 = 4, 10 = 8 |

- There is no "read only" condition. 11 is read *or* write, which is why `rwatch` has to guess.
- The lengths are not in size order: 8 bytes is `10`, 4 bytes is `11`. The 80386 had 1, 2 and 4
  bytes (00, 01, 11); 8 bytes came with 64-bit mode and got the free code, `10`.
- The address must be aligned to the length. The CPU ignores the low bits of the address when it
  compares: a 4-byte watch at `0x4019` would really watch `0x4018`–`0x401b`.
- When a watched access happens, the CPU finishes the instruction, sets bit B*n* (bits 0–3) in
  DR6 and raises a debug exception. The kernel turns that into a `SIGTRAP` for the process,
  which a tracer sees from `waitpid`. An `int3` breakpoint is also a trap, but there `rip` is
  one past the `0xCC`. After a watchpoint, `rip` is simply the next instruction, with nothing
  to undo.

gdb shows the registers it writes with `maint set show-debug-regs on`. This is the figure's
value, from `watch counter` and `rwatch limit` on `watched`:

```console
$ gdb -q -nx -batch -ex 'break main' -ex run -ex 'maint set show-debug-regs on' \
      -ex 'watch counter' -ex 'rwatch limit' -ex 'print &counter' -ex 'print &limit' \
      -ex continue build/targets/watched
...
$1 = (int *) 0x555555558018 <counter>
$2 = (int *) 0x555555558010 <limit>
...
insert_watchpoint (addr=0000555555558018, len=4, type=data-write):
	CONTROL (DR7): 0x0000000000df0105
	STATUS (DR6): 0x0000000000000000
	DR0: addr=0x0000555555558010, ref.count=1
	DR1: addr=0x0000555555558018, ref.count=1
```

Decode 0xdf0105: bits 0 and 2 (L0, L1) and bit 8 (LE) give the low `0x105`. R/W0 = 11 and
LEN0 = 11 (`limit`, 4 bytes, read or write) give `0xf` at bit 16. R/W1 = 01 and LEN1 = 11
(`counter`, 4 bytes, write) give `0xd` at bit 20. gdb inserted `limit` first, so it got DR0.

The debug registers are privileged: a user program can't run `mov %rax, %dr7`. A debugger asks
the kernel through `ptrace(PTRACE_POKEUSER, pid, offset, value)`. `offset` is the position of
`u_debugreg[n]` in `struct user` from `<sys/user.h>`, the same "user area" from which Chapter 3
read registers. The kernel checks every value: the address must be in user space and the
length and alignment must be valid. It loads the registers when it switches to the traced
thread. `PTRACE_PEEKUSER` at the same offsets reads them back, which is how a debugger reads
DR6.

## How system call catching works

`PTRACE_CONT` resumes a process until a signal. `PTRACE_SYSCALL` resumes it the same way, but
it also stops at the next system call *entry* or *exit*. A tracer that wants every system call
keeps resuming with `PTRACE_SYSCALL`. Three details make this work:

- **Telling the stops apart.** A system call stop is reported as `SIGTRAP`, the same signal as a
  breakpoint. With the option `PTRACE_O_TRACESYSGOOD` the kernel reports `SIGTRAP | 0x80`
  instead, so `WSTOPSIG(status) == (SIGTRAP | 0x80)` means "system call" and nothing else.
- **Entry or exit?** The two stops look the same. The classic answer is to count: the stops
  alternate, so the tracer flips a flag on each one. Linux 5.3 added
  `PTRACE_GET_SYSCALL_INFO`, which says so directly (exercise 2).
- **The registers.** At both stops `orig_rax` holds the system call number. The arguments are in
  `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`. That is the function-call order from Chapter 3, with
  `r10` in place of `rcx`, because the `syscall` instruction overwrites `rcx` (with the return
  address). At the entry stop, `rax` holds `-ENOSYS` (-38), which the kernel puts there in case
  nobody runs the call. At the exit, `rax` holds the result, and -4095 to -1 means `-errno`.

There is no breakpoint to insert and nothing in the program to patch. A catchpoint is just a
debugger that resumes with `PTRACE_SYSCALL` instead of `PTRACE_CONT` and filters the stops.
`strace` does the same and prints instead of stopping. That is also why every system call costs
two round trips through the tracer, even with only one catchpoint: the kernel stops at all of
them, and the tracer drops those it doesn't want.

## tdb, step 4: watch, rwatch, catch syscall

tdb v0.4 adds `watch LOC [LEN]`, `rwatch LOC [LEN]` and `catch syscall [NAME|NUMBER]`.
`project/ch04/README.md` lists what changed since Chapter 3:

- New: `src/debugreg.c`, `src/syscalls.c` with the generated `src/sysnames.h`
  (`tools/sysnames.sh`), targets `watched` and `files`, and `tests/test_debugreg.c`.
- `breakpoint.c`: one table for all kinds, each entry with a type, numbered together like gdb's.
- `proc.c`: `proc_syscall`, the `PTRACE_O_TRACESYSGOOD` option and an `in_syscall` flag.
- `run.c`: two new stop kinds, `STOP_WATCH` and `STOP_SYSCALL`.

### The debug registers

`debugreg.c` is 96 lines. DR7 is built by two pure functions, which can be unit-tested without
a process:

`project/ch04/src/debugreg.c` (part)
```c
/* Where debug register n is in struct user. */
#define DR_OFFSET(n) (offsetof(struct user, u_debugreg) + (n) * sizeof(unsigned long long))

/* The LEN field of DR7 for a length in bytes (Intel SDM, "Debug Registers").
 * Note 8 is 0b10, out of order: it was added last, for 64-bit mode. */
static uint64_t len_bits(int len)
{
    switch (len) {
    case 1: return 0;
    case 2: return 1;
    case 8: return 2;
    default: return 3;     /* 4 */
    }
}

uint64_t dr7_enable(uint64_t dr7, int slot, enum dr_kind kind, int len)
{
    dr7 = dr7_disable(dr7, slot);
    dr7 |= UINT64_C(1) << (2 * slot);                       /* L0..L3: local enable */
    dr7 |= (uint64_t)kind << (16 + 4 * slot);               /* R/W0..R/W3 */
    dr7 |= len_bits(len) << (18 + 4 * slot);                /* LEN0..LEN3 */
    return dr7;
}
```

`enum dr_kind` in `debugreg.h` holds the R/W codes: `{ DR_EXEC = 0, DR_WRITE = 1, DR_ACCESS =
3 }`. `dr7_disable` clears the slot's L and G bits and its four R/W and LEN bits. Enabling
starts with a disable, so a slot can be reused with a new kind or length. `dr_read` and
`dr_write` are a `PTRACE_PEEKUSER` and a `PTRACE_POKEUSER` at `DR_OFFSET(n)`. `dr_set` writes
the address into DR*n* first, then reads DR7, enables the slot and writes DR7 back, so a slot is
never enabled while it still holds an old address. `dr_clear` disables it again. The last
function tells which slot fired:

```c
int dr_hit(struct proc *p)
{
    uint64_t dr6;

    if (dr_read(p, 6, &dr6) < 0)
        return -1;
    /* The CPU never clears DR6 itself: do it, or the next stop looks like
     * this one again. */
    dr_write(p, 6, 0);
    for (int slot = 0; slot < DR_SLOTS; slot++)
        if (dr6 & (UINT64_C(1) << slot))
            return slot;
    return -1;
}
```

The comment is not decoration: Common mistakes shows what happens without the clear.

### One table for breakpoints, watchpoints and catchpoints

gdb numbers breakpoints, watchpoints and catchpoints in one sequence, and `delete 3` doesn't
care which kind 3 is. tdb now does the same. `struct breakpoint` gets a type (`BP_BREAK`,
`BP_WATCH`, `BP_RWATCH`, `BP_SYSCALL`) and a few fields: `len`, `slot` (the debug register),
`old` (the value at the last stop) and `sysno` (-1 for any system call). `bp_insert` and
`bp_remove` dispatch on the type. A breakpoint writes `0xCC`, a watchpoint calls `dr_set` and
reads the current value into `old`, and a catchpoint has nothing to insert. Before anything
reaches the kernel, `bp_add_watch` refuses what the hardware can't do: a length other than 1, 2,
4 or 8, an address with `addr % len != 0`, and a fifth watchpoint (`free_slot()` returns -1).
Unlike gdb, tdb picks the slot at `watch` time, so you hear about the limit at once. `LEN`
defaults to the symbol's size from the ELF symbol table (Chapter 3) when that is 1, 2, 4 or 8,
and to 8 otherwise, so a watchpoint on an `int` covers just that `int`.

### Stopping on a watchpoint

A watchpoint arrives as a plain `SIGTRAP`, like an `int3`. So on every `SIGTRAP`,
`run_continue` calls `check_watch` first and looks for an `int3` at `rip - 1` only if that
returns 0:

`project/ch04/src/run.c` (part)
```c
static int check_watch(struct proc *p, struct stop *st)
{
    int slot = dr_hit(p);
    if (slot < 0)
        return 0;
    struct breakpoint *bp = bp_by_slot(slot);
    if (bp == NULL)
        return 0;

    uint64_t value;
    if (bp_watch_value(p, bp, &value) < 0)
        return -1;
    uint64_t old = bp->old;
    bp->old = value;
    /* Like gdb, a write watchpoint only stops if the value changed. */
    if (bp->type == BP_WATCH && value == old)
        return 2;
    if (!count_hit(bp))
        return 2;
    ...
```

- 0 means "not a watchpoint", 1 means "stop" and 2 means "a watchpoint, but go on". 2 covers
  a write of the same value (`step(0)`) and a hit still skipped by `ignore` (Chapter 2).
- The hardware says only *that* the memory was touched. The old and new values come from
  tdb's own copy in `bp->old` and a fresh read.
- `step_over_breakpoint` calls `check_watch` too. A store can sit right at a breakpoint, and
  then the watchpoint trap arrives with the single-step's.

### Stopping at a system call

`proc_launch` now sets `PTRACE_O_TRACESYSGOOD` next to `PTRACE_O_EXITKILL` in its
`PTRACE_SETOPTIONS` call, so a system call stop can't be mistaken for a breakpoint. The old
body of `proc_continue` becomes `resume(p, request)`, shared by `proc_continue` (`PTRACE_CONT`)
and the new `proc_syscall` (`PTRACE_SYSCALL`). `run_continue` resumes with
`bp_catching_syscalls() ? proc_syscall(p) : proc_continue(p)`, so a program with no
catchpoints runs without system call stops. A `SIGTRAP | 0x80` goes to `check_syscall`:

`project/ch04/src/run.c` (part)
```c
    p->in_syscall = !p->in_syscall;
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    /* rax is overwritten by the result; the kernel keeps the number in
     * orig_rax. */
    struct breakpoint *bp = bp_syscall((long)regs.orig_rax);
    if (bp == NULL || !count_hit(bp))
        return 0;
    ...
    st->entry = p->in_syscall;
```

It then copies `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` into `st->args` and `rax` into
`st->ret`. The flag flips on *every* system call stop, wanted or not, or it would lose count.
`proc_continue` and `proc_step` reset it: a `PTRACE_CONT` from an entry stop runs through the
exit without stopping there.

Names come from `src/sysnames.h`, a table of about 360 names that `tools/sysnames.sh`
generates from the kernel header `asm/unistd_64.h` with one `awk` line: each `#define
__NR_write 1` becomes `[1] = "write",`. `sys_name(nr)` looks a number up and `sys_number`
goes the other way, also accepting a plain number (`catch syscall 1`). A result from -4095 to
-1 is printed with `strerrorname_np` and `strerror` (glibc 2.32 and later).

### Sessions

Watchpoints on `watched`, from `project/ch04`:

```console
$ make
$ build/tdb build/targets/watched
Reading symbols from build/targets/watched: 21 symbols.
(tdb) watch counter
Hardware watchpoint 1: counter (4 bytes at 0x4018, DR0)
(tdb) rwatch limit
Hardware read watchpoint 2: limit (4 bytes at 0x4010, DR1)
(tdb) run
Started process 521665: build/targets/watched
Hardware watchpoint 1: counter
Old value = 0 (0x0)
New value = 2 (0x2)
0x000055555555516f in step+38 ()
(tdb) x step+32 6
0x0000555555555169  89 05 a9 2e 00 00                                 |......|  <step+32>
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
```

- The size, 4 bytes, came from the symbol table. The addresses are file addresses, as for
  breakpoints, and `bp_insert` adds the load base when `run` writes the registers.
  `info breakpoints` shows the new Type column: `watchpoint` and `rwatch`, with
  `counter (4 bytes, DR0)` under What.
- `step(0)`'s store did not stop: the value stayed 0. The two that did stop report `step+38`.
  The six bytes at `step+32` are the store, `mov %eax,0x2ea9(%rip)` (`objdump -d` shows `1169:
  mov %eax,0x2ea9(%rip) # 4018 <counter>`). The report comes one instruction later, as in gdb.
- tdb's DR7 for these two commands is `0xd0001 | 0xf00004 = 0xfd0005`: `counter` in slot 0,
  `limit` in slot 1, no LE. gdb's `0xdf0105` differs only in slot order and the LE bit.

`config` again, run with tdb from `examples/ch04`. tdb has no debug info for libc, so the stops
say `?? ()`, but the arguments and the result tell the story anyway:

```console
$ ../../project/ch04/build/tdb ./config
Reading symbols from ./config: 19 symbols.
(tdb) catch syscall openat
Catchpoint 1 (syscall 'openat' [257])
(tdb) run
Started process 520768: ./config
Catchpoint 1 (call to syscall openat), 0x00007ffff7fe9b18 in ?? ()
    openat(0xffffff9c, 0x7ffff7ff121b, 0x80000)
(tdb) c
Catchpoint 1 (returned from syscall openat), 0x00007ffff7fe9b18 in ?? ()
    = 3 (0x3)
...
Catchpoint 1 (call to syscall openat), 0x00007ffff7d1468b in ?? ()
    openat(0xffffff9c, 0x555555556019, 0x0)
(tdb) x $rsi 16
0x0000555555556019  73 65 74 69 6e 67 73 2e  63 6f 6e 66 00 00 73 00  |setings.conf..s.|
(tdb) reg orig_rax
orig_rax = 0x101 (257)
(tdb) reg rax
rax = 0xffffffffffffffda (-38)
(tdb) c
Catchpoint 1 (returned from syscall openat), 0x00007ffff7d1468b in ?? ()
    = -2 (ENOENT: No such file or directory)
```

- `0xffffff9c` is `AT_FDCWD`, -100, as a 32-bit `int`. glibc passes the argument as an `int`,
  so only the low half of `rdi` is set, and tdb prints the full 64-bit register in hex without
  knowing the type. `0x80000` is `O_CLOEXEC`, and `0x0` is `O_RDONLY`.
- At the entry stop `rax` is -38 (`-ENOSYS`) and `orig_rax` is 257. At the exit `rax` is -2.
- The loader's two calls (the second, elided, opens `libc.so.6`) share one address: the loader
  has its own `syscall` instruction. The program's `fopen` uses libc's.

On the `files` target, `catch syscall write` stops at `write(0x1, 0x7fffffffe590, 0x1c)` and
its return `= 28 (0x1c)`: all 28 bytes written. A `catch syscall` with no name catches every
system call (`Catchpoint 2 (any syscall)`), and the last one is `exit_group(0x0, 0xe7, 0x3c)`.
`exit_group` takes one argument; the other two registers are leftovers from libc's `_exit`,
which keeps the numbers of `exit_group` (0xe7) and `exit` (0x3c) there. The process ends
inside `exit_group`, so that call has no return stop.

The other new messages, each from a real session on `watched`: `watch counter 3` says `A
watchpoint covers 1, 2, 4 or 8 bytes, not 3.`, and `watch counter+1 4` says `0x4019 is not
aligned to 4 bytes.` After four good watchpoints (DR0–DR3), a fifth says `No free debug
register: x86 has only 4.` `catch syscall nosuch` says `Unknown syscall name 'nosuch'.`, and
`catch syscall 1` says `Catchpoint 5 (syscall 'write' [1])`.

tdb's `rwatch` is simpler than gdb's. It uses the access condition, stops on every access,
reads and writes alike, and reports `Value =`. Exercise 1 adds `awatch` with gdb's report.

**Tests.** `make test` passes 14 unit and 29 integration tests. The unit tests check DR7
encodings (`dr7_enable(0, 0, DR_WRITE, 4)` is `0xd0001`, `(0, 1, DR_ACCESS, 8)` is
`0xb00004`), reusing a slot, and the name table both ways. The integration tests cover
`watch`, `rwatch`, a breakpoint on the store itself, the limits, `catch syscall` with an
`ENOENT`, deleting a catchpoint at an entry stop, and the error messages.

## Common mistakes

**Not clearing DR6.** The CPU sets B0–B3 in DR6 but never clears them; that is the debugger's
job. Here is tdb with the `dr_write(p, 6, 0)` removed from `dr_hit`, after `break step` (called
six times) and `watch counter`:

```console
(tdb) run
Started process 1397548: build/targets/watched
Breakpoint 1, 0x0000555555555149 in step ()
(tdb) c
Hardware watchpoint 2: counter
Old value = 0 (0x0)
New value = 2 (0x2)
0x000055555555516f in step+38 ()
(tdb) c
...
(tdb) c
counter=6 limit=3
[process exited with status 0]
```

The breakpoint stopped once instead of six times. `step(0)` stored an unchanged 0, which set B0.
From then on every `int3` `SIGTRAP` looked like a watchpoint hit with an unchanged value,
`check_watch` returned "go on", and the breakpoint was silently skipped. The fixed tdb stops at
`step` six times, with the two watchpoint stops in between.

**Looking at the reported line for the write.** The store has already run, and `rip` is the
next instruction: `step+38`, not `step+32`; line 35, not line 34. Look one back.

**LEN in size order.** Encoding 8 bytes as `11` gives you a 4-byte watchpoint without any error.
The encodings are 00, 01, 11, 10 for 1, 2, 4, 8 bytes.

**Unaligned addresses.** Linux refuses to enable an unaligned slot, and the error is a bare
`EINVAL`. With tdb's alignment check taken out, `watch counter+1 4` on a running process gives
`Cannot set debug register 0.`, and set before `run` it gives `Warning: cannot insert
breakpoint 1 at 0x555555558019.` Neither says why. Check in the debugger and say why.

**Losing count of entry and exit.** Delete a catchpoint at an entry stop and resume with
`PTRACE_CONT`. The system call finishes with no exit stop. If the flag is not reset, it is now
inverted. Here is tdb with the `in_syscall = false` lines removed from `proc_continue` and
`proc_step`. On `files`, `catch syscall openat` and `run` stop at the loader's first `openat`
entry; then `delete 1`, `break main` and `continue` reach `main`:

```console
(tdb) catch syscall write
Catchpoint 3 (syscall 'write' [1])
(tdb) continue
Catchpoint 3 (returned from syscall write), 0x00007ffff7d149d7 in ?? ()
    = -38 (ENOSYS: Function not implemented)
```

The entry is reported as a return. Its "result" is the -38 the kernel leaves in `rax` at every
entry. The correct tdb says `call to syscall write` and prints the three arguments. The
integration tests would not have caught this. Counting is fragile; exercise 2 asks the
kernel instead.

**Watching through a local without `-l`.** The watchpoint dies when the function returns,
usually before the bug happens.

## In the wild: debug registers and strace

**Debug registers.** Intel put them in the 80386: four address registers DR0–DR3, a
status register DR6 and a control register DR7, with an R/W field and a length of 1, 2 or 4
bytes per address. The 80386 Programmer's Reference Manual describes them in §12.2. The current
Intel SDM (Volume 3B, "Debug Registers") has the same four, and allows a length of 8 bytes in
64-bit mode, encoded `10`, the one code that had been left over. Every x86 debugger with
hardware watchpoints, gdb included, programs these registers.

**strace.** `strace`'s COPYING file begins with "Copyright (c) 1991, 1992 Paul Kranenburg". It
was written for SunOS and ported to Linux by Branko Lankester. According to the HISTORY section
of `strace(1)`, Rick Sladkey merged and extended the versions in 1993, Wichert Akkerman
maintained it from 1996, Roland McGrath from 2002, and it has been maintained by Dmitry Levin
since 2009. On Linux it is built on the same `ptrace(PTRACE_SYSCALL)` loop as tdb's
catchpoints.

## Summary

- `watch EXPR` stops when the value changes, just after the write: look at the line before.
  `rwatch` stops on reads and `awatch` on both. `watch -l` watches the address, past the scope.
- x86 has four debug registers of 1, 2, 4 or 8 aligned bytes; gdb says "too many" at
  `continue`. Software watchpoints single-step and were 20 times slower here.
- `catch syscall NAME|NUMBER|group:G` stops at the call and at the return. Read arguments in
  `rdi rsi rdx r10 r8 r9` on the way in and the result in `$rax` on the way out (-1…-4095 is
  `-errno`). `strace -e trace=…` prints the same calls without stopping.
- A debugger sets DR0–DR3 and DR7 with `PTRACE_POKEUSER` and finds the hit in DR6, which it must
  clear. It catches system calls with `PTRACE_SYSCALL` (stops marked `SIGTRAP | 0x80` by
  `PTRACE_O_TRACESYSGOOD`), counts entry and exit, and finds the number in `orig_rax`.
- tdb v0.4 has `watch`, `rwatch` and `catch syscall` in one numbered table.

## Exercises

1. **awatch in tdb.** Add `awatch LOC [LEN]`. Like `rwatch`, it uses the read-or-write
   condition, but it reports like gdb: `Old value`/`New value` when the value changed, and
   `Value =` otherwise. Run it on `counter` in `watched` and explain each stop.

   <details><summary>Solution</summary>

   Add `BP_AWATCH` to `enum bp_type`, before `BP_SYSCALL`. In `bp_insert` and `bp_remove`, the
   test for "a watchpoint" becomes `bp->type != BP_BREAK && bp->type != BP_SYSCALL`; `dr_set`
   already gets `DR_ACCESS` for anything but `BP_WATCH`, and `check_watch` already stops on
   every access for anything but `BP_WATCH`. Add `[BP_AWATCH] = "awatch"` to the type names of
   `info breakpoints`, dispatch `awatch` to `cmd_watch(arg1, arg2, BP_AWATCH)`, and say
   `access (read/write) ` in its messages. `report()` gets one case before the `Value =` one:

   ```c
           } else if (bp->type == BP_AWATCH && st->old != st->value) {
               printf("Hardware access (read/write) watchpoint %d: %s\n", bp->id, bp->what);
               printf("Old value = %s\n", int_text(st->old, (uint64_t)bp->len));
               printf("New value = %s\n", int_text(st->value, (uint64_t)bp->len));
           } else {
               printf("Hardware %s watchpoint %d: %s\n",
                      bp->type == BP_AWATCH ? "access (read/write)" : "read", bp->id, bp->what);
               printf("Value = %s\n", int_text(st->value, (uint64_t)bp->len));
           }
   ```

   ```console
   (tdb) awatch counter
   Hardware access (read/write) watchpoint 1: counter (4 bytes at 0x4018, DR0)
   (tdb) run
   Started process 1393955: build/targets/watched
   Hardware access (read/write) watchpoint 1: counter
   Value = 0 (0x0)
   0x0000555555555164 in step+27 ()
   (tdb) c
   Hardware access (read/write) watchpoint 1: counter
   Value = 0 (0x0)
   0x000055555555516f in step+38 ()
   (tdb) c
   Hardware access (read/write) watchpoint 1: counter
   Value = 0 (0x0)
   0x0000555555555164 in step+27 ()
   (tdb) c
   Hardware access (read/write) watchpoint 1: counter
   Old value = 0 (0x0)
   New value = 2 (0x2)
   0x000055555555516f in step+38 ()
   ```

   `step+27` is just after the load `mov 0x2eb4(%rip),%edx` at `step+21`, and `step+38` just
   after the store. `step(0)` gives a read and an unchanged store; `step(2)` and `step(4)` give a
   read and a store each (2→6 comes next). The last stop, `Value = 6` at `main+53`, is `main`
   reading `counter` for `printf`. `make test` still passes 29/29.
   </details>

2. **Ask the kernel: entry or exit.** Replace the `in_syscall` toggle with
   `PTRACE_GET_SYSCALL_INFO` (Linux 5.3; `struct __ptrace_syscall_info` in glibc's
   `<sys/ptrace.h>`). Remove the resets from `proc_continue` and `proc_step`, and check that the
   session from "Losing count of entry and exit" now says `call to syscall write`.

   <details><summary>Solution</summary>

   In `check_syscall` (`run.c` needs `<string.h>` and `<sys/ptrace.h>`):

   ```c
       struct __ptrace_syscall_info info;
       struct user_regs_struct regs;

       /* Ask the kernel whether this is an entry or an exit stop. */
       if (ptrace(PTRACE_GET_SYSCALL_INFO, p->pid, (void *)sizeof info, &info) < 0 ||
           proc_get_regs(p, &regs) < 0)
           return -1;
       p->in_syscall = info.op == PTRACE_SYSCALL_INFO_ENTRY;
       ...
       if (st->entry)
           memcpy(st->args, info.entry.args, sizeof st->args);
       else
           st->ret = info.exit.rval;
   ```

   The third argument is the buffer size, so old and new kernels agree on how much to fill in.
   `op` is `PTRACE_SYSCALL_INFO_ENTRY`, `_EXIT` or `_SECCOMP`, or `_NONE` when the stop is not
   a system call stop. `info.entry.nr` could replace `orig_rax` too. Then delete the two
   `p->in_syscall = false;` lines from `proc.c`. The session ends as it should:

   ```console
   (tdb) catch syscall write
   Catchpoint 3 (syscall 'write' [1])
   (tdb) continue
   Catchpoint 3 (call to syscall write), 0x00007ffff7d149d7 in ?? ()
       write(0x1, 0x7fffffffe590, 0x1c)
   ```

   All 29 integration tests still pass.
   </details>

3. **Arguments like strace.** At an `openat` entry, print `openat(AT_FDCWD, "setings.conf",
   O_RDONLY)` instead of three hex numbers: read the path from the process, and decode the
   directory and the flags.

   <details><summary>Solution</summary>

   First a `read_string(addr, buf, size)` that calls Chapter 3's `proc_read` for 8 bytes at a
   time and stops at the first NUL. A fixed 256-byte read could run off the end of a mapped
   page and fail. Then, in `tdb.c` (with `#include <fcntl.h>`):

   ```c
   static void print_openat(const uint64_t *args)
   {
       static const struct { int bit; const char *name; } flags[] = {
           { O_CREAT, "O_CREAT" }, { O_EXCL, "O_EXCL" }, { O_TRUNC, "O_TRUNC" },
           { O_APPEND, "O_APPEND" }, { O_NONBLOCK, "O_NONBLOCK" },
           { O_DIRECTORY, "O_DIRECTORY" }, { O_CLOEXEC, "O_CLOEXEC" },
       };
       static const char *const modes[] = { "O_RDONLY", "O_WRONLY", "O_RDWR", "3" };
       char path[256];
       int dirfd = (int)args[0], fl = (int)args[2];

       read_string(args[1], path, sizeof path);
       if (dirfd == AT_FDCWD)
           printf("    openat(AT_FDCWD, \"%s\", %s", path, modes[fl & O_ACCMODE]);
       else
           printf("    openat(%d, \"%s\", %s", dirfd, path, modes[fl & O_ACCMODE]);
       for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++)
           if (fl & flags[i].bit)
               printf("|%s", flags[i].name);
       if (fl & O_CREAT)
           printf(", 0%" PRIo64, args[3]);
       printf(")\n");
   }
   ```

   The cast to `int` is what makes `0xffffff9c` equal `AT_FDCWD`. `print_syscall` calls it for
   an entry whose name is `"openat"`:

   ```console
   Catchpoint 1 (call to syscall openat), 0x00007ffff7fe9b18 in ?? ()
       openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC)
   ...
   Catchpoint 1 (call to syscall openat), 0x00007ffff7fe9b18 in ?? ()
       openat(AT_FDCWD, "/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY|O_CLOEXEC)
   ...
   Catchpoint 1 (call to syscall openat), 0x00007ffff7d1468b in ?? ()
       openat(AT_FDCWD, "setings.conf", O_RDONLY)
   (tdb) c
   Catchpoint 1 (returned from syscall openat), 0x00007ffff7d1468b in ?? ()
       = -2 (ENOENT: No such file or directory)
   ```

   These are the lines strace printed. strace has decoders like this for hundreds of calls.
   </details>

4. **`run < FILE`.** tdb reads its commands from stdin, so it can't give `sensor` its readings.
   Add `run < FILE`, then find the write to `alarm_level` with tdb. tdb has no types, so work
   out the address from `rdi` at `filter_add`.

   <details><summary>Solution</summary>

   The redirection must happen in the child, after `fork` and before `execv`, as a shell does
   it. `proc_launch` gets an `input` argument (`proc.c` needs `<fcntl.h>`):

   ```c
           /* run < FILE: the shell's redirection, done by hand. */
           if (input != NULL) {
               int fd = open(input, O_RDONLY);
               if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) {
                   fprintf(stderr, "tdb: %s: %s\n", input, strerror(errno));
                   _exit(127);
               }
               close(fd);
           }
   ```

   `cmd_run(arg1, arg2)` accepts nothing, or `<` and a file name; anything else prints `Usage:
   run [< FILE]`. A missing file shows `tdb: nosuch.txt: No such file or directory` and then
   `Could not start ./sensor.`

   At the start of `filter_add`, `rdi` holds `f`. `alarm_level` follows five `int`s, at offset
   20 (gdb's `ptype/o struct filter` says so). Here `rdi` was `0x7fffffffece0`, so watch
   `0x7fffffffecf4`:

   ```console
   (tdb) break filter_add
   Breakpoint 1 at 0x1203
   (tdb) run < readings.txt
   Started process 1394175: ./sensor
   Breakpoint 1, 0x0000555555555203 in filter_add ()
   (tdb) reg rdi
   rdi = 0x7fffffffece0 (140737488350432)
   (tdb) watch 0x7fffffffecf4 4
   Hardware watchpoint 2: 0x7fffffffecf4 (4 bytes at 0x7fffffffecf4, DR0)
   (tdb) delete 1
   (tdb) c
   Hardware watchpoint 2: 0x7fffffffecf4
   Old value = 30 (0x1e)
   New value = 21 (0x15)
   0x0000555555555226 in filter_add+35 ()
   (tdb) c
   Hardware watchpoint 2: 0x7fffffffecf4
   Old value = 21 (0x15)
   New value = 21845 (0x5555)
   0x00007ffff7c4539a in ?? ()
   ```

   These are the same two stops as gdb's `watch -l`: the bad store in `filter_add`, and libc
   reusing the slot after `main` returned. The stack address depends on the environment, so
   take `rdi` from your own run.
   </details>

Sources for this chapter: [REFERENCES.md](REFERENCES.md#chapter-4--watchpoints-and-catchpoints)
