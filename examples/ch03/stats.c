/* stats.c - the mean and the largest of some measurements.
 *
 * Bug: the mean is wrong. The compiler even says why, if you read its
 * warnings; gdb shows it too.
 */
#include <stdio.h>

static double mean(const int data[])
{
    size_t n = sizeof data / sizeof data[0];
    long sum = 0;

    for (size_t i = 0; i < n; i++)
        sum += data[i];
    return (double)sum / n;
}

static int largest(const int *data, size_t n)
{
    int max = data[0];

    for (size_t i = 1; i < n; i++)
        if (data[i] > max)
            max = data[i];
    return max;
}

int main(void)
{
    int readings[10] = { 12, 15, 11, 19, 14, 22, 18, 13, 17, 16 };
    size_t n = sizeof readings / sizeof readings[0];

    printf("%zu readings, mean %.1f, largest %d\n", n, mean(readings),
           largest(readings, n));
    return 0;
}
