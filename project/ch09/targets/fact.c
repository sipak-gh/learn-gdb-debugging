/* fact.c - a recursive function, a function that returns nothing, and a
 * loop. A target for step, next and finish. */
#include <stdio.h>

long fact(int n)
{
    if (n <= 1)
        return 1;
    return n * fact(n - 1);
}

void show(int n, long f)
{
    printf("%d! = %ld\n", n, f);
}

int main(void)
{
    long f = fact(5);
    show(5, f);
    for (int i = 0; i < 3; i++)
        f += i;
    printf("%ld\n", f);
    return 0;
}
