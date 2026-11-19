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
    expect "$name" "$(printf "$commands" | timeout 30 "$B/tdb" "$B/targets/$target" 2>&1)" "$@"
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

check rerun hello 'run\nrun\nrun now\n' \
    '[process exited with status 0]' \
    'Started process' \
    '[process exited with status 0]' \
    '    tdb PROGRAM ARGS...'

check not-running hello 'continue\nkill\n' \
    'The program is not being run.' \
    'The program is not being run.'

check unknown hello 'frobnicate\n' \
    'Unknown command "frobnicate"'

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

check bt-crash deep 'run\nbt\nsiginfo\n' \
    'Program received signal SIGSEGV' \
    'Fault address 0x0: SEGV_MAPERR (address not mapped to object).' \
    '#0  0x' '#1  0x' ' in middle () at targets/deep.c:17' '#3  0x' ' in main () at targets/deep.c:29' \
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
        timeout 30 "$B/tdb" -p $pid 2>&1)
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
    expect attach-eperm "$(timeout 30 "$B/tdb" -p $pid </dev/null 2>&1)" \
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
expect no-lines "$(printf 'list\nbreak 12\n' | timeout 30 "$B/tdb" /bin/true 2>&1)" \
    'No line information: was the program compiled with -g?' \
    'No line information: was the program compiled with -g?'

echo "itest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
