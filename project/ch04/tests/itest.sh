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

# check NAME TARGET COMMANDS EXPECTED...
check() {
    name=$1 target=$2 commands=$3
    shift 3
    out=$(printf "$commands" | timeout 30 "$B/tdb" "$B/targets/$target" 2>&1)
    rest=$out
    for want in "$@"; do
        case $rest in
        *"$want"*)
            rest=${rest#*"$want"}
            ;;
        *)
            echo "FAIL $name: missing \"$want\""
            echo "--- output was:"
            echo "$out"
            echo "---"
            fail=$((fail + 1))
            return
            ;;
        esac
    done
    pass=$((pass + 1))
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

check rerun hello 'run\nrun\n' \
    '[process exited with status 0]' \
    'Started process' \
    '[process exited with status 0]'

check not-running hello 'continue\nkill\n' \
    'The program is not being run.' \
    'The program is not being run.'

check unknown hello 'frobnicate\n' \
    'Unknown command "frobnicate"'

SQUARE=$(addr loop square)
MAIN=$(addr loop main)

check breakpoint loop "break $SQUARE\nrun\ncontinue\ninfo breakpoints\n" \
    "Breakpoint 1 at $SQUARE" \
    'Breakpoint 1, 0x0000' \
    'Breakpoint 1, 0x0000' \
    'keep' '2'

# The program must still compute the right numbers: stepping over the
# breakpoints executed the original instructions.
check step-over loop "break $SQUARE\nrun\nc\nc\nc\nc\nc\n" \
    'i=0 total=0' 'i=1 total=1' 'i=2 total=5' 'i=3 total=14' 'i=4 total=30' \
    '[process exited with status 0]'

check tbreak loop "tbreak $MAIN\nrun\ninfo breakpoints\ncontinue\n" \
    'Temporary breakpoint 1, 0x0000' \
    'No breakpoints.' \
    '[process exited with status 0]'

check ignore loop "break $SQUARE\nignore 1 3\nrun\ninfo breakpoints\n" \
    'Will ignore next 3 crossings of breakpoint 1.' \
    'Breakpoint 1, 0x0000' \
    'keep  '"$SQUARE"'   0x0000' '4'

check delete loop "break $SQUARE\nrun\ndelete 1\ncontinue\n" \
    'Breakpoint 1, 0x0000' \
    'i=4 total=30' \
    '[process exited with status 0]'

check bad-break loop 'break nowhere\ndelete 7\nbreak\n' \
    'No symbol "nowhere" in the program.' \
    'No breakpoint number 7.' \
    'Usage: break ADDRESS|SYMBOL[+OFFSET]'

# Chapter 3: symbols, registers, memory.

check symbols loop 'break square\nrun\ninfo breakpoints\ninfo symbol $rip\n' \
    'Reading symbols from' \
    "Breakpoint 1 at $SQUARE" \
    'Breakpoint 1, 0x0000555555555149 in square ()' \
    '<square>' \
    'square'

check symbol-offset loop 'tbreak main+4\nrun\n' \
    "Temporary breakpoint 1 at $(printf '0x%x' $((MAIN + 4)))" \
    'Temporary breakpoint 1, 0x' 'in main+4 ()'

check regs loop 'break square\nrun\nregs\n' \
    'rax            0x' \
    'rip            0x555555555149      0x555555555149 <square>' \
    'eflags         0x' '[ ' ' IF ' \
    'orig_rax'

# The first argument is in rdi: square(10) instead of square(0).
check set-reg loop 'break square\nrun\nreg rdi 10\ndelete\ncontinue\n' \
    'rdi = 0xa (10)' \
    'i=0 total=100'

# tdb leaves its breakpoints in memory: x shows the 0xCC.
check x-shows-int3 loop 'break square\nrun\nx square 4\n' \
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
    'in main+' \
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
    'in step+' \
    'Old value = 2 (0x2)' 'New value = 6 (0x6)' \
    'counter=6 limit=3' 'exited with status 0'

check rwatch watched 'rwatch limit\nrun\ncontinue\n' \
    'Hardware read watchpoint 1: limit (4 bytes at 0x' \
    'Hardware read watchpoint 1: limit' 'Value = 3 (0x3)' 'in main+' \
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

echo "itest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
