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

echo "itest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
