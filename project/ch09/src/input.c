/* input.c - reading command lines from stdin or from files. */
#include "input.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "proc.h"

#ifdef TDB_READLINE
#include <readline/history.h>
#include <readline/readline.h>
#endif

#define INPUT_DEPTH 16

static struct source {
    FILE *f;
    char *name;
    int line;
} files[INPUT_DEPTH];
static int depth;               /* files being read; 0: reading stdin */

static bool batch;
static bool echo;               /* stdin is not a terminal: show each line */
static char buf[INPUT_LINE_MAX];

#ifdef TDB_READLINE
static bool use_readline;
static bool quit_typed;

/* readline reads each key with this. Ctrl-C makes read() fail with EINTR
 * (tdb's SIGINT handler has no SA_RESTART): throw the line away and end
 * it, so that readline returns. */
static int getc_or_quit(FILE *f)
{
    unsigned char c;

    for (;;) {
        ssize_t n = read(fileno(f), &c, 1);
        if (n == 1)
            return c;
        if (n == 0 || errno != EINTR)
            return EOF;
        if (proc_take_ctrl_c()) {
            quit_typed = true;
            rl_replace_line("", 0);
            return '\n';
        }
    }
}
#endif

void input_init(bool batch_mode)
{
    batch = batch_mode;
    echo = !isatty(STDIN_FILENO);
#ifdef TDB_READLINE
    if (!echo && !batch) {
        use_readline = true;
        rl_readline_name = "tdb";       /* for "$if tdb" in ~/.inputrc */
        rl_catch_signals = 0;           /* tdb handles SIGINT itself */
        rl_getc_function = getc_or_quit;
        using_history();
    }
#endif
}

int input_push(const char *path)
{
    if (depth == INPUT_DEPTH) {
        errno = ELOOP;
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    files[depth] = (struct source){ f, strdup(path), 0 };
    depth++;
    return 0;
}

void input_pop(void)
{
    if (depth == 0)
        return;
    depth--;
    fclose(files[depth].f);
    free(files[depth].name);
}

static char *read_stdin(const char *prompt)
{
    for (;;) {
#ifdef TDB_READLINE
        if (use_readline) {
            quit_typed = false;
            char *s = readline(prompt);
            if (s == NULL) {
                putchar('\n');          /* Ctrl-D: end the prompt's line */
                return NULL;
            }
            if (quit_typed) {
                free(s);
                printf("Quit\n");
                continue;
            }
            if (s[0] != '\0')
                add_history(s);
            snprintf(buf, sizeof buf, "%s", s);
            free(s);
            return buf;
        }
#endif
        printf("%s", prompt);
        fflush(stdout);
        if (fgets(buf, sizeof buf, stdin) == NULL) {
            if (ferror(stdin) && errno == EINTR) {
                /* Ctrl-C at the prompt: forget the line, as gdb does. */
                clearerr(stdin);
                proc_take_ctrl_c();
                printf("Quit\n");
                continue;
            }
            putchar('\n');
            return NULL;
        }
        buf[strcspn(buf, "\n")] = '\0';
        if (echo)
            printf("%s\n", buf);
        return buf;
    }
}

char *input_read(const char *prompt)
{
    if (depth == 0)
        return batch ? NULL : read_stdin(prompt);
    struct source *s = &files[depth - 1];
    if (fgets(buf, sizeof buf, s->f) == NULL)
        return NULL;
    s->line++;
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

bool input_from_user(void)
{
    return depth == 0 && !batch;
}

const char *input_where(void)
{
    static char where[INPUT_LINE_MAX];

    if (depth == 0)
        return NULL;
    snprintf(where, sizeof where, "%s:%d", files[depth - 1].name, files[depth - 1].line);
    return where;
}
