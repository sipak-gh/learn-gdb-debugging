# Chapter 10 — Remote Debugging

Until now gdb and the program ran on the same machine, and gdb made the ptrace calls itself.
Programs often crash somewhere else: on a board, in a container, on a server that has no
source and no debug info. For that, gdb splits in two. **gdbserver** runs next to the program
and makes the ptrace calls. **gdb** runs on your machine, reads the symbols and the sources, and
talks to gdbserver over a socket or a pipe in the **GDB Remote Serial Protocol** (RSP). This
chapter covers the gdb side: `gdbserver` and `target remote`, `set sysroot`, separate debug
files and build IDs, attaching to a running service, `--multi`, stdio, `monitor`, and `set
debug remote 1` to watch the packets. Then tdb v1.0 becomes a gdbserver: real gdb 12.1 drives
it with `target remote`, and every packet is answered by a function tdb already has. A
debugger *is* that small set of operations, and the protocol is the list of them. The chapter
ends the course with a look at tdb 1.0 next to gdb.

Previous: [Chapter 9](09-time-travel.md). Next: [Appendix A](appendix-a-cheatsheet.md).

Everything in this chapter runs on **one machine**, over 127.0.0.1. "The device" is the
directory `examples/ch10/device/`, with the stripped binary and the device's data; the host side
has the debug info. Between two real machines the commands are the same (`gdbserver :2345 ...`
there, `target remote HOST:2345` here), but that was not tested for this chapter. Install
gdbserver with `sudo apt install gdbserver`, and keep `libc6-dbg` installed.

## A crash on the device

`examples/ch10/meter.c`, after its header comment (which repeats the bug report) and includes:
```c
#define MAXREADINGS 1000

struct reading {
    long time;      /* seconds since midnight */
    long wh;        /* the meter's counter, in watt-hours */
};

static struct reading readings[MAXREADINGS];

/* Read the log into readings[]. Returns how many there are. */
static int load(const char *path)
{
    ...
    while (n < MAXREADINGS &&
           fscanf(f, "%ld %ld", &readings[n].time, &readings[n].wh) == 2)
        n++;
    ...
}

/* The average power between two readings, in watts. */
static long watts(const struct reading *a, const struct reading *b)
{
    long wh = b->wh - a->wh;
    long seconds = b->time - a->time;

    return wh * 3600 / seconds;
}

int main(int argc, char *argv[])
{
    long peak = 0;
    int n;

    ...
    n = load(argv[1]);
    for (int i = 1; i < n; i++) {
        long w = watts(&readings[i - 1], &readings[i]);

        printf("%5ld-%5ld s  %6ld W\n", readings[i - 1].time, readings[i].time, w);
        if (w > peak)
            peak = w;
    }
    printf("%d readings, peak %ld W\n", n, peak);
    return 0;
}
```

`examples/ch10/Makefile` builds `meter` and `poller` with `-std=c11 -Wall -Wextra -g -O2`, like
a release build that keeps its debug info, and then the device's copy of meter (the next
sections explain each step). Here, with the sample log, meter works. On the device, with the
device's own log, it dies:

```console
$ cd examples/ch10
$ make
cc -std=c11 -Wall -Wextra -g -O2    meter.c   -o meter
cc -std=c11 -Wall -Wextra -g -O2    poller.c   -o poller
objcopy --only-keep-debug meter meter.debug
objcopy --strip-all --add-gnu-debuglink=meter.debug meter device/meter
id=$(readelf -n device/meter | sed -n 's/.*Build ID: //p'); \
mkdir -p debug/.build-id/$(echo $id | cut -c1-2) && \
cp meter.debug debug/.build-id/$(echo $id | cut -c1-2)/$(echo $id | cut -c3-).debug
$ ./meter sample.log
28800-29700 s    1000 W
29700-30600 s    1360 W
30600-31500 s     880 W
31500-32400 s    1280 W
5 readings, peak 1360 W
$ cd device
$ ./meter meter.log
28800-29700 s     720 W
29700-30600 s     920 W
Floating point exception (core dumped)
$ echo $?
136
$ ls -l meter ../meter
-rwxr-xr-x 1 tbangma tbangma 21760 Nov 19 00:34 ../meter
-rwxr-xr-x 1 tbangma tbangma 14576 Nov 19 00:34 meter
```

- Exit status 136 is 128 + 8: killed by signal 8, `SIGFPE`. Whether you see "(core dumped)"
  depends on your system's core settings (Chapter 5).
- The name is historical: on x86, integer division by zero makes `idiv` trap, and the kernel
  reports that trap as `SIGFPE`, although no floating point is involved.
- `device/meter` is the stripped copy: 14576 bytes against 21760, no symbols and no DWARF. On a
  real device you would not have gdb or the source either. How do you debug a crash where it
  happens, with everything you need kept somewhere else?

## gdbserver: a debugger in two halves

Start the program under gdbserver on the device. It forks, execs and stops the program, then
waits for gdb on a TCP port:

```console
$ gdbserver :2345 ./meter meter.log
Process ./meter created; pid = 1296795
Listening on port 2345
```

In another terminal, on "your machine", give gdb the same stripped file first and connect:

```console
$ gdb -q device/meter
Reading symbols from device/meter...
(No debugging symbols found in device/meter)
(gdb) target remote :2345
Remote debugging using :2345
Reading /lib64/ld-linux-x86-64.so.2 from remote target...
warning: File transfers from remote targets can be slow. Use "set sysroot" to access files locally instead.
...
0x00007ffff7fe3290 in _start () from target:/lib64/ld-linux-x86-64.so.2
(gdb) continue
Continuing.
Reading /lib/x86_64-linux-gnu/libc.so.6 from remote target...

Program received signal SIGFPE, Arithmetic exception.
0x00005555555551ec in ?? ()
(gdb) bt
#0  0x00005555555551ec in ?? ()
#1  0x00007ffff7c29d90 in __libc_start_call_main (main=main@entry=0x555555555120, 
    argc=argc@entry=2, argv=argv@entry=0x7fffffffe6a8) at ../sysdeps/nptl/libc_start_call_main.h:58
...
(gdb) x/i $pc
=> 0x5555555551ec:	idiv   %rsi
(gdb) print $rsi
$1 = 0
```

Meanwhile, the device terminal shows `Remote debugging from host 127.0.0.1, port 60082` and the
program's first two lines of output.

- After `target remote`, the program is stopped at ld.so's `_start`, before its first
  instruction, as with `starti`, so that gdb can set breakpoints before anything runs.
- gdb reads the program's libraries through the connection ("from remote target"; the
  `target:` prefix). That is slow over a real link, hence the warning.
- Without symbols we learn little: a division (`idiv %rsi`) by zero (`$rsi` is 0) somewhere.
  The libc frames have names (with `libc6-dbg` installed), and one gives away `main`'s address
  (`main=0x555555555120`), but meter's own frames are `??`.

Now the same crash with the host's `./meter`: the same build before it was stripped (the next
section shows how gdb checks that). `set sysroot /` tells gdb to read the libraries from its own
disk, right here because host and "device" are the same system. With a real board you would
point it at a copy of the board's root file system.

```console
$ gdb -q ./meter
Reading symbols from ./meter...
(gdb) set sysroot /
(gdb) target remote :2345
Remote debugging using :2345
Reading symbols from /lib64/ld-linux-x86-64.so.2...
Reading symbols from /usr/lib/debug/.build-id/e6/d3f5d4f2a24a5464ab59ae321b9474d70410f8.debug...
0x00007ffff7fe3290 in _start () from /lib64/ld-linux-x86-64.so.2
(gdb) continue
Continuing.

Program received signal SIGFPE, Arithmetic exception.
0x00005555555551ec in watts (b=0x555555558070 <readings+48>, a=0x555555558060 <readings+32>) at meter.c:48
48	    return wh * 3600 / seconds;
(gdb) bt
#0  0x00005555555551ec in watts (b=0x555555558070 <readings+48>, a=0x555555558060 <readings+32>)
    at meter.c:48
#1  main (argc=<optimized out>, argv=<optimized out>) at meter.c:62
(gdb) info locals
wh = 0
seconds = 0
wh = <optimized out>
seconds = <optimized out>
(gdb) up
#1  main (argc=<optimized out>, argv=<optimized out>) at meter.c:62
62	        long w = watts(&readings[i - 1], &readings[i]);
(gdb) info locals
w = <optimized out>
i = 3
peak = 920
n = <optimized out>
(gdb) print readings[2]@3
$1 = {{time = 30600, wh = 61610}, {time = 30600, wh = 61610}, {time = 31500, wh = 61900}}
(gdb) quit
```

- Frame #1 has no address: at `-O2`, `watts` is **inlined** into `main`. There is no call, but
  the DWARF still describes the inlined body, and gdb shows it as a frame. The two pairs of
  `wh` and `seconds` in `info locals` come from the optimizer too; the first pair is the one
  at this pc.
- `readings[2]` and `readings[3]` have the same time: the meter logged the reading `30600
  61610` twice. `seconds` is 0. The arithmetic is fine; the data is not. Exercise 1 fixes it.

![The remote setup: gdb on your machine with the symbols and the debug file, gdbserver on the
device with the stripped meter, and the first packets between them](images/ch10-remote-setup.png)

The figure shows the split. gdbserver does what must run next to the program: fork, `ptrace`,
`waitpid`, memory and registers. gdb does everything that needs symbols: DWARF, expressions,
source lines, the command line. Between them go small text packets, here real ones from
gdbserver (see [Watching the packets](#watching-the-packets)). `$?#3f` asks why the program is
stopped; `$Z0,5555555551c8,1#bc` puts a breakpoint at `watts`; `$g#67` asks for the registers.
To resume, gdb sends `$vCont;c:p13fcb5.-1#a2`, "continue every thread of process 0x13fcb5",
because gdbserver says it supports `vCont`. tdb, later, does not, and there gdb selects a thread
with `Hc` and sends a plain `c`.

**Security.** Whoever connects to gdbserver can do anything the program can: write its memory,
set its `rip`, make it run any code. And `gdbserver :2345` listens on **all** interfaces:

```console
$ ss -ltn | grep 2346
LISTEN 0      1             0.0.0.0:2346      0.0.0.0:*          
```

(That was `gdbserver :2346 ./meter meter.log`, on my machine.) Writing a host does not help:
`gdbserver localhost:2347 ./meter meter.log` also showed `0.0.0.0:2347` in `ss`, with gdbserver
12.1. So use it only on a network you trust, behind a firewall, or with no port at all: the
stdio form in [gdbserver --multi, stdio, and monitor](#gdbserver---multi-stdio-and-monitor)
can run it over ssh (not tested here). tdb's server, below, binds 127.0.0.1 only.

## Debug info that is not on the device

The Makefile builds the device's binary in two `objcopy` steps:

- `objcopy --only-keep-debug meter meter.debug` copies the debug info alone (10392 bytes:
  DWARF, the symbol table, no code).
- `objcopy --strip-all --add-gnu-debuglink=meter.debug meter device/meter` removes everything
  but what is needed to run, and adds a `.gnu_debuglink` section: the name `meter.debug` and a
  CRC of that file.

The second link between the two files is the **build ID**, a hash the linker writes into a
note when it builds the program. Stripping does not change it:

```console
$ readelf -n device/meter | grep 'Build ID'
    Build ID: 7a476cf81ed6c4047acc1da4357a5c17ccd4947d
$ readelf --string-dump=.gnu_debuglink device/meter

String dump of section '.gnu_debuglink':
  [     0]  meter.debug
  [     e]  "�

$ find debug -type f
debug/.build-id/7a/476cf81ed6c4047acc1da4357a5c17ccd4947d.debug
```

(The second string is the CRC's bytes, printed as text.) For each directory in
`debug-file-directory`, gdb looks for `DIR/.build-id/7a/476cf8....debug`: the first two hex
digits as a directory, the rest as the file name. The default is `/usr/lib/debug`, where Ubuntu's
`-dbg` and `-dbgsym` packages install: every transcript since Chapter 1 that read ld.so's symbols
from `/usr/lib/debug/.build-id/e6/d3f5....debug` found them this way. The Makefile's `debug`
target builds the same tree for meter. The build is deterministic: the same compiler, flags and
path give the same build ID.

In `gdb -q` with `set sysroot /`, point gdb at `./debug` with `set debug-file-directory ./debug`
and load the stripped file with `file device/meter`: meter's symbols come from
`debug/.build-id/7a/476cf81ed6c4047acc1da4357a5c17ccd4947d.debug`. But the libraries lose theirs:
ld.so's `_start` becomes `0x00007ffff7fe3290 in ?? ()`, and `info sharedlibrary` marks ld.so and
libc "(*): Shared library is missing debugging information." The setting **replaced**
`/usr/lib/debug`. It is a list, separated by colons; append instead:

```console
(gdb) set debug-file-directory ./debug:/usr/lib/debug
(gdb) file device/meter
Reading symbols from device/meter...
Reading symbols from /home/tbangma/learn-gdb-debugging/examples/ch10/debug/.build-id/7a/476cf81ed6c4047acc1da4357a5c17ccd4947d.debug...
(gdb) target remote :2345
...
0x00007ffff7fe3290 in _start () from /lib64/ld-linux-x86-64.so.2
(gdb) continue
...
0x00005555555551ec in watts (b=0x555555558070 <readings+48>, a=0x555555558060 <readings+32>) at meter.c:48
48	    return wh * 3600 / seconds;
(gdb) print *a
$1 = {time = 30600, wh = 61610}
(gdb) print *b
$2 = {time = 30600, wh = 61610}
```

(`...` marks lines cut here: the same as in the earlier sessions.)

Symbols must come from **the same build**. To show what happens otherwise, the host's `./meter`
was rebuilt with exercise 1's fix, which gives it another build ID, and gdb was run against the
old program on the device:

```console
$ gdb -q ./meter
Reading symbols from ./meter...
(gdb) set sysroot /
(gdb) target remote :2345
Remote debugging using :2345
warning: Build ID mismatch between current exec-file /home/tbangma/learn-gdb-debugging/examples/ch10/meter
and automatically determined exec-file /home/tbangma/learn-gdb-debugging/examples/ch10/device/meter
exec-file-mismatch handling is currently "ask"
Load new symbol table from "/home/tbangma/learn-gdb-debugging/examples/ch10/device/meter"? (y or n)  n
warning: loading /home/tbangma/learn-gdb-debugging/examples/ch10/device/meter Not confirmed.
...
Program received signal SIGFPE, Arithmetic exception.
0x00005555555551ec in ?? ()
(gdb) bt
#0  0x00005555555551ec in ?? ()
#1  0x0000000000000002 in ?? ()
#2  0x0000000000000000 in ?? ()
(gdb) list
...
43	static long watts(const struct reading *a, const struct reading *b)
44	{
45	    long wh = b->wh - a->wh;
46	    long seconds = b->time - a->time;
```

- gdb read the program's path on the device through the connection, compared the build IDs,
  and asked. `set exec-file-mismatch ask|warn|off` controls that check in gdb 12.1.
- With `n`, gdb keeps the wrong symbols. The backtrace is garbage, because the unwind info
  describes another program, and `list` shows plausible source unrelated to the pc: worse than
  `??`, because it looks right. With `y`, gdb loads the stripped `device/meter`: `??` again,
  unless its debug file can be found. Keep the debug files of every build you ship.

## Attaching to a running service

`examples/ch10/poller.c`, after its header comment and includes:
```c
/* Read "wait-for FILE" from the config into flag. */
static void read_config(const char *path, char *flag, size_t size)
{
    FILE *f = fopen(path, "r");
    char line[256];

    if (f == NULL) {
        perror(path);
        exit(1);
    }
    while (fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, "wait-for ", 9) == 0)
            snprintf(flag, size, "%s", line + 9);
    fclose(f);
}

static void wait_for(const char *flag)
{
    for (int tries = 1; access(flag, F_OK) != 0; tries++) {
        if (tries % 5 == 0)
            printf("still waiting for %s\n", flag);
        sleep(1);
    }
}

int main(int argc, char *argv[])
{
    char flag[256] = "";

    ...
    read_config(argv[1], flag, sizeof flag);
    /* Let any process of ours attach a debugger (see ptrace_scope). */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("poller: pid %d\n", (int)getpid());
    wait_for(flag);
    printf("starting work\n");
    return 0;
}
```

`device/poller.conf` holds two lines, `# poller config` and `wait-for ready.flag`. Start the
service in `device/`, `touch ready.flag`, and it goes on waiting. Look closely at its output:
each "still waiting" line is followed by an **empty line**. The clue is already on the screen.

A service is already running; you cannot start it under gdbserver. `gdbserver --attach` takes
it over, like `gdb -p` in Chapter 5, and the same rule applies. With Yama's `ptrace_scope` at
1, a process may only trace its own descendants:

```console
$ cat /proc/sys/kernel/yama/ptrace_scope
1
$ sleep 30 &
[1] 1297268
$ gdbserver --attach :2345 1297268
Cannot attach to process 1297268: Operation not permitted (1)
Exiting
$ echo $?
1
```

poller allows it itself: its `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` lets any process of the
same user attach. A real service would not. On a real device you would run gdbserver as root (or
with `CAP_SYS_PTRACE`), or set `ptrace_scope` to 0; none of that was done here. With poller
running (its pid was 1297361):

```console
$ gdbserver --attach :2345 1297361
Attached; pid = 1297361
Listening on port 2345
```

```console
$ gdb -q ./poller
Reading symbols from ./poller...
(gdb) set sysroot /
(gdb) target remote :2345
Remote debugging using :2345
...
0x00007a4ab34e574a in __GI___clock_nanosleep (clock_id=clock_id@entry=0, flags=flags@entry=0, 
    req=req@entry=0x7ffc59a84a60, rem=rem@entry=0x7ffc59a84a60)
    at ../sysdeps/unix/sysv/linux/clock_nanosleep.c:78
78	../sysdeps/unix/sysv/linux/clock_nanosleep.c: No such file or directory.
(gdb) bt
#0  0x00007a4ab34e574a in __GI___clock_nanosleep (clock_id=clock_id@entry=0, flags=flags@entry=0, 
...
#3  0x00005f26e172338d in wait_for (flag=0x7ffc59a84ab0 "ready.flag\n") at poller.c:39
#4  main (argc=<optimized out>, argv=<optimized out>) at poller.c:56
(gdb) frame 3
#3  0x00005f26e172338d in wait_for (flag=0x7ffc59a84ab0 "ready.flag\n") at poller.c:39
39	        sleep(1);
(gdb) print flag
$1 = 0x7ffc59a84ab0 "ready.flag\n"
(gdb) print tries
$2 = 7
(gdb) detach
Detaching from program: /home/tbangma/learn-gdb-debugging/examples/ch10/poller, process 1297361
Ending remote debugging.
[Inferior 1 (process 1297361) detached]
(gdb) quit
```

The poller's terminal:

```console
$ ../poller poller.conf
poller: pid 1297361
still waiting for ready.flag

still waiting for ready.flag
```

- The service was stopped inside `sleep`, where it spends its time. The libc frames have line
  numbers (with `libc6-dbg`), but "No such file or directory": the package has no source.
- `flag` is `"ready.flag\n"`: `fgets` keeps the newline, and `read_config` copies it. The service
  waits for a file whose name ends in a newline. Exercise 2 fixes it.
- `detach` gives the process back, still running. (The addresses are random: no gdb, so ASLR.)

## gdbserver --multi, stdio, and monitor

Plain gdbserver serves one program and exits when the program ends. With `--multi` it starts
with no program, and gdb's `target extended-remote` lets you `run` programs on the device, one
after another:

```console
$ gdbserver --multi :2345
Listening on port 2345
```

```console
$ gdb -q ./meter
Reading symbols from ./meter...
(gdb) set sysroot /
(gdb) target extended-remote :2345
Remote debugging using :2345
(gdb) set remote exec-file /home/tbangma/learn-gdb-debugging/examples/ch10/device/meter
(gdb) run meter.log
Starting program: /home/tbangma/learn-gdb-debugging/examples/ch10/meter meter.log

Program received signal SIGFPE, Arithmetic exception.
0x00005555555551ec in watts (b=0x555555558070 <readings+48>, a=0x555555558060 <readings+32>) at meter.c:48
48	    return wh * 3600 / seconds;
(gdb) run meter.log
The program being debugged has been started already.
Start it from the beginning? (y or n) y
Starting program: /home/tbangma/learn-gdb-debugging/examples/ch10/meter meter.log
...
(gdb) info inferiors
  Num  Description       Connection                Executable        
* 1    process 1297865   1 (extended-remote :2345) /home/tbangma/learn-gdb-debugging/examples/ch10/meter 
(gdb) monitor help
The following monitor commands are supported:
  set debug <0|1>
    Enable general debugging messages
...
  exit
    Quit GDBserver
(gdb) kill
Kill the program being debugged? (y or n) y
[Inferior 1 (process 1297865) killed]
(gdb) info inferiors
  Num  Description       Connection                Executable        
* 1    <null>            1 (extended-remote :2345) /home/tbangma/learn-gdb-debugging/examples/ch10/meter 
(gdb) monitor exit
(gdb) quit
Remote connection closed
(gdb) quit
```

gdbserver's terminal shows `Process /home/tbangma/learn-gdb-debugging/examples/ch10/device/meter
created; pid = 1297864` and the program's two lines, then the same again with pid 1297865.

- `set remote exec-file` is the path **on the device**. `Starting program:` prints the host
  file's path, but gdbserver shows what really ran: `device/meter`, twice (cut here).
- `monitor CMD` sends a command to the server itself, not to the program; `monitor help` lists
  gdbserver's. `monitor exit` stops the server. The gdb manual says to follow it with
  `disconnect`; with `quit` instead, gdb 12.1 first finds the connection gone ("Remote
  connection closed") and needs a second `quit`.

gdb can also start gdbserver itself and talk to it over a pipe, `-` as the port meaning stdin
and stdout. `target remote | gdbserver - device/meter device/meter.log` printed `stdin/stdout
redirected`, `Process device/meter created; pid = 1297924` and `Remote debugging using stdio`,
and `continue` stopped with `SIGFPE` in `watts` at meter.c:48, as before. No port and no second
terminal. The usual use is `target remote | ssh device gdbserver - ./meter`, which tunnels the
protocol through ssh; that was not tested here. But the two lines of output never appeared. The
program's stdout is gdbserver's stdout, the pipe to gdb. A pipe is not a terminal, so stdio
buffers it fully, and the crash throws the buffer away. (With `sample.log`, a clean exit, all
the lines arrived at the end.) Use a port when you need the program's output.

## Watching the packets

`set debug remote 1` prints every packet. Here is a batch session against `gdbserver :2345
./meter meter.log`, with a breakpoint at `watts` (an excerpt of 395 lines; `...` marks cuts,
and long packets are cut at the right edge):

```console
$ gdb -q -nx -batch -ex 'set sysroot /' -ex 'break watts' -ex 'set debug remote 1' -ex 'target remote :2345' -ex continue -ex kill ./meter
Breakpoint 1 at 0x11c8: file meter.c, line 46.
[remote] start_remote_1: enter
  [remote] Sending packet: $qSupported:multiprocess+;swbreak+;hwbreak+;qRelocInsn+;fork-events+;vfork-events+;exec-events+;vContSupported+;...
  [remote] Received Ack
  [remote] Packet received: PacketSize=47ff;QPassSignals+;QProgramSignals+;QStartupWithShell+;...
  [remote] Sending packet: $vMustReplyEmpty#3a
  [remote] Received Ack
  [remote] Packet received: 
  [remote] Sending packet: $QStartNoAckMode#b0
  [remote] Received Ack
  [remote] Packet received: OK
...
  [remote] Sending packet: $?#3f
  [remote] Packet received: T0506:0000000000000000;07:c0e6ffffff7f0000;10:9032fef7ff7f0000;thread:p13fcb5.13fcb5;core:12;
...
  [remote] Sending packet: $qXfer:exec-file:read:13fcb5:0,1000#dd
  [remote] Packet received: l/home/tbangma/learn-gdb-debugging/examples/ch10/device/meter
  [remote] Sending packet: $vFile:setfs:13fcb5#53
  [remote] Packet received: F0
...
[remote] Sending packet: $Z0,5555555551c8,1#bc
[remote] Packet received: OK
[remote] packet_ok: Packet Z0 (software-breakpoint) is supported
[remote] Sending packet: $Z0,7ffff7fc6300,1#ab
[remote] Packet received: OK
...
[remote] Sending packet: $vCont?#49
[remote] Packet received: vCont;c;C;t;s;S;r
[remote] packet_ok: Packet vCont (verbose-resume) is supported
[remote] Sending packet: $vCont;c:p13fcb5.-1#a2
...
  [remote] Packet received: T05swbreak:;06:50e5ffffff7f0000;07:88e2ffffff7f0000;10:0063fcf7ff7f0000;thread:p13fcb5.13fcb5;core:14;
[remote] wait: exit
[remote] Sending packet: $qXfer:libraries-svr4:read::0,1000#20
[remote] Packet received: l<library-list-svr4 version="1.0" main-lm="0x7ffff7ffe2e0"><library name="/lib64/ld-linux-x86-64.so.2" lm="0x7ffff7ffdaf0" l_addr="0x7ffff7fc3000" l_ld="0x7ffff7ffce80"/>...
[remote] Sending packet: $z0,7ffff7fc6300,1#cb
[remote] Packet received: OK
...
[remote] Sending packet: $vCont;s:p13fcb5.13fcb5#18
...
[remote] Sending packet: $Z0,7ffff7fc6300,1#ab
...
[remote] Sending packet: $vKill;13fcb5#01
[remote] Packet received: OK
[remote] packet_ok: Packet vKill (kill) is supported
[Inferior 1 (process 1309877) killed]
```

Read it as a conversation:

- **`qSupported`**: each side says what it understands, gdbserver including the largest packet
  it takes (`PacketSize=47ff`). **`vMustReplyEmpty`** checks that the server answers an unknown
  packet with an **empty reply**, the protocol's "not supported".
- **`QStartNoAckMode`**: until now each packet was acknowledged with `+` ("Received Ack").
  Over TCP, which is already reliable, gdbserver agrees to drop the acks.
- **`?`**: why are you stopped? `T05` is signal 5, `SIGTRAP`, followed by some registers in
  numbered pairs (`06` rbp, `07` rsp, `10` rip: register 16) and the thread, `pPID.TID` in hex.
  0x13fcb5 is pid 1309877.
- **`qXfer:exec-file:read`** and **`vFile:...`**: Host I/O. gdb reads files *through* the
  server: the program's path on the device, then (without `set sysroot`) the files themselves.
- **`Z0,5555555551c8,1`**: insert a software breakpoint at that address, 1 byte long. That is
  `break watts`, at the run-time address.
- **`Z0,7ffff7fc6300,1`** is **gdb's own** breakpoint, not one you set. ld.so was loaded at
  0x7ffff7fc3000 (`l_addr` in the library list), and 0x7ffff7fc6300 is its `_dl_debug_state`,
  which ld.so calls each time the list of loaded libraries changes. `info breakpoints` hides it.
- **`vCont;c:p13fcb5.-1`**: continue all threads of process 0x13fcb5. The stop comes back as
  `T05swbreak:;...`: stopped by a software breakpoint (at the ld.so hook: rip, register `10`, is
  0x7ffff7fc6300). gdb reads the new library list, then steps over its breakpoint: `z0` removes
  it, `vCont;s` steps one instruction, `Z0` puts it back, as around every breakpoint it
  continues from. **`vKill`** ends the program.

The framing is the same for every packet: `$`, the data, `#`, and a checksum, the sum of the
data bytes modulo 256 in two hex digits. `g` is 0x67, so the packet is `$g#67`; `?` is 0x3f, so
`$?#3f`. The receiver answers `+` (good) or `-` (send it again) until no-ack mode. Inside
binary data, the bytes `#`, `$`, `}` and `*` are escaped as `}` followed by the byte XOR 0x20;
`*` starts run-length encoding in replies. And a lone byte 0x03, outside any packet, is
Ctrl-C: "stop the program".

## Building tdb: the packets

The project is tdb v1.0 in `project/ch10/`. Next to Chapter 9's, it has new files (`src/rsp.c`
and `.h`, `src/gdbserver.c` and `.h`, `tests/test_rsp.c`), and changes only in `tdb.c` (the
`gdbserver` command), the Makefile, the tests and the README: the server is built from the rest.

`src/rsp.c` (194 lines) is the protocol without sockets or ptrace, so it can be unit-tested:

- **Framing**: `rsp_checksum` adds the bytes into a `uint8_t`, which wraps at 256 by itself,
  and `rsp_frame` writes `$`, the data and `"#%02x"`.
- **Reading** is harder: bytes arrive in any chunks, and between packets come acks and Ctrl-C.
  `rsp_feed` is a state machine fed one byte at a time. Between packets, `$` starts a packet;
  `+`, `-` and 0x03 come back as `RSP_ACK`, `RSP_NAK` and `RSP_INTERRUPT`; anything else is
  noise. Inside a packet it sums the data up to `#`, reads the two checksum digits, and returns
  `RSP_PACKET`, or `RSP_BAD` for a wrong checksum or a packet longer than `RSP_MAX`.
- **Hex** in both directions, and `rsp_escape` for binary replies.
- **The `g` reply**: gdb's register order for x86-64 is rax, rbx, rcx, rdx, rsi, rdi, rbp,
  rsp, r8 to r15 and rip (17 × 8 bytes), then eflags, cs, ss, ds, es, fs and gs (7 × 4 bytes):
  `RSP_REGS_BYTES`. A table maps each to its offset in `struct user_regs_struct`. x86-64 is
  little-endian and so is the packet, so each field is hex-encoded as it lies in memory.
- **Signal numbers**: gdb numbers signals its own way, the same on every system (its
  `include/gdb/signals.def`), so tdb translates. `SIGFPE` is 8 in both, but `SIGBUS` is 7 on
  Linux and 10 for gdb, and `SIGUSR1` is 10 on Linux and 30 for gdb.

`tests/test_rsp.c` has six tests built from packets that real gdb 12.1 sent: for example
`rsp_checksum("g", 1)` is 0x67 and framing `?` gives `$?#3f`.

## Building tdb: a gdbserver

`src/gdbserver.c` (703 lines) is the server. tdb's `gdbserver [--remote-debug] PORT` starts the
program if needed, stopped at its first instruction, and calls `gs_serve`, which waits for one
connection:

```c
    /* Only this machine: whoever connects can make the process do
     * anything (M writes memory, P sets rip). */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ...
        /* Send each small packet at once: without this, TCP holds an ack
         * back until the answer to the last one arrives (Nagle), 40 ms a time. */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
```

Then it loops: `get_packet` feeds bytes to `rsp_feed` and answers `+` (or `-`); tdb keeps the
acks. `serve_packet` answers the packet. Here is its dispatch, the whole debugger as a list:

```c
    out[0] = '\0';              /* "": we do not know this packet */
    switch (pkt[0]) {
    case '?':                   /* why it stopped; "" until it has run */
        if (s->stop[0] == '\0')
            snprintf(out, sizeof out, "T05thread:%s;", thread_id(s, s->p->cur->tid));
        else
            strcpy(out, s->stop);
        break;
    case 'g':
        if (proc_get_regs(s->p, &regs) < 0)
            strcpy(out, "E01");
        else
            rsp_regs_encode(&regs, out);
        break;
    case 'P': write_register(s, pkt + 1, out); break;
    case 'm': read_memory(s, pkt + 1, out); break;
    case 'M': write_memory(s, pkt + 1, out); break;
    case 'Z': breakpoint_packet(s, true, pkt + 1, out); break;
    case 'z': breakpoint_packet(s, false, pkt + 1, out); break;
    case 'H': select_thread(s, pkt + 1, out); break;
    ...
    case 'c': case 'C': case 's': case 'S':
        if (!resume(s, pkt[0], pkt + 1))
            return GS_DISCONNECTED;
        return s->p->state == PROC_STOPPED ? -1 : GS_EXITED;
    ...
    }
    return put_packet(s, out) ? -1 : GS_DISCONNECTED;
```

Each packet is a function from an earlier chapter:

- **`g`** is `proc_get_regs` (Chapter 2). **`P`** sets one register: `proc_get_regs`, change
  one field, `proc_set_regs`. gdb sends `P` when `G`, "all registers", is not supported.
- **`m`** reads through `bp_read_code` (Chapter 6), so gdb never sees tdb's `0xCC` bytes.
  **`M`** is `proc_write` (Chapter 3).
- **`Z0`/`z0`** are `bp_add_internal` (Chapter 6) and `bp_delete`, at `addr - p->load_base`.
  **`Z2`** (write watchpoint) and **`Z4`** (access) are `bp_add_watch` on the debug registers
  (Chapter 4). **`Z3`** (read) gets the empty reply, because DR7 has no read-only mode; gdb's
  `rwatch total` then sends `Z4` instead.
- **`c`, `C`, `s`, `S`** are `run_continue` and `run_stepi`. `C` and `S` pass the signal the
  program stopped with, `c` and `s` drop it: Chapter 5's signal rule, with gdb deciding.
- **`bc`/`bs`** are Chapter 9's `rec_reverse_continue` and `rec_reverse_stepi`, offered only
  when tdb is recording.
- **`H`**, **`T`**, `qfThreadInfo` and `qC` are Chapter 7's thread table.
- **`qXfer`** sends, in pieces, `target.xml` (`i386:x86-64`, `GNU/Linux`), the auxiliary vector
  from `/proc/PID/auxv` (gdb needs `AT_ENTRY` for the PIE base and `AT_BASE` for ld.so) and the
  program's path. **`vFile`** reads files for gdb. Anything else gets the empty reply.

`resume` turns the result of the stop into a stop reply, kept in `s->stop` for the next `?`:
`W00` for an exit with status 0, `X0b` for a death by `SIGSEGV`, `T08thread:...;` for a signal,
`T05thread:...;swbreak:;` after a continue that hit an int3, `T05...watch:ADDR;` for a
watchpoint, and `T05...replaylog:begin;` at the start of tdb's record.

Now the README's demo. tdb serves `sum 10 20`, gdb sets a breakpoint and a watchpoint, changes a
variable and a register, and detaches. tdb is driven through a pipe here:

```console
$ printf 'gdbserver 1234\nreg rax\nprint total\ncontinue\n' | build/tdb build/targets/sum 10 20
Reading symbols from build/targets/sum: 21 symbols, 15 line table rows.
(tdb) gdbserver 1234
Process build/targets/sum created; pid = 1298294
Listening on port 1234
```

```console
$ gdb -q build/targets/sum
Reading symbols from build/targets/sum...
(gdb) target remote :1234
Remote debugging using :1234
...
0x00007ffff7fe3290 in _start () from target:/lib64/ld-linux-x86-64.so.2
(gdb) break add
Breakpoint 1 at 0x555555555175: file targets/sum.c, line 14.
(gdb) continue
Continuing.
Reading /lib/x86_64-linux-gnu/libc.so.6 from remote target...

Breakpoint 1, add (x=10) at targets/sum.c:14
14	    total += x;
(gdb) watch total
Hardware watchpoint 2: total
(gdb) continue
Continuing.

Hardware watchpoint 2: total

Old value = 0
New value = 10
add (x=10) at targets/sum.c:15
15	    calls++;
(gdb) finish
Run till exit from #0  add (x=10) at targets/sum.c:15
main (argc=3, argv=0x7fffffffe6f8) at targets/sum.c:20
20	    for (int i = 1; i < argc; i++)
(gdb) set var total = 1000
(gdb) print $rax = 42
$1 = 42
(gdb) detach
Detaching from program: /home/tbangma/learn-gdb-debugging/project/ch10/build/targets/sum, process 1298294
Ending remote debugging.
[Inferior 1 (process 1298294) detached]
```

The rest of tdb's terminal:

```console
Remote debugging from host 127.0.0.1, port 53738
gdb is gone; process 1298294 is tdb's again.
main () at targets/sum.c:20
20	    for (int i = 1; i < argc; i++)
(tdb) reg rax
rax = 0x2a (42)
(tdb) print total
total = 1000 (0x3e8), 8 bytes at 0x555555558018
(tdb) continue
sum = 1020
[process exited with status 0]
```

- Without `set sysroot`, gdb read ld.so and libc through tdb's `vFile`, as through gdbserver.
- The watchpoint is tdb's debug register, and the stop is `T05...watch:555555558018;`.
- gdb's changes are real: tdb sees `rax` = 42, and the sum is 1000 + 20.

Here is the whole conversation for `hello`, with `gdbserver --remote-debug 1234` in tdb, which
logs each packet (`<-` from gdb, `->` tdb's answer):

![The packets between gdb and tdb for hello, in seven bands from qSupported to
W00](images/ch10-rsp-exchange.png)

```console
...
[remote] <- vCont?
[remote] -> 
[remote] <- Hcp13cff4.0
[remote] -> OK
[remote] <- c
[remote] -> T05thread:p13cff4.13cff4;swbreak:;
...
[remote] <- c
hello from the target
[remote] -> W00
[process exited with status 0]
```

The figure follows the 344 lines of that log: the same conversation as with gdbserver, with fewer
features. tdb answers `vCont?` with the empty reply, so gdb selects the thread with `Hc` and sends
a plain `c`. The `Z0` at 0x7ffff7fc6300 is again gdb's own breakpoint on ld.so's
`_dl_debug_state` (base 0x7ffff7fc3000 plus 0x3300), which it later steps over with `z0`, `s`,
`Z0`. Through tdb, gdb read the ELF with 18 `vFile` packets and memory with 106 `m` packets.

The README has more demos (threads, attach, gdb's own `record`). The best one: **tdb's record,
driven by gdb**. tdb records `ledger` from line 65 (Chapter 9), then serves it. Because it is
recording, its `qSupported` answer offers `ReverseContinue+`:

```console
$ printf 'break 65\nrun\nrecord\ngdbserver 1234\n' | build/tdb build/targets/ledger targets/deposits.txt
```

In gdb, after `break 72` and `continue` (to the end of the loop):

```console
(gdb) watch -l accounts[4].balance
Hardware watchpoint 2: -location accounts[4].balance
(gdb) reverse-continue
Continuing.

Hardware watchpoint 2: -location accounts[4].balance

Old value = 182
New value = 167
0x000055555555545c in main (argc=2, argv=0x7fffffffe6e8) at targets/ledger.c:68
68	            a->balance += amount;
(gdb) reverse-continue
...
(gdb) reverse-continue
Continuing.

Hardware watchpoint 2: -location accounts[4].balance

Old value = 107
New value = 0
0x000055555555523b in set_name (a=0x555555558070 <accounts+48>, name=0x7fffffffe580 "frederick") at targets/ledger.c:25
25	        a->name[i] = name[i];
(gdb) info record
No recording is currently active.
```

That is Chapter 9's hunt, with gdb's interface and tdb's engine: gdb's own record is off, and gdb
only sends `bc`. A *hardware* watchpoint works backwards here, unlike in gdb's own record,
because tdb's reverse-continue checks the watched values as it replays.

**Tests.** `make test` runs 32 unit tests (six new in `test_rsp.c`) and 87 integration tests (seven
new). A `remote` test runs `gdbserver 0` (any free port) in tdb, reads the port from "Listening on
port N", runs `gdb -q -nx -batch` against it, and checks gdb's output and then tdb's; it is
skipped when gdb is not installed. `rsp-basic` checks that `x/4xb main` shows `0xf3 0x0f 0x1e
0xfa` (the `endbr64`), not tdb's int3.

```console
$ make test
...
unit: 32 passed, 0 failed
tests/itest.sh build
itest: 87 passed, 0 failed
```

**Limits** (the README has the list): one gdb, once, only on 127.0.0.1. Ctrl-C in gdb does not
stop a running program, because tdb does not read the socket while it runs (exercise 3). Not
supported: `G`, `Z1` (hardware breakpoints), `Z3`, `$_siginfo` (exercise 4), `monitor` (exercise
5), `vCont`, `QStartNoAckMode`, fork and exec events. And tdb's own breakpoints stay in while gdb
drives: the program stops at each, gdb, which has no breakpoint there, quietly continues, and
their hit counts go up.

**What went wrong while building it** (each re-checked with a copy of tdb that leaves the fix out):

- **Nagle.** Without `TCP_NODELAY`, `rsp-basic`'s gdb session took 17.6 seconds; with it, 0.19
  (on my machine, varies): each small reply waited for gdb's delayed ack, about 40 ms.
- **`multiprocess+`.** Without `pPID.TID` thread ids, the simple session works, but gdb 12.1's
  own `record` dies: "internal-error: thread_architecture: Assertion \`inf != NULL' failed."
- **Without `vFile`**, gdb 12.1 reads the host's files instead ("remote target does not support
  file transfer, attempting to access files from local filesystem."): wrong for a real device.
- **`?` must not touch the process**: it answers from the stored stop reply. And **the pid
  changes under replay** (Chapter 9 forks the snapshot), so `thread_id` keeps the first pid.
- **qXfer annexes contain colons** (`exec-file:read:13cff4:0,ffb`): parse from the *last*
  colon. **Binary replies** (auxv has zero bytes) need `put_packet_len`, not `strlen`.

## tdb 1.0

This is the last snapshot. tdb runs programs, stops them at breakpoints, watchpoints, catchpoints
and signals, steps by line and by instruction, reads DWARF line tables and ELF symbols, walks the
stack, debugs threads, takes checkpoints, records and replays, and serves gdb: about 8500 lines
of C (7229 in `src/*.c`, 1269 in headers), plus tests, on one system call, `ptrace`. What gdb has
that tdb does not is most of a real debugger: DWARF types and expressions (tdb prints integers),
call frame information (Chapter 9's wrong `bt` in libc), `next` over signals, non-stop mode,
Python, many architectures. Each chapter showed where one of those is needed.

Where to go next: the gdb manual and its appendix on the remote protocol, `ptrace(2)`, the DWARF 5
standard (Chapter 6), **rr** for record and replay of real programs, and **LLDB**.

## In the wild

- **The protocol** is documented in the gdb manual's appendix "Remote Protocol": the packet
  format and checksum, `+`/`-` acknowledgments and no-ack mode, the packets, the stop replies,
  Host I/O, and interrupts. Ctrl-C is "the single byte `0x03` without any of the usual packet
  overhead"; in non-stop mode gdb sends a `vCtrlC` packet instead.
- **QEMU** has a gdb stub built in: `-s` listens on TCP port 1234 and `-S` keeps the CPU stopped
  until gdb says go, so you can debug a kernel from its first instruction.
- **OpenOCD** drives JTAG debug probes for microcontrollers and boards, and "complies with the
  remote gdbserver protocol": gdb connects to it with `target extended-remote`.
- **LLDB** uses the same protocol between `lldb` and `lldb-server` (or `debugserver` on macOS),
  with its own extensions, documented on its page "GDB Remote Protocol Extensions".
- **rr** replays a recording as a gdb remote target: gdb connects to rr as to a gdbserver, and
  its reverse commands go to rr's replay, as they went to tdb's record above.
- **Separate debug files** are in the gdb manual, "Debugging Information in Separate Files":
  the debug link with its CRC, build IDs and `.build-id/nn/nnnnnnnn.debug`. gdb can also ask
  `debuginfod` servers for a debug file by build ID.

## Common mistakes

- **Debugging the stripped binary**: `?? ()` everywhere. Give gdb the binary with symbols, or
  its debug file. **Forgetting `set sysroot`**: every library comes through the connection.
- **`set debug-file-directory ./debug`** replaces the default; use `./debug:/usr/lib/debug`.
- **A host binary from another build**: answering `n` to the build-ID question gives a garbage
  backtrace and a misleading `list`.
- **`gdbserver --attach` refused** by `ptrace_scope` 1: "Operation not permitted (1)".
- **Trusting `Starting program:`** with `extended-remote`: what runs is `set remote exec-file`.
- **`monitor exit` then `quit`**: "Remote connection closed", and `quit` again. Use `disconnect`.
- **`target remote | gdbserver -`** loses the program's output on a crash.

## Summary

- `gdbserver :PORT PROG ARGS` on the device, `target remote HOST:PORT` in gdb: gdbserver does
  the ptrace calls, gdb does the symbols. The program starts stopped in ld.so.
- `set sysroot` says where the device's libraries are on your machine.
- Ship stripped binaries and keep the debug info: `objcopy --only-keep-debug`, a debug link,
  and the build ID, found under `DIR/.build-id/` for each `debug-file-directory`.
- `gdbserver --attach :PORT PID` for a running service (`ptrace_scope` permitting); `--multi`
  with `extended-remote` to run many; `target remote | gdbserver - ...`; `monitor`.
- `set debug remote 1` shows the packets: `$data#checksum`, `qSupported`, `?`, `g`, `m`, `Z0`,
  `vCont` or `c`, and the stop replies. tdb v1.0 serves them in about 900 lines (`rsp.c` and
  `gdbserver.c`), each packet a function it already had.

## Exercises

1. Fix `meter.c` so that the device's log gives a result. Should the fix be in `watts`?

<details><summary>Solution</summary>

Skip a reading whose time does not advance, in `main`'s loop, and say so on stderr:

```c
    for (int i = 1; i < n; i++) {
        if (readings[i].time <= readings[i - 1].time) {
            fprintf(stderr, "meter: skipping reading %d: time %ld does not advance\n",
                    i + 1, readings[i].time);
            continue;
        }
        long w = watts(&readings[i - 1], &readings[i]);
```

```console
$ ./meter device/meter.log
28800-29700 s     720 W
29700-30600 s     920 W
meter: skipping reading 4: time 30600 does not advance
30600-31500 s    1160 W
31500-32400 s    1080 W
6 readings, peak 1160 W
$ echo $?
0
```

Returning 0 from `watts` when `seconds` is 0 would print a fake "0 W" interval: the data is
wrong, not the arithmetic. `<=` also catches readings that go back in time. Note that this
build has another build ID: debugging the old `device/meter` with it is exactly the mismatch
shown above. Rebuild both with `make`.

</details>

2. Fix `poller.c` so that it starts work when `ready.flag` appears.

<details><summary>Solution</summary>

In `read_config`, copy the name up to the newline:

```c
            snprintf(flag, size, "%.*s", (int)strcspn(line + 9, "\n"), line + 9);
```

(Or `line[strcspn(line, "\n")] = '\0';` before the `strncmp`.) In `device/`:

```console
$ (sleep 6; touch ready.flag) &
$ ../poller poller.conf
poller: pid 1308922
still waiting for ready.flag
starting work
$ echo $?
0
```

The "still waiting" line has no empty line after it now. `make clean` removes `ready.flag`.

</details>

Exercises 3, 4 and 5 change tdb. Apply them one at a time, each to a fresh copy of
`project/ch10` (`cp -r project/ch10 ex3`, and so on): they touch the same files, and some of
exercise 5's changes do not apply on top of 3 and 4. To try one, run `printf 'gdbserver 1234\n' |
build/tdb build/targets/NAME` (`waiter` from Chapter 5, `crash` from Chapter 1), and in another
terminal `gdb -q build/targets/NAME`, `set sysroot /`, `target remote :1234`.

3. Make Ctrl-C in gdb stop a program that tdb is running for it. Today:

```console
(gdb) continue
Continuing.
^C^CThe target is not responding to interrupt requests.
Stop debugging it? (y or n) y
Disconnected from target.
```

<details><summary>Solution</summary>

While `run_continue` waits in `waitpid`, nobody reads the socket, so gdb's 0x03 sits there.
Ask the kernel to signal tdb when bytes arrive: `O_ASYNC` on the socket and a `SIGIO`
handler that sends the program `SIGINT`, "just like the user typed a ^C on the controlling
terminal", as gdbserver's comment on the same code says (gdbserver signals the process group;
this signals the one process). The program stops with `SIGINT`, `waitpid` returns, and the stop
reply is `T02`. In `src/gdbserver.c` (and `#include <signal.h>`):

```c
static volatile pid_t interrupt_pid;

static void on_sigio(int sig)
{
    (void)sig;
    if (interrupt_pid > 0)
        kill(interrupt_pid, SIGINT);
}

static void watch_gdb(struct server *s, bool on)
{
    int flags = fcntl(s->fd, F_GETFL);
    char c;

    if (!on) {
        fcntl(s->fd, F_SETFL, flags & ~O_ASYNC);
        interrupt_pid = 0;
        return;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigio;   /* no SA_RESTART: waitpid() must return */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGIO, &sa, NULL);
    interrupt_pid = s->p->pid;
    fcntl(s->fd, F_SETOWN, getpid());
    fcntl(s->fd, F_SETFL, flags | O_ASYNC);
    /* A Ctrl-C that came before O_ASYNC was on raises no SIGIO. */
    if (s->pos < s->len || recv(s->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 1)
        on_sigio(SIGIO);
}
```

and in `resume`, `watch_gdb(s, true)` before `run_continue` and `watch_gdb(s, false)` after.
With `waiter`:

```console
(gdb) continue
Continuing.
^C
Program received signal SIGINT, Interrupt.
0x00007ffff7ce574a in __GI___clock_nanosleep (clock_id=clock_id@entry=0, flags=flags@entry=0, req=0x7fffffffe530, rem=0x0) at ../sysdeps/unix/sysv/linux/clock_nanosleep.c:78
78	../sysdeps/unix/sysv/linux/clock_nanosleep.c: No such file or directory.
(gdb) bt
...
#2  0x0000555555555237 in work () at targets/waiter.c:20
#3  0x00005555555552ab in main () at targets/waiter.c:31
(gdb) print rounds
$1 = 1269
```

(The count varies; a second `continue` and Ctrl-C stop it again.) A test cannot use `gdb -batch
-x`, which never sends the Ctrl-C: gdb's event loop does not run between the commands. The test
`rsp-interrupt` feeds gdb's stdin instead (`continue &`, `shell sleep 0.5`, `interrupt`): itest
88/88.

</details>

4. Make `print $_siginfo` work through tdb. Today, after the crash in `crash`, it says "Unable
   to read siginfo" (while `ptype $_siginfo` works: the type is gdb's).

<details><summary>Solution</summary>

gdb reads it with `qXfer:siginfo:read::OFFSET,LENGTH`, and the answer is the raw `siginfo_t`
that Chapter 5's `proc_siginfo` (`PTRACE_GETSIGINFO`) gives. In `xfer()`:

```c
    } else if (strncmp(what, "siginfo:read::", 14) == 0) {
        siginfo_t si;

        if (proc_siginfo(s->p, &si) == 0) {
            memcpy(data, &si, sizeof si);
            n = (ssize_t)sizeof si;
        }
```

and add `qXfer:siginfo:read+` to the `qSupported` answer. With `crash`:

```console
Program received signal SIGSEGV, Segmentation fault.
0x000055555555519f in main () at targets/crash.c:10
10	    *p = 42;
(gdb) print $_siginfo
$1 = {si_signo = 11, si_errno = 0, si_code = 1, _sifields = {_pad = {0 <repeats 28 times>}, 
...
(gdb) print $_siginfo._sifields._sigfault.si_addr
$2 = (void *) 0x0
(gdb) print $_siginfo.si_code
$3 = 1
```

(`print $_siginfo` prints every member of the union; cut here.) With meter on the device (`tdb
./meter meter.log` in `device/`, and gdb with the host's `./meter`), `si_signo` is 8, `si_code`
1 (`FPE_INTDIV`), and `si_addr` is `0x5555555551ec <main+204>`, the same as `$pc`: for `SIGFPE`
the address is the faulting instruction, for `SIGSEGV` the data address (Chapter 5). itest 88/88.

</details>

5. Make gdb's `monitor LINE` run a tdb command and show its output in gdb, but only commands
   that look at the program, never ones that run it or change its breakpoints.

<details><summary>Solution</summary>

gdb sends `qRcmd,HEX`, the line in hex. The answer is any number of `O` packets, each the hex of
some output text, which gdb prints as they come, and then `OK`. Catch the output of tdb's own
`execute()` in a memory file. The heart of `monitor()`, after it has decoded the hex into
`line`:

```c
    int fd = memfd_create("monitor", 0);
    if (fd < 0)
        return put_packet(s, "E01");
    fflush(stdout);
    int saved = dup(1);
    dup2(fd, 1);
    s->monitor(line);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    while ((got = read(fd, text, sizeof text)) > 0) {
        out[0] = 'O';
        rsp_hex_encode(text, (size_t)got, out + 1);
        if (!put_packet(s, out)) {
            close(fd);
            return false;
        }
    }
    close(fd);
    return put_packet(s, "OK");
```

`gs_serve` takes `s->monitor` as a callback, so `gdbserver.c` does not depend on `tdb.c`. There,
`monitor_command` runs a line with `execute()` only for `bt`, `help`, `info`, `list`, `print`,
`reg`, `regs`, `show`, `siginfo` and `x`; anything else prints `"monitor %s" is not allowed: gdb
is in charge of the program.` With `ledger`, recorded by tdb from line 65:

```console
(gdb) monitor info record
Active record target: tdb
...
Log contains 13635 instructions.
...
(gdb) monitor info breakpoints
Num  Type        Disp  Address  In memory           Hits  What
1    breakpoint  keep  0x1467   0x0000555555555467  11    in main at targets/ledger.c:65
(gdb) monitor reg rip
rip = 0x55555555548c (93824992236684)
(gdb) monitor continue
"monitor continue" is not allowed: gdb is in charge of the program.
```

The 11 hits on tdb's breakpoint 1 are the hidden stops from the limits above, made visible:
the loop at line 65 ran on under gdb, and each pass stopped at tdb's breakpoint. itest 88/88.

</details>

6. Open-ended: pick one of the remaining limits and remove it. These are ideas, not solved
   here: `G` (all registers in one packet); `Z1` (a hardware breakpoint: a debug register with
   R/W bits 00); `vCont` (resume one thread while the others stay stopped, Chapter 7);
   `QStartNoAckMode`; fork events (`fork-events+` and a `T05fork:...;` stop reply); or
   `gdbserver HOST:PORT`, with a warning about who can connect.

Sources for this chapter: [REFERENCES.md](REFERENCES.md#chapter-10--remote-debugging)
