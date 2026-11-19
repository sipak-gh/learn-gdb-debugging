# Chapter 3 — Looking at Data

A breakpoint stops the program at the right place; then you have to read what it holds.
learn-c, Chapter 10 covered `print`, `x/s`, `x/12xb`, `info locals` and `display`. This
chapter adds types (`whatis`, `ptype/o`, `sizeof`), formats, `x` in bytes, words and giants,
the value history and convenience variables (with a `while` loop over a linked list),
artificial arrays (`*p@n`), `set var`, frames, and the registers and stack frame underneath.
Then it looks at what is left without debug information: the ELF symbol table (names and
sizes) and the calling convention (arguments in `rdi`, `rsi`, …). tdb v0.3 learns the same
three things: it reads the symbol table (`break square`, `in main+54 ()`), names registers,
and reads and writes memory at any length and alignment, though `ptrace` moves 8-byte words.

Previous: [Chapter 2](02-breakpoints.md). Next: [Chapter 4](04-watchpoints-and-catchpoints.md).

## A name too long

`examples/ch03/roster.c`
```c
/* roster.c - read "name score" lines, keep the students in a list sorted
 * by score, and print the list.
 *
 *     ./roster < class.txt
 *
 * Bug: one name comes out wrong. Find out why with gdb's print and x.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct student {
    char name[8];
    int score;
    struct student *next;
};

/* Insert s into the list (highest score first); return the new head. */
static struct student *insert(struct student *head, struct student *s)
{
    if (head == NULL || s->score > head->score) {
        s->next = head;
        return s;
    }
    struct student *p = head;
    while (p->next != NULL && p->next->score >= s->score)
        p = p->next;
    s->next = p->next;
    p->next = s;
    return head;
}

static struct student *new_student(const char *name, int score)
{
    struct student *s = malloc(sizeof *s);
    if (s == NULL) {
        perror("malloc");
        exit(1);
    }
    /* Names longer than the field are cut short. */
    strncpy(s->name, name, sizeof s->name);
    s->score = score;
    s->next = NULL;
    return s;
}

int main(void)
{
    struct student *head = NULL;
    char name[64];
    int score;

    while (scanf("%63s %d", name, &score) == 2)
        head = insert(head, new_student(name, score));

    int rank = 1;
    for (struct student *s = head; s != NULL; s = s->next)
        printf("%d. %-8s %3d\n", rank++, s->name, s->score);

    while (head != NULL) {
        struct student *next = head->next;
        free(head);
        head = next;
    }
    return 0;
}
```

`examples/ch03/class.txt`
```text
Ada 85
Grace 78
Margaret 90
Linus 64
Ken 71
```

`examples/ch03/Makefile` builds three programs with `-std=c11 -Wall -Wextra -g -O0`:
`roster`, `stats` (for [Frames](#frames)) and `roster-nodebug`, which is roster.c without `-g`.

```console
$ cd examples/ch03
$ make
cc -std=c11 -Wall -Wextra -g -O0    roster.c   -o roster
cc -std=c11 -Wall -Wextra -g -O0    stats.c   -o stats
stats.c: In function ‘mean’:
stats.c:10:23: warning: ‘sizeof’ on array function parameter ‘data’ will return size of ‘const int *’ [-Wsizeof-array-argument]
   10 |     size_t n = sizeof data / sizeof data[0];
      |                       ^~~~
stats.c:8:30: note: declared here
    8 | static double mean(const int data[])
      |                    ~~~~~~~~~~^~~~~~
cc -std=c11 -Wall -Wextra -O0 -o roster-nodebug roster.c
$ ./roster < class.txt
1. MargaretZ  90
2. Ada       85
3. Grace     78
4. Ken       71
5. Linus     64
```

The list is sorted, but the top student is called `MargaretZ`. "Margaret" has 8 letters and
the field is `char name[8]`. The comment says long names are cut short, yet this one came out
*longer*, with a letter that is not in the input. roster.c compiles without a warning and
does not crash. (The `stats.c` warning is for [Frames](#frames). Do not ignore it.)

## Structures and pointers

Line 58 is the `printf` in the loop. On the first pass, `s` points at Margaret's node:

```console
$ gdb -q ./roster
Reading symbols from ./roster...
(gdb) break 58
Breakpoint 1 at 0x13b5: file roster.c, line 58.
(gdb) run < class.txt
Starting program: .../roster < class.txt
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Breakpoint 1, main () at roster.c:58
58	        printf("%d. %-8s %3d\n", rank++, s->name, s->score);
(gdb) print *s
$2 = {name = "Margaret", score = 90, next = 0x55555555a2b0}
(gdb) print s->name
$3 = "Margaret"
(gdb) ptype s
type = struct student {
    char name[8];
    int score;
    struct student *next;
} *
(gdb) print sizeof(struct student)
$4 = 24
(gdb) x/16xb s
0x55555555a2f0:	0x4d	0x61	0x72	0x67	0x61	0x72	0x65	0x74
0x55555555a2f8:	0x5a	0x00	0x00	0x00	0x00	0x00	0x00	0x00
(gdb) print/c s->score
$5 = 90 'Z'
(gdb) print &s->score
$7 = (int *) 0x55555555a2f8
```

- `print s->name` says `"Margaret"`, which looks fine. But gdb is printing an *array* of 8
  chars, and it knows where the array ends. C's `%s` does not know that. It reads until it
  finds a zero byte.
- `x/16xb s` shows the bytes. The first 8 are `M a r g a r e t` (0x4d … 0x74), and none of them
  is zero. The next byte, at `s + 8`, is 0x5a. `print &s->score` shows that address is the
  `score` field, and `print/c` shows 90 as a character: `'Z'`. So `printf("%s")` printed eight
  letters, then the low byte of the score, then stopped at the zeros after it.
- The cause is `strncpy`. When the source is at least `count` bytes long, it copies exactly
  `count` bytes and writes **no** terminating zero. The shorter names get zero padding.

If Margaret had scored 0, the output would have looked right. With 7 points it would have
printed an invisible control character. Bugs like this hide easily.

### Types and formats

A second session at the same breakpoint asks about types:

```console
(gdb) whatis s->name
type = char [8]
(gdb) ptype/o struct student
/* offset      |    size */  type = struct student {
/*      0      |       8 */    char name[8];
/*      8      |       4 */    int score;
/* XXX  4-byte hole      */
/*     16      |       8 */    struct student *next;

                               /* total size (bytes):   24 */
                             }
```

`whatis` prints a type in one line. `ptype` expands structures to their fields, and `ptype/o`
adds each field's offset and size and marks the padding. A pointer must be 8-byte aligned (the
x86-64 psABI says so), so there is a 4-byte hole after `score` and the struct is 24 bytes,
not 20. `sizeof` works in gdb expressions with C's rules.

`print/F` prints a value in format F, and `x/NFU` examines N units of size U in format F.
The format letters are `x` hex, `d` signed, `u` unsigned, `o` octal, `t` binary, `c` char,
`a` address (with `<symbol+offset>`), plus `s` string and `i` instruction for `x`. The units
are `b` (1 byte), `h` (2), `w` (4) and `g` (giant, 8). The same session continues:

```console
(gdb) print/t s->score
$2 = 1011010
(gdb) print/x s->name
$4 = {0x4d, 0x61, 0x72, 0x67, 0x61, 0x72, 0x65, 0x74}
(gdb) print/x -1
$7 = 0xffffffff
(gdb) x/s s->name
0x55555555a2f0:	"MargaretZ"
(gdb) x/3xw s
0x55555555a2f0:	0x6772614d	0x74657261	0x0000005a
(gdb) x/2xg s
0x55555555a2f0:	0x746572616772614d	0x000000000000005a
```

(Some commands of this session are left out, hence the gaps in the `$` numbers.)

- A format applied to an array applies to each element. `print/x -1` is `0xffffffff` because
  the literal is a 4-byte `int`: a format changes how a value is written, never its size.
- `x/s` reads a C string from memory, so it says `"MargaretZ"`, like the program.
- `x/3xw` groups the same bytes into 4-byte words. The first is `0x6772614d`, which is
  `4d 61 72 67` ("Marg") backwards. x86-64 is **little-endian**: the byte at the lowest
  address is the least significant. `x/xb` shows memory order; `x/xw` and `x/xg` show the
  numbers those bytes make. The third word is the score, 90.

Memory is only bytes. A type or an `x` unit tells gdb how to group them, and a format tells
it how to write each group.

## Value history and convenience variables

Every `print` result goes into the **value history** as `$1`, `$2`, …. `$` is the last one,
and `$$n` is n back from it. The first session continues:

```console
(gdb) print *s->next
$8 = {name = "Ada\000\000\000\000", score = 85, next = 0x55555555a2d0}
(gdb) print s->next->next->name
$9 = "Grace\000\000"
(gdb) print $
$10 = "Grace\000\000"
(gdb) print $$2
$11 = {name = "Ada\000\000\000\000", score = 85, next = 0x55555555a2d0}
(gdb) set print pretty on
```

- gdb prints the whole array, so `strncpy`'s padding zeros show as `\000`. It leaves out the
  last zero, as a terminator. That is why "Ada" has four `\000` and not five. Margaret's array
  has no zero at all.
- `print $` makes a new entry, `$10`. When `print $$2` runs, `$10` is `$$0`, so `$$2` is `$8`.
  History values are copies. If the program changes the memory later, `$8` keeps the old value.
- `set print pretty on` prints one field per line from now on.

Two more variables are set by `x`: `$_` is the address of the last unit it examined, and
`$__` is that unit's contents. After `x/2xg s` in the second session, `print $_` gives
`(int64_t *) 0x55555555a2f8` and `print $__` gives 90. A plain `x` with no address continues
after `$_`.

### Convenience variables and a loop

A `$name` that is not a register or a history number is a **convenience variable**. It lives
in gdb, not in the program, and takes the type of whatever you assign to it. gdb's command
language has `while` and `if`, which is enough to walk the list:

```console
(gdb) set $p = head
(gdb) while $p
 >printf "%-10s %3d  next=%p\n", $p->name, $p->score, $p->next
 >set $p = $p->next
 >end
MargaretZ   90  next=0x55555555a2b0
Ada         85  next=0x55555555a2d0
Grace       78  next=0x55555555a330
Ken         71  next=0x55555555a310
Linus       64  next=(nil)
```

`$p` is a `struct student *` because `head` is one. gdb's `printf` command formats like C's,
and its `%s` reads a C string from the program's memory. So it prints `MargaretZ` too, unlike
`print`, which knows the array's length. [Chapter 8](08-automating-gdb.md) turns loops like
this into user commands.

`info locals` at this point shows the 64-byte input buffer as
`name = "Ken\000s\000et\000\350\377..."`. The last `scanf` wrote "Ken" and a zero over its
start and left the rest: `s` (the end of "Linus"), `et` (the end of "Margaret"), then whatever
was on the stack before. A buffer keeps the remains of every longer value written to it.

## Artificial arrays

`*p@n` makes gdb treat `*p` as the first of `n` consecutive objects: an **artificial array**.
A pointer does not say how many elements it points at, so this is how you print a run of
memory through one. A new session stops in `new_student` for Margaret only:

```console
$ gdb -q ./roster
Reading symbols from ./roster...
(gdb) break new_student if score == 90
Breakpoint 1 at 0x12d2: file roster.c, line 35.
(gdb) run < class.txt
Starting program: .../roster < class.txt
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Breakpoint 1, new_student (name=0x7fffffffe3c0 "Margaret", score=90) at roster.c:35
35	    struct student *s = malloc(sizeof *s);
(gdb) print *name@10
$2 = "Margaret", <incomplete sequence \350>
(gdb) print/x *name@10
$3 = {0x4d, 0x61, 0x72, 0x67, 0x61, 0x72, 0x65, 0x74, 0x0, 0xe8}
```

In the input buffer "Margaret" *does* end in a zero (byte 8). Byte 9 is 0xe8, stack leftovers
that gdb cannot print as a complete character. The left side of `@` must be an object in
memory (`*name`, `readings[3]`). `name@10` would be ten *pointers*, starting at the variable
`name`.

The trap is using `@` on things that are not in an array. Back at line 58 in the first
session, `*head->next@2` looks like "the next two nodes":

```console
(gdb) print *head->next@2
$13 = {{
    name = "Ada\000\000\000\000",
    score = 85,
    next = 0x55555555a2d0
  }, {
    name = "!\000\000\000\000\000\000",
    score = 1667330631,
    next = 0x4e
  }}
```

The second element is the 24 bytes after Ada's node, at `0x55555555a2c8`, and it is not
Grace's node:

- glibc's `malloc(24)` uses a 32-byte chunk, and an 8-byte size field sits in front of each
  chunk's memory. For the next chunk it is 0x20 (32) with the low bit set (PREV_INUSE: the
  chunk before is in use). 0x21 is `'!'`.
- `score` lands on the first 4 bytes of the next node, which is Grace's (read right after
  Ada, so allocated at `0x55555555a2d0`). Those bytes are `47 72 61 63`, "Grac", which is
  1667330631 as a little-endian `int`.
- `next` lands on Grace's `score` and the padding after it: 78, or 0x4e.

List nodes are wherever `malloc` put them. Use `@` on arrays and buffers, and follow the
pointers in a list.

## Changing data

`print x = value` assigns and prints. `set var x = value` assigns silently. Use `set var`,
not plain `set`, because `set` is also the command for gdb's own settings, and a variable
whose name starts like a setting is taken as one. At line 58:

```console
(gdb) set rank = 5
(gdb) print rank
$1 = 5
(gdb) set var rank = 6
(gdb) print rank
$2 = 6
(gdb) set s = 0
Ambiguous set command "s = 0": schedule-multiple, scheduler-locking, script-extension, serial, solib-absolute-prefix...
(gdb) print s
$3 = (struct student *) 0x55555555a2f0
```

Now test the fix in the running program. Continue the `new_student` session: `finish` runs to
the return and puts the return value in the history.

```console
(gdb) finish
Run till exit from #0  new_student (name=0x7fffffffe3c0 "Margaret", score=90) at roster.c:35
0x000055555555536a in main () at roster.c:54
54	        head = insert(head, new_student(name, score));
Value returned is $4 = (struct student *) 0x55555555a2f0
(gdb) print *$
$5 = {name = "Margaret", score = 90, next = 0x0}
(gdb) x/s $4->name
0x55555555a2f0:	"MargaretZ"
(gdb) set var $4->name[7] = '\0'
(gdb) x/s $4->name
0x55555555a2f0:	"Margare"
(gdb) continue
Continuing.
1. Margare   90
2. Ada       85
...
[Inferior 1 (process 470277) exited normally]
```

`$4` works like any pointer, though no variable holds it yet. Writing a zero into `name[7]`
is what the program should have done, and the output proves the diagnosis before you edit
anything. Then fix the source: copy one byte less and always terminate.

```c
    /* Names longer than the field are cut short. */
    strncpy(s->name, name, sizeof s->name - 1);
    s->name[sizeof s->name - 1] = '\0';
```

With this change, built with `-O2 -Wall -Wextra`, the program prints `1. Margare   90` and the
rest as before, with no warnings. (`snprintf(s->name, sizeof s->name, "%s", name)` also
terminates, but at `-O2` gcc 11 warns that it "may be truncated" (`-Wformat-truncation`), which
here is the intent.)

Could the compiler have caught the original? Only when optimising. At `-O0` there is no
warning, but at `-O2` gcc inlines `strncpy` and sees both sizes:

```console
$ gcc -std=c11 -O2 -Wall -Wextra -o roster-O2 roster.c 2>&1 | grep warning
/usr/include/x86_64-linux-gnu/bits/string_fortified.h:95:10: warning: ‘__builtin_strncpy’ output may be truncated copying 8 bytes from a string of length 63 [-Wstringop-truncation]
```

(The full message says the call was "inlined from ‘new_student’ at roster.c:41:5".) Many
warnings need analysis that only runs when optimising. An extra `-O2 -Wall -Wextra` build is a
cheap check, even if you debug at `-O0`.

## Frames

`examples/ch03/stats.c`
```c
/* stats.c - the mean and the largest of some measurements.
 *
 * Bug: the mean is wrong. The compiler even says why, if you read its
 * warnings; gdb shows it too.
 */
#include <stdio.h>

static double mean(const int data[])
{
    size_t n = sizeof data / sizeof data[0];
    long sum = 0;

    for (size_t i = 0; i < n; i++)
        sum += data[i];
    return (double)sum / n;
}

static int largest(const int *data, size_t n)
{
    int max = data[0];

    for (size_t i = 1; i < n; i++)
        if (data[i] > max)
            max = data[i];
    return max;
}

int main(void)
{
    int readings[10] = { 12, 15, 11, 19, 14, 22, 18, 13, 17, 16 };
    size_t n = sizeof readings / sizeof readings[0];

    printf("%zu readings, mean %.1f, largest %d\n", n, mean(readings),
           largest(readings, n));
    return 0;
}
```

```console
$ ./stats
10 readings, mean 13.5, largest 22
```

The readings add up to 157, so the mean is 15.7. Each call has a **frame** (its arguments,
locals and return address). `up` and `down` select a frame, and `print` and `info locals`
then work in it:

```console
$ gdb -q ./stats
Reading symbols from ./stats...
(gdb) break mean
Breakpoint 1 at 0x1175: file stats.c, line 10.
(gdb) run
Starting program: .../stats
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Breakpoint 1, mean (data=0x7fffffffe3d0) at stats.c:10
10	    size_t n = sizeof data / sizeof data[0];
(gdb) backtrace
#0  mean (data=0x7fffffffe3d0) at stats.c:10
#1  0x00005555555552f4 in main () at stats.c:33
(gdb) ptype data
type = const int *
(gdb) print sizeof data
$1 = 8
(gdb) print *data@10
$2 = {12, 15, 11, 19, 14, 22, 18, 13, 17, 16}
(gdb) up
#1  0x00005555555552f4 in main () at stats.c:33
33	    printf("%zu readings, mean %.1f, largest %d\n", n, mean(readings),
(gdb) ptype readings
type = int [10]
(gdb) print sizeof readings
$3 = 40
(gdb) print readings[3]@4
$5 = {19, 14, 22, 18}
(gdb) print &readings
$7 = (int (*)[10]) 0x7fffffffe3d0
(gdb) print readings + 1
$8 = (int *) 0x7fffffffe3d4
```

`data` was declared `const int data[]`, but `ptype` says `const int *`. In C a parameter
declared as an array *is* a pointer: the array decays to a pointer to its first element when
passed. `sizeof data` is 8, `n` is 8 / 4 = 2, and `mean` averages two readings: (12 + 15) / 2
= 13.5. One `up` later, `readings` is an `int [10]` of 40 bytes. The same expression gives
different answers in different frames, because gdb follows C exactly. `&readings` is a
pointer to the whole array (`int (*)[10]`). `readings + 1` decays first and is 4 bytes on.
The compiler said all this in `-Wsizeof-array-argument`, a warning that is on even without
`-Wall`. The fix is to pass the length, as `largest` does.

The machine code agrees: `x/4i $pc` disassembles line 10 as `movq $0x2,-0x8(%rbp)`, "store 2
into `n`". `sizeof` and the division happened at compile time.

### display, and what uninitialised looks like

`display EXPR` prints an expression at every stop. Back in frame 0 of the same session:

```console
(gdb) down
#0  mean (data=0x7fffffffe3d0) at stats.c:10
10	    size_t n = sizeof data / sizeof data[0];
(gdb) display sum
1: sum = 140737488348112
(gdb) display/x n
2: /x n = 0xa
(gdb) next
11	    long sum = 0;
1: sum = 140737488348112
2: /x n = 0x2
(gdb) next
13	    for (size_t i = 0; i < n; i++)
1: sum = 0
2: /x n = 0x2
(gdb) next
14	        sum += data[i];
1: sum = 0
2: /x n = 0x2
(gdb) undisplay 1
(gdb) finish
Run till exit from #0  mean (data=0x7fffffffe3d0) at stats.c:14
0x00005555555552f4 in main () at stats.c:33
33	    printf("%zu readings, mean %.1f, largest %d\n", n, mean(readings),
Value returned is $9 = 13.5
```

Look at the values *before* line 10 ran. `n` is `0xa`, which is 10 and looks right, but
nothing has assigned it yet. `sum` is 140737488348112, which is 0x7fffffffe3d0, the address
of `readings`. Both are leftovers from earlier use of those stack bytes. An uninitialised local
is not zero, and sometimes it holds a plausible value. Trust a variable only after the line
that sets it. `finish` prints a `double` here: `mean` returns in `xmm0`, and gdb knows that
from the type and the calling convention.

## Registers and the stack frame

gdb names registers `$rax`, `$rip` and so on, and has three aliases that work on any
architecture: `$pc`, `$sp` and `$fp`. A new session at the same breakpoint:

```console
(gdb) print $pc
$2 = (void (*)()) 0x555555555175 <mean+12>
(gdb) info registers rip rsp rbp rdi
rip            0x555555555175      0x555555555175 <mean+12>
rsp            0x7fffffffe3b0      0x7fffffffe3b0
rbp            0x7fffffffe3b0      0x7fffffffe3b0
rdi            0x7fffffffe3d0      140737488348112
(gdb) info frame
Stack level 0, frame at 0x7fffffffe3c0:
 rip = 0x555555555175 in mean (stats.c:10); saved rip = 0x5555555552f4
 called by frame at 0x7fffffffe420
 source language c.
 Arglist at 0x7fffffffe3b0, args: data=0x7fffffffe3d0
 Locals at 0x7fffffffe3b0, Previous frame's sp is 0x7fffffffe3c0
 Saved registers:
  rbp at 0x7fffffffe3b0, rip at 0x7fffffffe3b8
(gdb) x/2gx $rbp
0x7fffffffe3b0:	0x00007fffffffe410	0x00005555555552f4
(gdb) info symbol 0x00005555555552f4
main + 139 in section .text of /home/tbangma/learn-gdb-debugging/examples/ch03/stats
```

- `$pc` is `$rip` here. It has a function-pointer type, so gdb adds `<mean+12>`. 12 bytes in
  is the first instruction of line 10: the breakpoint is after the prologue.
- `rdi` holds the address of `readings`. The System V x86-64 ABI passes the first integer or
  pointer argument in `rdi`. The next section depends on that.
- "frame at 0x7fffffffe3c0" is the **canonical frame address** (CFA), the value of `rsp`
  just before the `call`. The `call` pushed the return address below it (at `...e3b8`), and
  the prologue pushed the caller's `rbp` below that (at `...e3b0`) and pointed `rbp` there.
- `x/2gx $rbp` reads both words: the saved `rbp` (`main`'s frame) and the return address.
  `info symbol` turns the return address into `main + 139`, the instruction after the call.
  A backtrace is built this way, one saved `rbp` and return address at a time
  ([Chapter 6](06-down-to-the-metal.md) shows how gdb does it without `rbp`).

![mean()'s stack frame: the saved rbp and return address at rbp, the locals n, i and sum
below rbp in the red zone, main's readings array above, data pointing at readings[0]](images/ch03-stack-frame.png)

`rsp` equals `rbp`: `mean` never moves the stack pointer down. It is a **leaf function** (it
calls nothing), so the ABI lets it use the 128 bytes below `rsp`, the *red zone*, without
reserving them. The next instructions (`x/4i $pc`) put `n` at `rbp - 8`, `sum` at
`rbp - 0x18` and `i` at `rbp - 0x10`.

Plain `info registers` lists the general registers, `rip`, `eflags` (with flag names, such as
`[ PF ZF IF ]`) and the segment registers. On my machine gdb 12.1 stops the list at `gs`, but
`$fs_base` and `$orig_rax` exist too. `info all-registers` adds the vector registers.

## Without debug info

`roster-nodebug` is roster.c built without `-g`. It has no types, no line numbers and no local
names. It has only the ELF symbol table, and the calling convention:

```console
$ gdb -q ./roster-nodebug
Reading symbols from ./roster-nodebug...
(No debugging symbols found in ./roster-nodebug)
(gdb) break new_student if $rsi == 90
Breakpoint 1 at 0x12c7
(gdb) run < class.txt
Starting program: .../roster-nodebug < class.txt
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".

Breakpoint 1, 0x00005555555552c7 in new_student ()
(gdb) info registers rdi rsi
rdi            0x7fffffffe3b0      140737488348080
rsi            0x5a                90
(gdb) x/s $rdi
0x7fffffffe3b0:	"Margaret"
(gdb) print (char *)$rdi
$2 = 0x7fffffffe3b0 "Margaret"
(gdb) info symbol $pc
new_student + 8 in section .text of /home/tbangma/learn-gdb-debugging/examples/ch03/roster-nodebug
(gdb) finish
Run till exit from #0  0x00005555555552c7 in new_student ()
0x000055555555536a in main ()
(gdb) x/24xb $rax
0x55555555a2f0:	0x4d	0x61	0x72	0x67	0x61	0x72	0x65	0x74
0x55555555a2f8:	0x5a	0x00	0x00	0x00	0x00	0x00	0x00	0x00
0x55555555a300:	0x00	0x00	0x00	0x00	0x00	0x00	0x00	0x00
```

- `new_student(name, score)`: `name` is in `rdi`, `score` in `rsi`. So `$rsi == 90` replaces
  `score == 90`, and `x/s $rdi` replaces `print name`. A cast, `(char *)$rdi`, gives gdb the
  type that debug info would have given.
- gdb stopped at `new_student + 8`, not `+0`. With no line table it analyses the prologue
  (`endbr64`, `push %rbp`, `mov %rsp,%rbp`: 4 + 1 + 3 bytes) and stops after it. The argument
  registers are still intact there. A few instructions later the compiler copies them to the
  stack and reuses the registers, so read them at the start of the function.
- `finish` cannot say "Value returned" without a type, but the value is in `rax`, and
  `x/24xb $rax` shows the new node: 8 letters, `Z`, padding and a `NULL` `next`.

| what (System V x86-64) | where |
|---|---|
| integer/pointer arguments 1–6 | `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` (more on the stack) |
| floating-point arguments 1–8 | `xmm0`–`xmm7` |
| return value | `rax` (integer/pointer), `xmm0` (floating point) |
| preserved across calls | `rbx`, `rbp`, `r12`–`r15` |

## How a debugger finds names: the ELF symbol table

Without `-g`, gdb still knew that `new_student` starts at `0x12bf`. That came from the
**symbol table** in the ELF file. The examples run in `project/ch03` after `make`, on tdb's new
demo target `data` (listed in the next section).

`readelf -h` shows the file header. It starts with the magic `7f 45 4c 46` (0x7f and `ELF`),
then says `ELF64`, little endian, type `DYN (Position-Independent Executable file)`, and gives
where the 37 section headers start. `readelf -S` lists them:

```console
$ readelf -S -W build/targets/data | grep -E 'Nr|dynsym|\.text|\.data |symtab|strtab'
  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  [ 6] .dynsym           DYNSYM          00000000000003d8 0003d8 0000a8 18   A  7   1  8
  [16] .text             PROGBITS        0000000000001060 001060 000161 00  AX  0   0 16
  [25] .data             PROGBITS        0000000000004000 003000 000040 00  WA  0   0 16
  [34] .symtab           SYMTAB          0000000000000000 003540 0003f0 18     35  18  8
  [35] .strtab           STRTAB          0000000000000000 003930 000202 00      0   0  1
```

- `DYN` means a position-independent executable: its addresses are offsets from the load base
  of [Chapter 2](02-breakpoints.md) (`0x555555554000` with randomisation off).
- Each section header says where the section is in the file. `.symtab` (type `SYMTAB`)
  starts at offset 0x3540, has 0x3f0 bytes of 0x18-byte entries (42 symbols), and links
  (`Lk`) to section 35, `.strtab`, which holds the names.
- `.symtab` has address 0 and no `A` (alloc) flag. It is not loaded into memory, so a
  debugger reads it from the *file*. `.dynsym` is the smaller table the dynamic linker needs.
  It is loaded, and it survives `strip`.

```console
$ readelf -s -W build/targets/data | grep -E ' (counter|main|bump|greeting|small|big|table)$'
    26: 0000000000004010    11 OBJECT  GLOBAL DEFAULT   25 greeting
    30: 000000000000401c     2 OBJECT  GLOBAL DEFAULT   25 small
    33: 0000000000004020     4 OBJECT  GLOBAL DEFAULT   25 counter
    35: 000000000000116d    84 FUNC    GLOBAL DEFAULT   16 main
    40: 0000000000001149    36 FUNC    GLOBAL DEFAULT   16 bump
```

A symbol has a value (its file address), a size, a type (`FUNC` or `OBJECT`) and a section
index (16 is `.text`, 25 is `.data`). `nm -S` shows the same table with letters (`T` text,
`D` data, `b` bss, lowercase for local `static` symbols such as `t deregister_tm_clones`).
The table has sizes but **no types**: `counter` is "4 bytes", not `int`.

Here is one entry, raw. Symbol 33 is at 0x3540 + 33 × 24 = 0x3858:

```console
$ xxd -s 0x3858 -l 24 build/targets/data
00003858: 9901 0000 1100 1900 2040 0000 0000 0000  ........ @......
00003868: 0400 0000 0000 0000                      ........
$ xxd -s $((0x3930+0x199)) -l 8 build/targets/data
00003ac9: 636f 756e 7465 7200                      counter.
```

```c
typedef struct {              /* <elf.h> */
    Elf64_Word    st_name;    /* 4: offset of the name in the string table */
    unsigned char st_info;    /* 1: binding << 4 | type */
    unsigned char st_other;   /* 1: visibility */
    Elf64_Section st_shndx;   /* 2: section index */
    Elf64_Addr    st_value;   /* 8: the address */
    Elf64_Xword   st_size;    /* 8: the size in bytes */
} Elf64_Sym;
```

Little-endian, the bytes say: `st_name` 0x199, `st_info` 0x11 (`STB_GLOBAL` 1, `STT_OBJECT`
1), `st_other` 0, `st_shndx` 0x19 = 25, `st_value` 0x4020, `st_size` 4. At `.strtab` + 0x199
is `counter` and a zero. That is all it takes to turn `counter` into `0x4020`.

`strip` removes `.symtab`:

```console
$ cp build/targets/data data-stripped && strip data-stripped
$ nm data-stripped
nm: data-stripped: no symbols
```

The file shrinks from 17888 to 14520 bytes. Only `.dynsym` is left (`nm -D`): the names the
program imports from libc, such as `U printf@GLIBC_2.2.5` (`U` for undefined), and four weak
ones. Its own functions are gone.

Types and line numbers come from other sections, added by `-g`. `readelf -S` lists
`.debug_aranges`, `.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_str` and
`.debug_line_str` in `roster`, and only `.symtab` in `roster-nodebug`. Those sections hold
**DWARF**, which [Chapter 6](06-down-to-the-metal.md) reads.

## tdb, step 3: symbols, registers, memory

At the end of [Chapter 2](02-breakpoints.md), tdb set breakpoints at numbers you looked up
with `nm`. v0.3 (`project/ch03/`) adds `src/symbols.c` (reads `.symtab`: name ↔ address,
`main+4`), `src/regs.c` (register names and `eflags` names), `src/mem.c` (one line of a hex
dump), `proc_read`/`proc_write` in `src/proc.c` (any length, any alignment), the commands
`regs`, `reg`, `x`, `print`, `set` and `info symbol` in `src/tdb.c`, a new target, and the
first unit tests.

`project/ch03/targets/data.c`
```c
/* data.c - globals of different sizes, a string and an array; a target for
 * reading and writing memory and registers. */
#include <stdio.h>

char greeting[] = "hello, tdb";
short small = -2;
int counter = 41;
long big = 0x1122334455667788;
int table[4] = { 10, 20, 30, 40 };

int bump(int by)
{
    counter += by;
    return counter;
}

int main(void)
{
    int r = bump(1);

    printf("%s: counter=%d r=%d table[2]=%d\n", greeting, counter, r, table[2]);
    return 0;
}
```

### Reading the symbol table

`sym_load` reads the whole file into memory. It checks the magic, the class and the section
header size, and it checks that every offset it uses lies inside the file, with
`fits(off, len, size)`, written as `off <= size && len <= size - off` so that it cannot
overflow. Then it finds the table by **type** (`SHT_SYMTAB`, else `SHT_DYNSYM`) and its
string table through `sh_link`. Then it copies what a debugger needs:

`project/ch03/src/symbols.c` (part)
```c
    for (size_t i = 0; i < count; i++) {
        int type = ELF64_ST_TYPE(st[i].st_info);
        if ((type != STT_FUNC && type != STT_OBJECT) || st[i].st_value == 0 ||
            st[i].st_name >= strtab->sh_size)
            continue;
        syms[nsyms++] = (struct symbol){
            .name = strings + st[i].st_name,
            .addr = st[i].st_value,
            .size = st[i].st_size,
            .kind = type == STT_FUNC ? SYM_FUNC : SYM_OBJECT,
        };
    }
    free(file);
    qsort(syms, nsyms, sizeof *syms, by_addr);
```

File names, sections and imports (value 0, like `printf`) cannot name an address in the
program, so they are skipped. `strings` is a copy of the string table with a zero added at the
end, so the file buffer can be freed. Sorted by address, `sym_by_addr` takes the last symbol
that starts at or before the address, and accepts it if the address is inside
`[addr, addr + size)`. A size-0 symbol matches only its exact address. Without that rule, every
address past the end of the code would be `_fini+something`. `sym_describe` writes `main` or
`main+13`. `sym_parse` reads `0x1149`, `4425`, `main`, `main+4` or `main+0x10`.

### File addresses and run-time addresses

Symbols hold file addresses (`bump` is `0x1149`). While the program runs, `bump` is at
`0x555555555149`. tdb follows gdb's rule: before `run`, a number is a file address. While the
program runs, it is a run-time address. A symbol is converted for you. For that, a small
function in `tdb.c`, `base()`, returns `proc.load_base` while the process is stopped
and 0 otherwise: "what to add to a file address to get the address the user sees".
`parse_location` accepts a number, a symbol, `symbol+offset` or a register (`$rsp`), and calls
`sym_parse(arg, base(), &addr)`. `cmd_break` stores `addr - base()`: the table keeps file
addresses, which stay right when the program is started again.

### Registers by name

`PTRACE_GETREGS` fills a `struct user_regs_struct` (`<sys/user.h>`), one 8-byte field per
register. A table of names and offsets replaces a 27-case `switch`:

`project/ch03/src/regs.c` (part)
```c
#define REG(name) { #name, offsetof(struct user_regs_struct, name) }

const struct reg_info reg_table[] = {
    REG(rax), REG(rbx), REG(rcx), REG(rdx), REG(rsi), REG(rdi), REG(rbp), REG(rsp),
    REG(r8),  REG(r9),  REG(r10), REG(r11), REG(r12), REG(r13), REG(r14), REG(r15),
    REG(rip), REG(eflags), REG(cs), REG(ss), REG(ds), REG(es), REG(fs), REG(gs),
    REG(fs_base), REG(gs_base), REG(orig_rax),
};
```

`#name` turns the macro argument into a string, so each name is written once and the string
cannot drift from the field. `reg_get` and `reg_set` `memcpy` 8 bytes at `offset`. The order
is gdb's `info registers` order, plus `fs_base`, `gs_base` and `orig_rax` (the system call
number, which matters in [Chapter 4](04-watchpoints-and-catchpoints.md)). `reg_flags` names
the set bits of `eflags`, low bit first as gdb does: `0x297` is `[ CF PF AF SF IF ]`.

### Memory at any length and alignment

`PTRACE_PEEKDATA` and `PTRACE_POKEDATA` move one 8-byte word: Chapter 2's
`proc_peek`/`proc_poke`. `proc_read` reads every aligned word that overlaps the range and
copies out the bytes inside it:

`project/ch03/src/proc.c` (part)
```c
int proc_read(struct proc *p, uint64_t addr, void *buf, size_t len)
{
    unsigned char *out = buf;
    uint64_t start = addr & ~(uint64_t)7;     /* the first aligned word */

    for (uint64_t w = start; w < addr + len; w += 8) {
        uint64_t word;
        if (proc_peek(p, w, &word) < 0)
            return -1;
        /* Copy the bytes of this word that lie inside [addr, addr + len). */
        for (int i = 0; i < 8; i++) {
            uint64_t a = w + i;
            if (a >= addr && a < addr + len)
                out[a - addr] = (unsigned char)(word >> (8 * i));
        }
    }
    return 0;
}
```

On a little-endian machine, `word >> (8 * i)` is the byte at `w + i`. `proc_write` runs the same
loop, but for each word it peeks, clears and replaces the bytes inside the range
(`word &= ~((uint64_t)0xff << (8 * i)); word |= (uint64_t)in[a - addr] << (8 * i);`), and pokes
the word back: "ptrace has no way to write less than a word". This read-modify-write is safe
because the program is stopped.

`print SYMBOL` reads `size` bytes (1, 2, 4 or 8) and prints a signed integer and hex, because
the table has no types. `set SYMBOL VALUE` writes "the low s->size bytes of v" (little-endian)
with `proc_write`. Other sizes get a hint to use `x`.

### Sessions

```console
$ cd project/ch03 && make
$ printf 'break square\nrun\nregs\nx square 16\ninfo symbol $rip\nreg rdi 10\ndelete\ncontinue\n' | build/tdb build/targets/loop
Reading symbols from build/targets/loop: 19 symbols.
(tdb) break square
Breakpoint 1 at 0x1149
(tdb) run
Started process 472219: build/targets/loop
Breakpoint 1, 0x0000555555555149 in square ()
(tdb) regs
...
rdi            0x0                 0
...
rip            0x555555555149      0x555555555149 <square>
eflags         0x297               [ CF PF AF SF IF ]
...
(tdb) x square 16
0x0000555555555149  cc 0f 1e fa 55 48 89 e5  89 7d fc 8b 45 fc 0f af  |....UH...}..E...|  <square>
(tdb) info symbol $rip
square
(tdb) reg rdi 10
rdi = 0xa (10)
(tdb) delete
(tdb) continue
i=0 total=100
i=1 total=101
i=2 total=105
i=3 total=114
i=4 total=130
[process exited with status 0]
(tdb) 
```

(`regs` printed all 27 registers in gdb's columns, ending with `orig_rax` = -1; the listing
is trimmed.) `rdi` is 0 at the first call, `square(0)`. Setting it to 10 before the function
reads it changes the argument, so the first line says `total=100`. Once the function has
copied `rdi` to its stack, changing the register does nothing. `x square 16` starts with `cc`:
tdb's breakpoint is in memory.

```console
$ printf 'break bump\nrun\nprint counter\nprint small\nprint greeting\nx greeting 16\nset counter 99\ncontinue\n' | build/tdb build/targets/data
Reading symbols from build/targets/data: 24 symbols.
(tdb) break bump
Breakpoint 1 at 0x1149
(tdb) run
Started process 472223: build/targets/data
Breakpoint 1, 0x0000555555555149 in bump ()
(tdb) print counter
counter = 41 (0x29), 4 bytes at 0x555555558020
(tdb) print small
small = -2 (0xfffe), 2 bytes at 0x55555555801c
(tdb) print greeting
greeting is an object of 11 bytes: use x greeting 11.
(tdb) x greeting 16
0x0000555555558010  68 65 6c 6c 6f 2c 20 74  64 62 00 00 fe ff 00 00  |hello, tdb......|  <greeting>
(tdb) set counter 99
counter = 99 (0x63), 4 bytes at 0x555555558020
(tdb) continue
hello, tdb: counter=100 r=100 table[2]=30
[process exited with status 0]
(tdb) 
```

`x greeting 16` runs past the 11-byte array, into a padding zero and then `fe ff`, which is
`small` at `...801c`. Neighbours in `.data` touch, and the first "Common mistakes" entry for
tdb depends on that. (`print main` says `main is a function of 84 bytes: use x main 84.`)

The `crash` target now stops with `0x000055555555519f in main+54 ()`, and `x $rip 16` starts
`c7 00 2a 00 00 00`, which is `movl $0x2a,(%rax)`: store 42 at the address in `rax`, which
`regs` shows is 0. No debug info was needed. On a stripped copy, tdb says
`Reading symbols from build/data-stripped: 0 symbols.` (`.dynsym` has no addresses in the
program), `break main` says `No symbol "main" in the program.`, and `break 0x116d` still
stops: `Breakpoint 1, 0x000055555555516d in ?? ()`.

### gdb hides its breakpoints; tdb does not (yet)

```console
$ gdb -q -batch -ex 'break *square' -ex run -ex 'x/4xb square' -ex 'set breakpoint always-inserted on' -ex 'x/4xb square' build/targets/loop
...
0x555555555149 <square>:	0xf3	0x0f	0x1e	0xfa
0x555555555149 <square>:	0xf3	0x0f	0x1e	0xfa
```

tdb's `x square 16` above showed its own `0xcc`. gdb shows the original `f3 0f 1e fa`
(`endbr64`), even with `set breakpoint always-inserted on`, which keeps the `0xCC` in memory
while stopped. gdb keeps the byte each breakpoint replaced (its *shadow contents*) and patches
it into the memory it shows you. Exercise 2 does the same for tdb.

**Tests.** v0.3 adds unit tests: a small framework in `tests/test.h` (`CHECK`, `CHECK_INT`,
`CHECK_STR` print `file:line` and carry on) and tests for symbols (on `build/targets/data`,
found through `-DTARGETS_DIR`), registers and the dump format. `tests/itest.sh` gains checks
for names, registers, `x`, `print`/`set` and the crash. `make test` ends with
`unit: 11 passed, 0 failed` and `itest: 21 passed, 0 failed`.

## Common mistakes

Four are covered above, each with the gdb command that shows it: **`strncpy` into a field
exactly as long as the string**, **`sizeof` on an array parameter**, **trusting a local before
its line has run** and **`*p@n` on a list**.

**(tdb) Writing a whole word to set a small variable.** The first `set` used
`proc_poke(&proc, addr, (uint64_t)v)`. For the 2-byte `small`, that writes 6 bytes too many:

```console
(tdb) set small -300
small = -300 (0xfed4), 2 bytes at 0x55555555801c
(tdb) print counter
counter = -1 (0xffffffff), 4 bytes at 0x555555558020
(tdb) x greeting 24
0x0000555555558010  68 65 6c 6c 6f 2c 20 74  64 62 00 00 d4 fe ff ff  |hello, tdb......|  <greeting>
0x0000555555558020  ff ff ff ff 00 00 00 00                           |........|  <counter>
(tdb) continue
hello, tdb: counter=0 r=0 table[2]=30
```

-300 as 8 bytes is `d4 fe ff ff ff ff ff ff`. The padding and all of `counter` became `ff`,
and `bump(1)` turned -1 into 0. With `proc_write`, the same `set` leaves
`... d4 fe 00 00` / `29 00 00 00 ...`, and `counter` stays 41.

**(tdb) Forgetting the load base.** While the program runs, `0x1149` is a run-time address,
and nothing is mapped there: `x 0x1149 4` says `Cannot access memory at address 0x1149`, while
`x bump 4` reads `0x555555555149`. In tdb's own code the same mistake shows up as `?? ()` in
every stop report. Subtract `base()` before a symbol lookup.

**(tdb) The flags in the wrong order.** The first `reg_flags` walked from the high bit down.
It looked fine on its own, but it did not match gdb, and the unit test caught it:

```console
$ make test
tests/test_regs.c:34: reg_flags(0x246, buf, sizeof buf) == "[ PF ZF IF ]": "[ IF ZF PF ]" != "[ PF ZF IF ]"
...
FAIL reg_flags
unit: 10 passed, 1 failed
make: *** [Makefile:47: test] Error 1
```

## In the wild: ELF

ELF, the Executable and Linkable Format, is the file format of every program in this course.
According to MaskRay's history of the format, it was created at Unix System Laboratories (USL)
for System V Release 4 in the late 1980s, and its dynamic-linking design came from SunOS 4.x.
The specification is the System V **generic ABI** (gABI), which covers headers, sections and
symbols. Each processor adds a supplement. For x86-64 that is the psABI, which also defines
the calling convention in this chapter. The Linux Foundation's refspecs site collects these
documents, and SCO hosts the gABI.

The split you saw with `readelf` (`.symtab` for names, `.debug_*` for the rest) is part of the
same design. The DWARF FAQ says DWARF "was developed along with ELF". ELF provides named
sections, and DWARF fills some of them with the types, scopes and line tables a source-level
debugger needs. [Chapter 6](06-down-to-the-metal.md) has DWARF's own story.

## Summary

- `whatis`, `ptype` and `ptype/o` (offsets, sizes, holes) show types; gdb's `sizeof` follows C,
  frame by frame.
- `print/F` and `x/NFU` share the format letters (`x d u o t c a`, plus `s i` for `x`); `x`
  units are `b h w g`. Little-endian: `x/xw` reads bytes backwards compared to `x/xb`.
- `print` knows an array's length; `x/s`, `printf "%s"` and the program read until a zero.
- The value history (`$`, `$$n`, `$N`), convenience variables, `while`/`if`/`end` and
  `$_`/`$__` let you work with results. `*p@n` suits arrays and buffers, not lists.
- `set var`, `print x = v` and `finish`'s `$N` test a fix in the running program.
- `up`, `down` and `info frame` show frames: the saved `rbp` at `rbp`, the return address at
  `rbp + 8`, a leaf function's locals in the red zone.
- Without `-g`: arguments in `rdi rsi rdx rcx r8 r9`, the result in `rax`, casts for types.
- `.symtab` (`Elf64_Sym`) holds names and sizes only, and `strip` removes it. Types and lines
  are DWARF.
- tdb v0.3 reads `.symtab`, keeps file addresses (adding the load base while running), names
  registers through an `offsetof` table, and reads and writes memory a word at a time.

## Exercises

1. **A register condition for tdb.** Add `condition N REG == VALUE` (and `condition N` to
   remove it). A false condition resumes like an ignored hit, without counting it. Test it with
   `break square` and `condition 1 rdi == 3` on `loop`: the first stop must be at `i = 3`.

   <details><summary>Solution</summary>

   Add `const struct reg_info *cond_reg; uint64_t cond_value;` to `struct breakpoint`
   (`breakpoint.h` includes `regs.h`). In `run.c`, after the `rip -= 1` rewind and before
   `bp->hits++`:

   ```c
           if (bp->cond_reg != NULL && reg_get(&regs, bp->cond_reg) != bp->cond_value)
               continue;
   ```

   `continue` goes round the loop again, which steps over the breakpoint and resumes, exactly as
   for an ignored hit. In `tdb.c`, `execute` reads two more words (`arg3`, `arg4`) with
   `strtok`, and `cmd_condition(n, reg, op, value)` checks the words (`parse_u64` for N and
   VALUE, `strcmp(op, "==")`, `bp_find`, `reg_find`, printing `Usage: condition N [REG ==
   VALUE]`, `No breakpoint number N.` or `Invalid register "X".`), then sets
   `bp->cond_reg` (`NULL` without REG) and `bp->cond_value`. Without REG it prints
   `Breakpoint N now unconditional.`

   ```console
   (tdb) break square
   Breakpoint 1 at 0x1149
   (tdb) condition 1 rdi == 3
   (tdb) run
   Started process 1379154: build/targets/loop
   Breakpoint 1, 0x0000555555555149 in square ()
   (tdb) reg rdi
   rdi = 0x3 (3)
   (tdb) condition 1
   Breakpoint 1 now unconditional.
   (tdb) continue
   Breakpoint 1, 0x0000555555555149 in square ()
   ```

   `info breakpoints` shows Hits 1 at the first stop, as in gdb, which counts a hit only when
   the condition is true. All 21 integration checks still pass.
   </details>

2. **Hide the 0xCC.** Make `x` show the program's own bytes where tdb has a breakpoint. Which
   integration check must change?

   <details><summary>Solution</summary>

   In `breakpoint.c` (declared in `breakpoint.h`):

   ```c
   void bp_shadow(const struct proc *p, uint64_t addr, unsigned char *buf, size_t len)
   {
       for (int i = 0; i < BP_MAX; i++) {
           uint64_t a = bps[i].addr + p->load_base;
           if (bps[i].id != 0 && bps[i].inserted && a >= addr && a - addr < len)
               buf[a - addr] = bps[i].saved;
       }
   }
   ```

   `cmd_x` calls `bp_shadow(&proc, addr, buf, count)` right after `proc_read`. Only `x` uses
   it: the breakpoint code must still see the real `0xCC`.

   ```console
   (tdb) x square 16
   0x0000555555555149  f3 0f 1e fa 55 48 89 e5  89 7d fc 8b 45 fc 0f af  |....UH...}..E...|  <square>
   ```

   The `x-shows-int3` check expected `'  cc 0f 1e fa '`. Make it `'  f3 0f 1e fa '`, and all
   21 checks pass.
   </details>

3. **Read memory faster.** `x _start 1024` costs 128 `PTRACE_PEEKDATA` calls. Read the range
   with one `process_vm_readv(2)` call instead, and keep the word loop as a fallback. Time 2000
   `x _start 1024` commands before and after.

   <details><summary>Solution</summary>

   Rename the old loop `proc_read_words` (and declare it in `proc.h`), include `<sys/uio.h>`,
   and try the system call first:

   ```c
   int proc_read(struct proc *p, uint64_t addr, void *buf, size_t len)
   {
       struct iovec local = { buf, len };
       struct iovec remote = { (void *)(uintptr_t)addr, len };
       if (process_vm_readv(p->pid, &local, 1, &remote, 1, 0) == (ssize_t)len)
           return 0;
       /* Partly unreadable, or process_vm_readv refused: the slow way. */
       return proc_read_words(p, addr, buf, len);
   }
   ```

   `process_vm_readv` copies between the two address spaces directly. It can return a partial
   count when the range runs into an unmapped page, so anything short of `len` falls back to
   the word loop, which reports the error as before (`x 0 8` still says "Cannot access memory
   at address 0x0"). Its write twin respects page protection, so it cannot put breakpoints
   into `.text`: writes stay with `POKEDATA`.

   A script of `break bump`, `run` and 2000 × `x _start 1024` (output to `/dev/null`) took
   0.22–0.23 s with the word loop and 0.13–0.14 s with `process_vm_readv` on my machine. Most
   of the rest is formatting 128 000 dump lines, and the numbers vary. All 21 checks pass.
   </details>

4. **Every name without a terminator, in gdb.** At roster.c line 56, after the list is built,
   write a `while $p` loop that prints only the names with no zero inside `name[8]`. Use gdb's
   convenience function `$_strlen`. Why do `$_strlen(head->name)` and
   `$_strlen((char *)head->name)` differ?

   <details><summary>Solution</summary>

   ```console
   (gdb) break 56
   Breakpoint 1 at 0x13a4: file roster.c, line 56.
   (gdb) run < class.txt
   ...
   Breakpoint 1, main () at roster.c:56
   56	    int rank = 1;
   (gdb) print $_strlen(head->name)
   $1 = 8
   (gdb) print $_strlen((char *)head->name)
   $2 = 9
   (gdb) set $p = head
   (gdb) while $p
    >if $_strlen((char *)$p->name) >= sizeof($p->name)
     >printf "%s: no NUL in name[%d]\n", $p->name, sizeof($p->name)
     >end
    >set $p = $p->next
    >end
   MargaretZ: no NUL in name[8]
   ```

   `head->name` is a `char[8]` *value*: gdb holds a copy of 8 bytes, and the string ends with
   the array, so the answer is 8. Cast to `char *`, it is a pointer, and `$_strlen` reads the
   program's memory until a zero, like C's `strlen`: 9. A name with no zero inside the array
   gives 8 or more. Prefer `$_strlen` to the program's own `strlen`: convenience functions run
   inside gdb and never run code in the program.
   </details>

Sources for this chapter: [REFERENCES.md](REFERENCES.md#chapter-3--looking-at-data)
