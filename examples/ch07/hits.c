/* hits.c - count the requests in a log with four threads, each counting
 * a quarter of them. The total is wrong, and different every run.
 *
 *     ./hits
 */
#include <pthread.h>
#include <stdio.h>

#define THREADS 4
#define PER_THREAD 1000000

long hits;

void *count(void *arg)
{
    (void)arg;
    for (int i = 0; i < PER_THREAD; i++)
        hits++;
    return NULL;
}

int main(void)
{
    pthread_t t[THREADS];

    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, count, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    printf("hits: %ld (expected %d)\n", hits, THREADS * PER_THREAD);
    return 0;
}
