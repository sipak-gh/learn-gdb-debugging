#!/bin/sh
# itest.sh - run tdb on the demo targets with scripted commands and check
# its output.
#
#     tests/itest.sh BUILD-DIR
#
# Each check feeds commands to tdb's stdin and lists texts that must appear
# in the output, in that order. Exit status: 0 if every check passed.

B=${1:-build}
pass=0
fail=0

# expect NAME OUTPUT EXPECTED...: the EXPECTED texts must appear in OUTPUT
# in that order. A text written as !TEXT must not appear after the
# previous one.
expect() {
    name=$1 out=$2
    shift 2
    rest=$out
    for want in "$@"; do
        case $want in
        !*)
            case $rest in
            *"${want#!}"*)
                report "$name" "$out" "unexpected \"${want#!}\""
                return
                ;;
            esac
            ;;
        *)
            case $rest in
            *"$want"*)
                rest=${rest#*"$want"}
                ;;
            *)
                report "$name" "$out" "missing \"$want\""
                return
                ;;
            esac
            ;;
        esac
    done
    pass=$((pass + 1))
}

report() {
    echo "FAIL $1: $3"
    echo "--- output was:"
    echo "$2"
    echo "---"
    fail=$((fail + 1))
}

# check NAME TARGET COMMANDS EXPECTED...: run tdb on TARGET with COMMANDS.
check() {
    name=$1 target=$2 commands=$3
    shift 3
    expect "$name" "$(printf "$commands" | timeout 30 "$B/tdb" -nx "$B/targets/$target" 2>&1)" "$@"
}

# addr TARGET SYMBOL: the symbol's address in the file, as nm shows it.
addr() {
    printf '0x%x' "0x$(nm "$B/targets/$1" | awk -v s="$2" '$3 == s { print $1 }')"
}

check exit-status hello 'run\n' \
    'hello from the target' \
    '[process exited with status 0]'

check exit-code exit3 'run\ncontinue\n' \
    'Program received signal SIGUSR1' \
    'target: got SIGUSR1' \
    '[process exited with status 3]'

check segfault crash 'run\ncontinue\n' \
    'about to crash' \
    'Program received signal SIGSEGV (Segmentation fault).' \
    '[process killed by SIGSEGV (Segmentation fault)]'

check rerun hello 'run\nrun\nr now\n' \
    '[process exited with status 0]' \
    'Started process' \
    '[process exited with status 0]' \
    'Started process' \
    '[process exited with status 0]'

check not-running hello 'continue\nkill\n' \
    'The program is not being run.' \
    'The program is not being run.'

check unknown hello 'frobnicate\n' \
    'Undefined command: "frobnicate".  Try "help".'

SQUARE=$(addr loop square)
MAIN=$(addr loop main)

check breakpoint loop "break $SQUARE\nrun\ncontinue\ninfo breakpoints\n" \
    "Breakpoint 1 at $SQUARE: file targets/loop.c, line 5." \
    'Breakpoint 1, square () at targets/loop.c:5' \
    'Breakpoint 1, square () at targets/loop.c:5' \
    'keep' '2'

# The program must still compute the right numbers: stepping over the
# breakpoints executed the original instructions.
check step-over loop "break $SQUARE\nrun\nc\nc\nc\nc\nc\n" \
    'i=0 total=0' 'i=1 total=1' 'i=2 total=5' 'i=3 total=14' 'i=4 total=30' \
    '[process exited with status 0]'

check tbreak loop "tbreak $MAIN\nrun\ninfo breakpoints\ncontinue\n" \
    'Temporary breakpoint 1, main () at targets/loop.c:10' \
    'No breakpoints.' \
    '[process exited with status 0]'

check ignore loop "break $SQUARE\nignore 1 3\nrun\ninfo breakpoints\n" \
    'Will ignore next 3 crossings of breakpoint 1.' \
    'Breakpoint 1, square () at' \
    'keep  '"$SQUARE"'   0x0000' '4'

check delete loop "break $SQUARE\nrun\ndelete 1\ncontinue\n" \
    'Breakpoint 1, square () at' \
    'i=4 total=30' \
    '[process exited with status 0]'

check bad-break loop 'break nowhere\ndelete 7\nbreak\n' \
    'No symbol "nowhere" in the program.' \
    'No breakpoint number 7.' \
    'Usage: break LINE|FILE:LINE|FUNCTION|*ADDRESS'

# Chapter 3: symbols, registers, memory.

check symbols loop 'break *square\nrun\ninfo breakpoints\ninfo symbol $rip\n' \
    'Reading symbols from' \
    "Breakpoint 1 at $SQUARE" \
    'Breakpoint 1, square () at targets/loop.c:5' \
    'in square at targets/loop.c:5' \
    'square'

check symbol-offset loop 'tbreak main+4\nrun\n' \
    "Temporary breakpoint 1 at $(printf '0x%x' $((MAIN + 4)))" \
    'Temporary breakpoint 1, 0x' ' in main () at targets/loop.c:10'

check regs loop 'break *square\nrun\nregs\n' \
    'rax            0x' \
    'rip            0x555555555149      0x555555555149 <square>' \
    'eflags         0x' '[ ' ' IF ' \
    'orig_rax'

# The first argument is in rdi: square(10) instead of square(0).
check set-reg loop 'break *square\nrun\nreg rdi 10\ndelete\ncontinue\n' \
    'rdi = 0xa (10)' \
    'i=0 total=100'

# tdb leaves its breakpoints in memory: x shows the 0xCC.
check x-shows-int3 loop 'break *square\nrun\nx square 4\n' \
    '  cc 0f 1e fa ' '<square>'

check x-unaligned data 'break bump\nrun\nx greeting+3 5\n' \
    '  6c 6f 2c 20 74 ' '|lo, t|  <greeting+3>'

check print-set data 'break bump\nrun\nprint counter\nprint small\nset counter 99\nprint greeting\ncontinue\n' \
    'counter = 41 (0x29), 4 bytes at 0x' \
    'small = -2 (0xfffe), 2 bytes at 0x' \
    'counter = 99 (0x63)' \
    'greeting is an object of 11 bytes: use x greeting 11.' \
    'counter=100 r=100'

# The crash: rip is at the faulting store, and the pointer (rax) is 0.
check crash-regs crash 'run\nreg rax\nx $rip 2\n' \
    'Program received signal SIGSEGV' \
    ' in main () at targets/crash.c:' \
    'rax = 0x0 (0)' \
    '  c7 00 '

check not-running-x data 'x main\nregs\nprint counter\nreg rflags\n' \
    'The program is not being run.' \
    'The program is not being run.' \
    'The program is not being run.' \
    'Invalid register "rflags".'

# Watchpoints: step(0) writes counter without changing it, so the first stop
# is at i = 2; the report comes after the writing instruction.
check watch watched 'watch counter\nrun\ncontinue\ncontinue\n' \
    'Hardware watchpoint 1: counter (4 bytes at 0x' ', DR0)' \
    'Hardware watchpoint 1: counter' 'Old value = 0 (0x0)' 'New value = 2 (0x2)' \
    'step () at targets/watched.c:13' \
    'Old value = 2 (0x2)' 'New value = 6 (0x6)' \
    'counter=6 limit=3' 'exited with status 0'

check rwatch watched 'rwatch limit\nrun\ncontinue\n' \
    'Hardware read watchpoint 1: limit (4 bytes at 0x' \
    'Hardware read watchpoint 1: limit' 'Value = 3 (0x3)' ' in main () at targets/watched.c:' \
    'exited with status 0'

# A breakpoint on the store itself: continuing steps over it and must still
# notice the watchpoint.
w=$(objdump -d "$B/targets/watched" | awk '/mov    %eax,.*<counter>/ { sub(":", "", $1); print "0x" $1; exit }')
check break-on-store watched "break $w\nwatch counter\nrun\ncontinue\ncontinue\n" \
    'Breakpoint 1, ' 'Breakpoint 1, ' \
    'Hardware watchpoint 2: counter' 'Old value = 0 (0x0)' 'New value = 2 (0x2)'

check watch-limits watched 'watch counter\nwatch limit\nwatch 0x4000\nwatch 0x4008\nwatch 0x4020\nwatch 0x4019 4\nwatch 0x4018 3\ndelete 3\nwatch 0x4020\ninfo breakpoints\n' \
    'No free debug register: x86 has only 4.' \
    '0x4019 is not aligned to 4 bytes.' \
    'A watchpoint covers 1, 2, 4 or 8 bytes, not 3.' \
    'Hardware watchpoint 5: 0x4020 (8 bytes at 0x4020, DR2)' \
    '1    watchpoint' '2    watchpoint' '4    watchpoint' '5    watchpoint'

# Catchpoints stop twice per call: at entry (arguments) and at exit (result).
check catch-syscall files 'catch syscall write\nrun\ncontinue\ncontinue\n' \
    "Catchpoint 1 (syscall 'write' [1])" \
    'Catchpoint 1 (call to syscall write), 0x' '    write(0x1, 0x' ', 0x1c)' \
    'no settings, using defaults' \
    'Catchpoint 1 (returned from syscall write), 0x' '    = 28 (0x1c)' \
    'exited with status 0'

check catch-errno files 'catch syscall openat\nrun\ncontinue\ncontinue\ncontinue\ncontinue\ncontinue\ninfo breakpoints\n' \
    'call to syscall openat' 'returned from syscall openat' \
    'call to syscall openat' 'returned from syscall openat' \
    'call to syscall openat' '    = -2 (ENOENT: No such file or directory)' \
    '1    catch' '6     syscall openat'

# Deleting the catchpoint at a syscall entry must not confuse a later one.
check catch-delete files 'catch syscall write\nrun\ndelete 1\ncatch syscall\ncontinue\ncontinue\n' \
    'call to syscall write' \
    'Catchpoint 2 (any syscall)' \
    'Catchpoint 2 (call to syscall exit_group)'

check catch-errors files 'catch syscall nosuch\ncatch\ncatch syscall 100000\nwatch\n' \
    "Unknown syscall name 'nosuch'." \
    'Usage: catch syscall [NAME|NUMBER]' \
    "Unknown syscall name '100000'." \
    'Usage: watch LOC [LEN]'

# Backtraces: at the first instruction, after push %rbp, in the body, and
# from inside libc (strlen has no frame pointer, so inner is missing).
check bt-entry deep 'break *middle\nrun\nbt\nbt 1\nbt x\n' \
    '#0  middle () at targets/deep.c:16' \
    '#1  0x' ' in outer () at targets/deep.c:22' '#2  0x' ' in main () at targets/deep.c:29' \
    '#0  middle ()' '!#1' 'Usage: bt [N]'

check bt-prologue deep 'break *middle+4\nbreak *middle+5\nrun\nbt\ncontinue\nbt\n' \
    'Breakpoint 1, ' '#1  0x' ' in outer () at targets/deep.c:22' '#2  0x' ' in main () at' \
    'Breakpoint 2, ' '#1  0x' ' in outer () at targets/deep.c:22' '#2  0x' ' in main () at'

# Chapter 7: the stack scan finds inner, which called strlen.
check bt-crash deep 'run\nbt\nsiginfo\n' \
    'Program received signal SIGSEGV' \
    'Fault address 0x0: SEGV_MAPERR (address not mapped to object).' \
    '#0  0x' ' in ?? () from /' '#1  0x' ' in inner () at targets/deep.c:12' \
    '#2  0x' ' in middle () at targets/deep.c:17' '#4  0x' ' in main () at targets/deep.c:29' \
    'si_signo = 11 (SIGSEGV)' 'si_addr  = 0x0'

# Signals: SIGALRM passes silently by default; SIGUSR1 stops.
check handle-default ticker 'run\nsiginfo\ncontinue\n' \
    '!SIGALRM' 'Program received signal SIGUSR1' \
    'si_code  = -6: SI_TKILL' 'si_pid   = ' \
    'got SIGUSR1' 'ticks=5' '[process exited with status 0]'

check handle-stop ticker 'handle SIGALRM stop\nrun\nsiginfo\nhandle SIGALRM nostop noprint\ncontinue\n' \
    'SIGALRM    Yes   Yes    Yes   Alarm clock' \
    'Program received signal SIGALRM' 'SI_KERNEL' \
    'SIGALRM    No    No     Yes' '!SIGALRM (' 'Program received signal SIGUSR1'

# "nopass" given at the stop still drops the signal at the next continue.
check handle-nopass ticker 'run\nhandle SIGUSR1 nopass\ncontinue\n' \
    'Program received signal SIGUSR1' 'SIGUSR1    Yes   Yes    No' \
    '!got SIGUSR1' 'ticks=5'

check handle-errors ticker 'handle SIGFOO\nhandle SIGALRM maybe\ninfo signals SIGALRM\nsiginfo\n' \
    'Unknown signal "SIGFOO".' 'Unknown word "maybe"' \
    'SIGALRM    No    No     Yes   Alarm clock' 'The program is not being run.'

# Attach to a running waiter (it allows that with prctl), stop it at a
# breakpoint, detach, and check that it still runs.
scope=$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)
if [ "$scope" -le 1 ]; then
    "$B/targets/waiter" >/dev/null &
    pid=$!
    sleep 0.2
    out=$(printf 'break work\ncontinue\nbt\ninfo breakpoints\ndetach\n' |
        timeout 30 "$B/tdb" -nx -p $pid 2>&1)
    expect attach "$out" "Attached to process $pid." \
        'Breakpoint 1, work () at targets/waiter.c:17' '#1  0x' ' in main () at targets/waiter.c:' \
        "Detached from process $pid."
    sleep 0.1
    if [ -d /proc/$pid ] && grep -q '^State:.*[RS]' /proc/$pid/status; then
        pass=$((pass + 1))
    else
        report detach-alive "" "waiter did not survive detach"
    fi
    kill $pid
    wait $pid 2>/dev/null
else
    echo "skip attach: ptrace_scope is $scope"
fi

# sleep is our child, not tdb's: with ptrace_scope 1 tdb may not attach.
if [ "$scope" -eq 1 ]; then
    sleep 5 &
    pid=$!
    expect attach-eperm "$(timeout 30 "$B/tdb" -nx -p $pid </dev/null 2>&1)" \
        "Could not attach to process $pid: Operation not permitted." \
        'ptrace_scope'
    kill $pid
    wait $pid 2>/dev/null
fi

# Chapter 6: source lines. Breakpoints by line skip the prologue when the
# line is a function's "{"; an empty line moves to the next one with code.
check break-line fact 'break 3\nbreak fact.c:9\nbreak main\nbreak 99\nbreak nosuch.c:1\ninfo breakpoints\nrun\n' \
    'Breakpoint 1 at 0x' ': file targets/fact.c, line 7.' \
    'Breakpoint 2 at 0x' ': file targets/fact.c, line 9.' \
    'Breakpoint 3 at 0x' ': file targets/fact.c, line 19.' \
    'No line 99 in file "targets/fact.c".' 'No source file named nosuch.c.' \
    'in fact at targets/fact.c:7' 'in main at targets/fact.c:19' \
    'Breakpoint 3, main () at targets/fact.c:19' '19	    long f = fact(5);'

check list fact 'list fact\nlist\nlist\nlist\nlist nosuch\n' \
    '1	/* fact.c' '10	}' '11	' '20	    show(5, f);' '21	' '25	}' \
    'Line number 26 out of range; "targets/fact.c" has 25 lines.' \
    'Function "nosuch" not defined.'

check list-at-stop fact 'break show\nrun\nlist\n' \
    'Breakpoint 1, show () at targets/fact.c:14' '9	' '18	{' '!19	'

# step enters fact (stopping after its prologue) but not printf (no lines);
# next runs calls to their end. A for loop goes back to its own line.
check step fact 'break main\nrun\nstep\nbt\ndelete\nfinish\nnext\nstep\nstep\nstep\nstep\n' \
    'Breakpoint 1, main () at targets/fact.c:19' \
    'fact () at targets/fact.c:7' '7	    if (n <= 1)' '#1  0x' ' in main () at targets/fact.c:19' \
    'Run till exit from #0  fact () at targets/fact.c:7' \
    ' in main () at targets/fact.c:19' 'Value returned in rax: 120 (0x78)' \
    '20	    show(5, f);' \
    'show () at targets/fact.c:14' '15	}' 'main () at targets/fact.c:21' '22	        f += i;'

check next fact 'break main\nrun\nnext\nnext\nnext\nnext\nnext\nnext\n' \
    '20	    show(5, f);' '21	    for' '22	        f += i;' '21	    for' '22	' '21	' \
    '!fact () at'

# Recursion: the return address inside fact is reached first by deeper
# calls returning. finish and next must wait for the right rsp.
check finish-recursive fact 'break fact\nrun\ncontinue\ndelete\nfinish\nbt\n' \
    'Breakpoint 1, fact () at targets/fact.c:7' 'Breakpoint 1, fact () at' \
    'fact () at targets/fact.c:9' 'Value returned in rax: 24 (0x18)' \
    '#0  fact () at targets/fact.c:9' '#1  0x' ' in main () at' '!#2'

check next-recursive fact 'break 9\nrun\ndelete\nnext\nbt\n' \
    '10	}' '#0  fact () at targets/fact.c:10' '#1  0x' ' in main () at' '!#2'

check finish-main fact 'break main\nrun\nfinish\n' \
    '"finish" not meaningful in the outermost frame.'

# stepi shows the address when it is not at the start of a statement, and
# landing on a breakpoint counts as hitting it.
check stepi fact 'break *main\nbreak *main+4\nrun\nstepi\nstepi\nsi\n' \
    'Breakpoint 1, main () at targets/fact.c:18' '18	{' \
    'Breakpoint 2, 0x' ' in main () at targets/fact.c:18' \
    '0x' '	18	{'

# next stops at a breakpoint inside the call it runs, and for a signal.
check next-stops fact 'break fact\nbreak main\nrun\nnext\n' \
    'Breakpoint 2, main ()' 'Breakpoint 1, fact () at targets/fact.c:7'

check next-signal deep 'break inner\nrun\nnext\ninfo breakpoints\n' \
    'Breakpoint 1, inner () at targets/deep.c:12' \
    'Program received signal SIGSEGV' \
    '1    breakpoint' '!-1'

# Two breakpoints at one address share the original byte: deleting one
# must leave the other working, and the program intact.
check same-address fact 'break fact\nbreak 7\nrun\ndelete 1\ncontinue\ndelete 2\ncontinue\n' \
    'Breakpoint 1, fact ()' 'Breakpoint 2, fact ()' '5! = 120' '123' \
    '[process exited with status 0]'

# A program without line information (a system one: stripped, no -g).
expect no-lines "$(printf 'list\nbreak 12\n' | timeout 30 "$B/tdb" -nx /bin/true 2>&1)" \
    'No line information: was the program compiled with -g?' \
    'No line information: was the program compiled with -g?'

# Chapter 7: threads and processes. Which worker reaches work() first
# varies, so these checks do not name thread numbers that may change.
check threads workers 'break work\nrun\ninfo threads\nthread 1\nthread\nthread apply all bt\nthread 9\nthread apply 1 info symbol $rip\nthread apply\n' \
    '[New LWP ' '[Switching to LWP ' 'hit Breakpoint 1, work () at targets/workers.c:14' \
    '  Id   Target Id               Frame' '  1    LWP ' '"workers"' '* ' ' work () at targets/workers.c:14' \
    '[Switching to thread 1 (LWP ' '[Current thread is 1 (LWP ' \
    'in worker () at targets/workers.c:28' 'Thread 1 (LWP ' '#0  0x' 'Invalid thread ID: 9' \
    'Thread 1 (LWP ' 'No symbol matches' 'Usage: thread apply all|N... COMMAND'

# The kernel does not copy debug registers into a new thread: tdb must.
check threads-watch workers 'break main\nrun\nwatch total\ncontinue\ncontinue\ncontinue\ncontinue\n' \
    'Hardware watchpoint 2: total' \
    'hit Hardware watchpoint 2: total' 'work () at targets/workers.c:18' \
    'hit Hardware watchpoint 2: total' 'hit Hardware watchpoint 2: total' \
    'New value = 600 (0x258)' 'total: 600' '[process exited with status 0]'

# The child of fork() gets a copy of the breakpoints' int3 bytes; tdb
# takes them out before letting the child go.
check fork forker 'break greet\nrun\ncontinue\n' \
    '!killed by signal' '[Detaching after fork from child process ' 'hello from the child' \
    'Breakpoint 1, greet () at targets/forker.c:11' 'hello from the parent' \
    '[process exited with status 0]'

# Ctrl-C: the terminal sends SIGINT to the whole foreground process group,
# tdb and the program. Do the same: signal both.
printf 'run\ninfo threads\nthread apply all bt\nkill\n' > "$B/ctrl-c.in"
timeout 30 "$B/tdb" -nx "$B/targets/deadlock" < "$B/ctrl-c.in" > "$B/ctrl-c.out" 2>&1 &
sleep 1
tdb=$(pgrep -P $! -x tdb)
pid=$(sed -n 's/^Started process \([0-9]*\):.*/\1/p' "$B/ctrl-c.out")
kill -INT $pid $tdb
wait
expect ctrl-c "$(cat "$B/ctrl-c.out")" \
    'Thread 1 received signal SIGINT (Interrupt).' ' in ?? () from /' \
    '* 1    LWP ' '  2    LWP ' '  3    LWP ' \
    'Thread 3 (LWP ' 'in take_both () at targets/deadlock.c:24' 'in two () at targets/deadlock.c:39' \
    'Thread 2 (LWP ' 'in take_both () at targets/deadlock.c:24' 'in one () at targets/deadlock.c:32' \
    'Thread 1 (LWP ' 'in main () at targets/deadlock.c:52' '[process killed]'
rm -f "$B/ctrl-c.in" "$B/ctrl-c.out"

# Attach to a process with threads: tdb must attach to each one.
if [ "$scope" -le 1 ]; then
    "$B/targets/deadlock" >/dev/null &
    pid=$!
    sleep 0.3
    out=$(printf 'info threads\nthread apply all bt\ndetach\n' | timeout 30 "$B/tdb" -nx -p $pid 2>&1)
    expect attach-threads "$out" "Attached to process $pid." '[New LWP ' '[New LWP ' \
        '  3    LWP ' 'Thread 3 (LWP ' 'in take_both () at targets/deadlock.c:24' \
        'Thread 1 (LWP ' 'in main () at targets/deadlock.c:52' "Detached from process $pid."
    if [ "$(ls /proc/$pid/task 2>/dev/null | wc -l)" -eq 3 ]; then
        pass=$((pass + 1))
    else
        report detach-threads "" "deadlock lost threads after detach"
    fi
    kill $pid
    wait $pid 2>/dev/null
fi

# Chapter 8: the command language.
check abbrev hello 'b main\nr\nste\nco\nfrobnicate\ninfo s\ninfo nosuch\ninfo\ni b\nc\n' \
    'Breakpoint 1 at ' 'Breakpoint 1, main () at targets/hello.c' \
    'Ambiguous command "ste": step, stepi.' \
    'Ambiguous command "co": commands, continue.' \
    'Undefined command: "frobnicate".  Try "help".' \
    'Ambiguous info command "s": signals, symbol.' \
    'Undefined info command: "nosuch".  Try "help info".' \
    '"info" must be followed by the name of a subcommand: breakpoints,' \
    'in main at targets/hello.c' '[process exited with status 0]'

check repeat sum 'break main\nrun\nnext\n\n\n' \
    '20	    for (int i = 1; i < argc; i++)' \
    '22	    printf' '23	    return 0;' '24	}'

check args sum 'run 1 2 3\nshow args\nrun\nset args 40 2\nrun\nshow args\n' \
    'sum = 6' 'program being debugged when it is started is "1 2 3".' \
    'sum = 6' 'sum = 42' 'started is "40 2".'

check commands sum 'break add\ncommands\nsilent\nreg rdi\ncontinue\nprint calls\nend\ninfo b\nrun 10 20 30\n' \
    '>silent' '>end' '        silent' '        print calls' \
    'rdi = 0xa (10)' 'rdi = 0x14 (20)' 'rdi = 0x1e (30)' 'sum = 60' \
    '!Breakpoint 1,' '!calls ='

check commands-stop sum 'break add\ncommands 1\nprint total\nend\nrun 5 6\ncontinue\ncontinue\n' \
    'Breakpoint 1, add () at targets/sum.c:14' 'total = 0 (0x0)' \
    'Breakpoint 1, add () at targets/sum.c:14' 'total = 5 (0x5)' 'sum = 11'

check define sum 'define upto\ntbreak $arg0\nrun $arg1 $arg2\nprint total\nend\nupto add 4 5\nnext\n\nprint total\nupto\nshow user upto\ndefine run\n' \
    'Type commands for definition of "upto".' \
    'Temporary breakpoint 1, add () at targets/sum.c:14' 'total = 0 (0x0)' \
    '15	    calls++;' '16	}' 'total = 4 (0x4)' \
    'Missing argument 0 in user function.' \
    'User command "upto":' '  run $arg1 $arg2' \
    '"run" is a tdb command: it cannot be redefined.'

check recursion sum 'define loop\nloop\nend\nloop\necho after\\n\n' \
    'Max user call depth exceeded -- command aborted.' 'after'

check echo sum 'echo a\\tb \\"c\\" \\\\ d\\n\n' 'a	b "c" \ d'

printf 'break add\ncommands\n  silent\n  print total\n  continue\nend\nfrobnicate\nrun 1 2\n' > "$B/bad.tdb"
check source sum "source $B/bad.tdb\ninfo b\nsource $B/nosuch.tdb\n" \
    "$B/bad.tdb:7: Error in sourced command file:" \
    'Undefined command: "frobnicate".' '!Started process' \
    '        print total' "$B/nosuch.tdb: No such file or directory."

out=$(timeout 30 "$B/tdb" -nx -batch -x scripts/trace-sum.tdb "$B/targets/sum" 10 20 30 2>&1)
expect batch "$out:$?" 'rdi = 0xa (10)' 'rdi = 0x1e (30)' \
    'Temporary breakpoint 2, main ()' 'total = 60' 'calls = 3' '[process killed]' \
    '!(tdb)' ':0'
out=$(timeout 30 "$B/tdb" -nx -batch -x "$B/bad.tdb" -ex 'echo still here\n' "$B/targets/sum" 2>&1)
expect batch-error "$out:$?" "$B/bad.tdb:7: Error in sourced command file:" 'still here' ':0'
out=$(timeout 30 "$B/tdb" -nx -batch -ex 'echo first\n' -ex 'print nosuch' "$B/targets/sum" 2>&1)
expect batch-fail "$out:$?" 'first' 'No symbol "nosuch" in the program.' ':1'
rm -f "$B/bad.tdb"

mkdir -p "$B/home"
printf 'echo [tdbinit]\\n\ndefine counts\nprint calls\nend\n' > "$B/home/.tdbinit"
out=$(printf 'break main\nrun\ncounts\n' | HOME="$B/home" timeout 30 "$B/tdb" "$B/targets/sum" 2>&1)
expect tdbinit "$out" '[tdbinit]' 'Reading symbols' 'calls = 0 (0x0)'
out=$(printf 'counts\n' | HOME="$B/home" timeout 30 "$B/tdb" -nx "$B/targets/sum" 2>&1)
expect tdbinit-nx "$out" '!tdbinit' 'Undefined command: "counts".'
rm -r "$B/home"

# Chapter 9: checkpoints, record and replay.
check checkpoint ledger 'break 72\nrun targets/deposits.txt\ncheckpoint\ninfo checkpoints\ncontinue\nrestart 1\ncontinue\ninfo checkpoints\ndelete checkpoint 1\ninfo checkpoints\nrestart 1\n' \
    'checkpoint 1: fork returned pid' '  1 process ' 'file targets/ledger.c, line 72' \
    'held 907' '[process exited with status 1]' 'Switching to process' \
    '#0  main () at targets/ledger.c:72' 'held 907' '[process exited with status 1]' \
    '  1 process ' 'No checkpoints.' 'Not found: checkpoint id 1'

check reverse-watch ledger 'break 65\nrun targets/deposits.txt\ndelete\nrecord\nbreak 72\ncontinue\nwatch accounts+64 8\nreverse-continue\nreverse-continue\nreverse-continue\nbt\nreverse-finish\ninfo record\ncontinue\n' \
    'Breakpoint 2, main () at targets/ledger.c:72' \
    'Old value = 182 (0xb6)' 'New value = 167 (0xa7)' 'targets/ledger.c:68' \
    'Old value = 167 (0xa7)' 'New value = 107 (0x6b)' \
    'Old value = 107 (0x6b)' 'New value = 0 (0x0)' 'in set_name () at targets/ledger.c:25' \
    '#2  0x' 'in main () at targets/ledger.c:66' \
    'Run back to call of #0  ' 'in find () at targets/ledger.c:47' \
    'Replay mode:' 'Current instruction number is' \
    'Old value = 0 (0x0)' 'New value = 107 (0x6b)' 'set_name () at targets/ledger.c:24'

check reverse-tour ledger 'break 65\nrun targets/deposits.txt\ndelete\nrecord\nbreak set_name\ncontinue\nreverse-finish\nreverse-next\nreverse-next\nreverse-stepi\nnext\nrecord goto begin\nrecord goto end\ninfo record\nrecord goto 99999\nnext\n' \
    'Breakpoint 2, set_name () at targets/ledger.c:24' \
    'in find () at targets/ledger.c:47' '47	    set_name(a, name);' \
    '46	    struct account *a = &accounts[naccounts++];' '	44	' \
    '46	    struct account' 'Go backward to insn number 0' '#0  main () at targets/ledger.c:65' \
    'Go forward to insn number' '#0  set_name () at targets/ledger.c:24' 'Record mode:' \
    "Target insn '99999' not found." '25	        a->name[i] = name[i];'

check reverse-crash todo 'break 72\nrun targets/plan.txt\ndelete\nrecord\ncontinue\nwatch cmds+72 8\nreverse-continue\nreverse-continue\ncontinue\ncontinue\ncontinue\n' \
    'Program received signal SIGSEGV' 'Old value = 1 (0x1)' 'New value = 0 (0x0)' \
    'in parse () at targets/todo.c:39' 'No more reverse-execution history.' \
    'main () at targets/todo.c:72' 'Old value = 0 (0x0)' 'New value = 1 (0x1)' \
    'No more reverse-execution history.' 'tdb cannot record that' \
    '[process killed by SIGSEGV'

check record-exit ledger 'break 72\nrun targets/deposits.txt\nrecord\ncontinue\ncontinue\nrecord stop\ninfo record\ncontinue\n' \
    'held 907' 'The next instruction is syscall exit_group.' \
    'The next instruction is syscall exit_group.' \
    'Process record is stopped and all execution logs are deleted.' \
    'No recording is currently active.' '[process exited with status 1]'

check record-diverge ledger 'break 65\nrun targets/deposits.txt\ndelete\nrecord\nbreak 72\ncontinue\nbreak find\nreverse-continue\ncheckpoint\nset naccounts 0\nnext\ninfo record\n' \
    'Breakpoint 3, find () at targets/ledger.c:41' \
    'Cannot make a checkpoint in the past' \
    'the replay went another way at instruction' 'Record mode:'

check record-errors ledger 'reverse-stepi\nbreak main\nrun\nreverse-stepi\nrecord\nrecord\nreverse-continue\nrecord goto 1\nrestart 9\ninfo record\n' \
    'The program is not being run.' 'Target native does not support this command.' \
    'The process is already being recorded.' 'No more reverse-execution history.' \
    "Target insn '1' not found." 'Not found: checkpoint id 9' 'Active record target: tdb'

check record-thread workers 'break main\nrun\nrecord\ncontinue\nrecord stop\ncontinue\n' \
    'makes a new thread or process' 'Process record is stopped' 'total: 600'

echo "itest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
