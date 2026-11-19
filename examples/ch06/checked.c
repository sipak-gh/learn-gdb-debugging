/* checked.c - add up the sizes given on the command line, refusing to
 * overflow an int.
 *
 *     ./checked 2000000000 100000000 200000000
 */
#include <stdio.h>
#include <stdlib.h>

/* Add n (> 0) to *total; return -1 instead if the sum does not fit. */
int add_size(int *total, int n)
{
    if (*total + n < *total)    /* wrapped around? */
        return -1;
    *total += n;
    return 0;
}

int main(int argc, char *argv[])
{
    int total = 0;

    for (int i = 1; i < argc; i++) {
        int n = atoi(argv[i]);
        if (n <= 0)
            continue;
        if (add_size(&total, n) < 0) {
            printf("too big: stopped at argument %d\n", i);
            return 1;
        }
    }
    printf("total: %d\n", total);
    return 0;
}
