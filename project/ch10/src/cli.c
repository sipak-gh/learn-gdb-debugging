/* cli.c - command lookup, argument expansion and echo escapes. */
#include "cli.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool starts_with(const char *name, const char *word)
{
    return strncmp(name, word, strlen(word)) == 0;
}

enum cli_match cli_lookup(const struct cli_cmd *table, int n, const char *word,
                          const struct cli_cmd **found)
{
    const struct cli_cmd *match = NULL;

    for (int i = 0; i < n; i++) {
        if (strcmp(table[i].name, word) == 0) {
            *found = &table[i];
            return CLI_FOUND;
        }
    }
    for (int i = 0; i < n; i++) {
        if (!starts_with(table[i].name, word))
            continue;
        if (match != NULL && match->id != table[i].id)
            return CLI_AMBIGUOUS;
        /* Prefer the command's own name to an alias of it. */
        if (match == NULL || (match->flags & CLI_ALIAS))
            match = &table[i];
    }
    if (match == NULL)
        return CLI_UNDEFINED;
    *found = match;
    return CLI_FOUND;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

char *cli_matches(const struct cli_cmd *table, int n, const char *word, char *buf,
                  size_t size)
{
    const char **names = malloc((size_t)n * sizeof *names);
    int count = 0;
    size_t len = 0;

    buf[0] = '\0';
    if (names == NULL)
        return buf;
    for (int i = 0; i < n; i++)
        if (starts_with(table[i].name, word))
            names[count++] = table[i].name;
    qsort(names, (size_t)count, sizeof *names, by_name);
    for (int i = 0; i < count && len < size; i++)
        len += (size_t)snprintf(buf + len, size - len, "%s%s", i > 0 ? ", " : "", names[i]);
    free(names);
    return buf;
}

int cli_expand(const char *line, char *const args[], int nargs, char *out, size_t size,
               int *missing)
{
    size_t len = 0;
    const char *p = line;

    while (*p != '\0') {
        const char *text = NULL;
        char number[16];
        char *end;

        if (strncmp(p, "$argc", 5) == 0 && !isalnum((unsigned char)p[5]) && p[5] != '_') {
            snprintf(number, sizeof number, "%d", nargs);
            text = number;
            p += 5;
        } else if (strncmp(p, "$arg", 4) == 0 && isdigit((unsigned char)p[4])) {
            long i = strtol(p + 4, &end, 10);
            if (i >= nargs) {
                *missing = (int)i;
                return -1;
            }
            text = args[i];
            p = end;
        }
        if (text == NULL) {
            if (len + 1 >= size)
                return -2;
            out[len++] = *p++;
            continue;
        }
        size_t tlen = strlen(text);
        if (len + tlen >= size)
            return -2;
        memcpy(out + len, text, tlen);
        len += tlen;
    }
    out[len] = '\0';
    return 0;
}

char *cli_unescape(const char *text, char *out, size_t size)
{
    size_t len = 0;

    for (const char *p = text; *p != '\0' && len + 1 < size; p++) {
        char c = *p;
        if (c == '\\' && p[1] != '\0') {
            c = *++p;
            if (c == 'n')
                c = '\n';
            else if (c == 't')
                c = '\t';
        } else if (c == '\\') {
            break;          /* a backslash at the very end */
        }
        out[len++] = c;
    }
    out[len] = '\0';
    return out;
}
