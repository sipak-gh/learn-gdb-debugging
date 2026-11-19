/* crash.c - writes through a NULL pointer. */
#include <stdio.h>

int main(void)
{
    int *p = NULL;

    puts("about to crash");
    fflush(stdout);
    *p = 42;
    return 0;
}
