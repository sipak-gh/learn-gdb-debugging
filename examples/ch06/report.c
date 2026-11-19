/* report.c - print the average score.
 *
 *     ./report
 */
#include <stdio.h>

int mean(const int *v, int n);

int main(void)
{
    int scores[] = { 7, 9, 8, 10 };

    printf("mean score: %d\n", mean(scores, 4));
    return 0;
}
