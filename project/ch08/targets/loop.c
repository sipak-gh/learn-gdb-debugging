/* loop.c - calls square() five times; a target for breakpoints. */
#include <stdio.h>

int square(int x)
{
    return x * x;
}

int main(void)
{
    int total = 0;

    for (int i = 0; i < 5; i++) {
        total += square(i);
        printf("i=%d total=%d\n", i, total);
    }
    return 0;
}
