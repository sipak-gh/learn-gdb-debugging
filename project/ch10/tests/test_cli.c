/* test_cli.c - tests for cli.c: lookup, $arg expansion, echo escapes. */
#include "cli.h"
#include "test.h"
#include "tests.h"

enum { STEP, STEPI, SET, SHOW, CONTINUE, COMMANDS, FINISH };

static const struct cli_cmd table[] = {
    { "step", STEP, 0 },         { "s", STEP, CLI_ALIAS },
    { "stepi", STEPI, 0 },       { "si", STEPI, CLI_ALIAS },
    { "set", SET, 0 },           { "show", SHOW, 0 },
    { "continue", CONTINUE, 0 }, { "c", CONTINUE, CLI_ALIAS },
    { "commands", COMMANDS, 0 }, { "finish", FINISH, 0 },
    { "fin", FINISH, CLI_ALIAS },
};
#define N ((int)(sizeof table / sizeof table[0]))

/* The id lookup finds for word, or -1 if ambiguous, -2 if undefined. */
static int lookup(const char *word)
{
    const struct cli_cmd *c;

    switch (cli_lookup(table, N, word, &c)) {
    case CLI_FOUND:
        return c->id;
    case CLI_AMBIGUOUS:
        return -1;
    default:
        return -2;
    }
}

void test_cli_lookup(void)
{
    CHECK_INT(lookup("step"), STEP);
    CHECK_INT(lookup("stepi"), STEPI);
    /* An exact name wins, even if it starts other names too. */
    CHECK_INT(lookup("s"), STEP);
    CHECK_INT(lookup("c"), CONTINUE);
    /* A unique start. */
    CHECK_INT(lookup("cont"), CONTINUE);
    CHECK_INT(lookup("sh"), SHOW);
    /* finish and its alias fin both start with "fi": one command. */
    CHECK_INT(lookup("fi"), FINISH);
    CHECK_INT(lookup("ste"), -1);
    CHECK_INT(lookup("co"), -1);
    CHECK_INT(lookup("frobnicate"), -2);
    CHECK_INT(lookup("steps"), -2);
}

void test_cli_matches(void)
{
    char buf[100];

    CHECK_STR(cli_matches(table, N, "ste", buf, sizeof buf), "step, stepi");
    CHECK_STR(cli_matches(table, N, "s", buf, sizeof buf), "s, set, show, si, step, stepi");
    CHECK_STR(cli_matches(table, N, "x", buf, sizeof buf), "");
    /* Cut short, not overrun. */
    CHECK_STR(cli_matches(table, N, "co", buf, 10), "commands,");
}

void test_cli_expand(void)
{
    char *args[] = { "total", "8" };
    char out[64];
    int missing = -1;

    CHECK_INT(cli_expand("x $arg0 $arg1", args, 2, out, sizeof out, &missing), 0);
    CHECK_STR(out, "x total 8");
    CHECK_INT(cli_expand("echo $argc args\\n", args, 2, out, sizeof out, &missing), 0);
    CHECK_STR(out, "echo 2 args\\n");
    /* Not arguments: $args, $argcount, $rip. */
    CHECK_INT(cli_expand("print $args $argcount $rip", args, 2, out, sizeof out, &missing), 0);
    CHECK_STR(out, "print $args $argcount $rip");
    CHECK_INT(cli_expand("x $arg2", args, 2, out, sizeof out, &missing), -1);
    CHECK_INT(missing, 2);
    CHECK_INT(cli_expand("x $arg0", args, 0, out, sizeof out, &missing), -1);
    CHECK_INT(missing, 0);
    CHECK_INT(cli_expand("x $arg0", args, 2, out, 7, &missing), -2);
    CHECK_INT(cli_expand("x $arg0", args, 2, out, 8, &missing), 0);
}

void test_cli_unescape(void)
{
    char out[32];

    CHECK_STR(cli_unescape("a\\tb\\n", out, sizeof out), "a\tb\n");
    CHECK_STR(cli_unescape("\\ two \\\"q\\\" \\\\", out, sizeof out), " two \"q\" \\");
    CHECK_STR(cli_unescape("end\\", out, sizeof out), "end");
    CHECK_STR(cli_unescape("abcdef", out, 4), "abc");
}
