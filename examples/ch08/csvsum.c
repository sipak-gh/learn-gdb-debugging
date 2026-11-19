/* csvsum.c - add up the amount column (the third) of a CSV file. A field
 * may be quoted, to hold commas ("Smith, John") or quotes, written twice
 * ("say ""hi""").
 *
 *     ./csvsum orders.csv
 */
#include <stdio.h>
#include <stdlib.h>

enum state { START, PLAIN, QUOTED, QUOTE };

/* Split line into fields, in place: fields[i] points into line. Returns
 * the number of fields (at most max). */
int split(char *line, char *fields[], int max)
{
    enum state st = START;
    char *out = line;           /* quotes are taken out as we go */
    int n = 0;

    for (char *p = line;; p++) {
        char c = *p;
        if (c == '\n')
            c = '\0';
        switch (st) {
        case START:
            if (n == max)
                return n;
            fields[n++] = out;
            if (c == '"') {
                st = QUOTED;
                break;
            }
            st = PLAIN;     /* and c is the field's first character: */
            /* fall through */
        case PLAIN:
            if (c == ',' || c == '\0') {
                *out++ = '\0';
                st = START;
                if (c == '\0')
                    return n;
            } else {
                *out++ = c;
            }
            break;
        case QUOTED:
            if (c == '"')
                st = QUOTE;     /* the end of the field, or a doubled quote */
            else if (c == '\0') {
                *out = '\0';    /* no closing quote: take what we have */
                return n;
            } else
                *out++ = c;
            break;
        case QUOTE:
            if (c == '"') {
                *out++ = '"';
                st = PLAIN;
            } else {
                st = PLAIN;     /* after the closing quote: a ',' or the end */
                p--;
            }
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    char line[256];
    char *fields[8];
    long total = 0;
    int orders = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: csvsum FILE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    fgets(line, sizeof line, f);        /* the header */
    while (fgets(line, sizeof line, f) != NULL) {
        int n = split(line, fields, 8);
        if (n >= 3) {
            total += atol(fields[2]);
            orders++;
        }
    }
    fclose(f);
    printf("%d orders, total %ld\n", orders, total);
    return 0;
}
