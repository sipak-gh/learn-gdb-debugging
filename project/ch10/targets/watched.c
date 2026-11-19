/* watched.c - a global changed in a loop and another one read once; a
 * target for watchpoints. */
#include <stdio.h>

int counter;
int limit = 3;

/* Adds even numbers only: step(0) writes counter without changing it. */
void step(int i)
{
    if (i % 2 == 0)
        counter += i;
}

int main(void)
{
    for (int i = 0; i < 6; i++)
        step(i);
    printf("counter=%d limit=%d\n", counter, limit);
    return 0;
}
