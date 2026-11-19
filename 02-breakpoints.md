# Chapter 2 — Breakpoints

learn-c, Chapter 10 used `break` with a function name or a line number. That is enough when
the bug shows up the first time the line runs. It is not enough when the line runs a thousand
times and goes wrong once. This chapter covers gdb's breakpoint tools for that case: hit
counts, `ignore`, conditions, temporary breakpoints, `until` and `advance`, command lists and
`dprintf`. They are applied to a ledger program that goes wrong on its 630th transaction. Then
the chapter looks underneath. A software breakpoint is one byte, `0xCC`, written over your
code. tdb learns to write it, notice it, and step over it without changing what the program
computes.

Previous: [Chapter 1](01-getting-started.md). Next: [Chapter 3](03-looking-at-data.md).

## The ledger that pays too much

`examples/ch02/ledger.c` replays a day of account transactions. `generate()` fills a global
array `struct txn txns[1000]` from a small random number generator, so every run gets the same
transactions. A `struct txn` has an `int id`, a `char kind` (`'D'` deposit, `'W'` withdrawal,
`'F'` fee) and an `int amount`: cents, or for a fee, basis points (hundredths of a percent).
The rest of the program, from line 40:

```c
static int fee(int balance, int basis_points)                      /* line 40 */
{
    return balance * basis_points / 10000;                          /* line 42 */
}

static int apply(int balance, const struct txn *t)                  /* line 45 */
{
    switch (t->kind) {                                              /* line 47 */
    case 'D':
        return balance + t->amount;
    case 'W':
        if (t->amount > balance)
            return balance;            /* refused: not enough money */
        return balance - t->amount;
    case 'F':
        return balance - fee(balance, t->amount);                   /* line 55 */
    }
    return balance;
}

int main(void)                                                      /* line 60 */
{
    int balance = 0;                                                /* line 62 */

    generate();
    for (int i = 0; i < NTXN; i++)                                  /* line 65 */
        balance = apply(balance, &txns[i]);
    printf("final balance: %d.%02d\n", balance / 100, balance % 100);
    return 0;
}
```

(The line comments are not in the file.) Money is kept in cents in an `int`, and a fee of 21
basis points is 0.21% of the balance, `balance * 21 / 10000`. Build and run it:

```console
$ cd examples/ch02
$ make            # compiler lines not shown
$ ./ledger
final balance: 1823574.84
```

The bank's statement says 1756312.29. The program reports 67262.55 too much, and it compiles
without a warning under `-Wall -Wextra`. You could step through it with `next`, but there are
1000 transactions and you don't know which one is wrong. You need a breakpoint that stops at
the right one, and the gdb half of this chapter builds up to it.

As in Chapter 1, the two `libthread_db` lines gdb prints after `Starting program` are trimmed
from every transcript, paths are shortened to `.../ledger`, and pids and some addresses will
differ on your machine.

## Breakpoint locations

A breakpoint needs a location. gdb accepts several forms, and they do not all mean the same
address:

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) break fee
Breakpoint 1 at 0x1352: file ledger.c, line 42.
(gdb) break *fee
Breakpoint 2 at 0x1344: file ledger.c, line 41.
(gdb) break ledger.c:55
Breakpoint 3 at 0x13d6: file ledger.c, line 55.
(gdb) break printf
Breakpoint 4 at 0x1050
(gdb) info breakpoints
Num     Type           Disp Enb Address            What
1       breakpoint     keep y   0x0000000000001352 in fee at ledger.c:42
2       breakpoint     keep y   0x0000000000001344 in fee at ledger.c:41
3       breakpoint     keep y   0x00000000000013d6 in apply at ledger.c:55
4       breakpoint     keep y   0x0000000000001050 <printf@plt>
(gdb) run
Starting program: .../ledger

Breakpoint 3, apply (balance=2144200, t=0x5555555580a0 <txns+96>) at ledger.c:55
55	        return balance - fee(balance, t->amount);
(gdb) continue
Continuing.

Breakpoint 2, fee (balance=32767, basis_points=-6103) at ledger.c:41
41	{
(gdb) info breakpoints
Num     Type           Disp Enb Address            What
1       breakpoint     keep y   0x0000555555555352 in fee at ledger.c:42
2       breakpoint     keep y   0x0000555555555344 in fee at ledger.c:41
	breakpoint already hit 1 time
3       breakpoint     keep y   0x00005555555553d6 in apply at ledger.c:55
	breakpoint already hit 1 time
4       breakpoint     keep y   0x00007ffff7c606f0 in __printf at ./stdio-common/printf.c:28
```

Four things to notice.

**`break fee` is not the first instruction of `fee`.** It is the first instruction of the
first line of the body, after the *prologue*. `break *fee` (a `*` means "this address")
is the real entry. The disassembly shows the difference:

```console
$ objdump -d --no-show-raw-insn ledger | sed -n '/<fee>:/,/^$/p'
0000000000001344 <fee>:
    1344:	endbr64
    1348:	push   %rbp
    1349:	mov    %rsp,%rbp
    134c:	mov    %edi,-0x4(%rbp)
    134f:	mov    %esi,-0x8(%rbp)
    1352:	mov    -0x4(%rbp),%eax
    ...
```

The arguments arrive in registers (`%edi`, `%esi`). At `-O0` the prologue builds a frame and
copies them into stack slots, and the debug info says the arguments live in those slots. At
`0x1344` the copies have not been made yet, so gdb reads leftover stack contents and prints
`balance=32767, basis_points=-6103`. The values are wrong, not the program. Stop after the
prologue (plain `break fee`) when you want the arguments.

**Addresses change at `run`.** Before the program runs, gdb shows `0x1352`; afterwards,
`0x0000555555555352`. `ledger` is a position-independent executable (PIE). The numbers in the
file are offsets from wherever the program is loaded, and in Chapter 1 you saw gdb's child load
it at `0x555555554000`. gdb writes its breakpoints into the program's memory only when there is
a program. Until then it remembers the file address, and at `run` it *relocates* each one by
adding the load address. tdb does the same in this chapter.

**`break printf` moved into libc.** Before `run`, the only `printf` gdb knows is
`printf@plt`, the stub in `ledger` through which calls go to the shared library. Once libc is
loaded, gdb re-reads the symbols and moves the breakpoint to the real function, glibc's
`__printf`. For a name it cannot find at all, gdb says `Function "nosuchfunc" not defined.`
and asks `Make breakpoint pending on future shared library load? (y or [n])`: a *pending*
breakpoint waits for a library that defines the name.

**Pointers into arrays print as symbol plus offset.** `t=0x5555555580a0 <txns+96>`: a
`struct txn` is 12 bytes (two `int`s and a `char`, padded to a multiple of 4), so this is
`txns[8]`, transaction 9, the first fee. A plain `break 55` means line 55 of the current file.

## Hit counts and ignore

gdb counts every time the program reaches a breakpoint, even when it does not stop there. That
gives a cheap way to count calls. Give the breakpoint an `ignore` count larger than it can ever
reach, run to the end, and read the count:

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) break fee
Breakpoint 1 at 0x1352: file ledger.c, line 42.
(gdb) ignore 1 1000000
Will ignore next 1000000 crossings of breakpoint 1.
(gdb) run
Starting program: .../ledger
final balance: 1823574.84
[Inferior 1 (process 1400210) exited normally]
(gdb) info breakpoints
Num     Type           Disp Enb Address            What
1       breakpoint     keep y   0x0000555555555352 in fee at ledger.c:42
	breakpoint already hit 90 times
	ignore next 999910 hits
```

There are 90 fees among the 1000 transactions. `ignore N COUNT` tells gdb to skip the next
COUNT hits of breakpoint N. Each skipped hit still counts, and still costs a stop (more on that
under "Conditions").

The other use of `ignore` is to get to the Nth hit directly. Suppose you already know that
transaction 630 is where the balance first goes wrong; the next section shows how to find
that out. Stop in `apply` once, then skip 628 crossings, and the next stop is the 630th call:

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) break apply
Breakpoint 1 at 0x1388: file ledger.c, line 47.
(gdb) run
Starting program: .../ledger

Breakpoint 1, apply (balance=0, t=0x555555558040 <txns>) at ledger.c:47
47	    switch (t->kind) {
(gdb) print *t
$1 = {id = 1, kind = 87 'W', amount = 146800}
(gdb) ignore 1 628
Will ignore next 628 crossings of breakpoint 1.
(gdb) continue
Continuing.

Breakpoint 1, apply (balance=121286986, t=0x555555559dbc <txns+7548>) at ledger.c:47
47	    switch (t->kind) {
(gdb) print *t
$2 = {id = 630, kind = 70 'F', amount = 21}
```

Hit 1 stopped, hits 2 to 629 were ignored (628 of them), and hit 630 stopped: transaction 630
(`7548 / 12 = 629`), a fee of 21 basis points on a balance of 1,212,869.86. A `char` prints as a
number and a character (`87 'W'`). The ignore count belongs to the breakpoint, not the process,
so `ignore 1 629` before `run` also stops at hit 630.

## Conditions

You rarely know the number of the bad hit in advance. You usually know what makes it bad. Here
the fee is negative when the product `balance * basis_points` overflows `int`, so stop exactly
then:

```console
(gdb) delete
Delete all breakpoints? (y or n) y
(gdb) break fee if balance * basis_points < 0
Breakpoint 2 at 0x555555555352: file ledger.c, line 42.
(gdb) run
The program being debugged has been started already.
Start it from the beginning? (y or n) y
Starting program: .../ledger

Breakpoint 2, fee (balance=121286986, basis_points=21) at ledger.c:42
42	    return balance * basis_points / 10000;
(gdb) bt
#0  fee (balance=121286986, basis_points=21) at ledger.c:42
#1  0x00005555555553e9 in apply (balance=121286986, t=0x555555559dbc <txns+7548>) at ledger.c:55
#2  0x0000555555555442 in main () at ledger.c:66
(gdb) print balance * basis_points
$3 = -1747940590
(gdb) print (long)balance * basis_points
$4 = 2547026706
(gdb) up
#1  0x00005555555553e9 in apply (balance=121286986, t=0x555555559dbc <txns+7548>) at ledger.c:55
55	        return balance - fee(balance, t->amount);
(gdb) print *t
$5 = {id = 630, kind = 70 'F', amount = 21}
```

This continues the `ignore` session. `delete` with no number deletes every breakpoint, and the
new one is number 2 because numbers are never reused in a session. The condition is a C
expression, evaluated in the breakpoint's frame each time the program gets there, with C's
rules and the types from the debug info: `balance * basis_points` is an `int` product and wraps
exactly as the program's does. Casting one side to `long` shows the true value. `121286986 * 21
= 2547026706`, which is more than `INT_MAX` (2147483647). The hardware keeps the low 32 bits,
`2547026706 - 2^32 = -1747940590`, so the fee is `-174794` cents. `apply` subtracts a negative
fee, and the customer is paid 1747.94 instead of charged 2547.02.

In C, signed overflow is undefined behaviour, not just wrapping. With `-O0` gcc uses a plain
`imul`, which wraps. gcc's UndefinedBehaviorSanitizer finds the same line without a debugger:

```console
$ gcc -std=c11 -g -fsanitize=undefined ledger.c -o ledger-ubsan
$ ./ledger-ubsan
ledger.c:42:20: runtime error: signed integer overflow: 121286986 * 21 cannot be represented in type 'int'
final balance: 1823574.84
```

UBSan reports each location once, so it says nothing about the later overflows. The fix is to
multiply in a wider type, `return (int)((long long)balance * basis_points / 10000);` (the
result fits in an `int` again after the division), and then the program prints
`final balance: 1756312.29`, the bank's number. Before you fix it, though, look at what the
condition does after the first stop:

```console
(gdb) tbreak 67
Temporary breakpoint 3 at 0x555555555452: file ledger.c, line 67.
(gdb) continue
Continuing.

Breakpoint 2, fee (balance=125574010, basis_points=21) at ledger.c:42
42	    return balance * basis_points / 10000;
```

The condition is still active, and the next overflowing fee stops the program before line 67 is
reached. There are more of them: the balance only grows from here.

### What a condition costs

A condition makes gdb *stop less often for you*. It does not make the program stop less
often. Every time the program reaches the breakpoint, it stops. The kernel switches to gdb,
which reads registers and memory, evaluates the expression, and resumes the program if it is
false. `project/ch02/targets/spin.c` calls a one-line function `tick(i)` 100000 times. Time it
alone, under gdb with a condition that is never true, and under tdb with an `ignore` count:

```console
$ cd project/ch02
$ make            # compiler lines not shown
$ time build/targets/spin
odd=50000

real	0m0.001s
user	0m0.000s
sys	0m0.001s
$ time gdb -q -batch -ex 'break tick if i < 0' -ex run build/targets/spin > /dev/null

real	0m19.685s
user	0m10.434s
sys	0m8.877s
$ time (printf 'break 0x1169\nignore 1 100000\nrun\n' | build/tdb build/targets/spin > /dev/null)

real	0m8.149s
user	0m0.238s
sys	0m6.149s
```

On my machine (your times will differ, and vary from run to run), a program that takes a
millisecond takes 20 seconds with a never-true condition: about 200 µs per hit, roughly 200,000
times slower. Only about a quarter of that is the condition: with `ignore 1 100000` instead,
gdb takes 14.9 s. tdb, which does much less per stop, still needs about 80 µs, most of it in
the kernel (`sys`). The section on tdb lists the system calls one hit costs.

So put conditional breakpoints on code that runs thousands of times, not millions. For a line
that runs in a tight loop, add a temporary `if` to the source and put the breakpoint inside it,
or use a hardware watchpoint (Chapter 4). gdb can also hand a condition to a remote target to
evaluate (`set breakpoint condition-evaluation`, for targets that support it, like gdbserver in
Chapter 10).

## Managing breakpoints

A longer session shows the commands for keeping a list of breakpoints in order:

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) rbreak ledger.c:.
Breakpoint 1 at 0x1403: file ledger.c, line 62.
int main(void);
Breakpoint 2 at 0x1388: file ledger.c, line 47.
static int apply(int, const struct txn *);
Breakpoint 3 at 0x1352: file ledger.c, line 42.
static int fee(int, int);
Breakpoint 4 at 0x1151: file ledger.c, line 21.
static void generate(void);
(gdb) delete 2 4
(gdb) condition 3 t->kind == 'F'
No symbol "t" in current context.
(gdb) condition 3 basis_points > 20
(gdb) disable 1
(gdb) run
Starting program: .../ledger

Breakpoint 3, fee (balance=16034667, basis_points=21) at ledger.c:42
42	    return balance * basis_points / 10000;
(gdb) info breakpoints
Num     Type           Disp Enb Address            What
1       breakpoint     keep n   0x0000555555555403 in main at ledger.c:62
3       breakpoint     keep y   0x0000555555555352 in fee at ledger.c:42
	stop only if basis_points > 20
	breakpoint already hit 1 time
(gdb) condition 3
Breakpoint 3 now unconditional.
(gdb) disable 3
(gdb) enable 1
(gdb) save breakpoints ledger.bp
Saved to file 'ledger.bp'.
(gdb) shell cat ledger.bp
break .../ledger.c:main
break .../ledger.c:fee
disable $bpnum
```

- **`rbreak REGEX`** sets a breakpoint on every function whose name matches the regular
  expression, and prints each function's declaration. `rbreak ledger.c:.` limits it to one
  file; `.` matches every name. Without the file, `rbreak .` matches every function gdb
  knows about, including those of any library already loaded.
- **`delete 2 4`** deletes several breakpoints at once (ranges like `2-4` work too).
- **`condition N EXPR`** sets or replaces a condition, and **`condition N`** removes it. gdb
  checks the expression in the breakpoint's scope: `fee` has no `t`. A typo is caught the same
  way, and `break fee if bp > 20` gives `No symbol "bp" in current context.`
- **`disable N` / `enable N`** switch a breakpoint off and on (the `Enb` column) and keep its
  condition, ignore count and commands.
- **`info breakpoints`** (`i b`) shows, under each breakpoint, its condition ("stop only if"),
  hit count, ignore count and commands. "breakpoint already hit 1 time" counts only the
  hits where the condition was true: `fee` had been reached five times before.
- **`save breakpoints FILE`** writes them as gdb commands, which `source FILE` in a later
  session reads back. It saved the location as you typed it (`fee`, not the address) plus the
  state: `disable $bpnum`, where `$bpnum` is the number of the last breakpoint created. A
  condition is saved as `  condition $bpnum basis_points > 20`.

`Disp` is the disposition: `keep` for ordinary breakpoints, `del` for temporary ones (next
section). gdb also warns when two breakpoints share an address, for example a second
`break fee` prints `Note: breakpoint 3 also set at pc 0x1352.` gdb handles that case
correctly. tdb does not yet (Chapter 6).

## tbreak, until, advance

A *temporary* breakpoint deletes itself after the first stop. `tbreak LOCATION` makes one, and
`start` is `tbreak main` followed by `run`, which is why it prints "Temporary breakpoint 1". Two
related commands move forward without leaving a breakpoint behind. The first is `until`:

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) start
Temporary breakpoint 1 at 0x1403: file ledger.c, line 62.
Starting program: .../ledger

Temporary breakpoint 1, main () at ledger.c:62
62	    int balance = 0;
(gdb) next
64	    generate();
(gdb) next
65	    for (int i = 0; i < NTXN; i++)
(gdb) next
66	        balance = apply(balance, &txns[i]);
(gdb) next
65	    for (int i = 0; i < NTXN; i++)
(gdb) next
66	        balance = apply(balance, &txns[i]);
(gdb) until
65	    for (int i = 0; i < NTXN; i++)
(gdb) until
67	    printf("final balance: %d.%02d\n", balance / 100, balance % 100);
(gdb) print i
No symbol "i" in current context.
(gdb) print balance
$1 = 182357484
```

`next` goes around the loop, 65, 66, 65, 66, 2000 times. `until` with no argument is a `next`
that never goes *backwards*. gcc puts a `for` loop's `i++` and test after the body, so line 65
comes both before line 66 (`i = 0`) and after it. From the test, the jump back to the body goes
to a lower address, so `until` does not stop there and runs on to the first line after the
loop. `print i` fails at line 67 because `i` exists only inside the `for`.

`until LOCATION` and `advance LOCATION` run to a location without making a numbered breakpoint:

```console
(gdb) start
The program being debugged has been started already.
Start it from the beginning? (y or n) y
Temporary breakpoint 2 at 0x555555555403: file ledger.c, line 62.
Starting program: .../ledger

Temporary breakpoint 2, main () at ledger.c:62
62	    int balance = 0;
(gdb) advance fee
fee (balance=2144200, basis_points=11) at ledger.c:42
42	    return balance * basis_points / 10000;
(gdb) up
#1  0x00005555555553e9 in apply (balance=2144200, t=0x5555555580a0 <txns+96>) at ledger.c:55
55	        return balance - fee(balance, t->amount);
(gdb) print *t
$2 = {id = 9, kind = 70 'F', amount = 11}
(gdb) advance 67
0x0000555555555442 in main () at ledger.c:66
66	        balance = apply(balance, &txns[i]);
(gdb) advance 67
main () at ledger.c:67
67	    printf("final balance: %d.%02d\n", balance / 100, balance % 100);
```

The first `advance 67` stopped early, in the middle of line 66, just after `apply` returned to
`main`. `advance` and `until LOCATION` also stop when the *selected* frame returns, and `up`
had selected `apply`'s frame. Give the command again, and it goes to line 67. The difference
between the two: `until LOCATION` only stops at the location in the current frame (useful in a
recursive function), and `advance` stops at it in any frame.

## Breakpoint commands and dprintf

A breakpoint can carry a list of gdb commands that run each time it stops the program. With
`silent` first and `continue` last, the breakpoint becomes a print statement you can add
without recompiling:

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) break fee
Breakpoint 1 at 0x1352: file ledger.c, line 42.
(gdb) commands
Type commands for breakpoint(s) 1, one per line.
End with a line saying just "end".
>silent
>printf "txn fee: balance=%d bp=%d product=%d\n", balance, basis_points, balance * basis_points
>continue
>end
(gdb) run
Starting program: .../ledger
txn fee: balance=2144200 bp=11 product=23586200
...
txn fee: balance=154962837 bp=16 product=-1815561904
txn fee: balance=155350693 bp=6 product=932104158
txn fee: balance=155257483 bp=21 product=-1034560153
...
txn fee: balance=181547286 bp=11 product=1997020146
final balance: 1823574.84
[Inferior 1 (process 1372195) exited normally]
```

- `commands` with no number applies to the last breakpoint made; `commands 3` to breakpoint 3.
  You type the list at a `>` prompt and end it with `end`. `info breakpoints` lists it.
- `silent` must come first. It suppresses the "Breakpoint 1, fee (...)" lines.
- gdb's `printf` takes a C format and expressions. Commands after a `continue` never run.

The trace has 90 lines. Once the balance passes about 102 million cents, a fee of 21 basis
points overflows, and from 134 million cents, even 16 points does. The bug gets worse as the
account grows, which is why it hides when you test with small numbers.

This is so common that gdb has a breakpoint type for it, the *dynamic printf*:

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) dprintf fee,"fee(%d, %d)\n", balance, basis_points
Dprintf 1 at 0x1352: file ledger.c, line 42.
(gdb) start
Temporary breakpoint 2 at 0x1403: file ledger.c, line 62.
Starting program: .../ledger

Temporary breakpoint 2, main () at ledger.c:62
62	    int balance = 0;
(gdb) until 67
fee(2144200, 11)
fee(9047242, 1)
fee(14169738, 16)
...
fee(181330283, 21)
fee(181547286, 11)
main () at ledger.c:67
67	    printf("final balance: %d.%02d\n", balance / 100, balance % 100);
```

`dprintf LOCATION,"FORMAT",ARGS...` makes a breakpoint that behaves like one with the command
list `silent`, `printf`, `continue`. `until 67` ran the whole loop, printing 90 lines on the
way, and stopped before the program's own `printf`. A dprintf costs what any breakpoint costs,
a full stop per hit. `set dprintf-style` chooses who prints: `gdb` (the default) prints to
gdb's console, and `call` makes gdb call `printf` inside your program, so the output goes to
the program's stdout, in order with its own output.

## How a breakpoint works: int3

Every breakpoint in this chapter so far has been a *software breakpoint*. To make one, the
debugger changes the program's code. Look at the bytes of `square` in tdb's target
`project/ch02/targets/loop.c` (`return x * x;`, called five times by `main`):

```console
$ objdump -d build/targets/loop | sed -n '/<square>:/,/^$/p'
0000000000001149 <square>:
    1149:	f3 0f 1e fa          	endbr64
    114d:	55                   	push   %rbp
    114e:	48 89 e5             	mov    %rsp,%rbp
    1151:	89 7d fc             	mov    %edi,-0x4(%rbp)
    1154:	8b 45 fc             	mov    -0x4(%rbp),%eax
    1157:	0f af c0             	imul   %eax,%eax
    115a:	5d                   	pop    %rbp
    115b:	c3                   	ret
```

x86-64 instructions are 1 to 15 bytes long. A breakpoint at `square` replaces the first byte
of the first instruction, `f3`, with `cc`, the opcode of `int3`, while the program is stopped:

```
before:  1149: f3 0f 1e fa   endbr64
after:   1149: cc 0f 1e fa   int3, then three bytes nobody runs until the f3 is back
```

When the CPU executes `int3`, it raises a breakpoint exception. Linux turns that into a
`SIGTRAP` signal for the process. Because a traced process stops before any signal is
delivered (Chapter 1), the debugger's `waitpid` returns with "stopped by `SIGTRAP`". `int3` is a
*trap*: the saved instruction pointer `rip` points to the next byte, `0x114a`, not to the
`int3` itself.

You can see this without a debugger. `examples/ch02/trap.c` contains its own `int3`: it prints
"before the int3", flushes stdout, executes `__asm__ volatile("int3");` and prints "after the
int3":

```console
$ ./trap
before the int3
Trace/breakpoint trap (core dumped)
$ echo $?
133
```

Nobody is tracing it, so `SIGTRAP` gets its default action: kill the process and dump core. The
shell reports the exit status as 128 plus the signal number, and `SIGTRAP` is 5. Under gdb:

```console
$ gdb -q ./trap
Reading symbols from ./trap...
(gdb) run
Starting program: .../trap
before the int3

Program received signal SIGTRAP, Trace/breakpoint trap.
main () at trap.c:12
12	    puts("after the int3");
(gdb) print $pc
$1 = (void (*)()) 0x555555555190 <main+39>
(gdb) x/i $pc - 1
   0x55555555518f <main+38>:	int3
(gdb) continue
Continuing.
after the int3
[Inferior 1 (process 1371405) exited normally]
```

gdb got the `SIGTRAP`, saw that no breakpoint of its own is at `$pc - 1`, and reported a signal
instead of "Breakpoint N". `$pc` (gdb's name for `rip`) is one byte past the `int3`, which `x/i`
(examine as instructions, Chapter 3) shows at `main+38`. `continue` works because this `int3` is
part of the program: there is no original byte to put back. gdb does not pass `SIGTRAP` on
(`info signals SIGTRAP` says `Pass to program: No`), so the program carries on with line 12.

For a breakpoint the debugger placed itself, there is more to do on every hit:

1. **Recognise it.** On a `SIGTRAP` stop, read `rip`. If a breakpoint is at `rip - 1`, this is
   that breakpoint; otherwise it is some other trap.
2. **Rewind.** Set `rip` back by one, to the breakpoint address. The instruction that was there
   has not run yet.
3. **Step over it when resuming.** Put the original byte back, execute exactly one instruction
   (`PTRACE_SINGLESTEP`), write the `0xCC` again, and only then continue. Otherwise the
   breakpoint would either be gone or hit again at once.

Why one byte? An instruction can be one byte long (`55`, `push %rbp`, above). If the breakpoint
instruction were longer, it would overwrite the start of the *next* instruction too, and a jump
to that next instruction would land in the middle of the breakpoint. `int3` fits over any
instruction. "In the wild" below quotes the Intel manual on exactly this.

## tdb, step 2: software breakpoints

`project/ch02/` is tdb v0.2. It adds `src/breakpoint.c` (a table of up to 32 breakpoints;
writing and removing the `0xCC`), `src/run.c` (continue with breakpoints: recognise, rewind,
step over, ignore counts, temporary breakpoints) and `src/elf.c` (the load address). `proc.c`
gains small wrappers for five more `ptrace` requests, and `tdb.c` gains the commands `break
ADDR` (`b`), `tbreak ADDR`, `delete [N]` (`d`), `ignore N COUNT` and `info breakpoints` (`info
b`). There are two new targets, `loop` and `spin`, and the tests grow to 12 checks: `make test`
in `project/ch02` ends with `itest: 12 passed, 0 failed`.

tdb does not read symbols yet (that is Chapter 3), so breakpoints are given as addresses in
the file, the way `nm` or `objdump` prints them: `nm build/targets/loop` shows
`0000000000001149 T square`.

### Reading and writing memory: peek and poke

`ptrace` reads and writes the tracee's memory one 8-byte word at a time:

```c
int proc_peek(struct proc *p, uint64_t addr, uint64_t *word)
{
    /* PEEKDATA returns the word itself, so -1 may be data: only errno
     * tells an error apart. */
    errno = 0;
    long w = ptrace(PTRACE_PEEKDATA, p->pid, (void *)addr, NULL);
    if (errno != 0)
        return -1;
    *word = (uint64_t)w;
    return 0;
}
```

`PTRACE_PEEKDATA` is an odd interface: the result *is* the return value, so `-1` can be eight
`0xff` bytes or an error. The only way to tell is to set `errno` to 0 before the call and check
it afterwards. `proc_poke` passes the new word as `PTRACE_POKEDATA`'s data argument, which can
write to the program's code even though the code is mapped
read-only (`r-xp` in Chapter 1's mappings). The kernel makes a private copy of the page for
this process and writes into the copy, so the file and other processes running the same
program are untouched. `proc_get_regs` and `proc_set_regs` wrap `PTRACE_GETREGS` and
`PTRACE_SETREGS`, which read and write all the registers at once as a
`struct user_regs_struct` (from `<sys/user.h>`). `proc_step` is `proc_continue` with
`PTRACE_SINGLESTEP` instead of `PTRACE_CONT`: the kernel sets the CPU's trap flag, and the
process stops with `SIGTRAP` after one instruction.

### The breakpoint table

```c
struct breakpoint {
    int id;              /* 1, 2, ... as shown to the user; 0: a free slot */
    uint64_t addr;       /* address in the file, as nm and objdump show it */
    bool temporary;      /* tbreak: delete after the first hit */
    bool inserted;       /* the 0xCC is in the process's memory now */
    uint8_t saved;       /* the byte the 0xCC replaced */
    unsigned hits;
    unsigned ignore;     /* hits still to skip without stopping */
};
```

A breakpoint keeps its *file* address. Its run-time address is `addr + p->load_base`, and
`load_base` is new for every run. This is gdb's "relocation" from "Breakpoint locations":
`break 0x1149` before `run` remembers `0x1149`, and `run` writes the `0xCC` at
`0x555555555149`. Inserting is a read-modify-write of one word:

```c
int bp_insert(struct proc *p, struct breakpoint *bp)
{
    uint64_t addr = bp->addr + p->load_base;
    uint64_t word;

    if (bp->inserted)
        return 0;
    /* ptrace reads and writes whole 8-byte words: change only the low byte
     * (x86-64 is little-endian, so that is the byte at addr). */
    if (proc_peek(p, addr, &word) < 0)
        return -1;
    bp->saved = word & 0xff;
    if (proc_poke(p, addr, (word & ~(uint64_t)0xff) | 0xcc) < 0)
        return -1;
    bp->inserted = true;
    return 0;
}
```

At `0x555555555149` the word is the eight bytes `f3 0f 1e fa 55 48 89 e5`. Read as a
little-endian number, the first byte is the lowest: `0xe5894855fa1e0ff3`. Replacing the low
byte gives `0xe5894855fa1e0fcc`, the same bytes with `cc` first. `bp_remove` writes `saved`
back the same way.

**Where the program is loaded.** `elf_load_base` in `src/elf.c` reads the ELF header at the
start of the file (Chapter 3 reads more of it): a type and an entry point, `e_entry`. A non-PIE
(`ET_EXEC`) runs at the addresses in the file, so the base is 0. For a PIE, the kernel gives
each new program an *auxiliary vector*, a list of (type, value) pairs that `/proc/PID/auxv`
shows, and its `AT_ENTRY` is the real address of the entry point. `AT_ENTRY - e_entry` is the
load address. `proc_launch` computes it at the exec stop, when the program is mapped but has
not run.

### run_continue

`cmd_run` launches the program, inserts every breakpoint (`bp_insert_all`), and calls
`run_continue`, as does `continue`. That is the whole algorithm from the last section:

```c
int run_continue(struct proc *p, struct stop *st)
{
    for (;;) {
        int r = step_over_breakpoint(p, st);
        if (r != 0)
            return r < 0 ? -1 : 0;
        if (proc_continue(p) < 0)
            return -1;
        if (p->state != PROC_STOPPED || WSTOPSIG(p->status) != SIGTRAP) {
            classify(p, st);
            return 0;
        }

        /* A SIGTRAP: did we just execute one of our int3 bytes? Then rip is
         * one past it. */
        struct user_regs_struct regs;
        if (proc_get_regs(p, &regs) < 0)
            return -1;
        struct breakpoint *bp = bp_at(p, regs.rip - 1);
        if (bp == NULL) {
            classify(p, st);
            return 0;
        }
        /* Back up so the original instruction runs when we go on. */
        regs.rip -= 1;
        if (proc_set_regs(p, &regs) < 0)
            return -1;
        bp->hits++;
        if (bp->ignore > 0) {
            bp->ignore--;
            continue;
        }
        st->why = STOP_BREAKPOINT;
        st->bp_id = bp->id;
        st->temporary = bp->temporary;
        if (bp->temporary)
            bp_delete(p, bp->id);
        return 0;
    }
}
```

- The loop is for ignore counts: an ignored hit rewinds, counts, and goes round again (step
  over, continue), a full stop per hit as in gdb.
- `struct stop` says why the program stopped: a breakpoint, a signal, a single step (used from
  Chapter 6), or the end. `tdb.c`'s `report` prints it.
- `classify` handles every stop that is not a breakpoint. A signal other than `SIGTRAP` is kept
  in `p->pending_sig` and delivered at the next resume, as v0.1 did. A `SIGTRAP` that is not
  ours is reported but never passed on, because `SIGTRAP` is the debugger's signal.

`step_over_breakpoint` does the work before every resume:

```c
static int step_over_breakpoint(struct proc *p, struct stop *st)
{
    struct user_regs_struct regs;
    if (proc_get_regs(p, &regs) < 0)
        return -1;
    struct breakpoint *bp = bp_at(p, regs.rip);
    if (bp == NULL)
        return 0;

    if (bp_remove(p, bp) < 0 || proc_step(p) < 0)
        return -1;
    if (p->state == PROC_STOPPED)
        bp_insert(p, bp);
    if (p->state == PROC_STOPPED && WSTOPSIG(p->status) == SIGTRAP)
        return 0;
    classify(p, st);
    return 1;
}
```

If the program sits exactly on an inserted breakpoint, take the `0xCC` out, single-step, and put
it back. The step may end in something other than the expected `SIGTRAP`: the instruction can
crash (`SIGSEGV`), or be the program's last (`exit_group`). Then the step's result is the stop
to report.

### Running it

```console
$ build/tdb build/targets/loop
(tdb) break 0x1149
Breakpoint 1 at 0x1149
(tdb) run
Started process 1369671: build/targets/loop
Breakpoint 1, 0x0000555555555149
(tdb) continue
i=0 total=0
Breakpoint 1, 0x0000555555555149
(tdb) continue
i=1 total=1
Breakpoint 1, 0x0000555555555149
(tdb) info breakpoints
Num  Disp  Address  In memory           Hits
1    keep  0x1149   0x0000555555555149  3
(tdb) delete 1
(tdb) continue
i=2 total=5
i=3 total=14
i=4 total=30
[process exited with status 0]
```

The first stop is `square(0)`, before anything is printed. Each `continue` steps over the
`endbr64` and runs to the next call. After three hits the breakpoint is deleted, and the program
finishes with the right totals (0 + 1 + 4 + 9 + 16 = 30): stepping over did not change the
computation. `info breakpoints` shows the file address and the run-time one.

Run on `examples/ch02/trap`, tdb v0.2 reports the program's own `int3` as
`Program received signal SIGTRAP (Trace/breakpoint trap).`, and `continue` prints "after the
int3" and `[process exited with status 0]`. tdb v0.1 passed every signal on, so there the same
`continue` ended in `[process killed by SIGTRAP (Trace/breakpoint trap)]`.

### What one hit costs

Count the system calls behind one ignored hit in the `spin` timing. The `int3` traps, the
kernel stops `spin` and wakes tdb in `waitpid`. tdb calls `GETREGS` (whose `int3`?) and
`SETREGS` (rewind). On resume: `GETREGS` (on a breakpoint?), `PEEKDATA` and `POKEDATA` (byte
back), `SINGLESTEP` and `waitpid` (a second stop), `PEEKDATA` and `POKEDATA` (`0xCC` again),
and `CONT`. That is two stops and eleven system calls to run one `endbr64`. On my machine it
adds up to about 80 µs per hit. gdb does more per stop: it removes and reinserts all its
breakpoints, checks threads, and evaluates conditions. Chapter 4's hardware breakpoints avoid
the step-over dance, but they don't avoid the stop.

## Common mistakes

**Forgetting to rewind.** Remove the three lines after `/* Back up so ... */` and resume from
`rip`, one past the `int3`. With `break 0x1149` it seems to work: tdb reports
`Breakpoint 1, 0x000055555555514a`, one too high, but the output is still right. It
carries on from `0x114a`, in the middle of `endbr64`, and by luck the bytes `0f 1e fa` are a
complete instruction on their own, a `nop`. One instruction further on there is no such luck:

```console
(tdb) break 0x114e
Breakpoint 1 at 0x114e
(tdb) run
Started process 1369738: build/targets/loop
Breakpoint 1, 0x000055555555514f
(tdb) continue
Program received signal SIGSEGV (Segmentation fault).
```

`48 89 e5` is `mov %rsp,%rbp`. Without its first byte it is `89 e5`, `mov %esp,%ebp`, which
copies only the low 32 bits of the stack pointer, and the next use of `%rbp` crashes. A missing
rewind does not always crash. It corrupts in whatever way the remaining bytes happen to mean.

**Forgetting to step over.** Rewind, but resume without taking the `0xCC` out, and the CPU
executes the same `int3` again at once (`break 0x1149` as before):

```console
(tdb) run
Started process 1369758: build/targets/loop
Breakpoint 1, 0x0000555555555149
(tdb) continue
Breakpoint 1, 0x0000555555555149
```

No output, and no progress: `i=0` never appears. Taking the byte out without putting it back
would make the opposite mistake, a breakpoint that works once.

**A breakpoint inside an instruction.** tdb believes any address you give it. In `spin`,
`0x1188` starts `89 7d ec` (`mov %edi,-0x14(%rbp)`, saving `argc`). `break 0x1189` puts the
`0xCC` on its *second* byte (`-v` makes tdb print each wait status):

```console
$ build/tdb -v build/targets/spin
(tdb) break 0x1189
Breakpoint 1 at 0x1189
(tdb) run
[waitpid: status 0x057f]
Started process 1369856: build/targets/spin
[waitpid: status 0x0b7f]
Program received signal SIGSEGV (Segmentation fault).
(tdb) continue
[waitpid: status 0x008b]
[process killed by SIGSEGV (Segmentation fault)]
```

The CPU never executes the `int3`. It decodes `89 cc` as `mov %ecx,%esp`, which wrecks the
stack pointer, and then `ec`, `in (%dx),%al`, an I/O instruction a normal program may not use,
so the CPU faults and the kernel sends `SIGSEGV` (`0x0b7f`, stopped by signal 11). With
`break 0x1188` it works, and the program prints `odd=50000`. gdb makes the same mistake if you ask it to:

```console
(gdb) break *main+13
Breakpoint 1 at 0x1189: file targets/spin.c, line 14.
(gdb) run
Starting program: .../build/targets/spin

Program received signal SIGSEGV, Segmentation fault.
0x000055555555518a in main (argc=0, argv=0x7fffffffe829) at targets/spin.c:14
14	{
(gdb) print $pc
$1 = (void (*)()) 0x55555555518a <main+14>
(gdb) print/x $rsp
$2 = 0x55557db8
(gdb) x/3xb main+12
0x555555555188 <main+12>:	0x89	0x7d	0xec
```

The fault is at `main+14`, the `ec`. `$rsp` holds a 32-bit value copied from `%ecx`, and `argc`
is garbage because the instruction that should have stored it never ran. `x` shows the
original bytes, not the `0xCC`: by default (`breakpoint always-inserted` is `off`) gdb writes
its breakpoints when the program resumes and takes them out when it stops. tdb leaves them in;
Chapter 3 has an exercise to hide them. Break on functions and lines, and use
`break *ADDRESS` only with an address taken from a disassembly.

**A condition that names the wrong thing.** gdb rejects a condition with an unknown name, but
not one that is merely wrong: `break fee if balance < 0` is accepted and never stops, because
the balance never goes negative. The product does. When a condition never triggers, test the
expression with `print` at an unconditional stop first.

## In the wild: INT3, the one-byte instruction

x86 has a general software-interrupt instruction, `INT n`, two bytes: `CD` and the interrupt
number. A breakpoint built from it, `CD 03`, would cover two bytes. Intel's manual describes the
separate one-byte form, `INT3`, and why it exists (Intel 64 and IA-32 Architectures Software
Developer's Manual, Volume 2, "INT n/INTO/INT3/INT1"):

> The INT3 instruction uses a one-byte opcode (CC) [...] This one-byte form is useful because
> it can replace the first byte of any instruction at which a breakpoint is desired, including
> other one-byte instructions, without overwriting other instructions.

That is exactly the requirement from "How a breakpoint works". The same manual entry lists
`INT1`, opcode `F1`, another one-byte instruction that raises the debug exception. Debuggers
on x86 still use `INT3` for software breakpoints, and the CPU's debug registers for hardware
breakpoints and watchpoints, which Chapter 4 covers.

The printf-style tracing from "Breakpoint commands and dprintf" became a breakpoint type of its
own later than most of gdb: the GDB NEWS file lists `dprintf` and `set dprintf-style` under
"Changes in GDB 7.5". Before that, the `commands` / `silent` / `printf` / `continue` recipe was
the way to do it, and it still works.

## Summary

- `break FUNC` stops after the prologue, `break *ADDR` exactly there, `break FILE:LINE` at a
  line, `rbreak REGEX` at many functions. PIE addresses are relocated at `run`; library
  functions resolve when the library loads, or stay *pending*.
- `info breakpoints` shows hit counts, conditions, ignore counts and commands. `ignore N COUNT`
  skips hits, and with a huge count it counts calls.
- `break LOC if EXPR` and `condition N [EXPR]`: a C expression in the breakpoint's frame,
  evaluated after a full stop (about 200 µs per hit here). `delete`, `disable`, `enable`,
  `save breakpoints FILE` keep the list in order.
- `tbreak` deletes itself; `until` leaves loops; `until LOC` and `advance LOC` run to a place.
  `commands` with `silent` and `continue`, or `dprintf`, print without recompiling.
- A software breakpoint is `int3` (`0xCC`) over an instruction's first byte. On `SIGTRAP` the
  debugger finds it at `rip - 1`, rewinds `rip`, and steps over the original instruction when
  resuming. tdb v0.2 does this with `PEEKDATA`/`POKEDATA`, `GETREGS`/`SETREGS` and
  `PTRACE_SINGLESTEP`, relocating file addresses by the load address from `AT_ENTRY`.

## Exercises

1. Make tdb's `info breakpoints` look more like gdb's: put the hit count on its own line,
`breakpoint already hit N time(s)`, only when it is not zero, and the ignore count on another,
`ignore next N hits`.

<details><summary>Solution</summary>

All the changes are in `bp_list` in `project/ch02/src/breakpoint.c`:

```diff
         if (n++ == 0)
-            printf("Num  Disp  Address  In memory           Hits\n");
+            printf("Num  Disp  Address  In memory\n");
         printf("%-4d %-5s 0x%-6" PRIx64 " ", bp->id, bp->temporary ? "del" : "keep", bp->addr);
         if (p->state == PROC_STOPPED)
-            printf("0x%016" PRIx64 "  %u", bp->addr + p->load_base, bp->hits);
+            printf("0x%016" PRIx64 "\n", bp->addr + p->load_base);
         else
-            printf("%-18s  %u", "(not running)", bp->hits);
+            printf("(not running)\n");
+        if (bp->hits > 0)
+            printf("\tbreakpoint already hit %u time%s\n", bp->hits, bp->hits == 1 ? "" : "s");
         if (bp->ignore > 0)
-            printf("  (ignore next %u hits)", bp->ignore);
-        putchar('\n');
+            printf("\tignore next %u hits\n", bp->ignore);
     }
```

```console
(tdb) break 0x1149
Breakpoint 1 at 0x1149
(tdb) ignore 1 2
Will ignore next 2 crossings of breakpoint 1.
(tdb) info breakpoints
Num  Disp  Address  In memory
1    keep  0x1149   (not running)
	ignore next 2 hits
(tdb) run
Started process 1378166: build/targets/loop
i=0 total=0
i=1 total=1
Breakpoint 1, 0x0000555555555149
(tdb) info breakpoints
Num  Disp  Address  In memory
1    keep  0x1149   0x0000555555555149
	breakpoint already hit 3 times
```

The ignored hits count, as in gdb. `make test` still passes all 12 checks: they look for the
breakpoint's line, not the header.

</details>

2. How many breakpoint hits per second can tdb handle on your machine? And gdb, with a
   condition? Time `spin` with two different N and work it out so that start-up time cancels.

<details><summary>Solution</summary>

`spin N` calls `tick` N times, and tdb takes the program's arguments on its command line. In
`project/ch02`, with N = 10000:

```console
$ time (printf 'break 0x1169\nignore 1 100000\nrun\n' | build/tdb build/targets/spin 10000)
(tdb) break 0x1169
Breakpoint 1 at 0x1169
(tdb) ignore 1 100000
Will ignore next 100000 crossings of breakpoint 1.
(tdb) run
Started process 1393974: build/targets/spin
odd=5000
[process exited with status 0]
(tdb)

real	0m0.697s
user	0m0.015s
sys	0m0.534s
```

`time gdb -q -batch -ex 'break tick if i < 0' -ex run --args build/targets/spin 10000` took
1.897 s (gdb put the breakpoint at `0x1174`, after `tick`'s prologue; the count is the same).
With the N = 100000 times from "What a condition costs" (8.149 s and 19.685 s), 90000 extra hits
took 7.45 s more in tdb and 17.79 s more in gdb: on my machine about **12,000 hits per second
for tdb** and **5,000 for gdb with a condition**.

</details>

3. In gdb, instead of a condition, give `break fee` a command list that prints
   `negative product at balance N` and stays stopped only when the product is negative, and
   continues otherwise. (Command lists can contain `if ... end`.) Which transaction is it? Then
   compare the speed of the two methods on `spin`.

<details><summary>Solution</summary>

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) break fee
Breakpoint 1 at 0x1352: file ledger.c, line 42.
(gdb) commands
Type commands for breakpoint(s) 1, one per line.
End with a line saying just "end".
>silent
>if balance * basis_points >= 0
 >continue
 >end
>printf "negative product at balance %d\n", balance
>end
(gdb) run
Starting program: .../ledger
negative product at balance 121286986
(gdb) up
#1  0x00005555555553e9 in apply (balance=121286986, t=0x555555559dbc <txns+7548>) at ledger.c:55
55	        return balance - fee(balance, t->amount);
(gdb) print t->id
$1 = 630
```

The `if` has its own `end`, and the prompt shows the nesting (` >`). When the product is not
negative, `continue` resumes and skips the rest of the list. Otherwise `printf` runs and the
program stays stopped.

For the timing, put the same thing for `spin` in a file and run gdb on it:

```console
$ cat cmdif.gdb
break tick
commands
silent
if i >= 0
continue
end
end
run
$ time gdb -q -batch -x cmdif.gdb build/targets/spin > /dev/null

real	0m22.142s
user	0m12.836s
sys	0m9.470s
```

22.1 s against 19.7 s for `break tick if i < 0` on my machine. gdb checks a condition while it
is still deciding whether to stop, but runs a command list only after it has handled the stop
like any other, so the list is somewhat slower. Both cost a full stop per hit; a command list
can do more, such as print several things.

</details>

4. Add `disable [N]` and `enable [N]` to tdb, with no number meaning all breakpoints, like gdb.
   A disabled breakpoint stays in the table but has no `0xCC` in memory. Make
   `info breakpoints` show `(disabled)`.

<details><summary>Solution</summary>

Add `bool enabled;` to `struct breakpoint`, set it to `true` in `bp_add`, and make `bp_insert`
refuse disabled breakpoints. Then `bp_insert_all` at `run` and the reinsert after a step-over
skip them with no further change:

```c
    if (bp->inserted || !bp->enabled)
        return 0;
```

The new function in `breakpoint.c` takes the byte in or out at once if the process is stopped:

```c
int bp_enable(struct proc *p, int id, bool on)
{
    struct breakpoint *bp = bp_find(id);
    if (bp == NULL)
        return -1;
    bp->enabled = on;
    if (p->state != PROC_STOPPED)
        return 0;
    return on ? bp_insert(p, bp) : bp_remove(p, bp);
}
```

In `bp_list`, print `"  (disabled)"` before the newline when `!bp->enabled`. In `tdb.c`, add
`cmd_enable(const char *arg, bool on)`, a copy of `cmd_delete` that calls `bp_enable` (for all
ids when `arg` is `NULL`), two `else if` lines in the dispatch, and two help lines:

After `break 0x1149` and `disable 1`, `run` prints all five lines and never stops. Then:

```console
(tdb) enable 1
(tdb) run
Started process 1377600: build/targets/loop
Breakpoint 1, 0x0000555555555149
(tdb) continue
i=0 total=0
Breakpoint 1, 0x0000555555555149
(tdb) disable
(tdb) info breakpoints
Num  Disp  Address  In memory           Hits
1    keep  0x1149   0x0000555555555149  2  (disabled)
(tdb) continue
i=1 total=1
i=2 total=5
i=3 total=14
i=4 total=30
[process exited with status 0]
```

The interesting case is the last one: `disable` while stopped *on* the breakpoint. The `0xCC`
comes out at once, so `bp_at` no longer finds an inserted breakpoint at `rip`, and
`step_over_breakpoint` has nothing to do: the original instruction is already there. All 12
tests still pass.

</details>

Sources for this chapter: [REFERENCES.md](REFERENCES.md#chapter-2--breakpoints)
