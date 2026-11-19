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

echo "itest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
