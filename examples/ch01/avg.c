/* avg.c - print the average of the numbers on the command line.
 *
 *     ./avg 90 85 77      should print 84.00
 */
#include <stdio.h>
#include <stdlib.h>

static double average(int n, char *nums[])
{
    int sum = 0;

    for (int i = 0; i < n; i++)
        sum += atoi(nums[i]);
    return sum / n;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: avg NUMBER...\n");
        return 2;
    }
    printf("%.2f\n", average(argc, argv));
    return 0;
}
