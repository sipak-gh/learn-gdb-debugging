/* spin.c - calls tick() N times (default 100000); for timing breakpoints.
 *
 *     spin [N]
 */
#include <stdio.h>
#include <stdlib.h>

int tick(int i)
{
    return i & 1;
}

int main(int argc, char *argv[])
{
    int n = argc > 1 ? atoi(argv[1]) : 100000;
    long odd = 0;

    for (int i = 0; i < n; i++)
        odd += tick(i);
    printf("odd=%ld\n", odd);
    return 0;
}
