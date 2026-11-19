/* reverse.c - reverse the order of the words in each argument.
 *
 *     ./reverse "one two three"     should print "three two one"
 */
#include <stdio.h>
#include <string.h>

#define MAXWORDS 8

/* Split s in place at spaces; store pointers to the words in words[]. */
static int split(char *s, char *words[])
{
    int n = 0;

    for (char *w = strtok(s, " "); w != NULL; w = strtok(NULL, " "))
        words[n++] = w;
    return n;
}

static void print_reversed(char *s)
{
    char *words[MAXWORDS];
    int n = split(s, words);

    for (int i = n; i > 0; i--)
        printf("%s%s", words[i], i > 1 ? " " : "\n");
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
        print_reversed(argv[i]);
    return 0;
}
