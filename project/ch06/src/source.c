/* source.c - showing lines of the program's source files. The line table
 * says which file and line; the file itself is read again each time, as
 * it is on disk now (if it was edited since the build, the lines shown
 * will not match the code: gdb has the same problem). */
#include "source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int src_print(const char *path, int first, int last)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char *text = NULL;
    size_t cap = 0;
    int n = 0, printed = 0;
    while (getline(&text, &cap, f) >= 0 && ++n <= last) {
        if (n >= first) {
            size_t len = strlen(text);
            printf("%d\t%s%s", n, text, len > 0 && text[len - 1] == '\n' ? "" : "\n");
            printed++;
        }
    }
    free(text);
    fclose(f);
    return printed;
}

int src_count(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    int n = 0, c, last = '\n';
    while ((c = getc(f)) != EOF) {
        if (c == '\n')
            n++;
        last = c;
    }
    fclose(f);
    return last == '\n' ? n : n + 1;    /* a last line without its newline */
}
