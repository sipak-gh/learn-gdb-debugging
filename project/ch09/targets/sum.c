/* sum.c - add up the numbers given as arguments:
 *
 *     sum 10 20 30
 */
#include <stdio.h>
#include <stdlib.h>

/* Globals, so that tdb's print can show them. */
long total;
int calls;

void add(long x)
{
    total += x;
    calls++;
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
        add(atol(argv[i]));
    printf("sum = %ld\n", total);
    return 0;
}
