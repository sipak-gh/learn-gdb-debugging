# Chapter 8 — Automating GDB

So far you typed every gdb command by hand, and usually only once. Real bugs are less polite.
Some need the same twenty commands on every run. Some live in data structures that gdb prints
badly: a hash table comes out as one pointer. Some show up only on the 900th call. This chapter
teaches gdb to do the repetitive work for you. On the gdb side you will write Python pretty
printers, Python commands and convenience functions, and conditions that call them. You will
also use `define` (with `$arg0`, `while` and `document`), `dprintf`, breakpoint `commands`
with `silent` and `continue`, and Python breakpoints whose `stop()` returns False. Then come
batch mode (`-batch -x -ex`, exit status, `-return-child-result`), `.gdbinit` and the auto-load
safe-path, and the command line's shortcuts.

On the tdb side, v0.8 gets a real command language. It has a command table with gdb's
abbreviations and messages, an empty line that repeats, `run ARGS`, breakpoint `commands`,
`define` with `$arg0` and `$argc`, `source`, `echo`, the `-x`/`-ex`/`-batch`/`-nx` options,
`~/.tdbinit`, and GNU readline at the prompt. tdb gets no Python. gdb's Python is a whole
interpreter, libpython, linked into gdb, which is far beyond a small debugger. tdb's scripting
is command files. That is all gdb had for its first two decades, until Python arrived in GDB
7.0 in 2009.

Previous: [Chapter 7](07-threads-and-processes.md). Next: [Chapter 9](09-time-travel.md).

## A hash table that loses things

`inventory` loads a stock list into a hash table, then looks every item up again. Each bucket
holds a chain of items. The table starts with 4 buckets and doubles when it holds twice as many
items as buckets.

`examples/ch08/inventory.c`
```c
/* inventory.c - load a stock list into a hash table, then look every item
 * up again. With a short list all is well; with a longer one some items
 * cannot be found.
 *
 *     ./inventory stock.txt
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item {
    char *name;
    int qty;
    struct item *next;          /* the next item in the same bucket */
};

struct table {
    struct item **buckets;
    size_t nbuckets;
    size_t count;
};

/* FNV-1a: a simple, good string hash. */
unsigned long hash(const char *s)
{
    unsigned long h = 14695981039346656037UL;

    for (; *s != '\0'; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211UL;
    }
    return h;
}

/* Twice as many buckets: every item moves to its bucket in the new array. */
void grow(struct table *t)
{
    size_t n = t->nbuckets * 2;
    struct item **b = calloc(n, sizeof *b);

    for (size_t i = 0; i < t->nbuckets; i++) {
        struct item *it = t->buckets[i];
        while (it != NULL) {
            struct item *next = it->next;
            size_t h = hash(it->name) % t->nbuckets;
            it->next = b[h];
            b[h] = it;
            it = next;
        }
    }
    free(t->buckets);
    t->buckets = b;
    t->nbuckets = n;
}

void table_put(struct table *t, const char *name, int qty)
{
    if (t->count >= 2 * t->nbuckets)
        grow(t);
    size_t h = hash(name) % t->nbuckets;
    struct item *it = malloc(sizeof *it);
    it->name = strdup(name);
    it->qty = qty;
    it->next = t->buckets[h];
    t->buckets[h] = it;
    t->count++;
}

struct item *table_get(const struct table *t, const char *name)
{
    size_t h = hash(name) % t->nbuckets;

    for (struct item *it = t->buckets[h]; it != NULL; it = it->next)
        if (strcmp(it->name, name) == 0)
            return it;
    return NULL;
}

int main(int argc, char *argv[])
{
    struct table stock = { calloc(4, sizeof(struct item *)), 4, 0 };
    char name[64];
    int qty, missing = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: inventory FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    while (fscanf(f, "%63s %d", name, &qty) == 2)
        table_put(&stock, name, qty);
    rewind(f);
    while (fscanf(f, "%63s %d", name, &qty) == 2) {
        if (table_get(&stock, name) == NULL) {
            printf("not found: %s\n", name);
            missing++;
        }
    }
    fclose(f);
    printf("%zu items, %d not found\n", stock.count, missing);
    return missing ? 1 : 0;
}
```

`small.txt` has 5 lines such as `bolt 120`, and `stock.txt` has 20. The `Makefile` builds with
`-std=c11 -Wall -Wextra -g -O0`. Paths in the output are shortened to `.../examples/ch08/...`.

```console
$ make
$ ./inventory small.txt
5 items, 0 not found
$ ./inventory stock.txt
not found: nut
not found: washer
...
not found: dowel
20 items, 9 not found
```

Five items fit in 4 buckets without growing, and all are found. Twenty items make the table
grow twice, and nearly half of them are lost, although the table says it holds 20. Run it under
gdb, then stop just before the end and look at the table:

```console
$ gdb -q ./inventory
Reading symbols from ./inventory...
(gdb) run stock.txt
Starting program: .../examples/ch08/inventory stock.txt
...
20 items, 9 not found
[Inferior 1 (process 967843) exited with code 01]
(gdb) break 104
Breakpoint 1 at 0x55555555570b: file inventory.c, line 104.
(gdb) run
Starting program: .../examples/ch08/inventory stock.txt
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".
not found: nut
...
not found: dowel

Breakpoint 1, main (argc=2, argv=0x7fffffffe508) at inventory.c:104
104	    fclose(f);
(gdb) print stock
$1 = {buckets = 0x55555555a910, nbuckets = 16, count = 20}
```

The table is a pointer to 16 pointers, each the head of a chain, and gdb prints the struct
exactly as declared. You could print `*stock.buckets@16` and follow each `->next` by hand: 16
heads and 20 items, typed again on every run. That is the kind of work to hand to gdb.

## Teaching gdb to print a table

gdb contains a Python interpreter. A Python file can register *pretty printers*: classes that
gdb calls to turn a value of a given type into text. `inventory.py` has two of them, plus a
command and a function that you will meet in the next two sections.

`examples/ch08/inventory.py`
```python
# inventory.py - gdb helpers for inventory.c. Load them in gdb with
#
#     (gdb) source inventory.py
#
# print shows a struct table as its items, check-table looks for items in
# the wrong bucket, and $misplaced(TABLE) counts them (for conditions).
import gdb
import gdb.printing


def fnv1a(name):
    """The same hash as hash() in inventory.c, on a Python bytes object."""
    h = 14695981039346656037
    for byte in name:
        h ^= byte
        h = (h * 1099511628211) % 2**64
    return h


def items(table):
    """Yield (bucket number, struct item value) for each item in a table."""
    buckets = table["buckets"]
    for i in range(int(table["nbuckets"])):
        it = buckets[i]
        while it != 0:
            yield i, it.dereference()
            it = it["next"]


def misplaced(table):
    """The items of a table that are not in the bucket their hash says."""
    n = int(table["nbuckets"])
    wrong = []
    for i, item in items(table):
        name = item["name"].string()
        want = fnv1a(name.encode()) % n
        if want != i:
            wrong.append((name, i, want))
    return wrong


class ItemPrinter:
    """struct item: its name and quantity (not the next pointer)."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "item \"%s\", qty %d" % (self.val["name"].string(), int(self.val["qty"]))


class TablePrinter:
    """struct table: a map from item names to quantities."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        return "table of %d items in %d buckets" % (int(self.val["count"]),
                                                    int(self.val["nbuckets"]))

    def children(self):
        for _, item in items(self.val):
            yield "name", item["name"].string()
            yield "qty", item["qty"]

    def display_hint(self):
        return "map"


def build_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("inventory")
    pp.add_printer("item", "^item$", ItemPrinter)
    pp.add_printer("table", "^table$", TablePrinter)
    return pp


class CheckTable(gdb.Command):
    """check-table EXPR: list the items of the struct table EXPR that are
not in the bucket their hash says (table_get cannot find those)."""

    def __init__(self):
        super().__init__("check-table", gdb.COMMAND_DATA, gdb.COMPLETE_EXPRESSION)

    def invoke(self, arg, from_tty):
        if not arg:
            raise gdb.GdbError("Usage: check-table EXPR")
        table = gdb.parse_and_eval(arg)
        if table.type.code == gdb.TYPE_CODE_PTR:
            table = table.dereference()
        wrong = misplaced(table)
        for name, i, want in wrong:
            print("%-8s in bucket %2d, belongs in bucket %2d" % (name, i, want))
        count = sum(1 for _ in items(table))
        print("%d items, %d in the wrong bucket" % (count, len(wrong)))


class Misplaced(gdb.Function):
    """$misplaced(TABLE): how many items of TABLE are in the wrong bucket."""

    def __init__(self):
        super().__init__("misplaced")

    def invoke(self, table):
        if table.type.code == gdb.TYPE_CODE_PTR:
            table = table.dereference()
        return len(misplaced(table))


gdb.printing.register_pretty_printer(gdb.current_objfile(), build_printer(), replace=True)
CheckTable()
Misplaced()
```

- `import gdb` only works inside gdb. A `gdb.Value` is a value from the program.
  `val["name"]` is a struct field, `int(...)` turns it into a Python number, `.string()` reads
  a C string, `.dereference()` follows a pointer, and `it != 0` compares a pointer with NULL.
- A printer has `to_string()`. `TablePrinter` also has `children()`, which yields (label,
  value) pairs; `display_hint()` returning `"map"` makes gdb show them as `[key] = value`.
  `RegexpCollectionPrettyPrinter` picks a printer by type name: `^table$` is `struct table`.
- `register_pretty_printer(gdb.current_objfile(), ...)` attaches the printers to the program
  file whose script is being loaded. When you `source` the file by hand, `current_objfile()`
  is `None` and the printers become global. You will see the other case under
  [.gdbinit and the safe-path](#gdbinit-and-the-safe-path).

Load the file at the same stop and print again:

```console
(gdb) source inventory.py
(gdb) print stock
$2 = table of 20 items in 16 buckets = {[nut] = 340, [staple] = 1200, [dowel] = 95, 
  [washer] = 500, [hook] = 85, [bracket] = 60, [anchor] = 150, [clamp] = 25, [spring] = 70, 
  [pin] = 600, [tack] = 800, [hinge] = 40, [nail] = 900, [cable] = 18, [chain] = 12, [bolt] = 120, 
  [rivet] = 410, [screw] = 275, [peg] = 130, [latch] = 30}
(gdb) print/r stock
$3 = {buckets = 0x55555555a910, nbuckets = 16, count = 20}
(gdb) print *stock.buckets[1]
$4 = item "washer", qty 500
(gdb) print stock.buckets[1]
$5 = (struct item *) 0x55555555a540
(gdb) info pretty-printer
global pretty-printers:
  builtin
    mpx_bound128
  inventory
    item
    table
```

- All 20 items are in the table, `nut` and `washer` too, so nothing was lost on the way in.
- `print/r` (raw) skips the printers. Use it when you suspect the printer itself.
- A printer applies to a value of the type, not to a pointer to it.
- `info pretty-printer` lists what is loaded; `mpx_bound128` comes with gdb.

## A command of your own, in Python

`CheckTable` subclasses `gdb.Command`. The constructor gives the name, a help class and how
to complete the argument. gdb calls `invoke(arg, from_tty)` with the rest of the line.
`gdb.parse_and_eval(arg)` evaluates it as a C expression in the current frame, and `raise
gdb.GdbError(...)` prints an error without a Python traceback. The command recomputes FNV-1a
(mod 2^64, as C's `unsigned long` wraps) and lists items not in the bucket their hash names:

```console
(gdb) check-table stock
nut      in bucket  0, belongs in bucket  8
dowel    in bucket  0, belongs in bucket  8
washer   in bucket  1, belongs in bucket  9
hook     in bucket  2, belongs in bucket 10
clamp    in bucket  4, belongs in bucket 12
spring   in bucket  4, belongs in bucket 12
tack     in bucket  4, belongs in bucket 12
hinge    in bucket  4, belongs in bucket 12
screw    in bucket  7, belongs in bucket 15
20 items, 9 in the wrong bucket
```

- These are exactly the 9 items `inventory` could not find. `table_get` looks only in the
  bucket the hash names, so an item anywhere else is invisible to it.
- Every wrong bucket is the right one minus 8: `h % 16` and `h % 8` differ by 8 when bit 3 of
  `h` is set. These items were placed with `% 8` in a 16-bucket table. Only `grow` moves items.

## Stopping when the table goes wrong

`Misplaced` subclasses `gdb.Function`. That makes `$misplaced(TABLE)` a *convenience function*,
usable in any gdb expression, including a breakpoint condition. Line 67, `t->count++`, is the
end of `table_put`. Stop there only once the table holds a misplaced item:

```console
$ gdb -q ./inventory
Reading symbols from ./inventory...
(gdb) source inventory.py
(gdb) break 67 if $misplaced(t) > 0
Breakpoint 1 at 0x14f0: file inventory.c, line 67.
(gdb) run stock.txt
Starting program: .../examples/ch08/inventory stock.txt
...

Breakpoint 1, table_put (t=0x7fffffffe380, name=0x7fffffffe3a0 "anchor", qty=150) at inventory.c:67
67	    t->count++;
(gdb) print *t
$1 = table of 8 items in 8 buckets = {[nut] = 340, [hinge] = 40, [washer] = 500, [nail] = 900, 
  [bolt] = 120, [hook] = 85, [screw] = 275, [bracket] = 60, [anchor] = 150}
(gdb) check-table t
hinge    in bucket  0, belongs in bucket  4
nail     in bucket  1, belongs in bucket  5
bolt     in bucket  2, belongs in bucket  6
screw    in bucket  3, belongs in bucket  7
9 items, 4 in the wrong bucket
(gdb) info breakpoints
Num     Type           Disp Enb Address            What
1       breakpoint     keep y   0x00005555555554f0 in table_put at inventory.c:67
	stop only if $misplaced(t) > 0
	breakpoint already hit 1 time
```

- `anchor` is the 9th item. The 8 before it filled 4 buckets to `count == 2 * nbuckets`, so
  this `table_put` called `grow` first. The table went wrong on the very first grow.
- "8 items" while 9 are listed is not a second bug. The stop is *before* line 67 runs.
- Every wrong bucket is the right one minus the old size, 4 this time: hinge 0 for 4, nail 1
  for 5, bolt 2 for 6, screw 3 for 7.
- gdb evaluates the condition, here Python, at every hit, and resumes if it is false. Fine for
  20 calls; on a breakpoint hit a million times, a heavy condition makes the run crawl.

Now look inside `grow` at the first item it moves, six `next`s in:

```console
$ gdb -q ./inventory
Reading symbols from ./inventory...
(gdb) break grow
Breakpoint 1 at 0x132f: file inventory.c, line 39.
(gdb) run stock.txt
Starting program: .../examples/ch08/inventory stock.txt
...

Breakpoint 1, grow (t=0x7fffffffe380) at inventory.c:39
39	    size_t n = t->nbuckets * 2;
(gdb) next
...
(gdb) next
46	            size_t h = hash(it->name) % t->nbuckets;
(gdb) print it->name
$1 = 0x55555555a620 "hinge"
(gdb) print h
$2 = 140737488347984
(gdb) print n
$3 = 8
(gdb) print hash(it->name) % n
$4 = 4
(gdb) print t->nbuckets
$5 = 4
(gdb) next
47	            it->next = b[h];
(gdb) print h
$6 = 0
```

- `print h` before line 46 runs shows whatever was in that stack slot:
  140737488347984 is 0x7fffffffe350, a leftover stack address. After `next`, `h` is 0.
- `print hash(it->name) % n` makes gdb call the program's own `hash` function, in the stopped
  process, and returns 4: hinge's right bucket in the new array.
- `t->nbuckets` is still 4, because line 54 updates it only after the loop. **Line 46 hashes
  with the old size.** It should be `% n`. Exercise 1 fixes it.

## Commands in gdb's own language: define

Python is not the only way to add commands. `define` builds one out of ordinary gdb commands.
In a new session, stopped at line 104 again (`break 104`, `run stock.txt`):

```console
(gdb) define pbucket
Type commands for definition of "pbucket".
End with a line saying just "end".
>set $it = stock.buckets[$arg0]
>while $it
 >printf "%-8s %4d\n", $it->name, $it->qty
 >set $it = $it->next
 >end
>end
(gdb) pbucket 4
anchor    150
clamp      25
spring     70
pin       600
tack      800
hinge      40
(gdb) pbucket 12
(gdb) document pbucket
Type documentation for "pbucket".
End with a line saying just "end".
>pbucket N: list the items in bucket N of stock (in main).
>end
(gdb) help pbucket
pbucket N: list the items in bucket N of stock (in main).
(gdb) help user-defined
User-defined commands.
...
pbucket -- pbucket N: list the items in bucket N of stock (in main).
...
(gdb) show user pbucket
User command "pbucket":
  set $it = stock.buckets[$arg0]
  while $it
    printf "%-8s %4d\n", $it->name, $it->qty
    set $it = $it->next
  end

(gdb) pbucket
Missing argument 0 in user function.
```

- `$arg0` to `$arg9` are the arguments, pasted into the body as text; `$argc` counts them.
- `$it` is a *convenience variable*: gdb's, not the program's, typed by what you assign.
- gdb's language has `while` and `if`; the `>` prompt indents once per nested block.
- Bucket 4 holds six items and bucket 12, where four of them belong, holds none.

`define` is quick for a few lines you would type anyway. Use Python as soon as there is real
logic: the FNV-1a hash in `check-table` would be painful in gdb's language.

If you define a command named `hook-stop`, gdb runs it at every stop. This file, run with
`gdb -q -batch -x hook.gdb ./inventory`, shows the next instruction each time:

```
define hook-stop
x/i $pc
end
break table_put
run stock.txt
next
```

```console
Breakpoint 1 at 0x1448: file inventory.c, line 59.
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".
=> 0x555555555448 <table_put+23>:	mov    -0x18(%rbp),%rax

Breakpoint 1, table_put (t=0x7fffffffe430, name=0x7fffffffe450 "bolt", qty=120) at inventory.c:59
59	    if (t->count >= 2 * t->nbuckets)
=> 0x55555555546c <table_put+59>:	mov    -0x20(%rbp),%rax
61	    size_t h = hash(name) % t->nbuckets;
```

The hook runs before gdb reports the stop. Exercise 4 adds `hook-stop` to tdb.

## Watching a state machine without stopping

`csvsum` adds up the third column of a CSV file. A field may be quoted, so that it can hold
commas, and a quote inside a quoted field is written twice. `split` is a state machine with
four states, and it takes the quotes out in place.

`examples/ch08/csvsum.c`
```c
/* csvsum.c - add up the amount column (the third) of a CSV file. A field
 * may be quoted, to hold commas ("Smith, John") or quotes, written twice
 * ("say ""hi""").
 *
 *     ./csvsum orders.csv
 */
#include <stdio.h>
#include <stdlib.h>

enum state { START, PLAIN, QUOTED, QUOTE };

/* Split line into fields, in place: fields[i] points into line. Returns
 * the number of fields (at most max). */
int split(char *line, char *fields[], int max)
{
    enum state st = START;
    char *out = line;           /* quotes are taken out as we go */
    int n = 0;

    for (char *p = line;; p++) {
        char c = *p;
        if (c == '\n')
            c = '\0';
        switch (st) {
        case START:
            if (n == max)
                return n;
            fields[n++] = out;
            if (c == '"') {
                st = QUOTED;
                break;
            }
            st = PLAIN;     /* and c is the field's first character: */
            /* fall through */
        case PLAIN:
            if (c == ',' || c == '\0') {
                *out++ = '\0';
                st = START;
                if (c == '\0')
                    return n;
            } else {
                *out++ = c;
            }
            break;
        case QUOTED:
            if (c == '"')
                st = QUOTE;     /* the end of the field, or a doubled quote */
            else if (c == '\0') {
                *out = '\0';    /* no closing quote: take what we have */
                return n;
            } else
                *out++ = c;
            break;
        case QUOTE:
            if (c == '"') {
                *out++ = '"';
                st = PLAIN;
            } else {
                st = PLAIN;     /* after the closing quote: a ',' or the end */
                p--;
            }
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    char line[256];
    char *fields[8];
    long total = 0;
    int orders = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: csvsum FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    fgets(line, sizeof line, f);        /* the header */
    while (fgets(line, sizeof line, f) != NULL) {
        int n = split(line, fields, 8);
        if (n >= 3) {
            total += atol(fields[2]);
            orders++;
        }
    }
    fclose(f);
    printf("%d orders, total %ld\n", orders, total);
    return 0;
}
```

`examples/ch08/orders.csv`
```
customer,item,amount
Alice,widget,120
"Smith, John",gadget,75
"Acme ""Big"" Tools, Inc.",widget,300
Bob,"gizmo, large",42
```

```console
$ ./csvsum orders.csv
4 orders, total 237
```

120 + 75 + 300 + 42 is 537. The missing 300 is the Acme line, the only one with a doubled quote.
Stepping through `split` one character at a time would take 40 `next`s per state change. A
`dprintf` prints instead of stopping:

```console
$ gdb -q ./csvsum
Reading symbols from ./csvsum...
(gdb) run orders.csv
...
4 orders, total 237
...
(gdb) break split if line[1] == 'A'
Breakpoint 1 at 0x55555555523c: file csvsum.c, line 16.
(gdb) run
...

Breakpoint 1, split (line=0x7fffffffe2e0 "\"Acme \"\"Big\"\" Tools, Inc.\",widget,300\n", fields=0x7fffffffe2a0, max=8) at csvsum.c:16
16	    enum state st = START;
(gdb) dprintf 24,"%c %d\n", *p, st
Dprintf 2 at 0x55555555526e: file csvsum.c, line 24.
(gdb) info breakpoints
Num     Type           Disp Enb Address            What
1       breakpoint     keep y   0x000055555555523c in split at csvsum.c:16
	stop only if line[1] == 'A'
	breakpoint already hit 1 time
2       dprintf        keep y   0x000055555555526e in split at csvsum.c:24
        printf "%c %d\n", *p, st
(gdb) finish
Run till exit from #0  split (line=0x7fffffffe2e0 "\"Acme \"\"Big\"\" Tools, Inc.\",widget,300\n", 
    fields=0x7fffffffe2a0, max=8) at csvsum.c:16
" 0
A 2
...
" 2
" 3
B 1
i 1
...
, 1
  0
I 1
...
0x00005555555554a5 in main (argc=2, argv=0x7fffffffe508) at csvsum.c:85
85	        int n = split(line, fields, 8);
Value returned is $2 = 4
(gdb) print fields[0]
$3 = 0x7fffffffe2e0 "Acme \"Big\"\" Tools"
(gdb) print fields[1]
$4 = 0x7fffffffe2f2 " Inc.\""
(gdb) print fields[2]
$5 = 0x7fffffffe2f9 "widget"
(gdb) print (enum state) 3
$6 = QUOTE
```

- `line[1] == 'A'` picks the Acme line, whose first character is a quote.
- `dprintf LOCATION,"FORMAT",ARGS` prints and goes on. At line 24, `switch (st)`, each output
  line is the next character and the state that handles it: 0 START, 1 PLAIN, 2 QUOTED,
  3 QUOTE, as `print (enum state) 3` confirms.
- `split` returned 4 fields, not 3. The amount field is `"widget"`, and `atol` makes it 0.

Read the trace around `Big`. The `"` after `Acme ` is handled in state 2 (QUOTED) and moves to
QUOTE. The second `"` is handled in state 3 (QUOTE): a doubled quote, so `split` writes one `"`.
Then `B` is handled in state 1, PLAIN. It should be back in QUOTED, inside the field. From
there on the closing quotes are copied as text and the comma inside the quotes ends the field.
**Line 57 should be `st = QUOTED;`.** Exercise 2 fixes it.

## Breakpoints that run commands

A dprintf is one case of something more general. `commands N` attaches a list of commands to
breakpoint N, run each time it stops the program. `silent` as the first command suppresses
the "Breakpoint N, ..." report, and `continue` as the last resumes the program. A dprintf is a
breakpoint with `printf` and `continue` commands.

You might want the trace on line 24 only for the Acme line, with the condition on line 24
itself. It does not work for long:

```console
$ gdb -q -batch -ex 'dprintf 24,"%c  %d   line = %s\n", *p, st, line' \
      -ex "condition 1 line[1] == 'A'" -ex run --args ./csvsum orders.csv
Dprintf 1 at 0x126e: file csvsum.c, line 24.
...
"  0   line = "Acme ""Big"" Tools, Inc.",widget,300

A  2   line = "Acme ""Big"" Tools, Inc.",widget,300

c  2   line = AAcme ""Big"" Tools, Inc.",widget,300

4 orders, total 237
```

`split` writes the unquoted field over `line`. Once `A` and `c` are copied to `line[0]` and
`line[1]`, the condition is false. `trace.gdb` decides once, on entry to `split`, and keeps the
answer in a convenience variable:

`examples/ch08/trace.gdb`
```
# trace.gdb - trace split()'s state machine on the line that starts "Acme.
#
#     gdb -q -batch -x trace.gdb --args ./csvsum orders.csv
#
# split() overwrites the line as it goes, so decide once, on entry.
break split
commands
  silent
  set $trace = line[1] == 'A'
  continue
end
break 24 if $trace
commands
  silent
  printf "%c  ", *p
  output st
  echo \n
  continue
end
run
```

```console
$ gdb -q -batch -x trace.gdb --args ./csvsum orders.csv
Breakpoint 1 at 0x123c: file csvsum.c, line 16.
Breakpoint 2 at 0x126e: file csvsum.c, line 24.
...
"  START
A  QUOTED
...
"  QUOTED
"  QUOTE
B  PLAIN
i  PLAIN
...
4 orders, total 237
[Inferior 1 (process 970921) exited normally]
```

- `output st` prints a value like `print`, without `$N = ` or a newline: the enum by name.
- `commands` with no number applies to the last breakpoint set.
- The list ends at `continue`: the program has moved on. A `printf` placed after the
  `continue` never printed.

## Python breakpoints

The trace above prints every character. You only care where the state changes. A
`gdb.Breakpoint` subclass can decide that in Python: gdb calls its `stop()` at each hit, and
the program stops only if it returns True.

`examples/ch08/trace.py`
```python
# trace.py - print each change of state in csvsum's split(), for every
# line, without stopping:
#
#     gdb -q -batch -x trace.py --args ./csvsum orders.csv
import gdb


class StateTrace(gdb.Breakpoint):
    """A breakpoint at the top of split()'s loop that never stops: stop()
    looks at the state and returns False, so the program goes on."""

    def __init__(self, location):
        super().__init__(location)
        self.silent = True
        self.last = None

    def stop(self):
        frame = gdb.selected_frame()
        st = str(frame.read_var("st"))
        p = frame.read_var("p")
        if self.last is None:
            print("line: %s" % p.string().rstrip("\n"))
        if st != self.last:
            # st is the state that will handle the character at p.
            print("  %-6s from %r" % (st, chr(p.dereference())))
        self.last = st
        return False


class LineStart(gdb.Breakpoint):
    """split() is called once per line: start a new trace."""

    def __init__(self, tracer):
        super().__init__("split")
        self.silent = True
        self.tracer = tracer

    def stop(self):
        self.tracer.last = None
        return False


LineStart(StateTrace("csvsum.c:24"))
gdb.execute("run")
```

```console
$ gdb -q -batch -x trace.py --args ./csvsum orders.csv
Breakpoint 1 at 0x126e: file csvsum.c, line 24.
Breakpoint 2 at 0x123c: file csvsum.c, line 16.
...
line: "Smith, John",gadget,75
  START  from '"'
  QUOTED from 'S'
  QUOTE  from ','
  PLAIN  from ','
  START  from 'g'
...
line: "Acme ""Big"" Tools, Inc.",widget,300
  START  from '"'
  QUOTED from 'A'
  QUOTE  from '"'
  PLAIN  from 'B'
  START  from ' '
  PLAIN  from 'I'
...
4 orders, total 237
[Inferior 1 (process 971006) exited normally]
```

- `gdb.selected_frame().read_var("st")` reads a local; `str()` of an enum value is its name.
- On the Smith line, `QUOTE from ','` then `PLAIN from ','` is correct: the closing quote's
  case does `p--`, so the comma is read again in PLAIN. On the Acme line, `PLAIN from 'B'` is
  the bug, in one line.
- Logic at a breakpoint without stopping is the most useful pattern in this chapter: `stop()`
  can count, collect, compare, and stop only when something is really wrong.

## Running gdb from scripts

`-batch` runs the `-x FILE` and `-ex COMMAND` items in order, then exits, killing the program
if it still runs. `--args` ends gdb's options. An error in an `-x` file stops that file, not
the items after it. Here `bad.gdb` is `break table_put`, `print nosuch`, `echo not reached\n`:

```console
$ gdb -q -batch -x bad.gdb -ex 'echo still here\n' ./inventory; echo "[exit $?]"
Breakpoint 1 at 0x1448: file inventory.c, line 59.
bad.gdb:2: Error in sourced command file:
No symbol "nosuch" in current context.
still here
[exit 0]
```

gdb's exit status is 1 if the *last* command failed, else 0. Without the `-ex`, the command
above exits with 1. The program's own exit status is ignored unless you ask for it:

```console
$ gdb -q -batch -ex run --args ./inventory stock.txt > /dev/null; echo "[exit $?]"
[exit 0]
$ gdb -q -batch -return-child-result -ex run --args ./inventory stock.txt > /dev/null; echo "[exit $?]"
[exit 1]
```

`set logging file trace.txt`, `set logging redirect on` and `set logging enabled on` send gdb's
output to a file. Around `source trace.gdb`, the terminal kept only the program's `4 orders,
total 237`, and `trace.txt` got gdb's 44 lines. gdb 12 renamed `set logging on`; the old name
still works but warns `Warning: 'set logging on', an alias for the command 'set logging
enabled', is deprecated.` `save breakpoints FILE` writes breakpoints as commands. After `break
table_put`, `break 104 if missing > 0` and `dprintf 67,"put %s\n", name`, the file holds:

```
break table_put
break .../examples/ch08/inventory.c:104
  condition $bpnum missing > 0
dprintf .../examples/ch08/inventory.c:67,"put %s\n", name
```

`source` it in the next session. Exercise 3 turns `check-table` into a check for a build.

## .gdbinit and the safe-path

gdb reads `~/.config/gdb/gdbinit` (or `~/.gdbinit`) at start-up, then a `.gdbinit` in the
current directory. The second is a risk: a source tree you downloaded could ship a `.gdbinit`
that runs `shell rm ...` or any Python. So gdb loads a local one only from directories in its
*auto-load safe-path*. Here `examples/ch08/.gdbinit` holds `source inventory.py` and `break
104`:

```console
$ gdb -q -batch -ex 'info breakpoints' ./inventory
warning: File ".../examples/ch08/.gdbinit" auto-loading has been declined by your `auto-load safe-path' set to "$debugdir:$datadir/auto-load".
To enable execution of this file add
	add-auto-load-safe-path .../examples/ch08/.gdbinit
line to your configuration file "~/.config/gdb/gdbinit".
...
No breakpoints or watchpoints.
$ gdb -q -iex 'add-auto-load-safe-path ~/learn-gdb-debugging' -batch -ex 'info breakpoints' ./inventory
Breakpoint 1 at 0x170b: file inventory.c, line 104.
Num     Type           Disp Enb Address            What
1       breakpoint     keep y   0x000000000000170b in main at inventory.c:104
```

- Add trusted directories one by one to `~/.config/gdb/gdbinit`. The warning also suggests
  `set auto-load safe-path /`; that trusts every directory on the machine.
- `-iex` runs after your home gdbinit but before the program and the local `.gdbinit` are
  loaded. `-ex` runs after them. `-nx` skips all init files.
- The same check covers scripts shipped with a program: `inventory-gdb.py` next to
  `inventory` loads with it if its directory is in the safe-path (`info auto-load
  python-scripts` says `Yes`). There `gdb.current_objfile()` is the program, so the printers
  apply to it only.

## The command line, faster

```console
(gdb) b table_put
Breakpoint 1 at 0x1448: file inventory.c, line 59.
(gdb) r stock.txt
...
Breakpoint 1, table_put (t=0x7fffffffe380, name=0x7fffffffe3a0 "bolt", qty=120) at inventory.c:59
59	    if (t->count >= 2 * t->nbuckets)
(gdb) n
61	    size_t h = hash(name) % t->nbuckets;
(gdb) 
62	    struct item *it = malloc(sizeof *it);
(gdb) 
63	    it->name = strdup(name);
(gdb) co
Ambiguous command "co": collect, commands, compare-sections, compile, complete, condition, continue, core-file.
(gdb) ste
Ambiguous command "ste": step, stepi, stepping.
(gdb) frobnicate
Undefined command: "frobnicate".  Try "help".
(gdb) c 3
Not stopped at any breakpoint; argument ignored.
Continuing.

Breakpoint 1, table_put (t=0x7fffffffe380, name=0x7fffffffe3a0 "nut", qty=340) at inventory.c:59
59	    if (t->count >= 2 * t->nbuckets)
```

- Any unique start works, and some short names are aliases although not unique: `b`, `c`,
  `n`, `s`, `r`, `i` (`i b` is `info breakpoints`).
- An empty line repeats `next`, `step`, `continue`, `list`, `x` and a few others, but not
  commands that would do harm twice.
- `continue N` ignores this breakpoint N−1 more times, so it needs a stop at a breakpoint.
- `show commands` lists the last ten lines of history (empty lines are not stored). The
  prompt is GNU readline: arrows, Ctrl-R to search, Tab to complete. `set history save on`
  keeps the history across sessions.

## Building tdb: a command language

Until now tdb read a line, compared its first word with each command name in a chain of
`strcmp`s, and ran it. v0.8 (`project/ch08/`) turns that into a small command language.
`tdb.c` grows from 1277 to 1830 lines, and two new files keep the new parts testable:
`src/cli.c` holds the pure functions (look a word up, expand `$arg0`, unescape `echo`), and
`src/input.c` decides where the next line comes from. Here is a session on a new target,
`targets/sum.c`, which adds its arguments into the globals `total` and `calls`; `add(long x)`
starts on line 14 with `total += x;`.

```console
$ build/tdb build/targets/sum
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
(tdb) r 1 2 3
Started process 1018348: build/targets/sum
rdi = 0x1 (1)
rdi = 0x2 (2)
rdi = 0x3 (3)
sum = 6
[process exited with status 0]
(tdb) tbreak main
Temporary breakpoint 2 at 0x11af: file targets/sum.c, line 20.
(tdb) r
Started process 1018349: build/targets/sum
Temporary breakpoint 2, main () at targets/sum.c:20
20	    for (int i = 1; i < argc; i++)
(tdb) n
21	        add(atol(argv[i]));
(tdb) 
rdi = 0x1 (1)
rdi = 0x2 (2)
rdi = 0x3 (3)
sum = 6
[process exited with status 0]
(tdb) 
The program is not being run.
(tdb) 
Quit
(tdb) ste
Ambiguous command "ste": step, stepi.
(tdb) info b
Num  Type        Disp  Address  In memory           Hits  What
1    breakpoint  keep  0x1175   (not running)       6     in add at targets/sum.c:14
        silent
        reg rdi
        continue
```

- The breakpoint's commands make it a dprintf without debug info for locals: `add`'s argument
  is in `rdi`, by the calling convention of Chapter 6.
- `r` with no arguments runs with the previous ones, `1 2 3`, as gdb does.
- The first empty line repeats `next`, which runs into `add`: three silent hits, then the end.
  The second repeats `next` again, with no program. Before the third, Ctrl-C was pressed at
  the prompt, and readline's line was thrown away with `Quit`.
- Hits counts 6 over the two runs.

### A table of commands

In `src/cli.h` a command is a `struct cli_cmd`: a name, an id for the `switch` in `execute()`,
and flags `CLI_ALIAS`, `CLI_REPEAT` (an empty line runs it again) and `CLI_REPEAT_BARE` (the
same, without arguments, for `list`). `builtins[]` in `tdb.c` has rows such as `{ "continue",
CMD_CONTINUE, CLI_REPEAT }` and `{ "c", CMD_CONTINUE, CLI_ALIAS | CLI_REPEAT }`. User commands
from `define` are appended to a copy of it, so one lookup finds both.

```c
enum cli_match cli_lookup(const struct cli_cmd *table, int n, const char *word,
                          const struct cli_cmd **found)
{
    const struct cli_cmd *match = NULL;

    for (int i = 0; i < n; i++) {
        if (strcmp(table[i].name, word) == 0) {
            *found = &table[i];
            return CLI_FOUND;
        }
    }
    for (int i = 0; i < n; i++) {
        if (!starts_with(table[i].name, word))
            continue;
        if (match != NULL && match->id != table[i].id)
            return CLI_AMBIGUOUS;
        /* Prefer the command's own name to an alias of it. */
        if (match == NULL || (match->flags & CLI_ALIAS))
            match = &table[i];
    }
    if (match == NULL)
        return CLI_UNDEFINED;
    *found = match;
    return CLI_FOUND;
}
```

- An exact name wins, so `s` is `step` although `set`, `show` and `si` also start with `s`.
- Otherwise the word must start exactly one command. Rows with the same id count once:
  `fi` matches `finish` only, `n` matches only `next`, and `ste` matches two commands.
- `find()` turns the result into gdb's messages, with `cli_matches()` listing the candidates
  in order: `Ambiguous command "ste": step, stepi.`, `Undefined command: "frobnicate".  Try
  "help".`, and for `info`, `Ambiguous info command "s": signals, symbol.`.
- `command_loop()` keeps the last line whose command has `CLI_REPEAT` and runs it again on an
  empty line.

`execute()` skips a line starting with `#`, splits the rest at spaces, finds the command and
switches on its id. All errors go through `error()`. Inside a command file it first prints
`FILE:LINE: Error in sourced command file:`, as gdb does, taking the place from `input.c`'s
stack of up to 16 `source` files. Then it prints the message and sets the `failed` flag that
stops the file.

### Commands at a breakpoint

The obvious place to run a breakpoint's commands is `report()`, which prints "Breakpoint 1,
...". But `report()` runs inside `continue`, so a `continue` in the commands would run inside
`continue`: each hit would add frames to tdb's own stack, and a million hits would overflow
it. So `report()` only copies the breakpoint's commands to `pending_commands` (and returns
early if they start with `silent`). They run after the command that resumed has returned:

```c
static void run_pending_commands(void)
{
    static bool active;

    if (active)
        return;             /* the loop below will see the new ones */
    active = true;
    while (pending_commands != NULL && !failed && !quitting) {
        char *list = pending_commands, *p = list;
        unsigned resumed = resumes;

        pending_commands = NULL;
        if (strncmp(p, "silent\n", 7) == 0)
            p += 7;
        while (*p != '\0' && resumes == resumed && !failed && !quitting) {
            char *nl = strchr(p, '\n');
            *nl = '\0';
            execute(p);
            p = nl + 1;
        }
        free(list);
    }
    free(pending_commands);         /* after an error: forget them */
    pending_commands = NULL;
    active = false;
}
```

- `resumes` counts every time tdb lets the program run. When it changes, the list ends: the
  program has moved on, so the lines after a `continue` are skipped, as in gdb.
- The `continue` in the list stops at the next hit, which queues the list again. The outer
  `while` runs it. A million hits are a million turns of this loop, at a constant stack depth.
- `active` makes a nested call (from `source` inside the list) leave the work to the outer
  loop.

![How a line reaches a handler in tdb v0.8: lines come from ~/.tdbinit (unless -nx), then -x files and -ex commands in order, then the terminal; execute() looks the word up with find() and cli_lookup; a handler that resumes the program leads to report(), which queues the breakpoint's commands; run_pending_commands() runs them after the command returns, and a list ends when the program is resumed again](images/ch08-command-flow.png)

### define, $arg0 and depth

```console
(tdb) define upto
...
>tbreak $arg0
>run $arg1 $arg2
>counts
>end
(tdb) upto add 4 5
Temporary breakpoint 1 at 0x1175: file targets/sum.c, line 14.
Started process 1018846: build/targets/sum
Temporary breakpoint 1, add () at targets/sum.c:14
14	    total += x;
total and calls:
total = 0 (0x0), 8 bytes at 0x555555558018
calls = 0 (0x0), 4 bytes at 0x555555558020
(tdb) upto
Missing argument 0 in user function.
(tdb) define break
"break" is a tdb command: it cannot be redefined.
(tdb) define loop
...
>loop
>end
(tdb) loop
Max user call depth exceeded -- command aborted.
```

`counts` was defined just before: `echo total and calls:\n`, `print total`, `print calls`.
`define` stores the body as a string of lines and adds a row to the command table (up to 32
user commands). `call_user()` runs a copy of the body (it may redefine itself) line by line,
and `cli_expand()` replaces `$argc` and `$arg0` to `$arg9` first. A depth counter stops at 64.

### Scripts, options and ~/.tdbinit

`main()` now takes `-x FILE` and `-ex COMMAND` (run in the order given), `-batch` (exit after
them) and `-nx`. It reads `~/.tdbinit` first, unless `-nx`. tdb reads no `.tdbinit` from the
current directory: without gdb's safe-path machinery, that would run any file a downloaded
directory contains.

```console
$ grep -v '^#' scripts/trace-sum.tdb
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
$ build/tdb -batch -nx -x scripts/trace-sum.tdb build/targets/sum 10 20 30; echo "[exit $?]"
Breakpoint 1 at 0x1175: file targets/sum.c, line 14.
Temporary breakpoint 2 at 0x11eb: file targets/sum.c, line 22.
Started process 1022296: build/targets/sum
rdi = 0xa (10)
rdi = 0x14 (20)
rdi = 0x1e (30)
Temporary breakpoint 2, main () at targets/sum.c:22
22	    printf("sum = %ld\n", total);
total = 60 (0x3c), 8 bytes at 0x555555558018
calls = 3 (0x3), 4 bytes at 0x555555558020
[process killed]
[exit 0]
$ build/tdb -batch -nx -x build/bad.tdb -ex 'echo still here\n' build/targets/sum; echo "[exit $?]"
Breakpoint 1 at 0x1175: file targets/sum.c, line 14.
build/bad.tdb:7: Error in sourced command file:
Undefined command: "frobnicate".  Try "help".
still here
[exit 0]
```

`build/bad.tdb` has a comment, `break add` with commands, then `frobnicate` on line 7. As in
gdb, the failing file stops but the next `-ex` runs, and the exit status is 1 only if the last
item failed (`-ex 'print nosuch'` last gives 1). The script stops at line 22 to print because
tdb's `print` reads a live process, even for globals.

### The prompt

When stdin is a terminal and tdb was built with GNU readline, `input.c` reads lines with
`readline(prompt)` and stores non-empty ones with `add_history()`. The `Makefile` turns this on
if `/usr/include/readline/readline.h` exists (package `libreadline-dev`), and `make
READLINE=no` builds without it. Ctrl-C needed care. Chapter 7 gave tdb a `SIGINT` handler
without `SA_RESTART`, so a Ctrl-C makes readline's `read()` fail with `EINTR`. tdb sets
`rl_catch_signals = 0` so that readline leaves the signal alone, and installs its own key
reader as `rl_getc_function`. On `EINTR` with a Ctrl-C pending, the reader empties the line
with `rl_replace_line("", 0)` and returns a newline, and `input_read()` prints `Quit`.

### Tests and limits

`make test` runs 26 unit tests (4 new in `tests/test_cli.c`) and 72 integration tests, 14 of
them new, from abbrev and repeat to batch-fail and tdbinit-nx. `itest.sh` passes `-nx`, so
your own `~/.tdbinit` cannot break them. Both pass with `make READLINE=no` too.

```console
$ make test
...
unit: 26 passed, 0 failed
itest: 72 passed, 0 failed
```

Limits: `run` splits its arguments at spaces, with no quotes and no `<` or `>` (gdb hands them
to a shell). There is no `if` or `while`, no convenience variables and no expressions, so
`define` and `commands` run plain lists. Only `~/.tdbinit` is read. Ctrl-C at the prompt still
reaches the program too, as in Chapter 7.

## Common mistakes

- **A condition on data the code is changing.** `line[1] == 'A'` at line 24 held for three
  characters. Decide on entry and keep the answer in a convenience variable.
- **Reading a variable before its line has run**, as `print h` at line 46 or `count` at 67.
- **Forgetting `silent` or `continue`.** Without `silent` every hit prints `Breakpoint 1,
  table_put (...)` and the source line; without `continue` the trace stops at every hit.
- **Commands after `continue`** in a `commands` list never run, in gdb or in tdb.
- **`continue N` right after `next`**: `Not stopped at any breakpoint; argument ignored.`
- **Expecting a local `.gdbinit` to load.** Read the warning; it says what to add.
- **A multi-line `define` through `-ex`.** Each `-ex` is one line. With `-ex 'define hi' -ex
  'echo hi\n' -ex 'end'`, `define` reads its body from stdin and gets nothing, `echo` runs at
  once, and `end` fails: `This command cannot be used at the top level.` Use a `-x` file.
- **Trusting gdb's exit status.** Without `-return-child-result` a failing program gives 0.
- **(tdb) Running breakpoint commands inside `report()`**: `continue` would recurse per hit.

## In the wild

GDB 7.0 was released on 6 October 2009. Its announcement lists Python scripting first among
the major new features, together with reverse debugging (Chapter 9), non-stop debugging and
multi-process debugging. Before that, gdb's only scripting was its own command language:
`define`, `if`, `while`, command files and breakpoint commands.

The most used pretty printers are the ones you never load by hand. The libstdc++ manual says
that printers for its containers "are distributed with GCC from version 4.5.0" and are
installed next to the library, so gdb finds them automatically. They are why `print v` of a
`std::vector<int>` shows the elements, not three pointers. The same manual warns that gdb
might refuse to auto-load them, and points to the safe-path, as you saw with `.gdbinit`.

gdb's NEWS file dates the other features of this chapter. `dprintf` and the auto-load
safe-path both came in GDB 7.5. `define-prefix`, which lets user commands have subcommands,
came in GDB 9. `set logging enabled on` replaced `set logging on` in GDB 12, which is why
gdb 12.1 prints the deprecation warning.

The line editor at gdb's prompt, and now tdb's, is GNU Readline, maintained by Chet Ramey. It
gives both emacs and vi editing modes and a history list. Its history half is also a separate
library, which gdb's `show commands` and `set history save` build on.

## Summary

- Pretty printers (`gdb.printing`, `to_string`, `children`, `display_hint`) make gdb print your
  data structures; `print/r` bypasses them.
- A `gdb.Command` adds a command, and a `gdb.Function` adds `$name(...)` for any expression,
  including breakpoint conditions. Conditions run at every hit.
- `define` builds commands from gdb commands, with `$arg0`, `$argc`, `while`, `if` and
  `document`. `hook-stop` runs at every stop.
- `dprintf`, breakpoint `commands` with `silent` and `continue`, and Python breakpoints whose
  `stop()` returns False all observe a program without stopping it.
- `gdb -batch -x FILE -ex CMD --args PROG ...` runs scripts. The exit status reflects the last
  command, or the program's with `-return-child-result`.
- A local `.gdbinit` and auto-loaded scripts need the auto-load safe-path. Add trusted
  directories one by one.
- Unique prefixes, an empty line to repeat, `show commands` and readline make typing faster.
- tdb v0.8 has a command table with gdb's lookup rules and messages, breakpoint commands run
  from a queue, `define` with arguments, `source`, `-x`/`-ex`/`-batch`/`-nx`, `~/.tdbinit` and
  readline.

## Exercises

1. Fix `inventory.c` and confirm it with `check-table`.

<details><summary>Solution</summary>

Line 46 must hash with the new size:

```c
            size_t h = hash(it->name) % n;
```

```console
$ make inventory && ./inventory stock.txt; echo "[exit $?]"
20 items, 0 not found
[exit 0]
$ gdb -q -batch -ex 'source inventory.py' -ex 'break 104' -ex run -ex 'check-table stock' \
      --args ./inventory stock.txt
...
104	    fclose(f);
20 items, 0 in the wrong bucket
```

</details>

2. Fix `csvsum.c` using the trace from `trace.gdb`, and check the total.

<details><summary>Solution</summary>

After a doubled quote the field is still quoted. Line 57 becomes:

```c
                st = QUOTED;
```

```console
$ make csvsum && ./csvsum orders.csv
4 orders, total 537
$ gdb -q -batch -x trace.gdb --args ./csvsum orders.csv
...
"  QUOTED
"  QUOTE
B  QUOTED
i  QUOTED
...
```

`B` is now handled inside the quoted field, and the comma in `Tools, Inc.` stays in it.

</details>

3. Write `check.gdb`, a check for a build script. It should run `inventory` to line 104 and
   make gdb exit with status 1 if any item is in the wrong bucket, after listing them, and
   with 0 otherwise. Use `$misplaced`, gdb's `if`, and `quit 1`.

<details><summary>Solution</summary>

```
source inventory.py
break 104
run
if $misplaced(stock) > 0
  check-table stock
  kill
  quit 1
end
echo table is fine\n
```

With the unfixed `inventory.c`:

```console
$ gdb -q -batch -x check.gdb --args ./inventory stock.txt; echo "[exit $?]"
...
104	    fclose(f);
nut      in bucket  0, belongs in bucket  8
...
screw    in bucket  7, belongs in bucket 15
20 items, 9 in the wrong bucket
Kill the program being debugged? (y or n) [answered Y; input not from terminal]
[Inferior 1 (process 1028645) killed]
[exit 1]
$ gdb -q -batch -x check.gdb --args ./inventory small.txt; echo "[exit $?]"
...
104	    fclose(f);
table is fine
[exit 0]
```

gdb answers its own question because input is not a terminal. Without `kill`, `quit` asks
"Quit anyway?" instead, also answered Y. The exit status comes from `quit 1`, so a `make check`
rule can call this directly.

</details>

4. Add gdb's `hook-stop` to tdb: if a user command named `hook-stop` exists, run it at every
   stop, before the stop is reported, but not when the program exits. Add an integration test.

<details><summary>Solution</summary>

In `tdb.c`, a function that finds and runs the hook, guarded so that a `stepi` inside the hook
does not run the hook again:

```c
static void run_hook_stop(void)
{
    static bool active;

    for (int i = 0; i < nusers && !active; i++) {
        if (strcmp(users[i].name, "hook-stop") == 0) {
            active = true;
            call_user(i, NULL, 0);
            active = false;
        }
    }
}
```

`report()` calls it after the `silent` check, before anything is printed (with a prototype
above `report()`):

```c
    if (stopped_at != NULL && stopped_at->silent)
        return;
    if (st->why != STOP_EXITED)
        run_hook_stop();
```

The test in `tests/itest.sh` checks the order of the lines, and that no `calls =` follows
`sum = 3`:

```sh
check hook-stop sum 'define hook-stop\nprint calls\nend\nbreak add\nrun 1 2\ncontinue\ncontinue\n' \
    'calls = 0 (0x0)' 'Breakpoint 1, add ()' 'calls = 1 (0x1)' 'Breakpoint 1, add ()' \
    'sum = 3' '!calls ='
```

`make test` then reports `itest: 73 passed, 0 failed`. The same session by hand:

```console
$ printf 'define hook-stop\nprint calls\nend\nbreak add\nrun 1 2\ncontinue\ncontinue\n' | build/tdb build/targets/sum
...
(tdb) run 1 2
Started process 1027576: build/targets/sum
calls = 0 (0x0), 4 bytes at 0x555555558020
Breakpoint 1, add () at targets/sum.c:14
14	    total += x;
(tdb) continue
calls = 1 (0x1), 4 bytes at 0x555555558020
Breakpoint 1, add () at targets/sum.c:14
14	    total += x;
(tdb) continue
sum = 3
[process exited with status 0]
```

The hook runs before each report, as in gdb, and not at the exit.

</details>

Sources for this chapter: [REFERENCES.md](REFERENCES.md#chapter-8--automating-gdb)
