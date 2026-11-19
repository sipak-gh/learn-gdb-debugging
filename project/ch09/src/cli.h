/* cli.h - the parts of tdb's command language that need no process:
 * finding a command from an abbreviation, putting a user command's
 * arguments into its lines, and the escapes of echo. */
#ifndef TDB_CLI_H
#define TDB_CLI_H

#include <stddef.h>

/* Flags of a command. */
#define CLI_ALIAS       1u   /* another name for a command, as "c" for continue */
#define CLI_REPEAT      2u   /* an empty line runs the command again */
#define CLI_REPEAT_BARE 4u   /* ... but without its arguments ("list") */

struct cli_cmd {
    const char *name;
    int id;              /* what the caller runs, in its own numbering */
    unsigned flags;
};

enum cli_match { CLI_FOUND, CLI_AMBIGUOUS, CLI_UNDEFINED };

/* Look word up in the n commands of table, as gdb does: a command called
 * word, else the one command whose name starts with word. Aliases are
 * names too, and names with the same id count as one command. On
 * CLI_FOUND, *found is the command. */
enum cli_match cli_lookup(const struct cli_cmd *table, int n, const char *word,
                          const struct cli_cmd **found);

/* The names in table that start with word, sorted, separated by ", ":
 * "step, stepi" (for the "Ambiguous command" message). Returns buf. */
char *cli_matches(const struct cli_cmd *table, int n, const char *word, char *buf,
                  size_t size);

/* Copy line to out (size bytes), replacing $argc by nargs and $argN by
 * args[N]. Returns 0; -1 if line uses an argument that was not given (its
 * number goes in *missing); -2 if out is too small. */
int cli_expand(const char *line, char *const args[], int nargs, char *out, size_t size,
               int *missing);

/* echo's text: \n, \t, \\, \" and "\ " are replaced; a backslash before
 * any other character is dropped. Returns out. */
char *cli_unescape(const char *text, char *out, size_t size);

#endif
