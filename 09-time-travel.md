# Chapter 9 — Time Travel

Every bug so far was found by stopping *before* the damage and stepping toward it. This chapter
is about bugs where the damage shows up long after the cause: a balance wrong at the end, a
crash in `printf` caused by a parse thousands of instructions earlier. gdb can run a program
backwards: `record` logs what each instruction changes, the `reverse-*` commands and `record
goto` undo it, and a watchpoint run backwards stops at the write that did the damage.
`checkpoint` and `restart` are fork-based bookmarks. You will also meet the real limits of
`record full` in gdb 12.1. tdb v0.9 gets checkpoints (it makes the program call `clone()`) and a
record and replay that works the other way from gdb's: it logs only addresses and system-call
results, and goes back by replaying a snapshot forward.

Previous: [Chapter 8](08-automating-gdb.md). Next: [Chapter 10](10-remote-debugging.md).

## A balance that is wrong at the end

`examples/ch09/ledger.c`
```c
/* ledger.c - read deposits ("NAME AMOUNT" lines), keep a balance per
 * account, and print the accounts. The money in the accounts should add
 * up to the money deposited. It does not.
 *
 *     ./ledger deposits.txt
 */
#include <stdio.h>

#define MAX_ACCOUNTS 8

struct account {
    long balance;
    char name[8];
};

struct account accounts[MAX_ACCOUNTS];
int naccounts;

/* Copy a name into an account. Names are short. */
void set_name(struct account *a, const char *name)
{
    int i;

    for (i = 0; name[i] != '\0'; i++)
        a->name[i] = name[i];
    a->name[i] = '\0';
}

int same_name(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The account called name; a new one if there is none yet. */
struct account *find(const char *name)
{
    for (int i = 0; i < naccounts; i++)
        if (same_name(accounts[i].name, name))
            return &accounts[i];
    if (naccounts == MAX_ACCOUNTS)
        return NULL;
    struct account *a = &accounts[naccounts++];
    set_name(a, name);
    return a;
}

int main(int argc, char *argv[])
{
    char name[64];
    long amount, deposited = 0, held = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: ledger FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    while (fscanf(f, "%63s %ld", name, &amount) == 2) {
        struct account *a = find(name);
        if (a != NULL) {
            a->balance += amount;
            deposited += amount;
        }
    }
    fclose(f);
    for (int i = 0; i < naccounts; i++) {
        printf("%-8.8s %6ld\n", accounts[i].name, accounts[i].balance);
        held += accounts[i].balance;
    }
    printf("deposited %ld, held %ld\n", deposited, held);
    return held == deposited ? 0 : 1;
}
```

`examples/ch09/deposits.txt`
```text
alice 100
bob 250
carol 75
alice 20
frederick 300
bob -50
carol 25
erin 60
alice 5
erin 15
```

```console
$ make
cc -std=c11 -Wall -Wextra -g -O0    ledger.c   -o ledger
cc -std=c11 -Wall -Wextra -g -O0    todo.c   -o todo
$ ./ledger deposits.txt
alice       125
bob         200
carol       100
frederic    300
erin        182
deposited 800, held 907
$ echo $?
1
```

- alice (100 + 20 + 5), bob (250 − 50) and carol (75 + 25) are right. erin should have
  60 + 15 = 75. She has 182, and the books hold 107 more than was deposited.
- `frederic` is only `%-8.8s` cutting the name at 8 characters. It is a clue, though.
- Watching erin's balance from the moment her account exists shows two correct deposits: the
  107 was there *before*. You would have to know which address to watch before the run began,
  and you know it only at the end. So stop where the damage is visible, ask "who wrote this?",
  and let the debugger run back to the answer.

## Recording and running backwards

gdb's `record` (short for `record full`) starts a log. From then on gdb single-steps the
program and, before each instruction, saves the registers and memory that the instruction is
about to change. With that log it can undo instructions one at a time: that is reverse
execution. The script `s1.gdb` holds the commands below (`gdb -q -x s1.gdb ./ledger` runs
them), but type them yourself the first time.

```console
$ gdb -q ./ledger
Reading symbols from ./ledger...
(gdb) break 65
Breakpoint 1 at 0x1467: file ledger.c, line 65.
(gdb) run deposits.txt
Starting program: .../ledger deposits.txt
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Breakpoint 1, main (argc=2, argv=0x7fffffffe4b8) at ledger.c:65
65	    while (fscanf(f, "%63s %ld", name, &amount) == 2) {
(gdb) delete 1
(gdb) record
(gdb) break 72
Breakpoint 2 at 0x55555555548c: file ledger.c, line 72.
(gdb) continue
Continuing.

Breakpoint 2, main (argc=2, argv=0x7fffffffe4b8) at ledger.c:72
72	    fclose(f);
(gdb) print accounts[4]
$1 = {balance = 182, name = "erin\000\000\000"}
(gdb) print sizeof(struct account)
$2 = 16
(gdb) set can-use-hw-watchpoints 0
(gdb) watch -l accounts[4].balance
Watchpoint 3: -location accounts[4].balance
```

- `record` is silent. It starts at line 65, after `fopen`; why not at `main` is in
  [What record full cannot do](#what-record-full-cannot-do).
- Line 72 is right after the reading loop: every balance is final there, and erin's is 182.
- `set can-use-hw-watchpoints 0` makes the watchpoint a *software* one ("Watchpoint 3", not
  "Hardware watchpoint 3"). A hardware watchpoint does not work backwards (same section).
- `-l` (`-location`) watches the 8 bytes at the address `&accounts[4].balance`, not the
  expression.

Now run back to the last change:

```console
(gdb) reverse-continue
Continuing.

Watchpoint 3: -location accounts[4].balance

Old value = 182
New value = 167
0x000055555555545c in main (argc=2, argv=0x7fffffffe4b8) at ledger.c:68
68	            a->balance += amount;
(gdb) reverse-continue
Continuing.

Watchpoint 3: -location accounts[4].balance

Old value = 167
New value = 107
0x000055555555545c in main (argc=2, argv=0x7fffffffe4b8) at ledger.c:68
68	            a->balance += amount;
(gdb) reverse-continue
Continuing.

Watchpoint 3: -location accounts[4].balance

Old value = 107
New value = 0
0x000055555555523b in set_name (a=0x555555558070 <accounts+48>, name=0x7fffffffe350 "frederick") at ledger.c:25
25	        a->name[i] = name[i];
```

Read "Old" and "New" in the direction you travel: going backwards, "Old value" is the value
*after* the write (later in time) and "New value" the one *before* it. The first stop undoes
erin's last deposit (182 → 167, the `+= 15`), the second her first one (167 → 107, the `+= 60`),
and the third shows where the 107 came from: before this instruction the balance was 0.

The third stop is not in erin's code at all. It is in `set_name`, copying `"frederick"` into
`a = accounts+48`:

```console
(gdb) bt
#0  0x000055555555523b in set_name (a=0x555555558070 <accounts+48>, 
    name=0x7fffffffe350 "frederick") at ledger.c:25
#1  0x0000555555555371 in find (name=0x7fffffffe350 "frederick") at ledger.c:47
#2  0x000055555555543f in main (argc=2, argv=0x7fffffffe4b8) at ledger.c:66
(gdb) print i
$3 = 8
(gdb) print name
$4 = 0x7fffffffe350 "frederick"
(gdb) print *a
$5 = {balance = 0, name = "frederic"}
```

A `struct account` is 16 bytes: 8 for `balance`, 8 for `name`. `accounts+48` is
`accounts[3]`, frederick's account. Its `name` takes bytes 56 to 63 of the array;
`accounts[4].balance` starts at byte 64. `i` is 8: the loop is about to write `name[8]`, one
past the end, which is the first byte of the next account's balance. The ninth letter of
"frederick" is `'k'`, 107 in ASCII. Then `a->name[9] = '\0'` writes a zero into the second
byte, which was zero anyway. Two deposits later, `find("erin")` takes `accounts[4]`, sets its
name and never touches its balance. It starts at 107.

You are stopped *before* the instruction that wrote the `'k'`, with the change undone. Two
more commands:

```console
(gdb) reverse-finish
Run back to call of #0  0x000055555555523b in set_name (a=0x555555558070 <accounts+48>, 
    name=0x7fffffffe350 "frederick") at ledger.c:25
0x000055555555536c in find (name=0x7fffffffe350 "frederick") at ledger.c:47
47	    set_name(a, name);
(gdb) info record
Active record target: record-full
Replay mode:
Lowest recorded instruction number is 1.
Current instruction number is 6732.
Highest recorded instruction number is 13635.
Log contains 13635 instructions.
Max logged instructions is 200000.
(gdb) continue
Continuing.

Watchpoint 3: -location accounts[4].balance

Old value = 0
New value = 107
set_name (a=0x555555558070 <accounts+48>, name=0x7fffffffe350 "frederick") at ledger.c:24
24	    for (i = 0; name[i] != '\0'; i++)
```

- `reverse-finish` runs back to the call of the current function, line 47 of `find`.
- `info record`: the log holds 13635 instructions (line 65 to 72), and you are at 6732 in
  **replay mode**: gdb is not running the program, it is moving through the log.
- `continue` in replay mode goes *forward* through the log, and the watchpoint stops after the
  write this time, so Old and New are the right way round.

Three `reverse-continue`s took you from "wrong at the end" to "the byte that did it". The fix
is exercise 1.

## The reverse commands

Every forward command has a reverse twin, with the same idea of "a line" and "a function":

| Command | Short | Goes back to |
|---|---|---|
| `reverse-stepi` | `rsi` | the previous instruction |
| `reverse-step` | `rs` | the previous line; into a function if that line called one |
| `reverse-next` | `rn` | the previous line in this function, calls skipped |
| `reverse-finish` | | the call of the current function |
| `reverse-continue` | `rc` | the last breakpoint or watchpoint hit before now |
| `record goto begin\|end\|N` | | instruction N of the log, the start, or the present |

(`set exec-direction reverse` turns the plain commands into their twins; the explicit ones are
clearer.) "The previous line" has surprises, so here is a tour (`s3.gdb`). Start as before,
with `break 65`, `run deposits.txt` and `delete` (it asks "Delete all breakpoints?"), then:

```console
(gdb) record
(gdb) break set_name
Breakpoint 2 at 0x555555555219: file ledger.c, line 24.
(gdb) continue
Continuing.

Breakpoint 2, set_name (a=0x555555558040 <accounts>, name=0x7fffffffe350 "alice") at ledger.c:24
24	    for (i = 0; name[i] != '\0'; i++)
(gdb) reverse-finish
Run back to call of #0  set_name (a=0x555555558040 <accounts>, name=0x7fffffffe350 "alice")
    at ledger.c:24
0x000055555555536c in find (name=0x7fffffffe350 "alice") at ledger.c:47
47	    set_name(a, name);
(gdb) reverse-next
47	    set_name(a, name);
(gdb) reverse-next
46	    struct account *a = &accounts[naccounts++];
(gdb) reverse-step
46	    struct account *a = &accounts[naccounts++];
(gdb) reverse-step
44	    if (naccounts == MAX_ACCOUNTS)
(gdb) next
46	    struct account *a = &accounts[naccounts++];
```

- After `reverse-finish` you are in the *middle* of line 47, on its `call` (hence the address).
  The first `reverse-next` goes to the **start** of line 47, four instructions back
  (`disassemble /s find`). It looks as if it did nothing.
- The second `reverse-next` goes to line 46. The first `reverse-step` stays on 46 because
  line 46 has two rows in the line table (Chapter 6): one at the code that loads and
  increments `naccounts`, one at the code that computes the address in `accounts`. Each row
  start counts as a line start. The next `reverse-step` reaches 44, skipping 45 (`return NULL`),
  which never ran.
- `next` still goes forward: you can go back and forth as often as you like. `info record` now
  says "Current instruction number is 1576." of 1595.

`record goto` jumps to an instruction number, the start or the end:

```console
(gdb) record goto begin
Go backward to insn number 0
#0  main (argc=2, argv=0x7fffffffe4b8) at ledger.c:65
65	    while (fscanf(f, "%63s %ld", name, &amount) == 2) {
(gdb) record goto end
Go forward to insn number 1595
#0  set_name (a=0x555555558040 <accounts>, name=0x7fffffffe350 "alice") at ledger.c:24
24	    for (i = 0; name[i] != '\0'; i++)
(gdb) info record
Active record target: record-full
Record mode:
...
(gdb) next
25	        a->name[i] = name[i];
(gdb) next
24	    for (i = 0; name[i] != '\0'; i++)
```

At the end of the log you are in the present, in **record mode**: the two `next`s execute
real instructions and add them to the log. The program only moves forward; your cursor moves
anywhere in the log.

## A crash caused long before

The second program crashes inside the C library, long after the bug.

`examples/ch09/todo.c`
```c
/* todo.c - run a to-do script. Each line of the file is a command:
 *
 *     add TEXT    add an item
 *     done N      mark item N as done
 *     list        print the items
 *
 * All commands are parsed first, then run, then echoed as a log.
 *
 *     ./todo plan.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CMDS 32
#define MAX_ITEMS 16

struct cmd {
    char verb[8];
    const char *arg;
};

struct item {
    char text[40];
    int done;
};

static char lines[MAX_CMDS][64];
static struct cmd cmds[MAX_CMDS];
static int ncmds;
static struct item items[MAX_ITEMS];
static int nitems;

/* Split "verb rest" into c->verb and c->arg (the rest). */
static void parse(char *line, struct cmd *c)
{
    line[strcspn(line, "\n")] = '\0';
    char *space = strchr(line, ' ');
    c->arg = space + 1;
    if (space != NULL)
        *space = '\0';
    snprintf(c->verb, sizeof c->verb, "%s", line);
}

static void run(const struct cmd *c)
{
    if (strcmp(c->verb, "add") == 0 && nitems < MAX_ITEMS) {
        snprintf(items[nitems].text, sizeof items[nitems].text, "%s", c->arg);
        items[nitems++].done = 0;
    } else if (strcmp(c->verb, "done") == 0) {
        int n = atoi(c->arg);
        if (n >= 1 && n <= nitems)
            items[n - 1].done = 1;
    } else if (strcmp(c->verb, "list") == 0) {
        for (int i = 0; i < nitems; i++)
            printf("%2d [%c] %s\n", i + 1, items[i].done ? 'x' : ' ',
                   items[i].text);
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: todo FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    while (ncmds < MAX_CMDS && fgets(lines[ncmds], sizeof lines[0], f) != NULL) {
        parse(lines[ncmds], &cmds[ncmds]);
        ncmds++;
    }
    fclose(f);
    for (int i = 0; i < ncmds; i++)
        run(&cmds[i]);
    printf("log:\n");
    for (int i = 0; i < ncmds; i++)
        printf("  %s %s\n", cmds[i].verb, cmds[i].arg);
    return 0;
}
```

`examples/ch09/plan.txt`
```text
add buy milk
add fix bike
add call mum
done 2
list
add water plants
done 1
```

```console
$ ./todo plan.txt
 1 [ ] buy milk
 2 [x] fix bike
 3 [ ] call mum
log:
  add buy milk
  add fix bike
  add call mum
  done 2
Segmentation fault (core dumped)
$ echo $?
139
```

(Whether the shell says "(core dumped)" depends on the system's core settings; Chapter 5.)
The items are right, and so are the first four log lines. The fifth would be `list`. A
backtrace of the crash shows *where* it died but not *why*; the why is a pointer that went bad
during parsing, before any command ran. Record from the start of the parse loop and let it
crash (`s2.gdb`):

```console
$ gdb -q ./todo
Reading symbols from ./todo...
(gdb) set environment GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-AVX_Fast_Unaligned_Load
(gdb) break 72
Breakpoint 1 at 0x15fd: file todo.c, line 72.
(gdb) run plan.txt
Starting program: .../todo plan.txt
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Breakpoint 1, main (argc=2, argv=0x7fffffffe478) at todo.c:72
72	    while (ncmds < MAX_CMDS && fgets(lines[ncmds], sizeof lines[0], f) != NULL) {
(gdb) delete 1
(gdb) record
(gdb) continue
Continuing.
 1 [ ] buy milk
 2 [x] fix bike
 3 [ ] call mum
log:
  add buy milk
  add fix bike
  add call mum
  done 2

Program received signal SIGSEGV, Segmentation fault.
__strlen_sse2 () at ../sysdeps/x86_64/multiarch/strlen-vec.S:126
126	../sysdeps/x86_64/multiarch/strlen-vec.S: No such file or directory.
```

The first line, `set environment GLIBC_TUNABLES=...`, is needed because of an AVX2 limit of
gdb 12.1 explained in the next section; without it the record stops after 590 instructions.
The crash is in `strlen`, and the "No such file or directory" only means that the glibc
sources are not installed. Go up to our code:

```console
(gdb) bt
#0  __strlen_sse2 () at ../sysdeps/x86_64/multiarch/strlen-vec.S:126
#1  0x00007ffff7c76cf1 in __vfprintf_internal (s=0x7ffff7e1b780 <_IO_2_1_stdout_>, 
    format=0x55555555603d "  %s %s\n", ap=ap@entry=0x7fffffffe260, mode_flags=mode_flags@entry=0)
    at ./stdio-common/vfprintf-internal.c:1517
#2  0x00007ffff7c6079f in __printf (format=<optimized out>) at ./stdio-common/printf.c:33
#3  0x00005555555556e1 in main (argc=2, argv=0x7fffffffe478) at todo.c:81
(gdb) frame 3
#3  0x00005555555556e1 in main (argc=2, argv=0x7fffffffe478) at todo.c:81
81	        printf("  %s %s\n", cmds[i].verb, cmds[i].arg);
(gdb) print i
$1 = 4
(gdb) print cmds[i]
$2 = {verb = "list\000\000\000", arg = 0x1 <error: Cannot access memory at address 0x1>}
```

(Frames #1 and #2 show glibc's file names and arguments because `libc6-dbg` is installed here:
`sudo apt install libc6-dbg`. Without it you see fewer details in those frames.)

`cmds[4].arg` is `0x1`. `printf("%s")` called `strlen` on it, and address 1 is not mapped.
Who stored a 1 in a pointer?

```console
(gdb) set can-use-hw-watchpoints 0
(gdb) watch -l cmds[i].arg
Watchpoint 2: -location cmds[i].arg
(gdb) reverse-continue
Continuing.

Watchpoint 2: -location cmds[i].arg

Old value = 0x1 <error: Cannot access memory at address 0x1>
New value = 0x0
0x00005555555552fe in parse (line=0x555555558140 <lines+256> "list", c=0x555555558880 <cmds+64>) at todo.c:39
39	    c->arg = space + 1;
(gdb) print space
$3 = 0x0
(gdb) bt
#0  0x00005555555552fe in parse (line=0x555555558140 <lines+256> "list", 
    c=0x555555558880 <cmds+64>) at todo.c:39
#1  0x00005555555555ee in main (argc=2, argv=0x7fffffffe478) at todo.c:73
(gdb) info record
...
Current instruction number is 4362.
Highest recorded instruction number is 21707.
...
```

One `reverse-continue` crossed 17345 instructions (21707 − 4362): the whole `run` loop, the
`printf`s, the four good log lines. It stopped in `parse` for the fifth line, `"list"`, which
has no space. `strchr` returned NULL, and line 39 computed `NULL + 1` (undefined behaviour in
C; here, the address 1) *before* line 40 checked for NULL. `run` never looked at the argument
of `list`, so nothing went wrong until the log loop printed it. The fix is exercise 2.

`-l` matters here. Without it gdb watches the *expression*, which changes when `i` does: tried
here, the reverse run stops in the log loop at line 80 where `i` became 4, "New value" the
`"2"` of `done 2`. Correct, and useless. `-l` watches the 8 bytes, whoever writes them.

## What record full cannot do

`record full` works by knowing, for every x86 instruction, which registers and memory it
writes, and for every system call, what memory the kernel writes. That model has gaps, and on a
stock Ubuntu 22.04 with gdb 12.1 you will hit them. Each one below is a real session.

**AVX2 instructions.** The same todo session without the tunable:

```console
(gdb) record
(gdb) continue
Continuing.
Process record does not support instruction 0xc5 at address 0x7ffff7d99389.
Process record: failed to record execution log.

Program stopped.
__memchr_avx2 () at ../sysdeps/x86_64/multiarch/memchr-avx2.S:72
72	../sysdeps/x86_64/multiarch/memchr-avx2.S: No such file or directory.
(gdb) x/i $pc
=> 0x7ffff7d99389 <__memchr_avx2+9>:	vmovd  %esi,%xmm0
```

`info record` says 590 instructions. `0xc5` is a VEX prefix, the start of an AVX instruction, and
gdb 12.1's recorder has no entry for `vmovd`. glibc picks its string functions at startup by the
CPU's features (its IFUNCs, Chapter 3): with AVX2, `memchr` is `__memchr_avx2`. The tunable
`glibc.cpu.hwcaps=-AVX2,-AVX_Fast_Unaligned_Load` makes glibc pick the SSE2 versions, which gdb
can record. It must be in the environment when the program starts: `set environment` before `run`,
or `GLIBC_TUNABLES=... gdb ...` in the shell. Even `printf` needs it: recording `ledger` from line
72 without it dies in `__strchrnul_avx2` after 662 instructions. (`s1.gdb` gets away without it:
the `fscanf` calls of lines 65–72 happen not to reach an AVX2 function.) Patches that teach the
recorder some AVX instructions were posted in 2024; this section is about gdb 12.1.

**The getrandom system call.** Record from `break main` (line 52):

```console
(gdb) record
(gdb) continue
Continuing.
Process record and replay target doesn't support syscall number 318
Process record: failed to record execution log.

Program stopped.
0x00007ffff7c46c85 in __GI___getrandom (buffer=buffer@entry=0x7ffff7e214d8 <tcache_key>, length=length@entry=8, flags=flags@entry=1) at ../sysdeps/unix/sysv/linux/getrandom.c:29
29	../sysdeps/unix/sysv/linux/getrandom.c: No such file or directory.
```

`bt` shows `tcache_key_initialize` (malloc.c:3162) under it. gdb's system call table has no entry
for `getrandom` (318 on x86-64), and glibc 2.35's `malloc` calls it on first use, to seed a key
that protects its free lists. The first `malloc` here is inside `fopen`. Start the record after
it: that is why `s1.gdb` starts at line 65 and `s2.gdb` at line 72.

**Hardware watchpoints in replay.** The `ledger` session without `set can-use-hw-watchpoints
0`:

```console
(gdb) watch -l accounts[4].balance
Hardware watchpoint 3: -location accounts[4].balance
(gdb) reverse-continue
Continuing.

No more reverse-execution history.
main (argc=2, argv=0x7fffffffe4b8) at ledger.c:65
65	    while (fscanf(f, "%63s %ld", name, &amount) == 2) {
```

It ran back past all three changes to instruction 0. A hardware watchpoint is a debug register
(Chapter 4), checked by the CPU when *it* executes a store. In replay the CPU executes nothing:
gdb puts the old values back itself, with ptrace. A software watchpoint is checked by gdb after
each instruction it replays. No error, just no stop: this is the trap of the chapter.

**Branch tracing.** `record btrace` uses the processor's branch trace hardware (Intel
Processor Trace or BTS, through perf events): much faster, but it records only the path, not
the data, so you can step back but not print old values. Here it fails: "Could not enable
branch tracing for Thread ... Failed to start recording: No such file or directory".

**The end of the program.** Once the process exits, its memory is gone, and with it anything to
go back into. So gdb stops before `exit_group`. Recording `ledger` from line 72 (with the
tunable) and continuing prints the totals, then:

```console
The next instruction is syscall exit_group.  It will make the program exit.  Do you want to stop the program?([y] or n) [answered Y; input not from terminal]
Process record: inferior program stopped.

Program stopped.
0x00007ffff7ceabef in __GI__exit (status=status@entry=1) at ../sysdeps/unix/sysv/linux/_exit.c:30
```

That was a batch run, which answers yes; an interactive session asks. You can still go back
from there. A reverse command with no record running is refused: `reverse-stepi` → "Target
multi-thread does not support this command." ("multi-thread" because libthread_db is loaded).

**Speed.** gdb records about 6000 instructions a second here: 28 s for the 170206 instructions
of `spin 10000` (the table in [Building tdb: record and replay](#building-tdb-record-and-replay)).
Record the last stretch only: break near the bug, then `record`. The log stops at 200000
instructions unless you raise `set record full insn-number-max`.

**Writing in the past.** Change memory while replaying, and the log no longer describes what the
program would do. Back at the first `find` (`break find`, `continue` twice, `reverse-continue`),
`print naccounts = 0` gets:

```text
Because GDB is in replay mode, writing to memory will make the execution log unusable from this point onward.  Write memory at address 0x5555555580c0?(y or n) [answered Y; input not from terminal]
```

Even writing the value that is already there (0 over 0) cuts the log at the current instruction:
`info record` says "Record mode" with 1569 instructions, and `next` runs the program for real.
Look in the past; change things in the present, or in a checkpoint.

When `record full` is too slow or too limited, people use **rr** (see [In the
wild](#in-the-wild)). It is not installed on this machine, so this course shows no rr output.

## Checkpoints: bookmarks in a run

A checkpoint is cheaper than a record. `checkpoint` forks the stopped program: the copy is kept
stopped, and `restart N` makes it the program being debugged. Nothing is logged, the program
runs at full speed, and you can only go back to where you put the checkpoints. Use it to try a
fix by hand and compare two outcomes in one session (`s4.gdb`). After `break 72` and `run
deposits.txt`:

```console
(gdb) checkpoint
checkpoint 1: fork returned pid 1085707.
(gdb) print accounts[4].balance = 75
$1 = 75
(gdb) next
73	    for (int i = 0; i < naccounts; i++) {
(gdb) next
74	        printf("%-8.8s %6ld\n", accounts[i].name, accounts[i].balance);
(gdb) info checkpoints
* 0 Thread 0x7ffff7fb0740 (LWP 1085703) (main process) at 0x0
  1 process 1085707 at 0x55555555548c, file ledger.c, line 72
(gdb) restart 1
Switching to Thread 0x7ffff7fb0740 (LWP 1085707)
#0  main (argc=2, argv=0x7fffffffe4b8) at ledger.c:72
72	    fclose(f);
(gdb) print accounts[4].balance
$2 = 182
```

- `checkpoint` names the copy by its pid, 1085707; the program goes on as 1085703.
- In `info checkpoints`, 0 is the program (`*` marks the one being debugged) and 1 the copy.
- After `restart 1` the copy is debugged. It was made before the `= 75`, so it has 182, while
  the original waits at line 74 with its 75. Let the copy run:

```console
(gdb) continue
Continuing.
alice       125
...
erin        182
deposited 800, held 907
[Inferior 1 (process 1085707) exited with code 01]
[Switching to Thread 0x7ffff7fb0740 (LWP 1085703)]
[Switching to process 1085703]
(gdb) info checkpoints
No checkpoints.
(gdb) print accounts[4].balance
$3 = 75
(gdb) continue
Continuing.
alice       125
...
erin         75
deposited 800, held 800
[Inferior 1 (process 1085703) exited normally]
```

When the copy exited, gdb **switched back by itself** to the original, still at line 74 with
the 75. One run, two endings: 907 with erin's 182, 800 with 75. That confirms the diagnosis
before you edit a line of code.

The checkpoint is a forked copy (gdb's `linux-fork.c`): another pid, the same memory and
registers. A process and its fork share open files *including* their offsets, so gdb notes
each file's offset at `checkpoint` and puts it back on `restart`. That includes output files:
run the program with `> out.txt`, restart a checkpoint, and the copy writes over what the first
run wrote (tried here: the file holds one copy of the output, not two). Text already on a
terminal stays there: a checkpoint does not undo the outside world. It costs one fork but only
gets you back to your bookmarks; a record goes anywhere, at a thousand times the cost.

## Building tdb: checkpoints

The project for this chapter is tdb v0.9 in `project/ch09/`. `diff -ru project/ch08
project/ch09` shows two new modules, `checkpoint.c` (252 lines) and `record.c` (732), and small
changes elsewhere: `run.c` sends `continue` and `stepi` through the record when it is on,
`proc.c` gains `proc_adopt` and `proc_move`, `breakpoint.c` gains `bp_watches`, `step.c` exports
`is_call`, and `tdb.c` gets the new commands. The new targets `ledger.c` and `todo.c` are the
files of `examples/ch09/`, with the same line numbers.

The first problem is surprising: a debugger cannot fork its program. `fork()` copies the
*calling* process, and no system call forks another one. So tdb makes the program call it,
for one instruction. `ckpt_fork` in `src/checkpoint.c`, shortened:

```c
    if ((long long)saved.orig_rax != -1) {
        printf("tdb: the program is in a system call; stepi first.\n");
        return -1;
    }
    errno = 0;
    long code = ptrace(PTRACE_PEEKTEXT, pid, (void *)saved.rip, NULL);
    ...
    /* 0f 05: syscall. Little-endian: the low bytes of the word come first. */
    long inject = (long)(((unsigned long)code & ~0xffffUL) | 0x050f);
    regs = saved;
    regs.rax = SYS_clone;
    regs.rdi = CLONE_PARENT | SIGCHLD;  /* flags; the exit signal is in the low byte */
    regs.rsi = 0;                       /* the stack: the same (copied) one */
    regs.rdx = regs.r10 = regs.r8 = 0;
    if (ptrace(PTRACE_POKETEXT, pid, (void *)saved.rip, (void *)inject) < 0)
        return fail("POKETEXT");
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) < 0)
        return fail("SETREGS");
    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) < 0 || wait_stop(pid, &status) < 0)
            return fail("SINGLESTEP");
        if (status >> 16 == PTRACE_EVENT_FORK) {
            ptrace(PTRACE_GETEVENTMSG, pid, NULL, &child);
            continue;
        }
        if (WSTOPSIG(status) == SIGTRAP)
            break;
    }
    ...   /* put code and saved back in both processes; return child */
```

- It is Chapter 2's breakpoint trick with another instruction. Save the registers and the 8
  bytes at `rip`; write the two bytes of `syscall` (`0f 05`) over the first two; put the
  system call number in `rax` and its arguments in `rdi`, `rsi`, `rdx`, `r10`, `r8` (the
  kernel's convention, Chapter 4); single-step that one instruction.
- tdb has used `PTRACE_O_TRACEFORK` since Chapter 7, so the kernel stops the program with
  `PTRACE_EVENT_FORK` (`status >> 16`), `PTRACE_GETEVENTMSG` gives the copy's pid, and the copy
  starts already traced by tdb, stopped by a `SIGSTOP`. Then the step ends with the usual
  `SIGTRAP`.
- Both processes now have the `syscall` bytes and a changed `rax`; both get the original code
  and registers back, and the copy is the program at the checkpoint, with another pid.
- Why `clone(CLONE_PARENT | SIGCHLD)` and not `fork`? With `fork` the *program* would be the
  copy's parent, and a copy that tdb kills would stay a zombie until the program reaped it,
  which it never will. With `CLONE_PARENT` the copy's parent is the program's parent: tdb,
  which reaps its copies itself (`ckpt_kill`).
- The `orig_rax` check refuses in the middle of a system call (after Ctrl-C in `read`, say),
  where the kernel is about to restart the interrupted call.
- The raw system call does not update glibc's cached thread id as `fork()` does, so `raise()`
  and `abort()` misbehave in a copy (a comment at the top of the file says so).

The rest of the module is bookkeeping. `ckpt_snapshot` refuses programs with more than one
thread ("tdb: only a program with one thread can be copied.": `fork` copies only the calling
thread) and takes tdb's `int3` bytes out of the copy with Chapter 7's `bp_remove_copy`.
`ckpt_restart` is the interesting one: it **forks the checkpoint again** (`ckpt_fork(c->pid)`),
kills the current process and debugs the new copy, so the checkpoint stays untouched and can be
restarted any number of times. `proc_adopt` (new in `proc.c`) makes `struct proc` describe the
already traced, stopped copy, with a status "as if it had just stepped". Then `bp_insert_all`
puts the breakpoints back, because the copy was made without them, and the watchpoints too:
`fork` does not copy debug registers (Chapter 7 found the same for threads).

The same session as gdb's, piped into `build/tdb build/targets/ledger` after `make` in
`project/ch09`: `checkpoint` at line 72 says "checkpoint 1: fork returned pid 1095260.", the
program exits with the buggy totals, and then:

```console
(tdb) restart 1
Switching to process 1095267
#0  main () at targets/ledger.c:72
72	    fclose(f);
(tdb) next
73	    for (int i = 0; i < naccounts; i++) {
(tdb) info checkpoints
  1 process 1095260 at 0x55555555548c, file targets/ledger.c, line 72
```

The restarted process is 1095267, not 1095260: checkpoint 1 is still there for the next
`restart 1`. Unlike gdb, tdb keeps the checkpoint after the program exits and does not switch
by itself. And tdb v0.9 does not restore file offsets: exercise 5 shows what goes wrong and
fixes it.

## Building tdb: record and replay

gdb's `record full` saves the old values of everything each instruction changes and goes back
by undoing: a model of every instruction (the AVX2 limit), costly to record, instant to go
back. tdb does it the other way. It logs the address of each instruction, its call depth, and,
for a system call, the result and the bytes the kernel wrote. To go back to instruction k, it
forks a snapshot taken at `record` and single-steps the new copy k times. The program is
deterministic, so the copy does what the program did, except where the outside world comes in:
system calls.

![tdb's record and replay on the ledger tour: the log along the top, the snapshot forked again and
stepped forward to state 1576 along the bottom, and gdb's way in the
corner](images/ch09-record-replay.png)

The figure follows the tour of [The reverse commands](#the-reverse-commands) in tdb. State 0
is the snapshot, a stopped copy made at `record` with `ckpt_snapshot`. State 1595 is the head:
the real program, put aside (`proc_move`) while you are in the past. The two system calls in
that stretch, `newfstatat` and a `read` of 91 bytes inside the first `fscanf`, are *emulated*
in the replay: the copy does not make them; tdb sets its registers to the logged result and
writes the logged bytes into its memory. The log, in `src/record.c`:

```c
struct entry {
    uint64_t pc;         /* the instruction */
    int32_t depth;       /* calls made minus returns, before it */
    int32_t sys;         /* a system call: its index in sys[]; else -1 */
};

struct sysrec {
    bool emulate;        /* give the recorded result; do not execute it */
    struct user_regs_struct after;   /* the registers after it */
    uint64_t addr;       /* emulate: the memory it wrote... */
    size_t len;
    unsigned char *data; /* ...and what */
};
```

`record_one` records one instruction. It reads the code at `rip`; recognizes a `syscall`
(`0f 05`) and, after the step, saves the registers and the written buffer (`written()` knows
`read`, `pread64`, `getrandom`, `fstat`, `newfstatat`, `clock_gettime` and `isatty`'s `ioctl`);
and counts depth with Chapter 6's `is_call` and a small `is_ret`. System calls that change
only the process itself (`brk`, `mmap`, `mprotect`, `rt_sigaction` and a few more, the
`re_execute` list) are made again in the replay, so the copy's memory is laid out as before.
`getrandom` is just another emulated call, so tdb can record from `main` where gdb 12.1
stops; and with no instruction model, AVX2 is no problem.

Going to a state is `go_to(p, target)`. Going forward in the past steps the same copy; going back
forks the snapshot again (`from_start`, as `restart` does); reaching the end of the log kills the
copy and makes the head the process again. Every step of the replay checks the copy's `rip` (and a
system call's `rax`) against the log (`verify`). The commands are searches in the log:

- `reverse-stepi` is `go_to(cur - 1)`.
- `reverse-continue` finds the last log entry at a breakpoint address (`last_bp`). With a
  watchpoint, `last_change` replays from 0 to now, reading the watched value after every
  instruction, and remembers the last change. A "hardware" watchpoint works backwards in tdb
  because tdb never asks the CPU: it compares values itself.
- `reverse-finish` walks back until the depth drops below the current one: that entry is the
  `call`.
- `reverse-next` walks back at the same depth (deeper entries are calls, skipped) to the start
  of the run of entries on the previous line.

The ledger hunt, in tdb (`watch` takes an address and a size; `accounts+64` is
`&accounts[4].balance`):

```console
$ printf 'break 65\nrun targets/deposits.txt\ndelete\nrecord\nbreak 72\ncontinue\nwatch accounts+64 8\nreverse-continue\nreverse-continue\nreverse-continue\nreverse-finish\ninfo record\n' | build/tdb build/targets/ledger
...
(tdb) watch accounts+64 8
Hardware watchpoint 3: accounts+64 (8 bytes at 0x555555558080, DR0)
(tdb) reverse-continue
Hardware watchpoint 3: accounts+64
Old value = 182 (0xb6)
New value = 167 (0xa7)
0x000055555555545c in main () at targets/ledger.c:68
68	            a->balance += amount;
...
(tdb) reverse-continue
Hardware watchpoint 3: accounts+64
Old value = 107 (0x6b)
New value = 0 (0x0)
0x000055555555523b in set_name () at targets/ledger.c:25
25	        a->name[i] = name[i];
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
...
System calls: 3 (3 emulated in the replay, 0 executed again).
```

The same stops, and the same instruction numbers as gdb: 6732 of 13635. Two debuggers that
work in opposite ways agree on every instruction. The todo hunt works too, recorded from
`main` with no tunable, and stops at `parse` line 39. One warning from that session: at the
crash, tdb's `bt` printed `#1 run () at todo.c:51`, a stale return address it found by
scanning the stack (Chapter 7's scan for frames in libc). gdb, which reads libc's unwind
tables, says `main` line 81. Trust the frame that stopped in your code.

**Divergence.** Write to memory in the past, and the replay no longer matches the log; tdb
notices at the next step. Back at the second `find`, `set naccounts 0` (it was 1), then
`next`:

```console
(tdb) next
tdb: the replay went another way at instruction 1205 (another address); back to the present.
```

gdb cuts the log at the write; tdb keeps the log and goes back to the head (exercise 6 is
about doing what gdb does). The other limits, each tested: the record stops before
`exit_group` ("record stop" to let the program exit); it refuses `clone` and `fork` (one
thread only); a signal deletes it ("tdb: the program is to take signal SIGSEGV: tdb cannot
record that; the / record is deleted."); and it holds 200000 instructions. Speed, on `spin
10000` (170206 instructions, the host busy, so ±30%):

| | gdb `record full` | tdb v0.9 |
|---|---|---|
| `continue` while recording | 28.0 s | 15.2 s (another run: 13.6 s) |
| `reverse-stepi` at the end | 0.0 s | 11.5 s (another run: 8.0 s) |
| `record goto begin` | 0.6 s | 0.0 s |

tdb records faster (one step and one register read per instruction) and goes back slowly (state
k costs k steps); gdb is the opposite. Exercise 4 fixes tdb's slow side the way rr does.

`make test` ends with "unit: 26 passed, 0 failed" and "itest: 80 passed, 0 failed"; the new
checks in `tests/itest.sh` cover the hunts above, checkpoints, divergence, exit, signals,
threads and a full log.

## In the wild

- **gdb.** Process record and the reverse commands came in GDB 7.0 (October 2009), together
  with Python scripting. Checkpoints are older: `linux-fork.c` carries copyright from 2005.
  gdb's checkpoint saves each open file's offset and, on restart, calls `lseek` *in the
  program* as an inferior function call (`call_lseek`). The manual says it "won't un-write
  data from a file", but rewinds the file pointer so it can be overwritten.
- **rr** (Mozilla; O'Callahan et al., USENIX ATC 2017) records only what is nondeterministic,
  system call results and signals, timed with hardware performance counters, so recording is
  cheap. It replays the whole process group exactly and serves gdb through gdb's remote
  protocol (Chapter 10). To go backwards it takes fork-based checkpoints during replay,
  tdb's approach plus exercise 4. The paper reports checkpoints "typically" under ten
  milliseconds.
- **UndoDB** (Undo, founded 2005 by Greg Law and Julian Smith) is a commercial reverse debugger
  for Linux.
- **`record btrace`** uses Intel's branch trace hardware: fast, path only, and not available
  here.
- **gdb and AVX.** gdb 12.1 stops at `vmovd`. Guinevere Larsen posted patches adding `vmovd`
  and `vmovq` to the recorder in 2024, a first step towards AVX support; check your gdb's
  release notes for which version carries them. With gdb 12.1, glibc's `glibc.cpu.hwcaps`
  tunable keeps the AVX2 string functions out of the way.

## Common mistakes

- **A hardware watchpoint in replay** never fires. `set can-use-hw-watchpoints 0` first.
- **Reading Old/New the wrong way round** going backwards: Old is the later value.
- **`record` too early** (`getrandom` in the first `malloc`) or **without the AVX2 tunable**:
  the record stops far from the bug.
- **`watch` without `-l`** going backwards: the expression changes with `i`.
- **Changing memory in the past** throws away the future (gdb cuts the log; tdb diverges).
- **Recording a whole long run**, or **expecting a checkpoint to undo output** on a terminal.

## Summary

- `record` logs from here on; `reverse-continue`, `reverse-step`, `reverse-next`,
  `reverse-finish`, `reverse-stepi` and `record goto` move back through the log; plain
  commands move forward in replay mode, and the end of the log is the present.
- A software watchpoint (`set can-use-hw-watchpoints 0`, `watch -l`) plus `reverse-continue`
  stops at the write that caused the damage.
- `record full` in gdb 12.1: no AVX2 (use the glibc tunable), no `getrandom`, about 6000
  instructions a second, 200000 by default, stops before exit.
- `checkpoint` / `restart N` fork the program: bookmarks at full speed, file offsets restored.
- tdb v0.9 makes the program fork itself (an injected `syscall`), and records by logging
  addresses and system-call results, going back by replaying a snapshot forward.

## Exercises

1. Fix `ledger.c` so that the books balance. Then think about what your fix does with two
   long names that start the same.

<details><summary>Solution</summary>

Bound the copy (lines 24–26):

```c
    for (i = 0; name[i] != '\0' && i < (int)sizeof a->name - 1; i++)
        a->name[i] = name[i];
    a->name[i] = '\0';
```

```console
$ ./ledger deposits.txt
...
frederi     300
erin         75
deposited 800, held 800
$ echo $?
0
```

Or make room: `char name[16];` gives "frederic    300" (still cut by `%-8.8s`) and the same
totals, exit 0. The bounded copy has a catch: "frederick" and "frederica" both become
"frederi" and share one account. A long name should be rejected, or the field made big
enough for every name the program accepts (`%63s` reads up to 63).

</details>

2. Fix `todo.c` so that the log prints all seven commands.

<details><summary>Solution</summary>

Line 39 must not compute `space + 1` before the NULL check:

```c
    c->arg = space != NULL ? space + 1 : "";
```

```console
$ ./todo plan.txt
...
log:
  add buy milk
  add fix bike
  add call mum
  done 2
  list 
  add water plants
  done 1
$ echo $?
0
```

(`list ` has a trailing space: the format is `"  %s %s\n"` with an empty argument.)

</details>

Exercises 3, 4 and 5 change tdb. Apply them one at a time, each to a fresh copy of
`project/ch09` (`cp -r project/ch09 ex3`, and so on): they touch the same files, and the README
hunks of exercise 5 do not apply on top of exercise 3.

3. Add `reverse-step` (alias `rs`) to tdb: like `reverse-next`, but going into a function whose
   call was on the previous line, at its last line. Functions without line information (libc)
   are skipped. Compare with gdb from `break 48`.

<details><summary>Solution</summary>

`rec_reverse_step` in `record.c` walks back from `cur`. A deeper entry with line information
means a call on this line: go to the start of the callee's last line (`run_start`). A deeper
entry without lines (libc) is skipped. A shallower entry is the call of this function: stop
at the last line seen, or, if that was the prologue, at the caller's line. An entry at the same
depth on another line is the previous line: stop there. `tdb.c` gets `CMD_RSTEP` and the `rs`
alias; one itest is added (81/81). From line 48, after `set_name` returned:

```console
(tdb) continue
Breakpoint 2, find () at targets/ledger.c:48
48	    return a;
(tdb) reverse-step
set_name () at targets/ledger.c:27
27	}
(tdb) reverse-step
26	    a->name[i] = '\0';
(tdb) reverse-step
24	    for (i = 0; name[i] != '\0'; i++)
```

Then 25, 24, 25: the same stops as gdb's `reverse-step`. From `break set_name` and
`reverse-continue`, `reverse-step` gives `find () at targets/ledger.c:47` at instruction 1585,
also as gdb. One difference remains in `reverse-next`: from `find` line 44, gdb skips line 41
(the `for` that ran zero times) and goes to `main` line 66; tdb stops at 41.

</details>

4. Make going back fast: take a snapshot every 10000 instructions while recording, and start
   each replay from the nearest one before the target. Time `reverse-stepi` on `spin 10000`
   before and after.

<details><summary>Solution</summary>

In `record.c`, `#define SNAP_EVERY 10000`, a table `static pid_t snaps[REC_MAX / SNAP_EVERY +
1]`, and `snap_before(target)`, which returns the last multiple of 10000 at or below the target
that has a snapshot (`snaps[k] != 0`; state 0 always has one).
`record_one` calls `ckpt_snapshot` when `len % SNAP_EVERY == 0`, but not right after a
system call (`orig_rax != -1`), where a copy cannot be made. `from_start(p, target)` forks
`snaps[snap_before(target) / SNAP_EVERY]` and sets `cur` to that state. `go_to` replays from a
snapshot when `target < cur` or when a snapshot lies between `cur` and the target.
`rec_discard` kills them, and `info record` counts them. With `make test`, the new itest passes
(81/81). On `spin 10000`, in seconds since the start (my machine):

| | v0.9 | with snapshots |
|---|---|---|
| `continue` (recording) | 11.78 | 12.05 |
| `reverse-stepi` | +11.3 | +0.01 |
| `record goto 100000` | +5.9 | +0.0 |
| `reverse-next` | +4.6 | +0.25 |

`info record` adds "Snapshots: 18, one every 10000 instructions.", at no visible cost.
`reverse-continue` with a watchpoint still replays the whole log; comparing
the value at each snapshot first would find the interval to replay. This is rr's design.

</details>

5. Make `restart` put file offsets back, as gdb does. First see the bug: checkpoint at line
   65, let the program finish, restart, continue.

<details><summary>Solution</summary>

The bug, in v0.9:

```console
$ printf 'break 65\nrun targets/deposits.txt\ncheckpoint\ndelete\ncontinue\nrestart 1\ncontinue\n' | build/tdb build/targets/ledger
...
deposited 800, held 907
[process exited with status 1]
(tdb) restart 1
...
(tdb) continue
deposited 0, held 0
[process exited with status 0]
```

The copy shares the open file with the program, which read it to the end. The fix, in
`checkpoint.c`: turn the injection in `ckpt_fork` into a general `syscall_in(pid, nr, a1, a2,
a3, &child)` (with `child` NULL for an ordinary call, returning `rax`). `save_offsets` reads
`/proc/PID/fdinfo/N` (its first line is `pos: N`) for every fd that is a regular file (`stat`
on `/proc/PID/fd/N`) and stores the offsets in the checkpoint. `ckpt_restart` then does:

```c
    for (int i = 0; i < c->nfiles; i++)
        syscall_in(pid, SYS_lseek, c->files[i].fd, c->files[i].pos, SEEK_SET, NULL);
```

Now `restart 1` and `continue` print the whole output again, "deposited 800, held 907", and
again after a second `restart 1`. The new itest passes (81/81). gdb does the same with an
inferior function call to `lseek`; tdb makes the system call directly and needs no libc.
A trap while testing: run tdb with its output to a *file*, and the restarted copy rewinds
fd 1, the file tdb's own transcript is going to, and writes over it. gdb does exactly the
same. Pipe through `cat` instead.

</details>

6. An open-ended idea, with no solution here: make tdb do what gdb does on a write in the
   past. Cut the log at `cur`, kill the old head, make the copy the present, and go on
   recording. What must happen to the snapshot, to the system-call records after `cur`, and
   to `live_depth`?

Sources for this chapter: [REFERENCES.md](REFERENCES.md#chapter-9--time-travel)
