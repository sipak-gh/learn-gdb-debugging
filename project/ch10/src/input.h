/* input.h - where tdb's commands come from: the terminal (or a pipe) on
 * stdin, or a file of commands (source, -x, ~/.tdbinit). Files nest: a
 * file can source another one. */
#ifndef TDB_INPUT_H
#define TDB_INPUT_H

#include <stdbool.h>

#define INPUT_LINE_MAX 1024

/* Call once. batch: never read stdin (tdb -batch). */
void input_init(bool batch);

/* Read the next line from now on from the file at path: 0, or -1 (with
 * errno set) if it cannot be opened. input_pop() goes back. */
int input_push(const char *path);
void input_pop(void);

/* The next line, without its newline, or NULL at the end of the input.
 * From stdin, prompt is shown first; when stdin is not a terminal the
 * line is shown after it, so a piped session reads like a typed one.
 * The line stays valid until the next call. */
char *input_read(const char *prompt);

/* Is the line being read typed by the user (or piped to stdin)? */
bool input_from_user(void);

/* "FILE:LINE" of the line just read from a file; NULL when reading stdin. */
const char *input_where(void);

#endif
