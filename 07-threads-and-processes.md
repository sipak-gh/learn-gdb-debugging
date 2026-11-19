# Chapter 7 — Threads and Processes

Every program so far had one thread in one process. Real programs are not so kind. They hang
because two threads each wait for the other. They print a wrong total because two threads
write one variable at once. They crash in a child process that the debugger never followed.
This chapter is about debugging all three. On the gdb side you will use `info threads`, `thread
N`, `thread apply all bt`, `set scheduler-locking`, watchpoints across threads, `set
follow-fork-mode`, `set detach-on-fork`, `info inferiors` and `catch fork`, plus
ThreadSanitizer, which finds races without needing luck. On the tdb side, v0.7 follows every
thread, stops them all when one stops (as gdb does by default), gets `info threads`, `thread`
and `thread apply`, cleans up after `fork`, and handles Ctrl-C.

Previous: [Chapter 6](06-down-to-the-metal.md). Next: [Chapter 8](08-automating-gdb.md).

## A program that never finishes

Two bank tellers run at the same time, one per thread. One moves money from alice to bob 100000
times, the other from bob to alice. Each account has a lock. A transfer holds both locks, so
nobody ever sees the money missing from both accounts or present in both.

`examples/ch07/bank.c`
```c
/* bank.c - two tellers move money back and forth between two accounts at
 * the same time. Each account has a lock. Sometimes it never finishes.
 *
 *     ./bank
 */
#include <pthread.h>
#include <stdio.h>

struct account {
    int id;
    long balance;
    pthread_mutex_t lock;
};

struct teller {
    struct account *from, *to;
    int transfers;
};

struct account alice = { 1, 1000, PTHREAD_MUTEX_INITIALIZER };
struct account bob = { 2, 1000, PTHREAD_MUTEX_INITIALIZER };

/* Move amount from one account to the other, holding both locks so that
 * nobody sees the money in neither (or both) accounts. */
void transfer(struct account *from, struct account *to, long amount)
{
    pthread_mutex_lock(&from->lock);
    pthread_mutex_lock(&to->lock);
    from->balance -= amount;
    to->balance += amount;
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

void *teller(void *arg)
{
    struct teller *t = arg;
    for (int i = 0; i < t->transfers; i++)
        transfer(t->from, t->to, 1);
    return NULL;
}

int main(void)
{
    struct teller t1 = { &alice, &bob, 100000 };
    struct teller t2 = { &bob, &alice, 100000 };
    pthread_t a, b;

    pthread_create(&a, NULL, teller, &t1);
    pthread_create(&b, NULL, teller, &t2);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("alice: %ld, bob: %ld, total: %ld\n", alice.balance, bob.balance,
           alice.balance + bob.balance);
    return 0;
}
```

```console
$ cd examples/ch07
$ make
cc -std=c11 -Wall -Wextra -g -O0 -pthread    bank.c   -o bank
cc -std=c11 -Wall -Wextra -g -O0 -pthread    hits.c   -o hits
cc -std=c11 -Wall -Wextra -g -O0 -pthread    linecount.c   -o linecount
$ timeout 3 ./bank; echo "[exit $?]"
[exit 124]
```

- `-pthread` compiles and links for POSIX threads (learn-c, Chapter 17 covers the API).
- Exit status 124 is `timeout` saying it had to kill the program. It hung in every run here. A
  lucky run could finish, since the bug depends on timing.

A hung program gives you no crash and no message. Run it under gdb and press Ctrl-C after a
second. Paths are shortened to `...` here and below, and after this session the two
`libthread_db` lines that gdb prints at each `run` are left out.

```console
$ gdb -q ./bank
Reading symbols from ./bank...
(gdb) run
Starting program: .../examples/ch07/bank
[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".
[New Thread 0x7ffff7bff640 (LWP 785575)]
[New Thread 0x7ffff73fe640 (LWP 785576)]
^C
Thread 1 "bank" received signal SIGINT, Interrupt.
__futex_abstimed_wait_common64 (private=128, cancel=true, abstime=0x0, op=265, expected=785575, futex_word=0x7ffff7bff910) at ./nptl/futex-internal.c:57
57	./nptl/futex-internal.c: No such file or directory.
(gdb) info threads
  Id   Target Id                                 Frame 
* 1    Thread 0x7ffff7fb0740 (LWP 785571) "bank" __futex_abstimed_wait_common64 (private=128, cancel=true, abstime=0x0, op=265, expected=785575, futex_word=0x7ffff7bff910) at ./nptl/futex-internal.c:57
  2    Thread 0x7ffff7bff640 (LWP 785575) "bank" futex_wait (private=0, expected=2, futex_word=0x555555558070 <bob+16>) at ../sysdeps/nptl/futex-internal.h:146
  3    Thread 0x7ffff73fe640 (LWP 785576) "bank" futex_wait (private=0, expected=2, futex_word=0x555555558030 <alice+16>) at ../sysdeps/nptl/futex-internal.h:146
```

- gdb prints `[New Thread ...]` for each new thread. The SIGINT went to thread 1, the main
  thread, which is waiting inside `pthread_join`.
- `info threads` lists every thread. `Id` is gdb's own number for it, and `*` marks the
  current thread, the one that `bt`, `print` and `step` work on.
- Threads 2 and 3 sit in `futex_wait`: a futex is what a taken glibc mutex sleeps on. gdb names
  an address by the nearest symbol, and 16 bytes into `struct account` (after `int id`, padding
  and `long balance`) is `lock`. So thread 2 waits for bob's lock and thread 3 for alice's.
- gdb can name glibc's functions and arguments because this machine has glibc's debug symbols
  (`libc6-dbg`). "No such file or directory" only means the glibc *source* is not installed.

Now look at every stack at once:

```console
(gdb) thread apply all bt

Thread 3 (Thread 0x7ffff73fe640 (LWP 785576) "bank"):
#0  futex_wait (private=0, expected=2, futex_word=0x555555558030 <alice+16>) at ../sysdeps/nptl/futex-internal.h:146
#1  __GI___lll_lock_wait (futex=futex@entry=0x555555558030 <alice+16>, private=0) at ./nptl/lowlevellock.c:49
#2  0x00007ffff7c97fc2 in lll_mutex_lock_optimized (mutex=0x555555558030 <alice+16>) at ./nptl/pthread_mutex_lock.c:48
#3  ___pthread_mutex_lock (mutex=0x555555558030 <alice+16>) at ./nptl/pthread_mutex_lock.c:93
#4  0x0000555555555221 in transfer (from=0x555555558060 <bob>, to=0x555555558020 <alice>, amount=1) at bank.c:28
#5  0x00005555555552b2 in teller (arg=0x7fffffffe3f0) at bank.c:39
#6  0x00007ffff7c94a83 in start_thread (arg=<optimized out>) at ./nptl/pthread_create.c:442
#7  0x00007ffff7d268e0 in clone3 () at ../sysdeps/unix/sysv/linux/x86_64/clone3.S:81

Thread 2 (Thread 0x7ffff7bff640 (LWP 785575) "bank"):
#0  futex_wait (private=0, expected=2, futex_word=0x555555558070 <bob+16>) at ../sysdeps/nptl/futex-internal.h:146
#1  __GI___lll_lock_wait (futex=futex@entry=0x555555558070 <bob+16>, private=0) at ./nptl/lowlevellock.c:49
#2  0x00007ffff7c97fc2 in lll_mutex_lock_optimized (mutex=0x555555558070 <bob+16>) at ./nptl/pthread_mutex_lock.c:48
#3  ___pthread_mutex_lock (mutex=0x555555558070 <bob+16>) at ./nptl/pthread_mutex_lock.c:93
#4  0x0000555555555221 in transfer (from=0x555555558020 <alice>, to=0x555555558060 <bob>, amount=1) at bank.c:28
#5  0x00005555555552b2 in teller (arg=0x7fffffffe3d0) at bank.c:39
#6  0x00007ffff7c94a83 in start_thread (arg=<optimized out>) at ./nptl/pthread_create.c:442
#7  0x00007ffff7d268e0 in clone3 () at ../sysdeps/unix/sysv/linux/x86_64/clone3.S:81

Thread 1 (Thread 0x7ffff7fb0740 (LWP 785571) "bank"):
...
#3  0x00007ffff7c965e4 in __pthread_clockjoin_ex (threadid=140737349940800, thread_return=0x0, clockid=0, abstime=0x0, block=<optimized out>) at ./nptl/pthread_join_common.c:105
#4  0x000055555555536d in main () at bank.c:51
```

- `thread apply all CMD` runs `CMD` in each thread, highest number first (`thread apply 2 3
  bt` picks threads by number). In a hang it is the first command to type.
- Frames #0–#3 are glibc's mutex code. Frame #4 is ours, and it is the same line in both
  threads: `bank.c:28`, the *second* lock. Thread 2 moves alice → bob, thread 3 bob → alice.
  Thread 1 waits in `pthread_join` (line 51) for thread 2, which will never end.

Who holds the locks? A glibc mutex records its owner's thread id. Go to thread 2's frame 4 and
look:

```console
(gdb) thread 2
[Switching to thread 2 (Thread 0x7ffff7bff640 (LWP 785575))]
#0  futex_wait (private=0, expected=2, futex_word=0x555555558070 <bob+16>) at ../sysdeps/nptl/futex-internal.h:146
146	../sysdeps/nptl/futex-internal.h: No such file or directory.
(gdb) frame 4
#4  0x0000555555555221 in transfer (from=0x555555558020 <alice>, to=0x555555558060 <bob>, amount=1) at bank.c:28
28	    pthread_mutex_lock(&to->lock);
(gdb) print *from
$1 = {id = 1, balance = -13601, lock = {__data = {__lock = 2, __count = 0, __owner = 785575, __nusers = 1, __kind = 0, __spins = 0, __elision = 0, __list = {__prev = 0x0, __next = 0x0}}, __size = "\002\000\000\000\000\000\000\000\247\374\v\000\001", '\000' <repeats 26 times>, __align = 2}}
(gdb) print from->lock.__data.__owner
$2 = 785575
(gdb) print to->lock.__data.__owner
$3 = 785576
(gdb) kill
Kill the program being debugged? (y or n) y
[Inferior 1 (process 785571) killed]
```

- `thread N` switches the current thread, and `frame 4` selects a frame in *that* thread's
  stack, so `print` sees its `from` and `to`.
- `__lock = 2` is glibc's "locked, and someone is waiting" state. `__owner` is the LWP of the
  holder. alice's lock (`from`) is owned by 785575, thread 2 itself. bob's lock is owned by
  785576, thread 3. `__owner` is a private glibc field: fine to read in a debugger, never in
  code.

Draw it, and the bug is plain. Thread 2 holds alice and wants bob. Thread 3 holds bob and wants
alice. Each waits for the other forever. This is a *deadlock*: two locks taken in opposite
orders by two threads. Each teller takes `from` first, and the two tellers have opposite
`from`s. The fix is to make every thread take the two locks in the same order (exercise 1).

ThreadSanitizer (TSan) can report this without waiting for the hang. learn-c, Chapter 17
introduced it, along with the `setarch -R` it needs on recent kernels. Build with
`-fsanitize=thread` and run:

```console
$ gcc -g -O0 -pthread -fsanitize=thread bank.c -o bank-tsan
$ timeout 5 setarch -R ./bank-tsan; echo "[exit $?]"
==================
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=1376682)
  Cycle in lock order graph: M0 (0x555555558030) => M1 (0x555555558070) => M0

  Mutex M1 acquired here while holding mutex M0 in thread T1:
    #0 pthread_mutex_lock ../../../../src/libsanitizer/sanitizer_common/sanitizer_common_interceptors.inc:4240 (libtsan.so.0+0x53908)
    #1 transfer .../examples/ch07/bank.c:28 (bank-tsan+0x130d)
...
SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock) .../examples/ch07/bank.c:28 in transfer
==================
[exit 124]
```

- The trimmed part shows the other half (M0 taken while holding M1, in thread T2) and where
  T1 and T2 were created. M0 is alice's lock (`alice+16`) and M1 is bob's.
- TSan records the order in which each thread takes locks. Once it has seen M0→M1 and M1→M0,
  it reports the cycle, even in a run that would have finished by luck. Then the program hangs
  anyway, and `timeout` kills it (124). TSan reports problems. It does not prevent them.

## Threads are processes that share memory

To debug threads you need to know what a thread is to the kernel, because the debugger works
with the kernel's view. On Linux, a thread is a *task*, the same kind of object as a process.
`pthread_create` calls `clone()` with flags such as `CLONE_VM` (share the address space),
`CLONE_FILES` and `CLONE_THREAD` (join the same thread group). The new task has its own task
id (*tid*), its own registers and its own stack. It shares everything else. The process id
that `getpid()` returns is the thread group's id, which is the main thread's tid. `ps -L`
lists the tasks as LWPs ("light-weight processes"), one per thread, and `/proc/PID/task/` has
one directory per thread. gdb uses the same name.

Because each thread is a task, `ptrace` works on threads, not processes. Every thread has its
own ptrace stops, registers (`PTRACE_GETREGS` takes a tid) and debug registers. A debugger has
to attach to each thread and hear about each new one.

gdb shows three names for one thread:

| name | example | where it comes from |
|---|---|---|
| thread number | `2` | gdb's own count: 1, 2, 3, ... in the order gdb saw threads start, never reused |
| `Thread 0x7ffff7bff640` | the `pthread_t` | glibc's thread descriptor, found through `libthread_db` |
| `LWP 785575` | the tid | the kernel's task id, what `ps -L` and `__owner` show |

The kernel knows nothing about `pthread_t`. glibc ships `libthread_db`, a library that a
debugger loads to read glibc's thread structures inside the program. "[Thread debugging using
libthread_db enabled]" means gdb loaded it. It is also how gdb finds thread-local variables
such as `errno`.

When any thread stops (a breakpoint, a signal, Ctrl-C), gdb stops *all* the others too. This
is *all-stop* mode, gdb's default, and why `info threads` above showed every thread at one
moment. `continue` resumes all of them. Stopping is not instant: the others run on for a
moment after the first one stops, and one of them can reach a breakpoint too. tdb will meet
that. gdb also has a *non-stop* mode (`set non-stop on`, before `run`), in which only the
thread that stopped is stopped. This course does not use it.

## A lost update

Four threads each count a million log lines into one shared counter.

`examples/ch07/hits.c`
```c
/* hits.c - count the requests in a log with four threads, each counting
 * a quarter of them. The total is wrong, and different every run.
 *
 *     ./hits
 */
#include <pthread.h>
#include <stdio.h>

#define THREADS 4
#define PER_THREAD 1000000

long hits;

void *count(void *arg)
{
    (void)arg;
    for (int i = 0; i < PER_THREAD; i++)
        hits++;
    return NULL;
}

int main(void)
{
    pthread_t t[THREADS];

    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, count, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    printf("hits: %ld (expected %d)\n", hits, THREADS * PER_THREAD);
    return 0;
}
```

```console
$ ./hits
hits: 1075802 (expected 4000000)
$ ./hits
hits: 1018263 (expected 4000000)
$ ./hits
hits: 1207864 (expected 4000000)
```

About three million increments vanish, and a different number each time. To see why, look at
what line 18 compiles to (`gdb -batch -ex 'disassemble /s count' ./hits`, the line-18 part):

```console
18	        hits++;
   0x00000000000011be <+21>:	mov    0x2e53(%rip),%rax        # 0x4018 <hits>
   0x00000000000011c5 <+28>:	add    $0x1,%rax
   0x00000000000011c9 <+32>:	mov    %rax,0x2e48(%rip)        # 0x4018 <hits>
```

`hits++` is three instructions: load `hits` into `rax`, add 1, and store `rax` back. Two
threads running this at once can both load the same value, both add 1, and both store it.
Two increments become one. That is a *lost update*, the classic data race. Each thread works
fine alone. The bug lives only in how the instructions of different threads interleave.

Timing bugs are hard to reproduce. With gdb you can make this one happen on purpose, one
instruction at a time. The tool is `set scheduler-locking on`: from then on, `continue`,
`step` and `stepi` resume *only the current thread*, and the others stay frozen.

```console
$ gdb -q ./hits
Reading symbols from ./hits...
(gdb) break 18
Breakpoint 1 at 0x11be: file hits.c, line 18.
(gdb) run
Starting program: .../examples/ch07/hits
[New Thread 0x7ffff7bff640 (LWP 881872)]
[New Thread 0x7ffff73fe640 (LWP 881873)]
[Switching to Thread 0x7ffff7bff640 (LWP 881872)]

Thread 2 "hits" hit Breakpoint 1, count (arg=0x0) at hits.c:18
18	        hits++;
(gdb) info threads
  Id   Target Id                                 Frame 
  1    Thread 0x7ffff7fb0740 (LWP 881852) "hits" clone3 () at ../sysdeps/unix/sysv/linux/x86_64/clone3.S:62
* 2    Thread 0x7ffff7bff640 (LWP 881872) "hits" count (arg=0x0) at hits.c:18
  3    Thread 0x7ffff73fe640 (LWP 881873) "hits" clone3 () at ../sysdeps/unix/sysv/linux/x86_64/clone3.S:62
(gdb) set scheduler-locking on
(gdb) print hits
$1 = 0
(gdb) stepi
0x00005555555551c5	18	        hits++;
(gdb) x/i $pc
=> 0x5555555551c5 <count+28>:	add    $0x1,%rax
(gdb) print $rax
$2 = 0
```

- With several threads, the stop line names the thread. "[Switching to Thread ...]" says the
  thread that stopped is not the one that was current before.
- Thread 3 is still in `clone3`, the system call that created it. Thread 1 is in `clone3` too,
  creating the next one.
- One `stepi` runs the load. Thread 2 now holds `hits` = 0 in `rax` and is about to add 1. This
  is the dangerous window.

Now let thread 3 run five increments while thread 2 sits in that window:

```console
(gdb) thread 3
[Switching to thread 3 (Thread 0x7ffff73fe640 (LWP 881873))]
#0  clone3 () at ../sysdeps/unix/sysv/linux/x86_64/clone3.S:62
62	../sysdeps/unix/sysv/linux/x86_64/clone3.S: No such file or directory.
(gdb) continue
Continuing.

Thread 3 "hits" hit Breakpoint 1, count (arg=0x0) at hits.c:18
18	        hits++;
(gdb) print hits
$3 = 0
(gdb) continue 5
Will ignore next 4 crossings of breakpoint 1.  Continuing.

Thread 3 "hits" hit Breakpoint 1, count (arg=0x0) at hits.c:18
18	        hits++;
(gdb) print hits
$4 = 5
(gdb) thread 2
[Switching to thread 2 (Thread 0x7ffff7bff640 (LWP 881872))]
#0  0x00005555555551c5 in count (arg=0x0) at hits.c:18
18	        hits++;
(gdb) stepi
0x00005555555551c9	18	        hits++;
(gdb) stepi
17	    for (int i = 0; i < PER_THREAD; i++)
(gdb) print hits
$5 = 1
```

- `continue 5` ignores the next 4 crossings of the breakpoint. With scheduler locking on, only
  thread 3 ran, and it did 5 increments: `hits` = 5.
- Back in thread 2, `rax` still holds the stale 0. Two `stepi`s do the add and the store, and
  `hits` becomes 1. **Five increments are lost**, made to happen by hand, the same way every
  time. In the real run it happens by chance, millions of times. (`set scheduler-locking off`,
  `delete` and `continue` then finish the run with yet another wrong total.)

`scheduler-locking` has four values. `off` lets every thread run on every command. `on` runs
only the current thread. `step` runs only the current thread for `step`, `next` and `stepi`,
but all of them on `continue`. `replay`, the default in gdb 12, acts like `off` unless you are
replaying a recording (Chapter 9). With the default, then, even a `stepi` lets the other
threads run while that one instruction executes. Remember to turn locking off again: with
`on`, a thread that waits for a lock held by a frozen thread waits forever.

ThreadSanitizer finds this race in a single run (built with `-fsanitize=thread`, and run under
`setarch -R`, without which it fails to start on this kernel):

```console
$ setarch -R ./hits-tsan
==================
WARNING: ThreadSanitizer: data race (pid=888063)
  Read of size 8 at 0x555555558018 by thread T2:
    #0 count .../examples/ch07/hits.c:18 (hits-tsan+0x127d)

  Previous write of size 8 at 0x555555558018 by thread T1:
    #0 count .../examples/ch07/hits.c:18 (hits-tsan+0x1297)
...
SUMMARY: ThreadSanitizer: data race .../examples/ch07/hits.c:18 in count
```

The read and the previous write are the load and the store from the disassembly, in two
threads (TSan's own numbering), with no lock or atomic between them. The trimmed part names
the global `hits` and where T1 and T2 were created. After a report the program exits with
status 66, which makes a test run fail. The gdb session showed *how* updates are lost. TSan
shows *that* a race exists, even when the total happens to be right. The fixes are an atomic
increment, a mutex, or (best) a count local to each thread, added to the total once at the
end. Exercise 2 does two of them.

## Watching a variable that every thread writes

Chapter 4 used hardware watchpoints in one thread. A debug register belongs to one CPU
context, which means one thread. So for `watch` to catch a write from *any* thread, the
debugger must set the debug registers in every thread, including threads that do not exist
yet when you type `watch`.

```console
$ gdb -q ./hits
Reading symbols from ./hits...
(gdb) break count
Breakpoint 1 at 0x11b5: file hits.c, line 17.
(gdb) run
...

Thread 2 "hits" hit Breakpoint 1, count (arg=0x0) at hits.c:17
17	    for (int i = 0; i < PER_THREAD; i++)
(gdb) watch hits
Hardware watchpoint 2: hits
(gdb) delete 1
(gdb) continue
Continuing.
[New Thread 0x7ffff6bfd640 (LWP 791611)]
[New Thread 0x7ffff63fc640 (LWP 791612)]
[Switching to Thread 0x7ffff73fe640 (LWP 791610)]

Thread 3 "hits" hit Hardware watchpoint 2: hits

Old value = 0
New value = 3
count (arg=0x0) at hits.c:17
17	    for (int i = 0; i < PER_THREAD; i++)
(gdb) continue
Continuing.
[Switching to Thread 0x7ffff6bfd640 (LWP 791611)]

Thread 4 "hits" hit Hardware watchpoint 2: hits

Old value = 3
New value = 4
count (arg=0x0) at hits.c:17
17	    for (int i = 0; i < PER_THREAD; i++)
(gdb) kill
Kill the program being debugged? (y or n) y
[Inferior 1 (process 791606) killed]
```

- When `watch hits` was typed, only threads 1–3 existed. Thread 4 was created later, and its
  write still stopped the program: gdb copied the debug registers into the new thread as it
  started. The kernel does not do that on `clone`, so tdb will have to.
- "Old value = 0, New value = 3" with one write? The trap fires *after* the write, in the
  writing thread, and until gdb has stopped the others, their writes keep landing. gdb reads
  the new value once everything has stopped. The race shows in the debugger's own numbers.
- The stop line is 17, the one *after* the write, as in Chapter 4. On a real program, add a
  condition (`watch hits if hits > 1000`) or watch something that changes rarely.

## Following a fork

Threads share memory. Processes made by `fork` do not: each child gets a copy. Here is a line
counter with one child per file:

`examples/ch07/linecount.c`
```c
/* linecount.c - count the lines of several files at once: one child
 * process per file.
 *
 *     ./linecount bank.c hits.c linecount.c
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

long count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    long lines = 0;
    int c;

    while ((c = getc(f)) != EOF)
        if (c == '\n')
            lines++;
    fclose(f);
    return lines;
}

int main(int argc, char *argv[])
{
    printf("counting lines in %d files\n", argc - 1);
    for (int i = 1; i < argc; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            printf("%8ld %s\n", count_lines(argv[i]), argv[i]);
            exit(0);
        }
    }
    int status, failed = 0;
    pid_t pid;
    while ((pid = wait(&status)) > 0) {
        if (WIFSIGNALED(status)) {
            printf("worker %d killed by signal %d (%s)\n", (int)pid, WTERMSIG(status),
                   strsignal(WTERMSIG(status)));
            failed++;
        }
    }
    return failed ? 1 : 0;
}
```

```console
$ ./linecount bank.c nosuch.txt hits.c; echo "[exit $?]"
counting lines in 3 files
      56 bank.c
      32 hits.c
worker 1381439 killed by signal 11 (Segmentation fault)
[exit 1]
```

One worker crashed. The parent only learns of it from `wait`. Under gdb:

```console
$ gdb -q ./linecount
Reading symbols from ./linecount...
(gdb) run bank.c nosuch.txt hits.c
Starting program: .../examples/ch07/linecount bank.c nosuch.txt hits.c
counting lines in 3 files
[Detaching after fork from child process 791084]
      56 bank.c
[Detaching after fork from child process 791085]
[Detaching after fork from child process 791086]
      32 hits.c
worker 791085 killed by signal 11 (Segmentation fault)
[Inferior 1 (process 791081) exited with code 01]
```

By default gdb follows the parent and *detaches* each child, after removing its breakpoints
from the child's copy of the code. The child runs free, and the crash happens where gdb cannot
see it. `set follow-fork-mode child` debugs the child instead. `set detach-on-fork off` keeps
the other process too, held stopped, as another *inferior*, gdb's word for a program being
debugged:

```console
(gdb) set follow-fork-mode child
(gdb) set detach-on-fork off
(gdb) run
Starting program: .../examples/ch07/linecount bank.c nosuch.txt hits.c
counting lines in 3 files
[Attaching after Thread 0x7ffff7fb0740 (LWP 791090) fork to child process 791091]
[New inferior 2 (process 791091)]
      56 bank.c
[Inferior 2 (process 791091) exited normally]
(gdb) inferior 1
[Switching to inferior 1 [process 791090] (.../examples/ch07/linecount)]
[Switching to thread 1.1 (Thread 0x7ffff7fb0740 (LWP 791090))]
#0  arch_fork (ctid=0x7ffff7fb0a10) at ../sysdeps/unix/sysv/linux/arch-fork.h:52
52	../sysdeps/unix/sysv/linux/arch-fork.h: No such file or directory.
```

The first child (bank.c) became inferior 2 and finished. The parent, inferior 1, is still held
where the fork returned, inside glibc's `fork`. `inferior 1` switches to it (`info inferiors`
lists them all), and thread numbers now take the form `INFERIOR.THREAD`. Continue the parent.
It forks the second child, which gdb follows:

```console
(gdb) continue
Continuing.
[Attaching after Thread 0x7ffff7fb0740 (LWP 791090) fork to child process 791101]
[New inferior 3 (process 791101)]

Thread 3.1 "linecount" received signal SIGSEGV, Segmentation fault.
[Switching to Thread 0x7ffff7fb0740 (LWP 791101)]
0x00007ffff7c8798f in _IO_getc (fp=0x0) at ./libio/getc.c:37
37	./libio/getc.c: No such file or directory.
(gdb) bt
#0  0x00007ffff7c8798f in _IO_getc (fp=0x0) at ./libio/getc.c:37
#1  0x0000555555555294 in count_lines (path=0x7fffffffe873 "nosuch.txt") at linecount.c:19
#2  0x000055555555533b in main (argc=4, argv=0x7fffffffe4e8) at linecount.c:32
(gdb) frame 1
#1  0x0000555555555294 in count_lines (path=0x7fffffffe873 "nosuch.txt") at linecount.c:19
19	    while ((c = getc(f)) != EOF)
(gdb) print f
$1 = (FILE *) 0x0
(gdb) kill inferiors 1 3
```

(Four "Reading symbols from /usr/lib/debug/.build-id/..." lines, gdb loading glibc's debug
symbols for the new inferior, are trimmed from this output.)

"Thread 3.1" is thread 1 of inferior 3, the child for "nosuch.txt". `fopen` returned NULL,
which went straight into `getc`. The crash is in glibc, but the bug is line 15: the program
never checks `fopen`. `kill inferiors 1 3` ends both held processes. `catch fork` and `catch
exec` stop at every fork or exec, which helps when you do not know where a program starts its
children. The next section uses `catch fork`.

## The line that printed three times

Send the output to a file and something else goes wrong:

```console
$ ./linecount bank.c hits.c > out.txt
$ cat out.txt
counting lines in 2 files
      56 bank.c
counting lines in 2 files
      32 hits.c
counting lines in 2 files
```

The header appears three times, once per process. Stop at the first fork and look inside
`stdout` (gdb's `run` accepts `>` redirections):

```console
$ gdb -q ./linecount
Reading symbols from ./linecount...
(gdb) catch fork
Catchpoint 1 (fork)
(gdb) run bank.c hits.c > out.txt
Starting program: .../examples/ch07/linecount bank.c hits.c > out.txt

Catchpoint 1 (forked process 791200), arch_fork (ctid=0x7ffff7fb0a10) at ../sysdeps/unix/sysv/linux/arch-fork.h:52
52	../sysdeps/unix/sysv/linux/arch-fork.h: No such file or directory.
(gdb) print stdout->_IO_write_ptr - stdout->_IO_write_base
$1 = 26
(gdb) x/s stdout->_IO_write_base
0x5555555592a0:	"counting lines in 2 files\n"
```

- At the fork, 26 bytes sit in `stdout`'s buffer: exactly the header. (`_IO_write_base` and
  `_IO_write_ptr` are glibc's private `FILE` fields, the buffer's start and next free byte.)
  When stdout is a file or a pipe, stdio buffers it fully. On a terminal it flushes at each
  newline, which is why the terminal run printed the header once.
- `fork` copies the whole address space, buffer included. Each child prints its count after
  the copied header and flushes both at `exit`, and the parent flushes its own copy at the end.
  Continuing this session writes the same `out.txt` as above.
- learn-c, Chapter 13 covered stdio buffering across `fork`. The fix is one `fflush(stdout)`
  before the loop (exercise 3).

## Building tdb: threads, all-stop, fork

At the end of Chapter 6, tdb traced one thread. Run it on a threaded program and it sees only
the main thread. The other threads are not traced at all, and a breakpoint they hit kills the
whole process with SIGTRAP. v0.7 (`project/ch07/`) fixes that. No files are added, but
`proc.c` grows from 264 to 647 lines and `tdb.c` from 1081 to 1277, and `run.c`, `debugreg.c`,
`frames.c` and `breakpoint.c` change too. In all, `src/*.c` grows from 3473 to 4210 lines.
Three new targets exercise the changes: `workers.c` (three threads named `worker-N`, each
adding `100 * id` to a total under a mutex: "total: 600"), `deadlock.c` (the bank deadlock made
certain with a 10 ms sleep between the two locks) and `forker.c` (a parent and its child both
call `greet()`).

### A table of threads

`struct proc` used to describe one thread. Now it holds a table of them:

`project/ch07/src/proc.h` (part)
```c
struct thread {
    pid_t tid;           /* 0: a free slot */
    int num;             /* 1, 2, ... in the order tdb saw them, as in gdb */
    bool running;        /* resumed, and not seen stopping since */
    bool stop_sent;      /* a SIGSTOP from tdb is on its way to it */
    int status;          /* waitpid() status of its last stop */
    int pending;         /* a stop tdb has not reported yet (a status), or 0 */
    int pending_sig;     /* signal to deliver when it resumes, or 0 */
    bool in_syscall;     /* between a syscall-entry stop and its exit stop */
    bool fresh;          /* new, and not stopped yet: give it the debug registers */
};

struct proc {
    ...
    struct thread *cur;  /* the current thread: registers, steps, backtraces */
    struct thread threads[PROC_MAX_THREADS];
    int next_num;        /* the number the next new thread gets */
    ...
```

Everything that used to take `p->pid` for registers or stepping now takes `p->cur->tid`.
Registers are per thread, so the wrong tid gives the wrong thread's `rip`. Memory is shared, so
any stopped thread's tid works for `PEEKDATA`. A signal waiting to be delivered (`pending_sig`)
belongs to one thread too: a SIGSEGV belongs to the thread that faulted.

### Hearing about every thread

Three ptrace options do the work:

`project/ch07/src/proc.c`
```c
/* SYSGOOD: system call stops are SIGTRAP | 0x80.
 * TRACECLONE: trace every new thread too, from its first instruction.
 * TRACEFORK: and the child of a fork(), so that we can clean it up. */
#define TRACE_OPTIONS (PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK)
```

With `PTRACE_O_TRACECLONE`, each `clone()` the program makes produces two things. The thread
that called `clone` stops with a *clone event*: `status >> 8` is `SIGTRAP | PTRACE_EVENT_CLONE
<< 8` (`0x3057f` in the trace below), and `PTRACE_GETEVENTMSG` gives the new tid. The new
thread starts out traced and stopped by a SIGSTOP. The two arrive in either order, and
`handle()` in `proc.c` accepts both. If the clone event comes first, it adds the new tid to the
table as "running, with a SIGSTOP on its way" (`stop_sent` set), and a later SIGSTOP with
`stop_sent` set is swallowed, whether it is tdb's own or a new thread's first. If the new
thread's stop comes first, `handle()` finds an unknown tid in `/proc/PID/task` and adds it
then. Either way the new thread gets the debug registers before it runs: at once, or at that
first SIGSTOP (the `fresh` flag).

- `handle()` returns 1 only for a stop the user should hear about. Thread events are handled
  inside it, and the thread is resumed. `proc_wait` loops until a stop returns 1.
- The wait is `waitpid(-1, &status, __WALL)`: any traced task, threads included. A thread's end
  is a plain exit status for its tid ("[LWP N exited]"). The main thread's exit is reported
  after all the others, so it means the process is gone.

### Stopping everyone

When `proc_wait` returns with one thread stopped, the others are still running. All-stop
means stopping them before tdb reports:

`project/ch07/src/proc.c`
```c
int proc_stop_all(struct proc *p)
{
    for (int i = 0; i < PROC_MAX_THREADS; i++) {
        struct thread *t = &p->threads[i];
        if (t->tid != 0 && t->running && !t->stop_sent) {
            /* tgkill: a signal to one thread (kill() would pick any). */
            syscall(SYS_tgkill, p->pid, t->tid, SIGSTOP);
            t->stop_sent = true;
        }
    }
    /* Wait until none is running. Threads may start or end meanwhile.
     * Wait for any thread, not each in turn: if the program is exiting,
     * the main thread is only reported after all the others. */
    while (p->state == PROC_STOPPED) {
        bool running = false;
        for (int i = 0; i < PROC_MAX_THREADS; i++)
            running |= p->threads[i].tid != 0 && p->threads[i].running;
        if (!running)
            break;
        int status;
        pid_t tid = wait_tid(p, -1, &status);
        if (tid < 0)
            return -1;
        int r = handle(p, tid, status, true);
        if (r < 0)
            return -1;
        if (r == 1)
            proc_thread(p, tid)->pending = status;  /* our SIGSTOP comes after it */
    }
    p->request = 0;
    return 0;
}
```

- `tgkill(pid, tid, sig)` sends a signal to *one* thread. `kill(pid, sig)` sends it to the
  process, and the kernel delivers it to any one thread. Worse, a process-wide SIGSTOP starts a
  *group-stop* of the whole process. "Common mistakes" shows what that does to tdb.
- Then tdb waits until no thread is running. Most of what arrives is tdb's own SIGSTOPs, which
  `handle()` swallows. But a thread can stop for a *real* reason before the SIGSTOP reaches
  it. It may hit a breakpoint, take a fault, or receive a signal. That stop must not be lost.
  It is kept in `pending`, and the SIGSTOP still on its way is swallowed later.

### A stop that arrives while stopping

This is the case you can't plan for. `-v` makes tdb print every `waitpid` result. The status
is printed in hex: `0x057f` is "stopped by signal 5 (SIGTRAP)", `0x137f` is "stopped by signal
19 (SIGSTOP)", `0x3057f` is a SIGTRAP with event 3 (`PTRACE_EVENT_CLONE`), and `0x0000` is
"exited with status 0". Here is one run:

```console
$ printf 'break work\nrun\ninfo threads\ncontinue\ncontinue\ncontinue\n' | build/tdb -v build/targets/workers
Reading symbols from build/targets/workers: 22 symbols, 33 line table rows.
(tdb) break work
Breakpoint 1 at 0x1258: file targets/workers.c, line 14.
(tdb) run
[waitpid: LWP 1313717, status 0x057f]
Started process 1313717: build/targets/workers
[waitpid: LWP 1313717, status 0x3057f]
[New LWP 1313718]
[waitpid: LWP 1313717, status 0x3057f]
[New LWP 1313719]
[waitpid: LWP 1313719, status 0x137f]
[waitpid: LWP 1313718, status 0x137f]
[waitpid: LWP 1313719, status 0x057f]
[waitpid: LWP 1313718, status 0x057f]
[waitpid: LWP 1313717, status 0x3057f]
[New LWP 1313720]
[waitpid: LWP 1313720, status 0x137f]
[Switching to LWP 1313719]
Thread 3 hit Breakpoint 1, work () at targets/workers.c:14
14	    long share = 100 * id;
(tdb) info threads
  Id   Target Id               Frame
  1    LWP 1313717 "workers"   0x00007ffff7d268cd in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
  2    LWP 1313718 "worker-1"  0x0000555555555259 in work () at targets/workers.c:14
* 3    LWP 1313719 "worker-2"  work () at targets/workers.c:14
  4    LWP 1313720 "workers"   0x00007ffff7d268cd in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
(tdb) continue
[Switching to LWP 1313718]
Thread 2 hit Breakpoint 1, work () at targets/workers.c:14
14	    long share = 100 * id;
(tdb) continue
[waitpid: LWP 1313718, status 0x137f]
[waitpid: LWP 1313718, status 0x057f]
[waitpid: LWP 1313719, status 0x057f]
[waitpid: LWP 1313717, status 0x137f]
[waitpid: LWP 1313720, status 0x057f]
[waitpid: LWP 1313717, status 0x137f]
[waitpid: LWP 1313719, status 0x0000]
[LWP 1313719 exited]
[waitpid: LWP 1313718, status 0x0000]
[LWP 1313718 exited]
[Switching to LWP 1313720]
Thread 4 hit Breakpoint 1, work () at targets/workers.c:14
14	    long share = 100 * id;
(tdb) continue
[waitpid: LWP 1313720, status 0x057f]
total: 600
[waitpid: LWP 1313720, status 0x0000]
[LWP 1313720 exited]
[waitpid: LWP 1313717, status 0x0000]
[process exited with status 0]
(tdb)
```

![A timeline of the run above: thread 3 hits the breakpoint; tdb sends SIGSTOP to threads 2 and 1 with tgkill; thread 2 reports its own breakpoint trap before the SIGSTOP and is kept pending; thread 1 reports a clone event and a fourth thread appears; the next continue reports thread 2 without resuming anything](images/ch07-all-stop.png)

Read it with the figure:

1. Thread 1 (1313717) makes two threads, and each reports its first SIGSTOP (`0x137f`). Thread
   3 (1313719) then hits the breakpoint (`0x057f`): the stop tdb will report.
2. `proc_stop_all` sends SIGSTOP to 1313718 and 1313717 with `tgkill`. But 1313718 (thread 2)
   reports `0x057f`, not `0x137f`: it hit the same breakpoint before tdb's SIGSTOP reached it.
   This is a real stop, and it goes into `pending`.
3. 1313717 reports a clone event: it was creating the third worker. Thread 4 (1313720)
   appears and reports its first stop. Now nothing runs, so tdb reports thread 3.
4. `info threads` shows the difference. Thread 3's pc has been moved back onto the breakpoint
   (`work () at ...:14`), as tdb has done after every `int3` since Chapter 2. Thread 2's pc is
   still one byte past it (`0x...5259`): its stop has not been processed yet.
5. `continue` finds the pending stop and reports it, "Thread 2 hit Breakpoint 1". No `waitpid`
   line is printed, because nothing was resumed. (`take_pending` in `run.c` does this: a
   thread with a pending stop becomes the current thread before anything is resumed.)
6. The next `continue` resumes everyone. The old SIGSTOPs for 1313718 and 1313717 finally
   arrive (`0x137f`), and tdb swallows them, since `stop_sent` is still set. Thread 4 hits the
   breakpoint, and the program ends with "total: 600".

**This run is not typical.** In 40 identical runs on this machine, a pending stop appeared in
only 8. In 36 of the 40, the first report was thread 2, not thread 3. All 40 printed "total:
600". A plain run has one breakpoint report per `continue`, each after a `waitpid`, and no
pending stop.

That nondeterminism is itself the lesson. Code for a rare interleaving runs only when the
timing lines up. One test run says almost nothing about it. You cannot single-step into it,
because stepping changes the timing. To see it you need a trace of the raw events (`-v`),
many runs, and an argument from the code about what can arrive in which order.

One more case needs care. If you `delete` the breakpoint between the two reports, the pending
SIGTRAP belongs to an `int3` that is no longer in memory. `run_continue` sees a trap with
`si_code == SI_KERNEL` (what an `int3` gives) and no breakpoint at `rip - 1`, backs `rip` up by
one, and goes on silently.

### Which threads run

`proc_resume_all` resumes every stopped thread without a pending stop, with `PTRACE_CONT` (or
`PTRACE_SYSCALL` when a syscall catchpoint is set). Before that, `step_all_over_breakpoints`
single-steps each thread that sits on an inserted breakpoint over it, as Chapter 2 did for one
thread. The rule tdb follows:

- `continue`, `finish`, and `step`/`next` over a call resume **all** threads.
- `stepi`, and `step`/`next` within a line, resume **only the current thread**.
- A breakpoint that another thread hits during `next` ends the `next` there, as in gdb.

gdb's default lets all threads run even on `stepi`. tdb's choice keeps a single step simple,
at a cost: a `stepi` over a `futex` wait for a lock held by a stopped thread never returns, the
`scheduler-locking on` trap from the `hits` section, built in.

### Debug registers in every thread

Chapter 4's `dr_set` wrote DR0–DR7 for one tid. Now `write_all` in `debugreg.c` writes them
into every stopped thread and keeps a copy in `p->dr[]`. `dr_setup_thread(p, tid)` copies that
into a new thread at its first stop (the `fresh` flag above), before the thread runs a single
instruction of its own. A watchpoint on `total` set in `main`, before any worker exists, then
stops three times, once per worker: "Old value = 0 (0x0)", "New value = 200 (0xc8)" in
thread 3, then 500 and 600 in the others, and "total: 600" (the `threads-watch` itest checks
it). The values step cleanly, unlike gdb's `hits` run, because `workers.c` updates `total`
under a mutex. "Common mistakes" shows the same run without `dr_setup_thread`.

### A forked child

`forker.c` forks. The child calls `greet("child")`. The parent waits for it, then calls
`greet("parent")` at line 27. The child is a copy of the parent, `0xCC` bytes included.
Untraced, it executes the `int3` and dies of SIGTRAP (see "Common mistakes").

With `PTRACE_O_TRACEFORK`, tdb hears of the fork (`PTRACE_EVENT_FORK`, with the child's pid
from `GETEVENTMSG`), and the child starts out traced and stopped. `detach_child` in `run.c`
writes the original bytes back into the *child's* memory (`bp_remove_copy`) and detaches it,
which is gdb's default `detach-on-fork on`. It leaves the breakpoint table alone, because the
parent keeps all its breakpoints. The child's stop can reach `waitpid(-1)` *before* the
parent's fork event. `handle()` then sees an unknown tid that is not in `/proc/PID/task`, so it
is not a thread, and keeps it in `early_children` until the event names it.

```console
$ printf 'break greet\nrun\nbt\ncontinue\n' | build/tdb build/targets/forker
Reading symbols from build/targets/forker: 20 symbols, 19 line table rows.
(tdb) break greet
Breakpoint 1 at 0x11f9: file targets/forker.c, line 11.
(tdb) run
Started process 864041: build/targets/forker
[Detaching after fork from child process 864042]
hello from the child
Breakpoint 1, greet () at targets/forker.c:11
11	    printf("hello from the %s\n", who);
(tdb) bt
#0  greet () at targets/forker.c:11
#1  0x00005555555552c5 in main () at targets/forker.c:27
(tdb) continue
hello from the parent
[process exited with status 0]
(tdb)
```

The child greeted without stopping, and the parent stopped at its own call.

### Backtraces from inside libc

When a thread is stopped inside libc, as all the waiting threads above are, Chapter 5's `%rbp`
chain fails. Ubuntu's glibc is built without frame pointers, so `%rbp` in a libc frame is not
a frame pointer. gdb reads the call frame information in `.eh_frame` for every frame. tdb has
no CFI reader, so v0.7 uses a heuristic, `scan_for_return` in `frames.c`. A return address is
the address right after a `call`, and a direct `call rel32` is 5 bytes starting with `0xe8`.
So tdb takes the first stack word, from `rsp` upwards (at most 1024 words), that points into
one of the program's functions and has `0xe8` five bytes before it. From that frame on, the
program's own frames have frame pointers, and the `%rbp` chain works again, because libc
preserved `%rbp`'s value. A stale return address left lower on the stack could fool the scan,
which is why it is a heuristic and not an answer. In return, it finds `take_both` under a
futex wait, and Chapter 6's `deep` target now shows `inner`. Frames inside a library show the
mapped file from `/proc/PID/maps` (`?? () from .../libc.so.6`): tdb reads no symbols from libc.

### The thread commands and Ctrl-C

Stops now name the thread when there is more than one ("Thread 2 hit Breakpoint 1"). tdb
numbers threads like gdb (1, 2, 3, ..., never reused) and prints "[Switching to LWP N]" when
the thread that stopped is not the current one. `info threads` takes each name from
`/proc/PID/task/TID/comm`, so a worker that has called `pthread_setname_np` shows "worker-1".
Without `libthread_db` there are no `Thread 0x...` names. `thread N` selects a thread, `thread`
alone prints "[Current thread is 1 (LWP N)]", and a bad number gives "Invalid thread ID: 9".
`thread apply all|N... CMD` selects each thread in turn, highest number first, runs `CMD`
through the normal `execute()`, and selects the old current thread again at the end.

Ctrl-C is how you stop a hung program. Until now it killed tdb itself, since SIGINT's default
action is to terminate. Now tdb installs a handler. Not `SIG_IGN`: an ignored signal stays
ignored across `exec`, and the program must still get its Ctrl-C. And no `SA_RESTART`, so a
blocked `waitpid` returns `EINTR`. A program that tdb started shares the terminal's foreground
process group, so the terminal's SIGINT reaches it directly, and its thread stops with SIGINT.
A program tdb *attached to* is in another group, so `wait_tid` forwards the signal with
`kill(pid, SIGINT)`. At the `(tdb)` prompt, `fgets` returns `EINTR` and tdb prints "Quit".

```console
$ build/tdb build/targets/deadlock
Reading symbols from build/targets/deadlock: 24 symbols, 31 line table rows.
(tdb) run
Started process 868888: build/targets/deadlock
deadlock: pid 868888
[New LWP 868889]
[New LWP 868890]
^CThread 1 received signal SIGINT (Interrupt).
0x00007ffff7c910d7 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
(tdb) info threads
  Id   Target Id               Frame
* 1    LWP 868888 "deadlock"   0x00007ffff7c910d7 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
  2    LWP 868889 "deadlock"   0x00007ffff7c91280 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
  3    LWP 868890 "deadlock"   0x00007ffff7c91280 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
(tdb) thread apply all bt

Thread 3 (LWP 868890 "deadlock"):
#0  0x00007ffff7c91280 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
#1  0x00005555555552e5 in take_both () at targets/deadlock.c:24
#2  0x000055555555536d in two () at targets/deadlock.c:39
#3  0x00007ffff7c94a83 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6

Thread 2 (LWP 868889 "deadlock"):
#0  0x00007ffff7c91280 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
#1  0x00005555555552e5 in take_both () at targets/deadlock.c:24
#2  0x000055555555533d in one () at targets/deadlock.c:32
#3  0x00007ffff7c94a83 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6

Thread 1 (LWP 868888 "deadlock"):
#0  0x00007ffff7c910d7 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
#1  0x000055555555542a in main () at targets/deadlock.c:52
(tdb) kill
[process killed]
```

The same deadlock as `bank`, found with a debugger you wrote. Both workers are in `take_both`
at line 24, the second lock, called from `one()` and `two()`, which take the locks in opposite
orders.

`attach PID` (or `tdb -p PID`) now attaches every thread: the main thread with
`PTRACE_ATTACH`, then each task in `/proc/PID/task`, reading the directory again until no new
tids appear, since a thread may be created meanwhile. `detach` lets them all go, and the
process stays hung, ready for the next tool. `deadlock` calls `prctl(PR_SET_PTRACER,
PR_SET_PTRACER_ANY)` so that tdb can attach to it at all: Ubuntu's Yama `ptrace_scope` setting
of 1 lets a process be attached only by its ancestors.

There is one wart. Press Ctrl-C at the prompt, then `continue`:

```console
(tdb) ^CQuit
(tdb) continue
Thread 1 received signal SIGINT (Interrupt).
0x00007ffff7c910d7 in ?? () from /usr/lib/x86_64-linux-gnu/libc.so.6
(tdb) ^CQuit
```

The program got that Ctrl-C too: it shares tdb's process group, and the stopped thread held
the SIGINT until it ran again. gdb gives the program the terminal while it runs and takes it
back at the prompt. Exercise 4 does the same for tdb.

### Tests and limits

`make test` in `project/ch07` ends with "unit: 22 passed, 0 failed" and "itest: 58 passed, 0
failed". The unit tests are unchanged. The new integration tests are `threads` (the thread
commands and their errors), `threads-watch` (the watchpoint above), `fork` (no "killed by
signal"), `ctrl-c` (it sends `kill -INT` to both tdb and the program, as a terminal does to its
foreground group) and `attach-threads` (after `detach`, three tasks remain). `bt-crash` now
expects `inner`. The README lists the limits: at most 64 threads; children of `vfork` (and so
of `posix_spawn`) are not traced, so a breakpoint they hit before `exec` kills them; and a
`stepi` over a system call that blocks on a stopped thread hangs.

## Common mistakes

**Leaving the breakpoints in a forked child.** Here is the Chapter 6 tdb on `forker`:

```console
$ printf 'break greet\nrun\nbt\ncontinue\n' | ../ch06/build/tdb build/targets/forker
...
(tdb) run
Started process 864085: build/targets/forker
child killed by signal 5 (Trace/breakpoint trap)
Breakpoint 1, greet () at targets/forker.c:11
```

The child ran the `int3`, untraced, and the default action of SIGTRAP killed it.

**Setting debug registers only in the threads that exist now.** A scratch build of v0.7
without the `dr_setup_thread` calls runs `workers` to the end ("total: 600") after `watch
total` in `main`, with no stop and no error. Thread 1 never writes `total`. A silent miss is
worse than a crash.

**Stopping the other threads with `kill` instead of `tgkill`.** A scratch build whose
`proc_stop_all` used `kill(p->pid, SIGSTOP)`:

```console
(tdb) continue
Thread 3 received signal SIGSTOP (Stopped (signal)).
tdb: ptrace(GETSIGINFO): Invalid argument
work () at targets/workers.c:14
14	    long share = 100 * id;
```

A process-directed SIGSTOP starts a *group-stop*: every thread reports a stop tdb did not ask
for, and the program never finishes. `GETSIGINFO` fails with `EINVAL` because a group-stop has
no siginfo, which ptrace(2) documents as the way to tell the two kinds of stop apart. It was
intermittent too: one run in five that time, three in three earlier.

**Bugs in this chapter's programs.** Taking two locks in opposite orders (`bank`): pick one
global order. A read-modify-write on shared data without a lock or an atomic (`hits++`): one
line of C is not one step for the CPU. Not checking `fopen`, and forking with unflushed stdio
(`linecount`): the second duplicates output only when stdout is not a terminal, which is to
say in production and in tests.

## In the wild

**Mars Pathfinder, 1997.** Pathfinder landed on 4 July 1997. A few days later, after it began
gathering weather data, the spacecraft began to reset itself, losing the rest of each day's
work. Mike Jones's widely circulated summary of a talk by David Wilner of Wind River (whose
VxWorks ran the spacecraft) gave the cause: *priority inversion*. A low-priority
meteorological task held a mutex, a high-priority bus task blocked on it, and medium-priority
tasks kept the holder from running. A watchdog saw that the bus task had missed its deadline
and reset the computer. Glenn Reeves, who led the flight software team, replied with details.
The mutex protected the wait list inside VxWorks's `select()`, used by a pipe between the
`bc_dist` task and the ASI/MET weather task. The team reproduced the reset on a lab replica in
under 18 hours, using a trace facility the flight software kept by design, and fixed it by
enabling priority inheritance on that semaphore through a global configuration variable.
Jones's account says the change was made through the on-board C interpreter. Reeves corrects
this: it was uploaded with the spacecraft's normal patch process. It is a deadlock's cousin:
no cycle, just a waiting order that the scheduler made fatal, found the way this chapter found
the bank deadlock, by looking at who held what while waiting.

**NPTL.** Every Linux thread is a kernel task that a debugger can `ptrace` because of how
glibc builds threads. Ulrich Drepper and Ingo Molnar's paper "The Native POSIX Thread Library
for Linux" sets out the design that replaced LinuxThreads: a 1-on-1 model (one kernel task per
user thread) rather than M-on-N, plus the kernel improvements it needed. That is why `info
threads` is a list of LWPs, `__owner` is a tid, and tdb can stop each thread with `tgkill`.

**ThreadSanitizer.** Konstantin Serebryany and Timur Iskhodzhanov described ThreadSanitizer at
WBIA '09 (December 2009). It was a race detector built on Valgrind, used on Google's code. The
`-fsanitize=thread` in GCC and Clang today is its compiler-based successor. The compiler
instruments every memory access, and a runtime library keeps the "happens-before" state that
produced the reports above.

The gdb manual's "Debugging Programs with Multiple Threads", "All-Stop Mode" and "Debugging
Forks" pages cover the commands of this chapter. The ptrace(2) and tgkill(2) man pages cover
`PTRACE_O_TRACECLONE`, the initial SIGSTOP of a traced thread, group-stops, and `__WALL`.

## Summary

- On Linux a thread is a task (LWP) with its own tid, registers and stack, sharing memory with
  the others. gdb shows its own thread number, the `pthread_t` (via `libthread_db`) and the LWP.
- For a hang: Ctrl-C, `info threads`, `thread apply all bt`, then `thread N`, `frame N` and
  `print` on the mutexes (`__data.__owner` is the holder's LWP).
- gdb is all-stop by default. `set scheduler-locking on` runs one thread alone, so you can make
  a race happen on purpose. Watchpoints must be in every thread, including later ones.
- After a fork gdb follows the parent and detaches the child, removing its breakpoints first.
  `follow-fork-mode`, `detach-on-fork`, `info inferiors` and `catch fork` give you the child.
  Flush stdio before forking.
- ThreadSanitizer (`-fsanitize=thread`, with `setarch -R` here) reports races and lock-order
  inversions from one run.
- tdb v0.7: `TRACECLONE`/`TRACEFORK`, `waitpid(-1, __WALL)`, a thread table, stop-all with
  `tgkill`, pending stops, debug registers in new threads, clean fork children, a stack scan
  through libc, the thread commands, Ctrl-C and multi-thread attach.
- Rare interleavings show up only in some runs (the pending stop: 8 of 40). Debug them with raw
  event traces and reasoning, not one lucky run.

## Exercises

1. Fix `bank.c` so it always finishes. Every thread must take the two locks in the same order,
   lower `id` first. Check that the total is still 2000 and that TSan stays silent.

<details><summary>Solution</summary>

```c
void transfer(struct account *from, struct account *to, long amount)
{
    /* Every thread takes the two locks in the same order: lower id first. */
    struct account *first = from->id < to->id ? from : to;
    struct account *second = from->id < to->id ? to : from;

    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);
    from->balance -= amount;
    to->balance += amount;
    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
}
```

```console
$ gcc -g -O0 -pthread -fsanitize=thread bank-fixed.c -o bank-fixed-tsan
$ timeout 10 setarch -R ./bank-fixed-tsan; echo "[exit $?]"
alice: 1000, bob: 1000, total: 2000
[exit 0]
```

Both tellers take alice's lock (id 1) first, so neither can hold bob's while waiting for
alice's. Ordering by `id` rather than by address is stable across runs and easy to document.

</details>

2. Fix `hits.c` two ways. First, make `hits` an `_Atomic long` and find the instruction that
   makes it work with `disassemble /s count`. Second, keep a count local to each thread and
   add it to `hits` once, under a mutex. Compare the run times.

<details><summary>Solution</summary>

The atomic version adds `#include <stdatomic.h>` and declares `_Atomic long hits;`:

```console
$ ./hits-atomic
hits: 4000000 (expected 4000000)
$ gdb -q -batch -ex 'disassemble /s count' ./hits-atomic
...
19	        hits++;
   0x00000000000011d1 <+40>:	movl   $0x1,-0x18(%rbp)
   0x00000000000011d8 <+47>:	mov    -0x18(%rbp),%eax
   0x00000000000011db <+50>:	cltq   
   0x00000000000011dd <+52>:	lock xadd %rax,0x2e32(%rip)        # 0x4018 <hits>
   0x00000000000011e6 <+61>:	mov    %rax,-0x10(%rbp)
```

`++` on an `_Atomic` object is a fetch-and-add. `lock xadd` adds `rax` to memory and returns
the old value as one indivisible step: no other core can touch that cache line in between.

The local version's `count`, with a global `pthread_mutex_t lock =
PTHREAD_MUTEX_INITIALIZER;`:

```c
    long mine = 0;              /* this thread's own count: no sharing */

    for (int i = 0; i < PER_THREAD; i++)
        mine++;
    pthread_mutex_lock(&lock);  /* one shared update per thread */
    hits += mine;
    pthread_mutex_unlock(&lock);
```

It also prints "hits: 4000000". On this machine (timings vary) the broken version takes about
0.01 s, the atomic one 0.05–0.07 s, because four cores fight over one cache line, and the local
one under 0.01 s. When you can, arrange the work so threads don't share at all.

</details>

3. Fix `linecount.c`. A missing file should print an error and count −1, not crash a worker,
   and the header should appear once in `out.txt`.

<details><summary>Solution</summary>

```c
    FILE *f = fopen(path, "r");
    ...
    if (f == NULL) {
        perror(path);
        return -1;
    }
```

and in `main`, after the header's `printf`, `fflush(stdout);`.

```console
$ ./linecount-fixed bank.c nosuch.txt hits.c > out.txt; echo "[exit $?]"
nosuch.txt: No such file or directory
[exit 0]
$ cat out.txt
counting lines in 3 files
      56 bank.c
      -1 nosuch.txt
      32 hits.c
```

`perror` writes to stderr, which is unbuffered and still the terminal. The order of the count
lines depends on which child finishes first.

</details>

4. tdb: stop the program from getting the Ctrl-C you type at the `(tdb)` prompt. Do what gdb
   does: put the program in a process group of its own, give that group the terminal
   (`tcsetpgrp`) while it runs, and take the terminal back before tdb reports a stop.

<details><summary>Solution</summary>

In `proc_launch` (`src/proc.c`), the child calls `setpgid(0, 0)` and `signal(SIGTTOU,
SIG_DFL)` before `PTRACE_TRACEME`, and the parent calls `setpgid(pid, pid)` too, because either
may run first. The child restores SIGTTOU's default because tdb will ignore it, and an ignored
signal stays ignored across `exec`. In `src/tdb.c`:In `src/tdb.c`:

```c
/* Who gets Ctrl-C (and may read the terminal): the program while it
 * runs, tdb at the prompt. Calling tcsetpgrp() from a background group
 * raises SIGTTOU, which main() ignores. */
static void terminal_inferior(void)
{
    if (isatty(STDIN_FILENO) && !proc.attached)
        tcsetpgrp(STDIN_FILENO, proc.pid);
}

static void terminal_ours(void)
{
    if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, getpgrp());
}
```

`resuming()` calls `terminal_inferior()` first, `report()` calls `terminal_ours()` first, and
`main` adds `signal(SIGTTOU, SIG_IGN);` after `proc_catch_ctrl_c()`.

In the `deadlock` session, the Ctrl-C at the prompt now prints only "Quit", and `continue`
runs until the *next* Ctrl-C, which the terminal sends to the program's group and tdb sees as a
ptrace stop. `make test` still passes (58 itests). Its
`ctrl-c` test has no terminal, where both functions do nothing.

</details>

Sources for this chapter: [REFERENCES.md](REFERENCES.md#chapter-7--threads-and-processes)
