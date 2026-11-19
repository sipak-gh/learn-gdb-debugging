# Chapter 1 — Getting Started

learn-c, Chapter 10 taught the gdb commands you use every day: `run`, `bt`, `print`, `next`,
`step`, `finish`, a breakpoint, a watchpoint, a core file. This course goes further in two
directions at once. The first is gdb itself: every chapter takes a small buggy program and
uses more of gdb to find the bug. This chapter is about how a session starts. It covers
`start`, `starti`, program arguments, exit codes, `info proc`, and what `-g`, `-O0`, `-Og` and
`-O2` do to what gdb can show you. The second direction is how a debugger works. You will
build one, `tdb`, a little at a time in `project/`. At the end of this chapter tdb can start a
program under the kernel's `ptrace` interface, let it run, and report how it stopped or
ended, the same way gdb does underneath.

Next: [Chapter 2](02-breakpoints.md).

## What you need

gcc 11.4, GNU make 4.3, gdb 12.1, strace 5.16 (exercise 3) and, optionally, `libc6-dbg` 2.35
(debug info for glibc, so backtraces through libc show source lines). On Ubuntu 22.04:

```console
$ sudo apt install build-essential gdb strace libc6-dbg
```

Every example directory builds with `make` and cleans with `make clean`. The course runs on
Linux x86-64 only: tdb uses Linux's `ptrace` and x86-64 registers directly. If gdb asks
`Enable debuginfod for this session? (y or [n])`, answer `n` (or put
`set debuginfod enabled off` in `~/.gdbinit`): the transcripts here were made without it.

## What a debugger is

A debugger is a normal program that controls another program: it starts it or attaches to
it, stops it, reads and writes its memory and registers, and lets it go again. One process
cannot look inside another, so the kernel does this on the debugger's behalf. On Linux the
interface is one system call, `ptrace(2)` ("process trace"). The debugger is the *tracer* and
the program is the *tracee*. Roughly, gdb has three parts:

- **Target control**: start, stop, resume, single-step and kill the program; read and write
  memory and registers; insert breakpoints. On Linux this is `ptrace` and `waitpid`.
- **A symbol reader**: the symbol table and the *debug information* (DWARF, Chapter 6). That
  is how gdb knows that `0x119c` is line 10 of `avg.c` and `sum` is an `int` at `-0x14(%rbp)`.
- **An expression evaluator**: `print (long)balance * basis_points` is parsed as C and
  evaluated against the stopped program's memory, using the types from the debug info.

tdb will grow the same three parts. This chapter builds the first one.

## Two broken programs

`examples/ch01/avg.c`
```c
/* avg.c - print the average of the numbers on the command line.
 *
 *     ./avg 90 85 77      should print 84.00
 */
#include <stdio.h>
#include <stdlib.h>

static double average(int n, char *nums[])
{
    int sum = 0;

    for (int i = 0; i < n; i++)
        sum += atoi(nums[i]);
    return sum / n;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: avg NUMBER...\n");
        return 2;
    }
    printf("%.2f\n", average(argc, argv));
    return 0;
}
```

`examples/ch01/reverse.c`
```c
/* reverse.c - reverse the order of the words in each argument.
 *
 *     ./reverse "one two three"     should print "three two one"
 */
#include <stdio.h>
#include <string.h>

#define MAXWORDS 8

/* Split s in place at spaces; store pointers to the words in words[]. */
static int split(char *s, char *words[])
{
    int n = 0;

    for (char *w = strtok(s, " "); w != NULL; w = strtok(NULL, " "))
        words[n++] = w;
    return n;
}

static void print_reversed(char *s)
{
    char *words[MAXWORDS];
    int n = split(s, words);

    for (int i = n; i > 0; i--)
        printf("%s%s", words[i], i > 1 ? " " : "\n");
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
        print_reversed(argv[i]);
    return 0;
}
```

`examples/ch01/Makefile` builds both programs with `-g -O0`, and `avg.c` three more ways:

```console
$ cd examples/ch01
$ make
cc -std=c11 -Wall -Wextra -g -O0    avg.c   -o avg
cc -std=c11 -Wall -Wextra -g -O0    reverse.c   -o reverse
cc -std=c11 -Wall -Wextra -g -O2 -o avg-O2 avg.c
cc -std=c11 -Wall -Wextra -g -Og -o avg-Og avg.c
cc -std=c11 -Wall -Wextra -O0 -o avg-nodebug avg.c
$ ./avg 90 85 77
63.00
$ ./reverse "one two three"
Segmentation fault (core dumped)
$ echo $?
139
```

`avg` compiles without a warning and prints a wrong answer: (90 + 85 + 77) / 3 is 84.
`reverse` crashes. Exit status 139 is 128 + 11: the shell's way of saying "killed by signal
11, `SIGSEGV`". Later you will see the raw form of that number, the *wait status*, which is
what a debugger receives. Neither bug is hard; they are here so you can watch what gdb shows
you, and then what is underneath.

## Building for the debugger: -g, -O0, -Og, -O2

The Makefile builds `avg.c` four ways. First, what `-g` adds to the file:

```console
$ file avg avg-nodebug
avg:         ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), dynamically linked, interpreter /lib64/ld-linux-x86-64.so.2, BuildID[sha1]=..., for GNU/Linux 3.2.0, with debug_info, not stripped
avg-nodebug: ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), dynamically linked, interpreter /lib64/ld-linux-x86-64.so.2, BuildID[sha1]=..., for GNU/Linux 3.2.0, not stripped
$ readelf -S -W avg | grep -E 'debug|symtab'
  [28] .debug_aranges    PROGBITS        0000000000000000 00303d 000030 00      0   0  1
  [29] .debug_info       PROGBITS        0000000000000000 00306d 000364 00      0   0  1
  [30] .debug_abbrev     PROGBITS        0000000000000000 0033d1 00015f 00      0   0  1
  [31] .debug_line       PROGBITS        0000000000000000 003530 0000c4 00      0   0  1
  [32] .debug_str        PROGBITS        0000000000000000 0035f4 000294 01  MS  0   0  1
  [33] .debug_line_str   PROGBITS        0000000000000000 003888 0000e8 01  MS  0   0  1
  [34] .symtab           SYMTAB          0000000000000000 003970 0003c0 18     35  19  8
```

(The BuildID hash is shortened to `...`.) `-g` adds the six `.debug_*` sections, 2355 bytes of
DWARF for this tiny program. `.debug_info` describes the functions, variables and types.
`.debug_line` maps addresses to source lines. None of them is loaded into memory when the
program runs: they sit in the file for the debugger to read. The *symbol table* `.symtab` is
separate. It is there with or without `-g` (`avg-nodebug` has the same `.symtab`, and no
`.debug_*`) and lists function and global names with their addresses. `strip` removes both.

### Without -g

```console
$ gdb -q ./avg-nodebug
Reading symbols from ./avg-nodebug...
(No debugging symbols found in ./avg-nodebug)
(gdb) break average
Breakpoint 1 at 0x1191
(gdb) run 90 85 77
Starting program: .../avg-nodebug 90 85 77
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Breakpoint 1, 0x0000555555555191 in average ()
(gdb) info locals
No symbol table info available.
(gdb) list
1	../sysdeps/x86/abi-note.c: No such file or directory.
(gdb) next
Single stepping until exit from function average,
which has no line number information.
0x0000555555555249 in main ()
(gdb) bt
#0  0x0000555555555249 in main ()
```

- The two `libthread_db` lines appear in every session of a program linked with libc: gdb
  loads its thread-debugging helper. They are trimmed from the transcripts after this one.
- `break average` still works. The name comes from `.symtab`, not from the debug info. What is
  missing is everything that needs DWARF: no file and line, no arguments in
  `average ()`, no locals, and `next` cannot step by line.
- `list` printed line 1 of `../sysdeps/x86/abi-note.c`, a glibc file. A few bytes of startup
  code that glibc links into every program carry their own debug info, and with nothing else
  to go on, gdb picked it. A program contains more code than you wrote.
- In a stripped program even `break average` fails, and you are left with addresses.

### -O2: correct code, confusing session

```console
$ gdb -q ./avg-O2
Reading symbols from ./avg-O2...
(gdb) break average
Breakpoint 1 at 0x10bc: file avg.c, line 10.
(gdb) run 90 85 77
Starting program: .../avg-O2 90 85 77

Breakpoint 1, 0x00005555555550bc in average (nums=<optimized out>, n=<optimized out>) at avg.c:10
10	    int sum = 0;
(gdb) bt
#0  0x00005555555550bc in average (nums=<optimized out>, n=<optimized out>) at avg.c:10
#1  main (argc=4, argv=0x7fffffffe4f8) at avg.c:23
(gdb) info locals
sum = <optimized out>
sum = <optimized out>
i = <optimized out>
(gdb) next
13	        sum += atoi(nums[i]);
(gdb) next
12	    for (int i = 0; i < n; i++)
```

- `average` was *inlined*: gcc copied its body into `main`. Frame `#1` has no address, since
  no return address was pushed; gdb shows a frame for `average` anyway, because the DWARF
  records "these instructions came from inlining `average` at avg.c:23".
- The address `0x10bc` is inside `main`'s code: the first instruction of the inlined body.
- `<optimized out>` means the debug info has no location for that variable at this address:
  its register was reused, or the value never existed as such.
- `sum` appears twice (the inlined body has its own scopes), and the line order jumps
  (13, then 12) because the compiler reordered the loop.

### -Og: optimised for debugging

```console
$ gdb -q ./avg-Og
Reading symbols from ./avg-Og...
(gdb) break average
Breakpoint 1 at 0x1189: file avg.c, line 9.
(gdb) run 90 85 77
Starting program: .../avg-Og 90 85 77

Breakpoint 1, average (n=4, nums=0x7fffffffe4f8) at avg.c:9
9	{
(gdb) bt
#0  average (n=4, nums=0x7fffffffe4f8) at avg.c:9
#1  0x00005555555551f1 in main (argc=<optimized out>, argv=<optimized out>) at avg.c:23
```

`-Og` keeps optimisations that do not get in the debugger's way. `average` is a real call
again, its arguments are visible, and after a `next` or two `sum` and `i` are too. It stops at
line 9 (`{`), because there is no stack frame setup to skip. Some values are still gone:
`main`'s `argc` and `argv` are dead after the call, so their registers were reused.

| flags | what you get in gdb |
|---|---|
| no `-g` | function names from `.symtab`; no lines, types, arguments or locals |
| `-g -O0` | everything; every variable lives in memory; line order matches the source |
| `-g -Og` | almost everything; some dead values `<optimized out>` |
| `-g -O2` | inlining, reordering, many `<optimized out>`; correct but hard to follow |

The course uses `-g -O0`. Some bugs appear only at `-O2`; you can still debug those, but read
`<optimized out>` as "the compiler threw this away", not as a gdb fault.

## Starting a program: run, start, starti, arguments

```console
$ gdb -q ./avg
Reading symbols from ./avg...
(gdb) run
Starting program: .../avg
usage: avg NUMBER...
[Inferior 1 (process 1362017) exited with code 02]
(gdb) print $_exitcode
$1 = 2
(gdb) start 1 2
Temporary breakpoint 1 at 0x555555555208: file avg.c, line 19.
Starting program: .../avg 1 2

Temporary breakpoint 1, main (argc=3, argv=0x7fffffffe508) at avg.c:19
19	    if (argc < 2) {
(gdb) info inferiors
  Num  Description       Connection           Executable        
* 1    process 1362020   2 (native)           /home/tbangma/learn-gdb-debugging/examples/ch01/avg 
(gdb) info proc
process 1362020
cmdline = '/home/tbangma/learn-gdb-debugging/examples/ch01/avg 1 2'
cwd = '/home/tbangma/learn-gdb-debugging/examples/ch01'
exe = '/home/tbangma/learn-gdb-debugging/examples/ch01/avg'
(gdb) next
23	    printf("%.2f\n", average(argc, argv));
(gdb) next
1.00
24	    return 0;
(gdb) next
25	}
(gdb) kill
Kill the program being debugged? (y or n) y
[Inferior 1 (process 1362020) killed]
(gdb) starti
Starting program: .../avg 1 2

Program stopped.
0x00007ffff7fe3290 in _start () from /lib64/ld-linux-x86-64.so.2
(gdb) x/i $pc
=> 0x7ffff7fe3290 <_start>:	mov    %rsp,%rdi
(gdb) info sharedlibrary
From                To                  Syms Read   Shared Object Library
0x00007ffff7fc5090  0x00007ffff7fee315  Yes         /lib64/ld-linux-x86-64.so.2
```

Process IDs vary from run to run. What each command did:

- **`run ARGS`** starts the program. Without arguments, `avg` exited with 2. gdb reports the
  code in **octal**: `02` here, `012` for 10, and `exited normally` for 0.
- **`$_exitcode`** is a *convenience variable* that gdb sets when the program exits, so a
  script can test it. Printed values go into the history `$1`, `$2`, ... (more below).
- **`start ARGS`** is `tbreak main` followed by `run ARGS`: a one-time breakpoint on `main`.
  Note the address: `0x555555555208`. The program was already loaded from the previous
  `run`, so gdb shows the address in memory. In a fresh session it would print `0x1208`, the
  address in the file. Chapter 2 explains the difference.
- gdb remembers the arguments: `starti` and later `run`s reuse `1 2`. `show args` prints them,
  `set args` changes them.
- **`info inferiors`** lists the programs gdb controls (gdb calls the program being debugged
  the *inferior*). **`info proc`** shows what `/proc/PID` knows about the process.
- The second `next` printed `1.00`, the program's own output. The right answer is 1.50: a
  hint about the second bug in `avg`.
- **`kill`** ends the process, but gdb, the breakpoints and the arguments stay.
- **`starti`** stops at the very first instruction the process executes, in the *dynamic
  loader* `ld-linux-x86-64.so.2`. The kernel maps the program and the loader and starts the
  loader, which maps libc, resolves symbols and only then calls the program's `_start`. So
  `info sharedlibrary` lists only the loader.

This is the stop every debugger gets first: when a traced process calls `execve`, the kernel
stops it before its first instruction. tdb, later in this chapter, starts at exactly that
stop. gdb hides it unless you ask with `starti`.

## Finding the bug in avg

```console
$ gdb -q ./avg
Reading symbols from ./avg...
(gdb) break average
Breakpoint 1 at 0x119c: file avg.c, line 10.
(gdb) run 90 85 77
Starting program: .../avg 90 85 77

Breakpoint 1, average (n=4, nums=0x7fffffffe4f8) at avg.c:10
10	    int sum = 0;
(gdb) info args
n = 4
nums = 0x7fffffffe4f8
(gdb) print nums[0]
$1 = 0x7fffffffe847 "/home/tbangma/learn-gdb-debugging/examples/ch01/avg"
(gdb) print nums[1]
$2 = 0x7fffffffe87b "90"
(gdb) print *nums@n
$3 = {0x7fffffffe847 "/home/tbangma/learn-gdb-debugging/examples/ch01/avg", 0x7fffffffe87b "90", 
  0x7fffffffe87e "85", 0x7fffffffe881 "77"}
```

`n = 4` for three numbers is already the first bug. **`*nums@n`** is gdb's *artificial
array* syntax: start at `*nums` and show `n` elements, which is how you look at an array that
has decayed to a pointer. The first element is `argv[0]`, the program's own path. `atoi` of
a path is 0, so the sum is right and the count is one too many.

The strings are near `0x7fffffffe000`: the kernel copies the arguments and the environment
to the top of the new process's stack. The addresses depend on the path and your
environment, so yours differ, but under gdb they repeat from run to run (ASLR is off).

Keep going:

```console
(gdb) next
12	    for (int i = 0; i < n; i++)
(gdb) next
13	        sum += atoi(nums[i]);
(gdb) next
12	    for (int i = 0; i < n; i++)
(gdb) print sum
$4 = 0
(gdb) info frame
Stack level 0, frame at 0x7fffffffe3d0:
 rip = 0x5555555551ce in average (avg.c:12); saved rip = 0x555555555249
 called by frame at 0x7fffffffe3f0
 source language c.
 Arglist at 0x7fffffffe3c0, args: n=4, nums=0x7fffffffe4f8
 Locals at 0x7fffffffe3c0, Previous frame's sp is 0x7fffffffe3d0
 Saved registers:
  rbp at 0x7fffffffe3c0, rip at 0x7fffffffe3c8
(gdb) finish
Run till exit from #0  average (n=4, nums=0x7fffffffe4f8) at avg.c:12
0x0000555555555249 in main (argc=4, argv=0x7fffffffe4f8) at avg.c:23
23	    printf("%.2f\n", average(argc, argv));
Value returned is $5 = 63
(gdb) print $
$6 = 63
(gdb) print $ * 4
$7 = 252
```

- After the first loop pass `sum` is still 0. That is `atoi("/home/...")`.
- **`info frame`** describes the current stack frame. "frame at `0x7fffffffe3d0`" is the
  frame's *canonical frame address*, the value `rsp` had just before the `call`. `rip` is
  where the program is now. `saved rip = 0x555555555249` is the *return address*: the
  instruction in `main` after the `call average`, stored on the stack at `0x7fffffffe3c8`.
  `rbp` was pushed just below it. Chapter 3 draws this frame byte by byte. Chapter 6 shows
  how gdb finds frames without `rbp`.
- **`finish`** runs until `average` returns. It stops at `0x0000555555555249`, the saved `rip`
  from `info frame`. Then it prints the return value and saves it as `$5`.
- Every value gdb prints goes into the *value history*. **`$`** is the last one, `$$` the one
  before, and `$N` is entry N. `print $ * 4` is 252, the sum: 90 + 85 + 77 + 0.

Fixing the count (`average(argc - 1, argv + 1)`) makes `./avg 90 85 77` print 84.00, but
`./avg 1 2` still prints 1.00, as in the `start` session above. The second bug: `sum / n`
divides two `int`s, so 3 / 2 is 1, and only then is the result converted to `double`. Both
fixes:

```diff
-    return sum / n;
+    return (double)sum / n;
@@
-    printf("%.2f\n", average(argc, argv));
+    printf("%.2f\n", average(argc - 1, argv + 1));
```

```console
$ ./avg 90 85 77
84.00
$ ./avg 1 2
1.50
```

One input is never a test: the debugger found the bug you were looking at, and a second input
found the one hiding behind it.

## A crash in someone else's code: reverse

```console
$ gdb -q ./reverse
Reading symbols from ./reverse...
(gdb) run "one two three"
Starting program: .../reverse "one two three"

Program received signal SIGSEGV, Segmentation fault.
__strlen_avx2 () at ../sysdeps/x86_64/multiarch/strlen-avx2.S:74
74	../sysdeps/x86_64/multiarch/strlen-avx2.S: No such file or directory.
(gdb) bt
#0  __strlen_avx2 () at ../sysdeps/x86_64/multiarch/strlen-avx2.S:74
#1  0x00007ffff7c76cf1 in __vfprintf_internal (s=0x7ffff7e1b780 <_IO_2_1_stdout_>, 
    format=0x555555556008 "%s%s", ap=ap@entry=0x7fffffffe260, mode_flags=mode_flags@entry=0)
    at ./stdio-common/vfprintf-internal.c:1517
#2  0x00007ffff7c6079f in __printf (format=<optimized out>) at ./stdio-common/printf.c:33
#3  0x000055555555527d in print_reversed (s=0x7fffffffe872 "one") at reverse.c:26
#4  0x00005555555552d9 in main (argc=2, argv=0x7fffffffe4f8) at reverse.c:32
```

The crash is in glibc, inside `strlen`. That does not mean glibc has a bug. It almost never
does. It means glibc was given a bad pointer. Read the backtrace from the bottom up and find
the last frame in *your* code: `#3 print_reversed` called `printf`, and `printf` called
`strlen` on one of its `%s` arguments.

- glibc's file names and lines appear because `libc6-dbg` is installed. Without it, frames
  #0–#2 read like `0x00007ffff7c76cf1 in ?? () from /lib/x86_64-linux-gnu/libc.so.6`. "No
  such file or directory" only means the glibc *source* is not on disk. That is fine.
- glibc has several `strlen`s and picks the fastest for your CPU at startup. Yours may say
  `__strlen_evex` or `__strlen_sse2`. `ap@entry` is the value `ap` had on entry.

Select frame 3 and look around (two earlier `print`s are not shown):

```console
(gdb) frame 3
#3  0x000055555555527d in print_reversed (s=0x7fffffffe872 "one") at reverse.c:26
26	        printf("%s%s", words[i], i > 1 ? " " : "\n");
(gdb) info locals
i = 3
words = {0x7fffffffe872 "one", 0x7fffffffe876 "two", 0x7fffffffe87a "three", 
  0x1 <error: Cannot access memory at address 0x1>, 0x555555554040 "\006", 
  0x7ffff7fe283c <_dl_sysdep_start+1020> "H\213D$XH\203\304h[]A\\A]A^A_\303H\213=\241\242\001", 
  0x6f0 <error: Cannot access memory at address 0x6f0>, 
  0x7fffffffe819 "\354\367\326<b\245\001+}\263\206\315dV\234\334x86_64"}
n = 3
(gdb) print words[i]
$3 = 0x1 <error: Cannot access memory at address 0x1>
(gdb) print i == n
$4 = 1
```

- `words` holds three good pointers and then five uninitialised ones. The loop starts at
  `i = n`, one past the last word, and `words[3]` is whatever was on the stack before:
  `0x1`. `printf("%s", 0x1)` makes `strlen` read address 1, and the kernel sends `SIGSEGV`.
- Stack garbage is never random: it is old data from earlier calls. `_dl_sysdep_start+1020`
  is a return address left by the dynamic loader. The last pointer leads to the 16 random
  bytes the kernel puts below the argument strings, then the platform string `x86_64`.
- `print` and `info locals` work in the *selected* frame: frame 3 made `i`, `n` and `words`
  visible.

One more oddity: the argument is `s = "one"`, but the program was given `"one two three"`. In
a new session:

```console
(gdb) print *s@13
$1 = "one\000two\000three"
(gdb) up
#4  0x00005555555552d9 in main (argc=2, argv=0x7fffffffe4f8) at reverse.c:32
32	        print_reversed(argv[i]);
(gdb) print argv[1]
$2 = 0x7fffffffe872 "one"
(gdb) down
#3  0x000055555555527d in print_reversed (s=0x7fffffffe872 "one") at reverse.c:26
26	        printf("%s%s", words[i], i > 1 ? " " : "\n");
```

`strtok` splits a string *in place*: it writes a `'\0'` over each separator. So `s`, and
`argv[1]` in `main` (the same address), now read as `"one"`. The `@` syntax shows the bytes
behind the first NUL. `up` and `down` move one frame at a time (`frame 4`, `frame 3` here).

The fix makes the loop run from `n - 1` down to 0:

```diff
-    for (int i = n; i > 0; i--)
-        printf("%s%s", words[i], i > 1 ? " " : "\n");
+    for (int i = n - 1; i >= 0; i--)
+        printf("%s%s", words[i], i > 0 ? " " : "\n");
```

```console
$ ./reverse "one two three" "hello world"
three two one
world hello
```

`split` still has a bug of the same kind: `words[n++]` never checks `MAXWORDS`, so a ninth word
writes past the array. A debugger finds the crash you have. It does not find the one you have
not triggered yet. Chapter 4's watchpoints and Chapter 5's AddressSanitizer help there.

## How gdb runs your program: fork, PTRACE_TRACEME, exec

learn-c, Chapter 13 started programs with `fork` and `execvp` and collected their status with
`waitpid`. A debugger does the same with one extra step in the child:

1. **fork.** gdb makes a child process.
2. **`ptrace(PTRACE_TRACEME)`** in the child says "my parent traces me". From now on, any
   signal sent to the child stops it instead of being delivered, and the parent hears about
   the stop from `waitpid`, as it would about an exit.
3. **exec.** For a traced process, a successful `execve` stops it with `SIGTRAP` before the
   new program's first instruction: the stop `starti` showed.
4. The parent's `waitpid` returns. The program is loaded and has not run yet. The debugger
   can set breakpoints and resume it with `ptrace(PTRACE_CONT)`.

You can watch this from inside gdb:

```console
$ gdb -q ./avg
Reading symbols from ./avg...
(gdb) start 90 85 77
Temporary breakpoint 1 at 0x1208: file avg.c, line 19.
Starting program: .../avg 90 85 77

Temporary breakpoint 1, main (argc=4, argv=0x7fffffffe4f8) at avg.c:19
19	    if (argc < 2) {
(gdb) pipe info proc status | grep -E "^(Name|State|PPid|TracerPid)"
Name:	avg
State:	t (tracing stop)
PPid:	1365034
TracerPid:	1365034
(gdb) shell echo "gdb is process $PPID"
gdb is process 1365034
(gdb) show disable-randomization
Disabling randomization of debuggee's virtual address space is on.
(gdb) show startup-with-shell
Use of shell to start subprocesses is on.
```

- `pipe COMMAND | SHELL-COMMAND` filters a gdb command's output through the shell.
- `State: t (tracing stop)` is a stopped tracee. `TracerPid` is gdb (inside `shell`, `$PPID`
  is gdb's pid); an untraced process has `TracerPid: 0`, and some programs read this line to
  detect debuggers.
- **`startup-with-shell`**: gdb runs your shell with `-c "exec PROGRAM ARGS"`, so globs and
  redirections work in `run`. The shell execs your program into the same process (so `PPid`
  is still gdb), and gdb skips the shell's exec stop. tdb has no shell.
- **`disable-randomization`**: normally the kernel loads a position-independent executable
  (PIE), the libraries and the stack at random addresses (ASLR). gdb turns that off in the
  child with `personality(ADDR_NO_RANDOMIZE)`, so addresses repeat and PIEs land at
  `0x555555554000`.

```console
(gdb) info proc mappings
process 1365068
Mapped address spaces:

          Start Addr           End Addr       Size     Offset  Perms  objfile
      0x555555554000     0x555555555000     0x1000        0x0  r--p   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x555555555000     0x555555556000     0x1000     0x1000  r-xp   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x555555556000     0x555555557000     0x1000     0x2000  r--p   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x555555557000     0x555555558000     0x1000     0x2000  r--p   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x555555558000     0x555555559000     0x1000     0x3000  rw-p   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x7ffff7c00000     0x7ffff7c28000    0x28000        0x0  r--p   /usr/lib/x86_64-linux-gnu/libc.so.6
...
      0x7ffffffde000     0x7ffffffff000    0x21000        0x0  rw-p   [stack]
```

This is `/proc/PID/maps`, formatted; 15 lines (the rest of libc, three anonymous regions,
`[vvar]`, `[vdso]` and the loader) are cut at `...`. `avg` is mapped at `0x555555554000` in
five pieces: headers, code (`r-x`), read-only data, *RELRO* (made read-only after the loader
fixed up pointers) and writable data. libc follows at `0x7ffff7c00000`, and the stack ends at
`0x7ffffffff000`. Exercise 4 compares this list with the one at `starti`.

**Who may trace whom.** A tracer can read every secret in the tracee's memory, so Linux allows
it only for the same user (and no setuid), and the Yama security module adds a rule: at
`/proc/sys/kernel/yama/ptrace_scope` = 1, Ubuntu's default, a process may *attach* only to its
own descendants. `PTRACE_TRACEME` is always allowed below level 3, and that is how gdb and tdb
start programs. Chapter 5 attaches to running processes. A process can have only one tracer at
a time; exercise 3 shows what happens when there are two.

### The wait status

`waitpid` packs "what happened" into 16 bits. learn-c used the macros; a debugger author
needs the bits too, because they show up in logs:

| status | low byte | high byte | macros | meaning |
|---|---|---|---|---|
| `0x057f` | `0x7f` | `0x05` | `WIFSTOPPED`, `WSTOPSIG` = 5 | stopped by `SIGTRAP` (the exec stop) |
| `0x0a7f` | `0x7f` | `0x0a` | `WSTOPSIG` = 10 | stopped by `SIGUSR1` |
| `0x0b7f` | `0x7f` | `0x0b` | `WSTOPSIG` = 11 | stopped by `SIGSEGV` |
| `0x0300` | `0x00` | `0x03` | `WIFEXITED`, `WEXITSTATUS` = 3 | exited with code 3 |
| `0x008b` | `0x8b` | `0x00` | `WIFSIGNALED`, `WTERMSIG` = 11, `WCOREDUMP` | killed by `SIGSEGV`, core dumped |

Low byte `0x7f`: *stopped*, by the signal in the high byte. Low byte zero: *exited*, with the
code in the high byte. Anything else: *killed*, by the signal in the low 7 bits, and `0x80`
means a core was dumped. The shell's `139` is the same as 128 + 11. Only a traced process (or
one stopped by `SIGSTOP` and waited for with `WUNTRACED`) produces the "stopped" kind.

### Signals stop the tracee before delivery

```console
$ cd project/ch01 && make
$ gdb -q build/targets/exit3
Reading symbols from build/targets/exit3...
(gdb) run
Starting program: .../exit3

Program received signal SIGUSR1, User defined signal 1.
__pthread_kill_implementation (no_tid=0, signo=10, threadid=140737353811776) at ./nptl/pthread_kill.c:44
44	./nptl/pthread_kill.c: No such file or directory.
(gdb) continue
Continuing.
target: got SIGUSR1
[Inferior 1 (process 1365843) exited with code 03]
(gdb) run
Starting program: .../exit3
...                                      (the same SIGUSR1 stop)
(gdb) signal 0
Continuing with no signal.
[Inferior 1 (process 1365846) exited with code 03]
```

`targets/exit3.c` installs a `SIGUSR1` handler, calls `raise(SIGUSR1)` and returns 3. When the
signal arrives, the kernel stops the tracee *before* delivering it, and gdb reports "Program
received signal". The stop is inside glibc's `raise` (it calls `pthread_kill`). The handler has
not run yet.

What happens next is the tracer's choice. `continue` resumes and passes the signal on, so the
handler prints its line. **`signal 0`** resumes with no signal: the handler never runs, as if
the signal had not been sent. The kernel interface behind both is the last argument of
`ptrace(PTRACE_CONT, pid, 0, sig)`: the signal to deliver, or 0 for none. gdb's `handle`
command (Chapter 5) sets the default per signal.

`SIGSEGV` works the same way. In `build/targets/crash`, which writes through NULL, gdb stops
at `*p = 42;` with "Program received signal SIGSEGV". `continue` only delivers the signal,
and the default action kills the process: "Program terminated with signal SIGSEGV,
Segmentation fault." / "The program no longer exists."

## tdb, step 1: launching and waiting

![tdb, the kernel and the child during `tdb -v build/targets/exit3`: fork, PTRACE_TRACEME,
execv, the exec SIGTRAP stop (status 0x057f), PTRACE_CONT, the SIGUSR1 stop (0x0a7f),
PTRACE_CONT with SIGUSR1, the handler, exit(3) (0x0300); a box decodes the status
word](images/ch01-ptrace-lifecycle.png)

The figure is a real run of tdb v0.1, which is in `project/ch01/`: `src/tdb.c` (the command
loop and reporting), `src/proc.c` and `proc.h` (fork, `PTRACE_TRACEME`, exec, continue, wait,
kill), three programs to debug in `targets/` (`hello`, `exit3`, `crash`) and `tests/itest.sh`,
which runs scripted sessions and compares their output.

### The Makefile and proc.h

`project/ch01/Makefile` uses two sets of flags:

- **tdb itself** uses `-std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -pedantic -Werror`.
  learn-c used `-D_POSIX_C_SOURCE` to stay portable. A debugger cannot be portable: `ptrace`
  requests, `personality` and `sigabbrev_np` are Linux or glibc extensions, and `_GNU_SOURCE`
  declares them. `-g` lets you debug the debugger with gdb.
- **The targets** use `-g -O0 -fno-omit-frame-pointer`, the friendliest code to debug.

`project/ch01/src/proc.h` declares `struct proc`: everything tdb knows about the process.
That is its `pid`, a `state` (`PROC_NONE`, `PROC_STOPPED`, `PROC_EXITED`), the last wait
`status`, and `pending_sig`, a signal to pass on at the next resume. It also declares the
four operations `proc_launch`, `proc_continue`, `proc_wait` and `proc_kill`, and
`sig_name`, which turns 11 into `"SIGSEGV"` with glibc's `sigabbrev_np`.

### proc.c: start, resume, wait

`project/ch01/src/proc.c`
```c
int proc_wait(struct proc *p)
{
    int status;

    while (waitpid(p->pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("tdb: waitpid");
            return -1;
        }
    }
    if (proc_verbose)
        printf("[waitpid: status 0x%04x]\n", (unsigned)status);
    p->status = status;
    if (WIFSTOPPED(status)) {
        p->state = PROC_STOPPED;
    } else {
        p->state = PROC_EXITED;   /* WIFEXITED or WIFSIGNALED */
        p->pid = 0;
    }
    return 0;
}
```

`proc_wait` is the only place tdb learns anything about the process. With `-v` it prints the
raw status word, which is how you will see the table above in action. A stopped process stays
a process. An exited or killed one is gone (`waitpid` reaped it), so `pid` is cleared.

```c
int proc_launch(struct proc *p, char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("tdb: fork");
        return -1;
    }
    if (pid == 0) {
        /* The child: from here on, every signal it receives (and the exec
         * below) stops it and wakes its parent's waitpid(). */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
            perror("tdb: ptrace(TRACEME)");
            _exit(127);
        }
        /* Same addresses every run, as gdb does by default. */
        personality(ADDR_NO_RANDOMIZE);
        execv(argv[0], argv);
        fprintf(stderr, "tdb: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    p->pid = pid;
    p->pending_sig = 0;
    if (proc_wait(p) < 0)
        return -1;
    if (p->state != PROC_STOPPED || WSTOPSIG(p->status) != SIGTRAP) {
        /* exec failed: the child exited with 127 */
        p->state = PROC_NONE;
        return -1;
    }
    /* If tdb dies, the kernel kills the tracee too. */
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(long)PTRACE_O_EXITKILL);
    return 0;
}
```

This is the four steps from the previous section in C:

- `personality(ADDR_NO_RANDOMIZE)` is gdb's `disable-randomization`. The setting survives
  `execv`, so the program is loaded without ASLR. Exercise 2 makes it optional.
- `execv`, not `execvp`: exactly the path given, no `PATH` search, no shell.
- If `execv` returns, it failed: the child exits with 127, the shell's "command not found".
  `_exit`, not `exit`: the child is a copy of tdb and must not flush tdb's stdio buffers
  a second time. The parent expects the exec stop (stopped, by `SIGTRAP`); anything else
  means the exec failed.
- **`PTRACE_O_EXITKILL`** (Linux 3.8 and later) tells the kernel to kill the tracee if the
  tracer exits. Without it, a tdb that crashes would leave a traced, stopped program behind.
  The option is set here, at the first stop, because options can only be set on a stopped
  tracee.

```c
int proc_continue(struct proc *p)
{
    int sig = p->pending_sig;

    p->pending_sig = 0;
    /* The last argument of PTRACE_CONT is the signal to deliver. */
    if (ptrace(PTRACE_CONT, p->pid, NULL, (void *)(long)sig) < 0) {
        perror("tdb: ptrace(CONT)");
        return -1;
    }
    return proc_wait(p);
}
```

`ptrace`'s `data` argument has type `void *`, so the signal number is cast through `long`.
`pending_sig` is cleared once used: the signal is delivered once, not at every resume.
`proc_kill` sends `SIGKILL`, which no handler and no tracer can intercept, and reaps the
process with `waitpid`.

### tdb.c: the command loop

`project/ch01/src/tdb.c` (excerpt)
```c
static struct proc proc;
static char **prog_argv;

/* Say why the process stopped or ended. */
static void report(void)
{
    int st = proc.status;

    if (proc.state == PROC_STOPPED) {
        int sig = WSTOPSIG(st);
        printf("Program received signal %s (%s).\n", sig_name(sig), strsignal(sig));
        /* Like gdb, pass the signal on when the user continues. */
        proc.pending_sig = sig;
    } else if (WIFEXITED(st)) {
        printf("[process exited with status %d]\n", WEXITSTATUS(st));
    } else if (WIFSIGNALED(st)) {
        int sig = WTERMSIG(st);
        printf("[process killed by %s (%s)]\n", sig_name(sig), strsignal(sig));
    }
}

```

- `report` turns a wait status into words, using the three macros from the table. A stop
  remembers its signal in `pending_sig`, so the next `continue` delivers it, like gdb's
  `continue` (and unlike `signal 0`).
- `cmd_run` (not shown) kills a running process if there is one, calls `proc_launch`, which
  ends at the exec stop, prints "Started process N", and continues at once with
  `proc_continue` and `report`. tdb has no breakpoints yet, so there is nothing to do at the
  exec stop. gdb does the same unless you use `starti`.

The rest of `tdb.c` is a plain loop: print `(tdb) `, read a line with `fgets`, split it with
`strtok`, and dispatch `run`/`r`, `continue`/`c`, `kill`, `help` and `quit`/`q`. Two details
make it scriptable. `setvbuf(stdout, NULL, _IOLBF, 0)` flushes tdb's output line by line, so
it stays in order with the target's. And when stdin is not a terminal
(`!isatty(STDIN_FILENO)`), tdb echoes each command after the prompt, so a piped session reads
like a typed one. That is how every tdb transcript in this course is made, and how
`tests/itest.sh` checks them.

### Running it

```console
$ cd project/ch01
$ make            # compiler lines not shown
$ make test
tests/itest.sh build
itest: 6 passed, 0 failed
$ build/tdb
usage: tdb [-v] PROGRAM [ARGS...]
$ printf 'run\ncontinue\n' | build/tdb build/targets/hello
(tdb) run
Started process 1311711: build/targets/hello
hello from the target
[process exited with status 0]
(tdb) continue
The program is not being run.
(tdb) 
```

With `-v`, every status word is shown. `targets/crash.c` prints "about to crash" and writes
through a NULL pointer:

```console
$ printf 'run\ncontinue\n' | build/tdb -v build/targets/exit3
(tdb) run
[waitpid: status 0x057f]
Started process 1311716: build/targets/exit3
[waitpid: status 0x0a7f]
Program received signal SIGUSR1 (User defined signal 1).
(tdb) continue
target: got SIGUSR1
[waitpid: status 0x0300]
[process exited with status 3]
(tdb) 
$ printf 'run\ncontinue\n' | build/tdb -v build/targets/crash
(tdb) run
[waitpid: status 0x057f]
Started process 1311721: build/targets/crash
about to crash
[waitpid: status 0x0b7f]
Program received signal SIGSEGV (Segmentation fault).
(tdb) continue
[waitpid: status 0x008b]
[process killed by SIGSEGV (Segmentation fault)]
(tdb) 
```

Every line of the status table appears here: the exec stop `0x057f`; the `SIGUSR1` stop
`0x0a7f`, before the handler; `continue` passes `SIGUSR1`, the handler prints, and `main`
returns 3: `0x0300`. For `crash`, the `SIGSEGV` stop `0x0b7f`, then the signal is delivered
and the process is killed with a core dump: `0x008b`. These are the stops gdb reported for
the same programs, and now you know where they come from.

A program that cannot be started:

```console
$ printf 'run\n' | build/tdb build/targets/nope
(tdb) run
tdb: cannot run build/targets/nope: No such file or directory
Could not start build/targets/nope.
(tdb) 
```

The first line comes from the child after `execv` failed. The second comes from the parent,
which saw an exit with 127 instead of the exec stop.

## Common mistakes

**Debugging without `-g`.** You saw it with `avg-nodebug`: no lines, no variables,
`next` steps out of the whole function. If gdb says "(No debugging symbols found in ...)",
rebuild with `-g`. Check a binary with `file` ("with debug_info") before you start.

**Debugging an old binary.** Edit the source, forget to rebuild, and gdb shows the *new*
source next to the *old* code. With line 14 fixed in `avg.c` but the old `avg` still built,
a breakpoint there prints `warning: Source file is more recent than executable.` and then the
new line `return (double)sum / n;`, while `print sum / n` still gives 63: the program runs
the old code. gdb reads source files from disk when it shows them; the debug info only says
"line 14 of avg.c". The warning is the only hint, so do not ignore it.

**Building tdb without `_GNU_SOURCE`.** Leave `-D_GNU_SOURCE` out and `-Werror` makes the
Linux-only functions fatal:

```console
$ gcc -std=c11 -Wall -Wextra -pedantic -Werror -c src/proc.c -o /dev/null
src/proc.c: In function ‘sig_name’:
src/proc.c:18:26: error: implicit declaration of function ‘sigabbrev_np’ [-Werror=implicit-function-declaration]
src/proc.c:18:26: error: initialization of ‘const char *’ from ‘int’ makes pointer from integer without a cast [-Werror=int-conversion]
src/proc.c: In function ‘proc_kill’:
src/proc.c:102:5: error: implicit declaration of function ‘kill’ [-Werror=implicit-function-declaration]
cc1: all warnings being treated as errors
```

(Source excerpts are trimmed from the compiler output.) `-std=c11` asks for strict ISO C, and
glibc then hides everything outside ISO C, even POSIX's `kill`. Without `-Werror` this would
only warn, and the second error shows why that would be worse: an undeclared function is
assumed to return `int`, and a 64-bit pointer squeezed through an `int` crashes at run time.

**Expecting a signal to be gone after the stop.** "Program received signal SIGSEGV" does not
mean the signal was handled. It is still pending, and `continue` delivers it. To go on as if
it never happened, use `signal 0` in gdb. tdb v0.1 always delivers it.

## In the wild: gdb's history

The gdb manual's "Contributors" section names Richard Stallman as the original author of GDB
and says the changes before version 2.0 are "lost in the mists of time". In a lecture at the
Royal Institute of Technology (KTH) in Stockholm on 30 October 1986, Stallman described the
GNU tools he had written so far:

> I developed a debugger which I called GDB which is a symbolic debugger for C programs [...]
> to a large extent in the spirit of DBX, which is a debugger that comes with Berkeley Unix.

The same Contributors section records how gdb grew afterwards: Cygnus Solutions sponsored its
maintenance from 1991, Stu Grossman wrote gdbserver (Chapter 10), and Hewlett-Packard
contributed the text user interface, the TUI (Chapter 6).

The system call underneath is much older than gdb. `ptrace`, "process trace", is already in
the Sixth Edition Unix programmer's manual with today's shape: a request, a process ID, an
address and a data word, reading and writing a stopped child's memory one word at a time, as
tdb does in Chapter 2. Linux added many requests and the Yama rules, but a debugger today
still forks, asks to be traced, and waits.

## Summary

- A debugger is a process that controls another through the kernel: on Linux, `ptrace` plus
  `waitpid`.
- `-g` adds DWARF sections that only the debugger reads; without it gdb has only `.symtab`
  names. `-O2` inlines and reorders (`<optimized out>`), `-Og` is a compromise, the course
  uses `-g -O0`.
- `run ARGS`, `start ARGS` (= `tbreak main` + `run`), `starti` (first instruction, in the
  dynamic loader). Exit codes print in octal; `$_exitcode` holds the last one. `info
  inferiors`, `info proc`, `info proc mappings`, `info sharedlibrary` show what runs where.
- `*ptr@n`, `info frame`, `finish`, and the value history `$`, `$$`, `$N`. In a crash inside a
  library, walk up the backtrace to the last frame in your code.
- gdb starts programs with fork, `PTRACE_TRACEME` and exec (through a shell), with ASLR off;
  the exec stops the program before its first instruction.
- Wait status: `0x..7f` stopped by the signal in the high byte, `0x..00` exited with the code
  in the high byte, otherwise killed by the signal in the low 7 bits (`0x80` = core).
- Signals stop a tracee *before* delivery, and the tracer decides: `continue` versus
  `signal 0`, the data argument of `PTRACE_CONT`. tdb v0.1 launches, continues, reports, and
  passes signals on.

## Exercises

1. Make tdb report exits the way gdb does: `[Inferior 1 (process N) exited normally]` for 0,
   and `[Inferior 1 (process N) exited with code NN]` otherwise, with the code in octal. Note
   that `proc.pid` is already 0 when `report` runs.

<details><summary>Solution</summary>

`proc_wait` clears `pid` when the process is gone, so remember it at launch. In
`project/ch01/src/tdb.c`:

```diff
 static struct proc proc;
 static char **prog_argv;
+static pid_t last_pid;     /* proc.pid is 0 once the process is gone */
@@
         printf("Program received signal %s (%s).\n", sig_name(sig), strsignal(sig));
         /* Like gdb, pass the signal on when the user continues. */
         proc.pending_sig = sig;
+    } else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
+        printf("[Inferior 1 (process %d) exited normally]\n", (int)last_pid);
     } else if (WIFEXITED(st)) {
-        printf("[process exited with status %d]\n", WEXITSTATUS(st));
+        printf("[Inferior 1 (process %d) exited with code %02o]\n", (int)last_pid,
+               WEXITSTATUS(st));
     } else if (WIFSIGNALED(st)) {
@@
     printf("Started process %d: %s\n", (int)proc.pid, prog_argv[0]);
+    last_pid = proc.pid;
     if (proc_continue(&proc) == 0)
         report();
```

```console
$ printf 'run\n' | build/tdb build/targets/hello
(tdb) run
Started process 1388813: build/targets/hello
hello from the target
[Inferior 1 (process 1388813) exited normally]
(tdb) 
$ printf 'run\ncontinue\n' | build/tdb build/targets/exit3 | tail -2
[Inferior 1 (process 1388818) exited with code 03]
(tdb) 
```

`%02o` prints 3 as `03` and 10 as `12`, as gdb does. `tests/itest.sh` checks the old wording,
so update its expected strings too.

</details>

2. Add an option `tdb -n` that leaves address randomisation on, and make `run` print where the
   program was loaded: the start of the first line of `/proc/PID/maps`. Run `hello` twice
   with and twice without `-n`.

<details><summary>Solution</summary>

`project/ch01/src/proc.h` gets `extern bool proc_aslr;` and `proc.c` defines it and uses it in
the child:

```diff
 bool proc_verbose;
+bool proc_aslr;
@@
         /* Same addresses every run, as gdb does by default. */
-        personality(ADDR_NO_RANDOMIZE);
+        if (!proc_aslr)
+            personality(ADDR_NO_RANDOMIZE);
         execv(argv[0], argv);
```

In `tdb.c`, a helper prints the load address:

```c
/* The first line of /proc/PID/maps: where the program's file is mapped. */
static void show_load_address(pid_t pid)
{
    char path[64], line[256];

    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    if (fgets(line, sizeof line, f) != NULL)
        printf("Loaded at 0x%.*s\n", (int)strcspn(line, "-"), line);
    fclose(f);
}
```

Call it in `cmd_run` right after the "Started process" line. In `main`, turn the `-v` test
into a loop over leading options that also sets `proc_aslr = true` for `-n`, and add `[-n]`
to the usage message.

```console
$ for o in "" "" -n -n; do printf 'run\n' | build/tdb $o build/targets/hello | grep Loaded; done
Loaded at 0x555555554000
Loaded at 0x555555554000
Loaded at 0x5909569c6000
Loaded at 0x5829ae9d9000
```

Without ASLR the PIE is always at `0x555555554000` (two thirds of the 47-bit user address
space, rounded to a page: the kernel's default base for PIEs). With it, the base changes
every run. The `-n` addresses will differ on your machine. That is the point. gdb's
`set disable-randomization off` does the same.

</details>

3. Watch tdb's system calls with strace:
   `printf 'run\n' | strace -e trace=ptrace,wait4,clone build/tdb build/targets/hello`. Find
   the fork, the exec stop and the continue. Then add `-f`, which makes strace follow (trace)
   the children too. What happens, and why?

<details><summary>Solution</summary>

```console
$ printf 'run\n' | strace -e trace=ptrace,wait4,clone build/tdb build/targets/hello
(tdb) run
clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, child_tidptr=0x746ec3adea10) = 449308
wait4(449308, [{WIFSTOPPED(s) && WSTOPSIG(s) == SIGTRAP}], 0, NULL) = 449308
--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_TRAPPED, si_pid=449308, si_uid=1000, si_status=SIGTRAP, si_utime=0, si_stime=0} ---
ptrace(PTRACE_SETOPTIONS, 449308, NULL, PTRACE_O_EXITKILL) = 0
Started process 449308: build/targets/hello
ptrace(PTRACE_CONT, 449308, NULL, 0)    = 0
wait4(449308, hello from the target
[{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL) = 449308
--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=449308, si_uid=1000, si_status=0, si_utime=0, si_stime=0} ---
[process exited with status 0]
(tdb) 
+++ exited with 0 +++
```

- glibc's `fork()` is the `clone` system call, and `waitpid` is `wait4`. strace decodes the
  status: `WSTOPSIG(s) == SIGTRAP` is the `0x057f` exec stop (and tdb got a `SIGCHLD` with
  `CLD_TRAPPED`). `PTRACE_CONT` has data `0`: no signal to deliver.
- The child's own `ptrace(PTRACE_TRACEME)` and `execve` are not shown: without `-f`, strace
  traces only tdb.

With `-f`:

```console
$ printf 'run\n' | strace -f -e trace=ptrace,wait4,execve build/tdb build/targets/hello
execve("build/tdb", ["build/tdb", "build/targets/hello"], 0x7ffddd2a5030 /* 44 vars */) = 0
(tdb) run
strace: Process 449315 attached
[pid 449314] wait4(449315,  <unfinished ...>
[pid 449315] ptrace(PTRACE_TRACEME)     = -1 EPERM (Operation not permitted)
tdb: ptrace(TRACEME): Operation not permitted
[pid 449315] +++ exited with 127 +++
<... wait4 resumed>[{WIFEXITED(s) && WEXITSTATUS(s) == 127}], 0, NULL) = 449315
--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=449315, si_uid=1000, si_status=127, si_utime=0, si_stime=0} ---
Could not start build/targets/hello.
(tdb) 
+++ exited with 0 +++
```

strace is a ptrace-based tool too. With `-f` it attaches to tdb's child as soon as it is
created. A process can have only one tracer, so the child's `PTRACE_TRACEME` fails with
`EPERM`, the child exits with 127, and tdb reports that it could not start the program. Some
programs use the same trick against debuggers: they call `ptrace(PTRACE_TRACEME)` on
themselves, and if that fails, someone is already tracing them.

</details>

4. In gdb, compare `info proc mappings` at `starti` with the list at `main`. What is missing at
   the first instruction, what changed, and why?

<details><summary>Solution</summary>

```console
$ gdb -q ./avg
Reading symbols from ./avg...
(gdb) starti 1 2
Starting program: .../avg 1 2

Program stopped.
0x00007ffff7fe3290 in _start () from /lib64/ld-linux-x86-64.so.2
(gdb) info proc mappings
process 1367281
Mapped address spaces:

          Start Addr           End Addr       Size     Offset  Perms  objfile
      0x555555554000     0x555555555000     0x1000        0x0  r--p   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x555555555000     0x555555556000     0x1000     0x1000  r-xp   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x555555556000     0x555555557000     0x1000     0x2000  r--p   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x555555557000     0x555555559000     0x2000     0x2000  rw-p   /home/tbangma/learn-gdb-debugging/examples/ch01/avg
      0x7ffff7fbd000     0x7ffff7fc1000     0x4000        0x0  r--p   [vvar]
      0x7ffff7fc1000     0x7ffff7fc3000     0x2000        0x0  r-xp   [vdso]
      0x7ffff7fc3000     0x7ffff7fc5000     0x2000        0x0  r--p   /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
      0x7ffff7fc5000     0x7ffff7fef000    0x2a000     0x2000  r-xp   /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
      0x7ffff7fef000     0x7ffff7ffa000     0xb000    0x2c000  r--p   /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
      0x7ffff7ffb000     0x7ffff7fff000     0x4000    0x37000  rw-p   /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
      0x7ffffffde000     0x7ffffffff000    0x21000        0x0  rw-p   [stack]
```

Compare it with the list at `main` in "How gdb runs your program".

- At `starti` only what the kernel's `execve` mapped is there: the program, the loader, the
  stack, and `[vvar]`/`[vdso]` (kernel data and a small kernel-provided library for fast
  `gettimeofday`-style calls). libc is missing: the dynamic loader has not run yet.
- At `main`, libc has six mappings at `0x7ffff7c00000`, plus three anonymous `rw-p` regions
  (the loader's and libc's own data, such as thread-local storage).
- The program's last mapping `0x555555557000–0x555555559000 rw-p` was split in two: the loader
  filled in the program's pointers to libc functions and then made the first page read-only
  with `mprotect` (RELRO, "relocation read-only"). The loader's own last mapping was split the
  same way. The `libthread_db` lines appear only after libc arrives.

</details>

Sources for this chapter: [REFERENCES.md](REFERENCES.md#chapter-1--getting-started)
