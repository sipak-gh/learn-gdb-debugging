/* mean.c - the average of n values. */

double mean(const int *v, int n)
{
    long sum = 0;

    for (int i = 0; i < n; i++)
        sum += v[i];
    return (double)sum / n;
}
