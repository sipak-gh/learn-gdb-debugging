/* data.c - globals of different sizes, a string and an array; a target for
 * reading and writing memory and registers. */
#include <stdio.h>

char greeting[] = "hello, tdb";
short small = -2;
int counter = 41;
long big = 0x1122334455667788;
int table[4] = { 10, 20, 30, 40 };

int bump(int by)
{
    counter += by;
    return counter;
}

int main(void)
{
    int r = bump(1);

    printf("%s: counter=%d r=%d table[2]=%d\n", greeting, counter, r, table[2]);
    return 0;
}
