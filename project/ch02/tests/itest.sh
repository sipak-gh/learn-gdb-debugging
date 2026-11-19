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

check bad-break loop 'break nowhere\ndelete 7\n' \
    'Usage: break ADDRESS' \
    'No breakpoint number 7.'

echo "itest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
