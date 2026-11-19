/* deep.c - main calls outer, outer calls middle, middle calls inner, and
 * inner calls strlen. With no argument the string is NULL and strlen
 * crashes inside libc. A target for backtraces.
 *
 *     deep [WORD]
 */
#include <stdio.h>
#include <string.h>

size_t inner(const char *s)
{
    return strlen(s);
}

size_t middle(const char *s)
{
    return inner(s) + 1;
}

size_t outer(const char *s)
{
    return middle(s) * 2;
}

int main(int argc, char **argv)
{
    const char *word = argc > 1 ? argv[1] : NULL;

    printf("%zu\n", outer(word));
    return 0;
}
