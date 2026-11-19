/* scores.c - keep the best N scores of a game in a heap array.
 *
 *     ./scores-asan
 *
 * Bug: it prints the right result, but AddressSanitizer reports a
 * heap-buffer-overflow. Build scores-asan and find the bad write.
 */
#include <stdio.h>
#include <stdlib.h>

/* Insert score into the sorted (best first) array best[0..n-1]. */
static void insert(int *best, int n, int score)
{
    int i = n;

    while (i > 0 && best[i - 1] < score) {
        best[i] = best[i - 1];
        i--;
    }
    if (i < n)
        best[i] = score;
}

int main(void)
{
    int scores[] = { 40, 75, 10, 90, 60, 85 };
    int n = 3;
    int *best = calloc(n, sizeof *best);

    for (size_t k = 0; k < sizeof scores / sizeof scores[0]; k++)
        insert(best, n, scores[k]);
    printf("top %d: %d %d %d\n", n, best[0], best[1], best[2]);
    free(best);
    return 0;
}
